from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import csv
import os
from datetime import datetime

HOST = "0.0.0.0"
PORT = 8080
CSV_FILE = "pet_received.csv"

CSV_HEADER = [
    "pc_time",
    "boot_id",
    "time_ms",
    "epoch_ms",
    "time_valid",
    "time_str",
    "state",
    "candidate",
    "event",
    "acc",
    "gyro",
    "acc_std",
    "gyro_std",
    "pitch",
    "roll",
    "state_duration_ms",
    "rest_like_ms",
]


def ensure_csv_header():
    need_header = not os.path.exists(CSV_FILE) or os.path.getsize(CSV_FILE) == 0
    if need_header:
        with open(CSV_FILE, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(CSV_HEADER)


def append_csv(data):
    ensure_csv_header()

    row = [
        datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        data.get("boot_id", ""),
        data.get("time_ms", ""),
        data.get("epoch_ms", ""),
        data.get("time_valid", ""),
        data.get("time_str", ""),
        data.get("state", ""),
        data.get("candidate", ""),
        data.get("event", ""),
        data.get("acc", ""),
        data.get("gyro", ""),
        data.get("acc_std", ""),
        data.get("gyro_std", ""),
        data.get("pitch", ""),
        data.get("roll", ""),
        data.get("state_duration_ms", ""),
        data.get("rest_like_ms", ""),
    ]

    with open(CSV_FILE, "a", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(row)


class PetHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            body = (
                "Pet server is running.\n"
                "POST JSON to /pet\n"
                f"Saving to {CSV_FILE}\n"
            ).encode("utf-8")

            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self.send_response(404)
        self.end_headers()

    def do_POST(self):
        if self.path != "/pet":
            self.send_response(404)
            self.end_headers()
            return

        content_len = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(content_len)

        try:
            data = json.loads(body.decode("utf-8"))
            append_csv(data)

            print(
                f"[{datetime.now().strftime('%H:%M:%S')}] "
                f"boot={data.get('boot_id')} "
                f"valid={data.get('time_valid')} "
                f"time={data.get('time_str')} "
                f"state={data.get('state')} "
                f"candidate={data.get('candidate')} "
                f"event={data.get('event')} "
                f"acc={data.get('acc')} "
                f"gyro={data.get('gyro')}"
            )

            resp = b"OK"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)

        except Exception as e:
            print("POST parse error:", e)
            self.send_response(400)
            self.end_headers()

    def log_message(self, format, *args):
        return


if __name__ == "__main__":
    ensure_csv_header()
    server = HTTPServer((HOST, PORT), PetHandler)

    print(f"Pet server listening on http://{HOST}:{PORT}")
    print("ESP32 should POST to: http://http://192.168.1.12:8080/pet")
    print(f"Saving data to: {CSV_FILE}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")