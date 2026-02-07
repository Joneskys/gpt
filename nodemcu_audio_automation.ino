#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>
#include <time.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

// ------------------------- Hardware mapping -------------------------
static const uint8_t PIN_DFPLAYER_BUSY = D6;  // LOW = playing, HIGH = idle
static const uint8_t PIN_DFPLAYER_RX   = D4;  // ESP RX <- DF TX
static const uint8_t PIN_DFPLAYER_TX   = D5;  // ESP TX -> DF RX through 1k resistor

// ------------------------- EEPROM layout ----------------------------
static const uint32_t SETTINGS_MAGIC = 0xA50D10A1;
static const uint8_t SETTINGS_VERSION = 1;

struct Settings {
  uint32_t magic;
  uint8_t version;
  uint8_t hourVol[24];   // 0..30
  uint8_t manualVol;     // 0..30
  uint8_t mute;          // 0/1
  uint8_t bibleHour[3];  // morning/noon/night
  uint8_t songHour[2];   // morning/night
  uint8_t reserved[8];
};

Settings settings;
bool settingsDirty = false;

// ------------------------- Networking/Time --------------------------
ESP8266WebServer server(80);
unsigned long lastWifiReconnectMs = 0;
unsigned long lastNtpRetryMs = 0;
unsigned long lastInternetPromptMs = 0;
bool ntpSynced = false;
bool bootHourTriggered = false;
int lastTriggeredHour = -1;

// Fill these with your bot credentials
String telegramBotToken = "REPLACE_WITH_BOT_TOKEN";
String telegramChatId = "REPLACE_WITH_CHAT_ID";
bool telegramSent = false;

static const long IST_OFFSET_SEC = 19800;

// ------------------------- DFPlayer + queue -------------------------
SoftwareSerial dfSerial(PIN_DFPLAYER_RX, PIN_DFPLAYER_TX);
DFRobotDFPlayerMini dfPlayer;

enum AudioState { AUDIO_IDLE, AUDIO_STARTING, AUDIO_PLAYING };
AudioState audioState = AUDIO_IDLE;
unsigned long audioStateSinceMs = 0;

struct TrackRequest {
  uint8_t folder;
  uint8_t file;
  bool isManual;
};

static const uint8_t AUDIO_QUEUE_CAP = 32;
TrackRequest queueBuf[AUDIO_QUEUE_CAP];
uint8_t qHead = 0;
uint8_t qTail = 0;
uint8_t qCount = 0;
TrackRequest currentTrack = {0, 0, false};

// ------------------------- Utility helpers --------------------------
int clampVol(int v) {
  if (v < 0) return 0;
  if (v > 30) return 30;
  return v;
}

int clampHour(int h) {
  if (h < 0) return 0;
  if (h > 23) return 23;
  return h;
}

bool enqueueTrack(uint8_t folder, uint8_t file, bool isManual) {
  if (qCount >= AUDIO_QUEUE_CAP) return false;
  queueBuf[qTail] = {folder, file, isManual};
  qTail = (qTail + 1) % AUDIO_QUEUE_CAP;
  qCount++;
  return true;
}

bool dequeueTrack(TrackRequest &out) {
  if (qCount == 0) return false;
  out = queueBuf[qHead];
  qHead = (qHead + 1) % AUDIO_QUEUE_CAP;
  qCount--;
  return true;
}

bool hasValidTime() {
  time_t now = time(nullptr);
  return now > 1700000000;
}

void markSettingsDirty() {
  settingsDirty = true;
}

void saveSettingsIfNeeded() {
  if (!settingsDirty) return;
  Settings currentInEeprom;
  EEPROM.get(0, currentInEeprom);
  if (memcmp(&currentInEeprom, &settings, sizeof(Settings)) != 0) {
    EEPROM.put(0, settings);
    EEPROM.commit();
  }
  settingsDirty = false;
}

