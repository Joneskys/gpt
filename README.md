# ESP8266 + DFPlayer Mini Production Audio Automation

This repository provides a **single-sketch NodeMCU firmware** focused on deterministic, long-uptime audio automation with DFPlayer Mini using BUSY-pin-controlled sequencing.

## Files
- `nodemcu_audio_automation.ino` - Main firmware sketch.
- `remote/flask_app.py` - PythonAnywhere Flask bridge for remote control.
- `remote/github_control.html` - GitHub Pages-compatible remote UI.

## Hardware Wiring (fixed)
- DFPlayer BUSY -> NodeMCU D6
- DFPlayer RX -> NodeMCU D4 **through 1k resistor**
- DFPlayer TX -> NodeMCU D5
- DFPlayer VCC -> VIN (5V)

## Key reliability design points
- Busy pin driven state machine: explicit **playback start** and **playback end** detection.
- Serialized queue engine: no overlap, no skip by design, no track interruption.
- Hourly scheduler independent from web UI and resilient to Wi-Fi loss.
- NTP failure fallback: every 5 minutes queues `22/014.mp3` (check internet prompt).
- EEPROM persistence for:
  - 24-hour alarm volume table
  - manual volume
  - mute state
  - bible hours (3)
  - song hours (2)

## Telegram one-time notification
In sketch:
```cpp
String telegramBotToken = "REPLACE_WITH_BOT_TOKEN";
String telegramChatId = "REPLACE_WITH_CHAT_ID";
```
Create bot token with BotFather, set chat ID, then flash.

Telegram API used:
`https://api.telegram.org/bot<token>/sendMessage`

## PythonAnywhere bridge
Update `NODEMCU_BASE` in `remote/flask_app.py`, deploy app, and use the exposed routes:
- `/play?folder=&file=`
- `/vol?value=`
- `/mute?state=0|1`
- `/hourvol?hour=&value=`
- `/bible1?hour=`
- `/bible2?hour=`
- `/bible3?hour=`
- `/song1?hour=`
- `/song2?hour=`

## GitHub HTML remote
Use `remote/github_control.html` in GitHub Pages and point its `Bridge URL` to PythonAnywhere endpoint.
