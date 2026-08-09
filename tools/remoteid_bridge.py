#!/usr/bin/env python3
"""
remoteid_bridge.py -- reads Remote ID payloads off an ESP32 receiver's USB
serial output and feeds them into the Data Flight server's
POST /api/remoteid/decode endpoint for live tracking.

Usage:
    python remoteid_bridge.py --port COM5 --api-key YOUR_KEY

What this expects from the serial output: one ODID message-pack payload per
line, as a hex string (e.g. "F21904021231535A..."). Most ESP32 OpenDroneID
receiver firmware prints the raw payload bytes in this form specifically so
it can be re-decoded elsewhere. If your firmware instead prints JSON, CSV,
or something else, paste one real line from your serial monitor once the
board is flashed and this parser will get fixed to match it -- there's no
way to know the exact format in advance of picking a specific firmware
project.
"""
import argparse
import re
import sys
import time

try:
    import requests
except ImportError:
    sys.exit("Missing dependency: pip install requests pyserial")
try:
    import serial  # pyserial
except ImportError:
    sys.exit("Missing dependency: pip install requests pyserial")

HEX_RE = re.compile(r'[0-9a-fA-F]{50,}')  # at least one 25-byte ODID message = 50 hex chars


def extract_hex_payload(line: str):
    """Pull a hex ODID payload out of one line of serial output -- handles
    a bare hex string or one embedded in a JSON/log line."""
    m = HEX_RE.search(line)
    return m.group(0) if m else None


def main():
    ap = argparse.ArgumentParser(description="Bridge an ESP32 Remote ID receiver into Data Flight")
    ap.add_argument("--port", required=True, help="Serial port the ESP32 is on, e.g. COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--server", default="http://localhost:5050")
    ap.add_argument("--api-key", required=True, help="Same value as DETECTIONS_API_KEY")
    ap.add_argument("--sensor-id", default="esp32-remoteid-1")
    args = ap.parse_args()

    url = args.server.rstrip("/") + "/api/remoteid/decode"
    headers = {"X-API-Key": args.api_key, "Content-Type": "application/json"}

    print(f"Opening {args.port} @ {args.baud} baud...")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        sys.exit(f"Could not open {args.port}: {e}")

    print("Listening for Remote ID payloads. Ctrl+C to stop.")
    with ser:
        while True:
            try:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode(errors="ignore").strip()
                if not line:
                    continue

                payload_hex = extract_hex_payload(line)
                if not payload_hex:
                    continue  # not a payload line (firmware boot/debug noise, etc.)

                resp = requests.post(
                    url, headers=headers,
                    json={"payload_hex": payload_hex, "sensor_id": args.sensor_id},
                    timeout=5,
                )
                if resp.status_code == 201:
                    data = resp.json()
                    track = data.get("track")
                    if track:
                        print(f"track {track['track_id']}: {track['classification']} "
                              f"({track['confidence']:.2f}) at {track['lat']:.5f},{track['lon']:.5f}")
                    else:
                        types = [m.get("type") for m in data.get("messages", [])]
                        print(f"decoded, no position yet: {types}")
                else:
                    print(f"server rejected payload ({resp.status_code}): {resp.text[:200]}")
            except KeyboardInterrupt:
                print("\nStopping.")
                break
            except requests.RequestException as e:
                print(f"server unreachable, retrying in 2s: {e}")
                time.sleep(2)


if __name__ == "__main__":
    main()