void loadDefaultSettings() {
  memset(&settings, 0, sizeof(settings));
  settings.magic = SETTINGS_MAGIC;
  settings.version = SETTINGS_VERSION;
  for (int i = 0; i < 24; i++) settings.hourVol[i] = 20;
  settings.manualVol = 20;
  settings.mute = 0;
  settings.songHour[0] = 6;
  settings.songHour[1] = 21;
  settings.bibleHour[0] = 6;
  settings.bibleHour[1] = 12;
  settings.bibleHour[2] = 21;
  settingsDirty = true;
}

void loadSettings() {
  EEPROM.begin(256);
  EEPROM.get(0, settings);
  if (settings.magic != SETTINGS_MAGIC || settings.version != SETTINGS_VERSION) {
    loadDefaultSettings();
    saveSettingsIfNeeded();
  }
}

void applyVolumeForTrack(const TrackRequest &tr) {
  int vol = 0;
  if (settings.mute) {
    vol = 0;
  } else if (tr.isManual) {
    vol = settings.manualVol;
  } else {
    time_t now = time(nullptr);
    struct tm tinfo;
    localtime_r(&now, &tinfo);
    int h = hasValidTime() ? tinfo.tm_hour : 0;
    vol = settings.hourVol[h];
  }
  dfPlayer.volume(clampVol(vol));
}

void processAudioEngine() {
  bool busyPlaying = (digitalRead(PIN_DFPLAYER_BUSY) == LOW);

  if (audioState == AUDIO_IDLE) {
    if (dequeueTrack(currentTrack)) {
      applyVolumeForTrack(currentTrack);
      dfPlayer.playFolder(currentTrack.folder, currentTrack.file);
      audioState = AUDIO_STARTING;
      audioStateSinceMs = millis();
    }
    return;
  }

  if (audioState == AUDIO_STARTING) {
    if (busyPlaying) {
      audioState = AUDIO_PLAYING; // playback start detected
      return;
    }
    if (millis() - audioStateSinceMs > 2500) {
      // Start not confirmed; skip to avoid deadlock.
      audioState = AUDIO_IDLE;
    }
    return;
  }

  if (audioState == AUDIO_PLAYING) {
    if (!busyPlaying) {
      // Playback end detected
      audioState = AUDIO_IDLE;
    }
  }
}

void queueHourlySequence(int hour, int dayOfMonth) {
  uint8_t f = (uint8_t)(hour + 1); // file numbering 001..024
  enqueueTrack(11, f, false);
  enqueueTrack(22, f, false);

  if (hour == settings.songHour[0]) {
    uint8_t fileNo = (uint8_t)(dayOfMonth * 2 - 1);
    enqueueTrack(33, fileNo, false);
  }
  if (hour == settings.songHour[1]) {
    uint8_t fileNo = (uint8_t)(dayOfMonth * 2);
    enqueueTrack(33, fileNo, false);
  }

  if (hour == settings.bibleHour[0]) {
    uint8_t fileNo = (uint8_t)(dayOfMonth * 3 - 2);
    enqueueTrack(44, fileNo, false);
  }
  if (hour == settings.bibleHour[1]) {
    uint8_t fileNo = (uint8_t)(dayOfMonth * 3 - 1);
    enqueueTrack(44, fileNo, false);
  }
  if (hour == settings.bibleHour[2]) {
    uint8_t fileNo = (uint8_t)(dayOfMonth * 3);
    enqueueTrack(44, fileNo, false);
  }

  enqueueTrack(55, f, false);
}

