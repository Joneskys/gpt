from flask import Flask, request, jsonify
import requests

app = Flask(__name__)

# Publicly reachable IP/DNS of NodeMCU on LAN/VPN/port-forward target
NODEMCU_BASE = "http://192.168.1.50"


def forward(path, params):
    try:
        r = requests.get(f"{NODEMCU_BASE}{path}", params=params, timeout=4)
        return jsonify({"ok": r.ok, "status": r.text[:200]}), (200 if r.ok else 502)
    except Exception as exc:
        return jsonify({"ok": False, "error": str(exc)}), 502


@app.route("/")
def health():
    return jsonify({"service": "audio-bridge", "target": NODEMCU_BASE})


@app.route("/play")
def play():
    return forward("/play", {"folder": request.args.get("folder", "11"), "file": request.args.get("file", "1")})


@app.route("/vol")
def vol():
    return forward("/vol", {"value": request.args.get("value", "20")})


@app.route("/mute")
def mute():
    return forward("/mute", {"state": request.args.get("state", "0")})


@app.route("/hourvol")
def hourvol():
    return forward("/hourvol", {"hour": request.args.get("hour", "0"), "value": request.args.get("value", "20")})


@app.route("/bible1")
def bible1():
    return forward("/bible1", {"hour": request.args.get("hour", "6")})


@app.route("/bible2")
def bible2():
    return forward("/bible2", {"hour": request.args.get("hour", "12")})


@app.route("/bible3")
def bible3():
    return forward("/bible3", {"hour": request.args.get("hour", "21")})


@app.route("/song1")
def song1():
    return forward("/song1", {"hour": request.args.get("hour", "6")})


@app.route("/song2")
def song2():
    return forward("/song2", {"hour": request.args.get("hour", "21")})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)
