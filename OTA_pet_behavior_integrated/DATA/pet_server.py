from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import csv
import os
from datetime import datetime

HOST = "0.0.0.0"
PORT = 8080

JSON_CSV_FILE = "pet_received.csv"
UPLOAD_ROOT = "received_files"

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


def ensure_json_csv_header():
    need_header = not os.path.exists(JSON_CSV_FILE) or os.path.getsize(JSON_CSV_FILE) == 0
    if need_header:
        with open(JSON_CSV_FILE, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(CSV_HEADER)


def append_json_csv(data):
    ensure_json_csv_header()

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

    with open(JSON_CSV_FILE, "a", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(row)


def safe_name(name: str) -> str:
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
    cleaned = "".join(ch for ch in name if ch in allowed)
    return cleaned or "UNKNOWN"


def safe_session_path(session: str) -> str:
    parts = []
    for part in session.replace("\\", "/").split("/"):
        if not part or part == "." or part == "..":
            continue
        parts.append(safe_name(part))
    return os.path.join(*parts) if parts else "unknown_session"


class PetHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            body = (
                "Pet server is running.\n"
                "POST JSON to /pet\n"
                "POST CSV file to /upload\n"
                f"JSON data saved to {JSON_CSV_FILE}\n"
                f"CSV files saved under {UPLOAD_ROOT}/\n"
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
        if self.path == "/pet":
            self.handle_pet_json()
            return

        if self.path == "/upload":
            self.handle_file_upload()
            return

        self.send_response(404)
        self.end_headers()

    def handle_pet_json(self):
        content_len = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(content_len)

        try:
            data = json.loads(body.decode("utf-8"))
            append_json_csv(data)

            print(
                f"[{datetime.now().strftime('%H:%M:%S')}] JSON "
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
            print("POST /pet parse error:", e)
            self.send_response(400)
            self.end_headers()

    def handle_file_upload(self):
        content_len = int(self.headers.get("Content-Length", "0"))

        file_name = safe_name(self.headers.get("X-File-Name", "UPLOAD.CSV"))
        session = safe_session_path(self.headers.get("X-Session", "unknown_session"))

        save_dir = os.path.join(UPLOAD_ROOT, session)
        os.makedirs(save_dir, exist_ok=True)

        save_path = os.path.join(save_dir, file_name)

        try:
            body = self.rfile.read(content_len)

            with open(save_path, "wb") as f:
                f.write(body)

            print(
                f"[{datetime.now().strftime('%H:%M:%S')}] FILE "
                f"session={session} "
                f"name={file_name} "
                f"size={len(body)} "
                f"saved={save_path}"
            )

            resp = b"OK"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)

        except Exception as e:
            print("POST /upload error:", e)
            self.send_response(500)
            self.end_headers()

    def log_message(self, format, *args):
        return


if __name__ == "__main__":
    ensure_json_csv_header()
    os.makedirs(UPLOAD_ROOT, exist_ok=True)

    server = HTTPServer((HOST, PORT), PetHandler)

    print(f"Pet server listening on http://{HOST}:{PORT}")
    print("ESP32 JSON URL:  http://192.168.1.12/pet")
    print("ESP32 FILE URL:  http://192.168.1.12/upload")
    print(f"JSON data saved to: {JSON_CSV_FILE}")
    print(f"CSV files saved under: {UPLOAD_ROOT}/")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer stopped.")