void trySyncTime() {
  if (ntpSynced) return;
  if (millis() - lastNtpRetryMs < 10000) return;
  lastNtpRetryMs = millis();

  configTime(IST_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov", "in.pool.ntp.org");
  for (int i = 0; i < 5; i++) {
    delay(150);
    if (hasValidTime()) {
      ntpSynced = true;
      return;
    }
  }
}

void queueInternetCheckPromptIfNeeded() {
  if (ntpSynced) return;
  if (millis() - lastInternetPromptMs >= 300000UL) {
    lastInternetPromptMs = millis();
    enqueueTrack(22, 14, false); // 014.mp3: "Check internet"
  }
}

void schedulerTick() {
  if (!hasValidTime()) {
    queueInternetCheckPromptIfNeeded();
    return;
  }

  ntpSynced = true;
  time_t now = time(nullptr);
  struct tm tinfo;
  localtime_r(&now, &tinfo);

  int hour = tinfo.tm_hour;
  int day = tinfo.tm_mday;

  if (!bootHourTriggered) {
    queueHourlySequence(hour, day);
    lastTriggeredHour = hour;
    bootHourTriggered = true;
    return;
  }

  if (hour != lastTriggeredHour) {
    queueHourlySequence(hour, day);
    lastTriggeredHour = hour;
  }
}

void sendTelegramIpOnce() {
  if (telegramSent) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (telegramBotToken.startsWith("REPLACE") || telegramChatId.startsWith("REPLACE")) {
    telegramSent = true;
    return;
  }

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + telegramBotToken + "/sendMessage";
  String body = "chat_id=" + telegramChatId +
                "&text=ESP8266%20connected%0ASSID:%20" + WiFi.SSID() +
                "%0AIP:%20" + WiFi.localIP().toString();

  if (https.begin(*client, url)) {
    https.addHeader("Content-Type", "application/x-www-form-urlencoded");
    https.POST(body);
    https.end();
  }
  telegramSent = true;
}

void wifiMaintainTick() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiReconnectMs > 30000UL) {
    lastWifiReconnectMs = millis();
    WiFi.reconnect();
  }
}

String htmlPage() {
  String s = F(R"HTML(
<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>ESP Audio Control</title>
<style>body{font-family:Arial;margin:16px;max-width:720px}label{display:block;margin-top:10px}input,select,button{padding:8px;margin-top:4px}button{cursor:pointer}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}.small{font-size:12px;color:#555}</style>
</head><body>
<h2>ESP8266 Audio Automation</h2>
<div id='msg'></div>
<label>Manual Play folder/file <input id='folder' type='number' min='1' max='99' value='11'> <input id='file' type='number' min='1' max='255' value='1'> <button onclick='playNow()'>Play</button></label>
<label>Manual Volume (0-30) <input id='manvol' type='number' min='0' max='30' value='20'> <button onclick='setManualVol()'>Set</button></label>
<label>Mute <select id='mute'><option value='0'>Unmute</option><option value='1'>Mute</option></select> <button onclick='setMute()'>Apply</button></label>
<h3>Hour-wise Alarm Volume</h3><div class='grid' id='volGrid'></div><button onclick='saveHourVol()'>Save Volumes</button>
<h3>Bible Hours</h3>
<label>Morning <select id='b1'></select></label>
<label>Noon <select id='b2'></select></label>
<label>Night <select id='b3'></select></label>
<button onclick='saveBible()'>Save Bible Hours</button>
<h3>Song Hours</h3>
<label>Morning <select id='s1'></select></label>
<label>Night <select id='s2'></select></label>
<button onclick='saveSong()'>Save Song Hours</button>
<p class='small'>All actions are async and do not reload page.</p>
<script>
const msg=(t)=>{document.getElementById('msg').innerText=t;setTimeout(()=>document.getElementById('msg').innerText='',2500)};
const req=(u)=>fetch(u).then(r=>r.json()).then(j=>msg(j.status||'ok')).catch(()=>msg('request failed'));
for(let h=0;h<24;h++){const d=document.createElement('div');d.innerHTML=`${h}: <input id='hv${h}' type='number' min='0' max='30' value='20' style='width:58px'>`;document.getElementById('volGrid').appendChild(d)}
for(let h=0;h<24;h++){['b1','b2','b3','s1','s2'].forEach(id=>{const o=document.createElement('option');o.value=h;o.text=('0'+h).slice(-2);document.getElementById(id).appendChild(o);});}
function playNow(){req(`/play?folder=${folder.value}&file=${file.value}`)}
function setManualVol(){req(`/vol?value=${manvol.value}`)}
function setMute(){req(`/mute?state=${mute.value}`)}
function saveHourVol(){for(let h=0;h<24;h++)req(`/hourvol?hour=${h}&value=${document.getElementById('hv'+h).value}`)}
function saveBible(){req(`/bible1?hour=${b1.value}`);req(`/bible2?hour=${b2.value}`);req(`/bible3?hour=${b3.value}`)}
function saveSong(){req(`/song1?hour=${s1.value}`);req(`/song2?hour=${s2.value}`)}
</script></body></html>
)HTML");
  return s;
}

void sendJsonOk(const String &status) {
  server.send(200, "application/json", "{\"status\":\"" + status + "\"}");
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", htmlPage());
  });

  server.on("/play", HTTP_GET, []() {
    int folder = server.arg("folder").toInt();
    int file = server.arg("file").toInt();
    if (folder < 1 || folder > 99 || file < 1 || file > 255) {
      server.send(400, "application/json", "{\"status\":\"invalid folder/file\"}");
      return;
    }
    bool ok = enqueueTrack((uint8_t)folder, (uint8_t)file, true);
    sendJsonOk(ok ? "queued" : "queue_full");
  });

  server.on("/vol", HTTP_GET, []() {
    int value = clampVol(server.arg("value").toInt());
    if (settings.manualVol != value) {
      settings.manualVol = value;
      markSettingsDirty();
    }
    sendJsonOk("manual_volume_saved");
  });

  server.on("/mute", HTTP_GET, []() {
    int st = server.arg("state").toInt() ? 1 : 0;
    if (settings.mute != st) {
      settings.mute = st;
      markSettingsDirty();
    }
    sendJsonOk(st ? "muted" : "unmuted");
  });

  server.on("/hourvol", HTTP_GET, []() {
    int h = clampHour(server.arg("hour").toInt());
    int v = clampVol(server.arg("value").toInt());
    if (settings.hourVol[h] != v) {
      settings.hourVol[h] = v;
      markSettingsDirty();
    }
    sendJsonOk("hour_volume_saved");
  });

  server.on("/bible1", HTTP_GET, []() {
    int h = clampHour(server.arg("hour").toInt());
    if (settings.bibleHour[0] != h) {
      settings.bibleHour[0] = h;
      markSettingsDirty();
    }
    sendJsonOk("bible1_saved");
  });

  server.on("/bible2", HTTP_GET, []() {
    int h = clampHour(server.arg("hour").toInt());
    if (settings.bibleHour[1] != h) {
      settings.bibleHour[1] = h;
      markSettingsDirty();
    }
    sendJsonOk("bible2_saved");
  });

  server.on("/bible3", HTTP_GET, []() {
    int h = clampHour(server.arg("hour").toInt());
    if (settings.bibleHour[2] != h) {
      settings.bibleHour[2] = h;
      markSettingsDirty();
    }
    sendJsonOk("bible3_saved");
  });

  server.on("/song1", HTTP_GET, []() {
    int h = clampHour(server.arg("hour").toInt());
    if (settings.songHour[0] != h) {
      settings.songHour[0] = h;
      markSettingsDirty();
    }
    sendJsonOk("song1_saved");
  });

  server.on("/song2", HTTP_GET, []() {
    int h = clampHour(server.arg("hour").toInt());
    if (settings.songHour[1] != h) {
      settings.songHour[1] = h;
      markSettingsDirty();
    }
    sendJsonOk("song2_saved");
  });

  server.begin();
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.autoConnect("AudioAutomationSetup");
}

void setupDfPlayer() {
  pinMode(PIN_DFPLAYER_BUSY, INPUT_PULLUP);
  dfSerial.begin(9600);
  if (dfPlayer.begin(dfSerial, true, false)) {
    dfPlayer.setTimeOut(300);
    dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
    dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
    dfPlayer.volume(settings.manualVol);
  }
}

void setup() {
  loadSettings();
  setupDfPlayer();
  setupWifi();
  setupWebServer();
  trySyncTime();
  sendTelegramIpOnce();
}

void loop() {
  server.handleClient();
  processAudioEngine();
  schedulerTick();
  wifiMaintainTick();
  trySyncTime();
  sendTelegramIpOnce();
  saveSettingsIfNeeded();
  delay(2);
}
