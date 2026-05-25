#!/usr/bin/env python3
"""
IPET local web console with manual recording sessions.

Run:
  python pet_server_web.py

Open:
  http://127.0.0.1:8080/
  or http://<computer-lan-ip>:8080/

ESP32 CONFIG.TXT:
  pet_http_url=http://<computer-lan-ip>:8080/pet
  pet_file_upload_url=http://<computer-lan-ip>:8080/upload
  OTA_COMMAND_URL=http://<computer-lan-ip>:8080/ota_cmd
  RECORD_COMMAND_URL=http://<computer-lan-ip>:8080/record_cmd
  SD_COMMAND_URL=http://<computer-lan-ip>:8080/sd_cmd
  ENABLE_REMOTE_OTA=1
  ENABLE_REMOTE_RECORD=1
  ENABLE_REMOTE_SD=1
"""

from __future__ import annotations

import csv
import json
import os
import posixpath
import shutil
import socket
import threading
import time
import uuid
import zipfile
from dataclasses import dataclass, asdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional
from urllib.parse import parse_qs, urlparse, unquote

HOST = "0.0.0.0"
PORT = 8080

DATA_CSV = Path("pet_received.csv")
UPLOAD_DIR = Path("received_files")
OTA_DIR = Path("ota_files")
RECORD_DIR = Path("recordings")

state_lock = threading.RLock()
last_pet: Optional[dict] = None
last_pet_received_at: Optional[float] = None
recent_events: list[dict] = []
MAX_EVENTS = 250

console_options = {"watch_pet": False, "http_log": False}


@dataclass
class OtaCommand:
    command_id: str
    firmware_path: Optional[Path] = None
    external_url: Optional[str] = None
    created_at: float = 0.0
    served_count: int = 0
    consumed: bool = False
    downloaded_count: int = 0
    last_download_at: Optional[float] = None
    last_download_ip: Optional[str] = None


@dataclass
class RecordingSession:
    session_id: str
    label: str
    note: str
    folder: Path
    started_at: float
    ended_at: Optional[float] = None
    sample_period_ms: int = 5
    raw_sample_log: bool = True
    pet_rows: int = 0
    uploaded_files: int = 0


ota_command: Optional[OtaCommand] = None
active_recording: Optional[RecordingSession] = None
record_command_payload: dict = {"record": 0, "id": "idle"}
# 录制结束后，设备通常还需要几十秒上传刚关闭的 R/S/E CSV。
# 这里保留一个“上传归属会话”，让 stop 后到来的 raw CSV 仍能复制进本次录制文件夹。
upload_capture_session: Optional[RecordingSession] = None
upload_capture_until: float = 0.0
UPLOAD_CAPTURE_GRACE_SEC = 10 * 60
# 结束录制后继续让 ESP32 立即扫描上传，避免 R000xxx.CSV 要等下一轮定时扫描。
UPLOAD_COMMAND_GRACE_SEC = 90


# SD 卡远程管理：网页发命令，ESP32 轮询 /sd_cmd 执行，结果 POST 到 /sd_result。
sd_command_payload: dict = {"sd": 0, "id": "idle"}
sd_last_result: Optional[dict] = None
sd_files_cache: list[dict] = []
sd_last_command_at: Optional[float] = None
sd_last_result_at: Optional[float] = None
sd_command_seq = 0


def add_event(kind: str, message: str, **extra) -> None:
    item = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "kind": kind, "message": message}
    item.update(extra)
    with state_lock:
        recent_events.append(item)
        del recent_events[:-MAX_EVENTS]


def local_ip_guess() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def safe_filename(name: str, default: str = "file.bin") -> str:
    name = unquote(name or default)
    name = os.path.basename(name)
    keep = []
    for ch in name:
        if ch.isalnum() or ch in (".", "_", "-", " ", "(", ")"):
            keep.append(ch)
    cleaned = "".join(keep).strip()
    return cleaned or default


def safe_slug(text: str, default: str = "record") -> str:
    text = (text or default).strip()
    keep = []
    for ch in text:
        if ch.isalnum() or ch in ("_", "-", " "):
            keep.append(ch)
    s = "".join(keep).strip().replace(" ", "_")
    return s[:60] or default


def write_dynamic_csv(path: Path, row: dict) -> None:
    """Append a dict row to CSV. If new fields appear, rewrite header safely."""
    path.parent.mkdir(parents=True, exist_ok=True)
    row = dict(row)
    if not path.exists() or path.stat().st_size == 0:
        fieldnames = list(row.keys())
        with path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
            writer.writeheader()
            writer.writerow(row)
        return

    with path.open("r", newline="", encoding="utf-8", errors="replace") as f:
        reader = csv.DictReader(f)
        fieldnames = list(reader.fieldnames or [])
        rows = list(reader)
    missing = [k for k in row.keys() if k not in fieldnames]
    if missing:
        fieldnames += missing
        with path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(rows)
            writer.writerow(row)
    else:
        with path.open("a", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
            writer.writerow(row)


def append_json_row(data: dict) -> None:
    row = dict(data)
    row.setdefault("received_at", time.strftime("%Y-%m-%d %H:%M:%S"))
    write_dynamic_csv(DATA_CSV, row)


def recording_to_dict(s: RecordingSession, active: bool = False) -> dict:
    duration_end = time.time() if s.ended_at is None else s.ended_at
    return {
        "id": s.session_id,
        "label": s.label,
        "note": s.note,
        "folder": str(s.folder),
        "started_at": s.started_at,
        "started_at_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(s.started_at)),
        "ended_at": s.ended_at,
        "ended_at_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(s.ended_at)) if s.ended_at else None,
        "is_active": bool(active and s.ended_at is None),
        "sample_period_ms": s.sample_period_ms,
        "raw_sample_log": s.raw_sample_log,
        "pet_rows": s.pet_rows,
        "uploaded_files": s.uploaded_files,
        "duration_sec": max(0, duration_end - s.started_at),
    }


def active_recording_to_dict() -> Optional[dict]:
    if not active_recording:
        return None
    return recording_to_dict(active_recording, active=True)


def recording_meta_path(session: RecordingSession) -> Path:
    return session.folder / "metadata.json"


def save_recording_meta(session: RecordingSession) -> None:
    # 旧版这里在 active_recording 存在时没有写 ended_at_text，
    # 导致网页“录制会话”一直显示录制中。这里统一走 recording_to_dict。
    meta = recording_to_dict(session, active=(active_recording is not None and active_recording.session_id == session.session_id))
    session.folder.mkdir(parents=True, exist_ok=True)
    recording_meta_path(session).write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")


def start_recording(label: str, note: str = "", sample_period_ms: int = 5, raw_sample_log: bool = True) -> RecordingSession:
    global active_recording, record_command_payload
    sample_period_ms = int(sample_period_ms or 5)
    if sample_period_ms < 5:
        sample_period_ms = 5
    if sample_period_ms > 20:
        sample_period_ms = 20
    raw_sample_log = bool(raw_sample_log)
    with state_lock:
        if active_recording is not None:
            raise RuntimeError("已有录制正在进行，请先结束当前录制")
        label_slug = safe_slug(label or "manual")
        sid = time.strftime("%Y%m%d_%H%M%S") + "_" + label_slug + "_" + uuid.uuid4().hex[:6]
        folder = RECORD_DIR / sid
        session = RecordingSession(session_id=sid, label=label or "manual", note=note or "", folder=folder, started_at=time.time(), sample_period_ms=sample_period_ms, raw_sample_log=raw_sample_log)
        (folder / "uploads").mkdir(parents=True, exist_ok=True)
        write_dynamic_csv(folder / "pet_record.csv", {
            "record_marker": "START",
            "received_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "label": session.label,
            "note": session.note,
            "sample_period_ms": session.sample_period_ms,
            "raw_sample_log": 1 if session.raw_sample_log else 0,
        })
        record_command_payload = {
            "record": 1,
            "id": session.session_id,
            "label": session.label,
            "sample_period_ms": session.sample_period_ms,
            "raw": 1 if session.raw_sample_log else 0,
        }
        active_recording = session
        save_recording_meta(session)
    add_event("record", f"开始录制：{session.label}", id=sid)
    return session


def stop_recording() -> RecordingSession:
    global active_recording, record_command_payload, upload_capture_session, upload_capture_until
    with state_lock:
        if active_recording is None:
            raise RuntimeError("当前没有录制")
        session = active_recording
        session.ended_at = time.time()
        write_dynamic_csv(session.folder / "pet_record.csv", {
            "record_marker": "END",
            "received_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "label": session.label,
            "duration_sec": round(session.ended_at - session.started_at, 3),
            "pet_rows": session.pet_rows,
            "uploaded_files": session.uploaded_files,
        })
        # 先关闭 active，再保存 meta，保证 ended_at_text/is_active 正确。
        active_recording = None
        upload_capture_session = session
        upload_capture_until = time.time() + UPLOAD_CAPTURE_GRACE_SEC
        record_command_payload = {
            "record": 0,
            "id": session.session_id + "_stop",
            "upload_now": 1,
            "upload_grace_sec": UPLOAD_COMMAND_GRACE_SEC,
        }
        save_recording_meta(session)
    add_event("record", f"结束录制：{session.label}，等待设备上传 raw CSV", id=session.session_id, folder=str(session.folder))
    return session


def record_pet_row(data: dict) -> None:
    with state_lock:
        session = active_recording
        if not session:
            return
        row = dict(data)
        row.setdefault("received_at", time.strftime("%Y-%m-%d %H:%M:%S"))
        row["record_session"] = session.session_id
        row["record_label"] = session.label
        write_dynamic_csv(session.folder / "pet_record.csv", row)
        session.pet_rows += 1
        save_recording_meta(session)


def record_uploaded_file(source: Path, original_name: str) -> None:
    global upload_capture_session, upload_capture_until
    with state_lock:
        session = active_recording
        if session is None and upload_capture_session is not None:
            if time.time() <= upload_capture_until:
                session = upload_capture_session
            else:
                upload_capture_session = None
                upload_capture_until = 0.0
        if not session:
            return
        target_dir = session.folder / "uploads"
        target_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%H%M%S")
        target = target_dir / f"{stamp}_{safe_filename(original_name, source.name)}"
        try:
            shutil.copy2(source, target)
            session.uploaded_files += 1
            write_dynamic_csv(session.folder / "upload_index.csv", {
                "received_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                "original_name": original_name,
                "saved_as": str(target),
                "size": target.stat().st_size,
            })
            save_recording_meta(session)
            add_event("record", f"文件已加入录制会话：{original_name}", id=session.session_id)
        except Exception as e:
            add_event("error", f"录制上传文件复制失败：{e}")


def list_recordings(limit: int = 50) -> list[dict]:
    if not RECORD_DIR.exists():
        return []
    out = []
    for folder in RECORD_DIR.iterdir():
        if not folder.is_dir():
            continue
        meta_file = folder / "metadata.json"
        meta = {}
        if meta_file.exists():
            try:
                meta = json.loads(meta_file.read_text(encoding="utf-8"))
            except Exception:
                meta = {}
        stat = folder.stat()
        out.append({
            "id": folder.name,
            "folder": str(folder),
            "label": meta.get("label", folder.name),
            "note": meta.get("note", ""),
            "started_at_text": meta.get("started_at_text", time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(stat.st_mtime))),
            "ended_at_text": meta.get("ended_at_text"),
            "pet_rows": meta.get("pet_rows", 0),
            "uploaded_files": meta.get("uploaded_files", 0),
            "mtime": stat.st_mtime,
        })
    out.sort(key=lambda x: x["mtime"], reverse=True)
    return out[:limit]


def resolve_recording_folder(session_id: str) -> Path:
    session_id = safe_slug(session_id, "")
    if not session_id:
        raise ValueError("missing session id")
    path = (RECORD_DIR / session_id).resolve()
    root = RECORD_DIR.resolve()
    if path != root and root not in path.parents:
        raise ValueError("invalid recording id")
    if not path.exists() or not path.is_dir():
        raise FileNotFoundError(session_id)
    return path


def _recording_time_window(folder: Path) -> tuple[float, float]:
    """Return a broad time window for files that may belong to this recording.

    Some ESP32 CSV files are uploaded only after recording has stopped.
    If the server was restarted or upload_capture_session was missed, we still
    try to include matching files from received_files/ by mtime when creating
    the recording zip.
    """
    meta_file = folder / "metadata.json"
    started = folder.stat().st_mtime
    ended = time.time()
    if meta_file.exists():
        try:
            meta = json.loads(meta_file.read_text(encoding="utf-8"))
            started = float(meta.get("started_at") or started)
            ended = float(meta.get("ended_at") or ended)
        except Exception:
            pass
    # Allow early/late upload slack. Raw CSV can arrive minutes after END.
    return started - 60.0, ended + UPLOAD_CAPTURE_GRACE_SEC


def _auto_attach_uploaded_files(z: zipfile.ZipFile, folder: Path, written: set[str]) -> int:
    if not UPLOAD_DIR.exists():
        return 0
    t0, t1 = _recording_time_window(folder)
    count = 0
    for p in UPLOAD_DIR.rglob("*"):
        if not p.is_file():
            continue
        if p.suffix.lower() != ".csv":
            continue
        try:
            st = p.stat()
        except OSError:
            continue
        if st.st_mtime < t0 or st.st_mtime > t1:
            continue
        arc = folder.name + "/auto_attached_uploads/" + str(p.relative_to(UPLOAD_DIR)).replace("\\", "/")
        if arc in written:
            continue
        z.write(p, arc)
        written.add(arc)
        count += 1
    return count


def zip_recording(session_id: str) -> Path:
    folder = resolve_recording_folder(session_id)
    zip_path = folder.with_suffix(".zip")
    written: set[str] = set()
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for p in folder.rglob("*"):
            if p.is_file():
                arc = str(p.relative_to(folder.parent)).replace("\\", "/")
                z.write(p, arc)
                written.add(arc)
        attached = _auto_attach_uploaded_files(z, folder, written)
        if attached:
            add_event("record", f"下载录制 zip 时自动补入 {attached} 个 received_files CSV", id=folder.name)
    return zip_path


def queue_ota_file(path: Path) -> OtaCommand:
    global ota_command
    path = path.expanduser().resolve()
    if not path.exists() or not path.is_file():
        raise FileNotFoundError(str(path))
    cmd_id = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
    cmd = OtaCommand(command_id=cmd_id, firmware_path=path, created_at=time.time())
    with state_lock:
        ota_command = cmd
    add_event("ota", f"OTA 已排队：{path.name}", id=cmd_id, path=str(path))
    return cmd


def queue_ota_url(url: str) -> OtaCommand:
    global ota_command
    url = url.strip()
    if not (url.startswith("http://") or url.startswith("https://")):
        raise ValueError("URL must start with http:// or https://")
    cmd_id = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
    cmd = OtaCommand(command_id=cmd_id, external_url=url, created_at=time.time())
    with state_lock:
        ota_command = cmd
    add_event("ota", "OTA URL 已排队", id=cmd_id, url=url)
    return cmd


def ota_to_dict(cmd: Optional[OtaCommand]) -> Optional[dict]:
    if not cmd:
        return None
    return {
        "id": cmd.command_id,
        "firmware_path": str(cmd.firmware_path) if cmd.firmware_path else None,
        "firmware_name": cmd.firmware_path.name if cmd.firmware_path else None,
        "external_url": cmd.external_url,
        "created_at_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(cmd.created_at)) if cmd.created_at else "",
        "served_count": cmd.served_count,
        "consumed": cmd.consumed,
        "downloaded_count": cmd.downloaded_count,
        "last_download_ip": cmd.last_download_ip,
        "last_download_at_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(cmd.last_download_at)) if cmd.last_download_at else "",
    }


def latest_uploaded_files(limit: int = 12) -> list[dict]:
    if not UPLOAD_DIR.exists():
        return []
    files = []
    for p in UPLOAD_DIR.rglob("*"):
        if p.is_file():
            try:
                stat = p.stat()
            except OSError:
                continue
            files.append({
                "path": str(p),
                "name": p.name,
                "size": stat.st_size,
                "mtime": stat.st_mtime,
                "mtime_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(stat.st_mtime)),
            })
    files.sort(key=lambda x: x["mtime"], reverse=True)
    return files[:limit]



def file_kind(path: Path) -> str:
    name = path.name.upper()
    if name == DATA_CSV.name.upper():
        return "实时状态"
    if name == "PET_RECORD.CSV":
        return "录制状态"
    if name == "UPLOAD_INDEX.CSV":
        return "上传索引"
    if len(name) == 11 and name.endswith(".CSV"):
        if name.startswith("R"):
            return "高频RAW"
        if name.startswith("S"):
            return "状态CSV"
        if name.startswith("E"):
            return "事件CSV"
    return "CSV"


def file_token(path: Path) -> str:
    return str(path.resolve())


def resolve_data_path(path_value: str) -> Path:
    if not path_value:
        raise ValueError("missing path")
    p = Path(unquote(path_value)).expanduser().resolve()
    roots = [Path.cwd().resolve(), UPLOAD_DIR.resolve(), RECORD_DIR.resolve(), OTA_DIR.resolve()]
    allowed = False
    for root in roots:
        try:
            if p == root or root in p.parents:
                allowed = True
                break
        except Exception:
            pass
    if not allowed:
        raise ValueError("path outside server data folders")
    if not p.exists() or not p.is_file():
        raise FileNotFoundError(str(p))
    return p


def list_csv_files(limit: int = 500) -> list[dict]:
    paths: list[Path] = []
    if DATA_CSV.exists():
        paths.append(DATA_CSV)
    for root in (RECORD_DIR, UPLOAD_DIR):
        if root.exists():
            paths.extend([p for p in root.rglob("*.csv") if p.is_file()])
            paths.extend([p for p in root.rglob("*.CSV") if p.is_file()])
    seen = set()
    rows = []
    for p in paths:
        try:
            rp = p.resolve()
            if str(rp) in seen:
                continue
            seen.add(str(rp))
            st = p.stat()
            rows.append({
                "name": p.name,
                "kind": file_kind(p),
                "path": str(p),
                "token": file_token(p),
                "size": st.st_size,
                "size_kb": round(st.st_size / 1024.0, 1),
                "mtime": st.st_mtime,
                "mtime_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(st.st_mtime)),
            })
        except OSError:
            continue
    rows.sort(key=lambda x: x["mtime"], reverse=True)
    return rows[:limit]


def read_csv_preview(path: Path, recent: int = 1000) -> dict:
    from collections import deque
    recent = max(10, min(int(recent or 1000), 20000))
    with path.open("r", newline="", encoding="utf-8", errors="replace") as f:
        reader = csv.DictReader(f)
        columns = list(reader.fieldnames or [])
        dq = deque(maxlen=recent)
        total = 0
        for row in reader:
            total += 1
            dq.append(row)
    rows = list(dq)
    numeric_columns = []
    stats = {}
    for col in columns:
        vals = []
        for r in rows:
            v = r.get(col, "")
            if v is None or v == "":
                continue
            try:
                vals.append(float(v))
            except Exception:
                pass
        if vals and len(vals) >= max(3, int(len(rows) * 0.25)):
            numeric_columns.append(col)
            stats[col] = {
                "count": len(vals),
                "min": min(vals),
                "max": max(vals),
                "avg": sum(vals) / len(vals),
            }
    return {
        "ok": True,
        "file": {"name": path.name, "path": str(path), "kind": file_kind(path), "size": path.stat().st_size},
        "columns": columns,
        "numeric_columns": numeric_columns,
        "rows": rows,
        "total_rows": total,
        "returned_rows": len(rows),
        "stats": stats,
    }


def _new_sd_command_id(prefix: str) -> str:
    global sd_command_seq
    sd_command_seq += 1
    return time.strftime("%Y%m%d-%H%M%S") + f"-{prefix}-" + uuid.uuid4().hex[:6]


def queue_sd_command(cmd: str, path: str = "/sdcard", files: Optional[list[str]] = None) -> dict:
    global sd_command_payload, sd_last_command_at
    files = files or []
    command_id = _new_sd_command_id(cmd)
    payload = {
        "sd": 1,
        "id": command_id,
        "cmd": cmd,
        "path": path or "/sdcard",
        "files": files[:12],
    }
    with state_lock:
        sd_command_payload = payload
        sd_last_command_at = time.time()
    add_event("sd", f"SD 命令已排队：{cmd}", id=command_id, path=path, files=len(files))
    return payload


def sd_status_payload() -> dict:
    with state_lock:
        cmd = dict(sd_command_payload)
        result = dict(sd_last_result) if isinstance(sd_last_result, dict) else None
        files = list(sd_files_cache)
        cmd_at = sd_last_command_at
        result_at = sd_last_result_at
    return {
        "ok": True,
        "command": cmd,
        "last_result": result,
        "files": files,
        "file_count": len(files),
        "last_command_at_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(cmd_at)) if cmd_at else None,
        "last_result_at_text": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(result_at)) if result_at else None,
    }


def status_payload() -> dict:
    now = time.time()
    with state_lock:
        pet = dict(last_pet) if last_pet else None
        age = None if last_pet_received_at is None else max(0.0, now - last_pet_received_at)
        cmd = ota_to_dict(ota_command)
        rec = active_recording_to_dict()
        events = list(recent_events[-80:])

    if age is None:
        connection_state = "no_data"
        connection_text = "等待设备数据"
    elif age < 3:
        connection_state = "online"
        connection_text = "设备在线"
    elif age < 10:
        connection_state = "delayed"
        connection_text = "设备延迟"
    elif age < 20:
        connection_state = "unstable"
        connection_text = "连接不稳定"
    else:
        connection_state = "offline"
        connection_text = "设备离线"

    return {
        "ok": True,
        "server_time": time.strftime("%Y-%m-%d %H:%M:%S"),
        "server_ip": local_ip_guess(),
        "port": PORT,
        "last_pet": pet,
        "last_pet_age_sec": age,
        "device_online": bool(age is not None and age < 20),
        "device_connection_state": connection_state,
        "device_connection_text": connection_text,
        "ota_command": cmd,
        "active_recording": rec,
        "record_command": dict(record_command_payload),
        "recordings": list_recordings(30),
        "events": events,
        "received_csv": str(DATA_CSV.resolve()),
        "upload_dir": str(UPLOAD_DIR.resolve()),
        "record_dir": str(RECORD_DIR.resolve()),
        "recent_uploads": latest_uploaded_files(12),
        "sd": sd_status_payload(),
    }


INDEX_HTML = r"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>IPET 控制台</title>
  <style>
    :root{--bg:#f5f7fb;--card:#fff;--text:#172033;--muted:#667085;--line:#e5e9f2;--ok:#0f9f6e;--bad:#dc2626;--blue:#2563eb;--purple:#7c3aed;--shadow:0 14px 34px rgba(21,31,53,.09);--radius:18px}*{box-sizing:border-box}body{margin:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"PingFang SC","Microsoft YaHei",Arial,sans-serif;color:var(--text);background:radial-gradient(circle at top left,#eaf1ff 0,transparent 34%),var(--bg)}header{padding:22px 28px 10px;display:flex;justify-content:space-between;gap:18px;align-items:flex-start}h1{margin:0 0 6px;font-size:28px}.subtitle{color:var(--muted);font-size:14px}.pill{display:inline-flex;align-items:center;gap:8px;border:1px solid var(--line);background:rgba(255,255,255,.8);padding:9px 13px;border-radius:999px;color:var(--muted);font-size:13px;white-space:nowrap}.dot{width:10px;height:10px;border-radius:999px;background:#adb5c7;box-shadow:0 0 0 4px rgba(173,181,199,.18)}.dot.online{background:var(--ok);box-shadow:0 0 0 4px rgba(15,159,110,.15)}.dot.delayed{background:#f59e0b;box-shadow:0 0 0 4px rgba(245,158,11,.15)}.dot.unstable{background:#fb923c;box-shadow:0 0 0 4px rgba(251,146,60,.15)}.dot.offline,.dot.no_data{background:var(--bad);box-shadow:0 0 0 4px rgba(220,38,38,.15)}main{padding:0 28px 28px}.tabs{display:flex;gap:10px;flex-wrap:wrap;margin:8px 0 18px}.tab{background:#e8edf7;color:#263247}.tab.active{background:var(--blue);color:#fff}.page{display:none}.page.active{display:block}.grid{display:grid;grid-template-columns:1.05fr .95fr;gap:18px}.grid3{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}.card{background:rgba(255,255,255,.94);border:1px solid rgba(228,233,242,.95);border-radius:var(--radius);box-shadow:var(--shadow);padding:18px;backdrop-filter:blur(10px)}.card h2{margin:0 0 14px;font-size:17px}.state-main{display:flex;align-items:center;gap:20px}.state-badge{width:136px;height:136px;border-radius:34px;display:grid;place-items:center;color:#fff;font-weight:900;font-size:26px;background:linear-gradient(135deg,#94a3b8,#475569);box-shadow:inset 0 -22px 50px rgba(0,0,0,.18),0 16px 28px rgba(51,65,85,.2)}.state-badge.rest{background:linear-gradient(135deg,#22c55e,#087f5b)}.state-badge.walk{background:linear-gradient(135deg,#3b82f6,#1e40af)}.state-badge.run{background:linear-gradient(135deg,#f97316,#9a3412)}.state-badge.sleep{background:linear-gradient(135deg,#8b5cf6,#4c1d95)}.state-badge.notworn{background:linear-gradient(135deg,#94a3b8,#475569)}.big-number{font-size:32px;font-weight:900;margin:0}.muted{color:var(--muted)}.metric{border:1px solid var(--line);border-radius:14px;padding:12px;background:#fbfcff;min-width:0}.metric .label{color:var(--muted);font-size:12px;margin-bottom:5px}.metric .value{font-size:16px;font-weight:800;overflow-wrap:anywhere}.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}input,select,textarea{width:100%;border:1px solid var(--line);background:#fff;color:var(--text);border-radius:12px;padding:10px 11px;font-size:14px;outline:none}textarea{min-height:70px;resize:vertical}button{border:0;border-radius:12px;padding:10px 13px;font-size:14px;font-weight:800;cursor:pointer;background:var(--blue);color:#fff}button.secondary{background:#e8edf7;color:#263247}button.danger{background:#dc2626}button.purple{background:var(--purple)}.events{max-height:420px;overflow:auto;display:flex;flex-direction:column;gap:8px;padding-right:4px}.event{border:1px solid var(--line);background:#fbfcff;border-radius:12px;padding:10px 11px;font-size:13px}.event .time{color:var(--muted);font-size:11px;margin-bottom:4px}.small{font-size:12px}.help{font-size:12px;color:var(--muted);margin-top:8px;line-height:1.55}.toast{position:fixed;right:22px;bottom:22px;background:#111827;color:#fff;padding:12px 14px;border-radius:14px;box-shadow:var(--shadow);max-width:520px;display:none;z-index:10}.tablewrap{overflow:auto;max-height:420px;border:1px solid var(--line);border-radius:14px}table{border-collapse:collapse;width:100%;font-size:12px;background:white}th,td{border-bottom:1px solid var(--line);padding:7px 9px;text-align:left;white-space:nowrap}th{position:sticky;top:0;background:#f8fafc;z-index:1}.checks{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:8px;max-height:210px;overflow:auto;border:1px solid var(--line);border-radius:14px;padding:10px;background:#fbfcff}.checks label{font-size:12px;display:flex;gap:7px;align-items:center}.checks input{width:auto}.fileitem{cursor:pointer}.fileitem:hover{border-color:#9db7ff}canvas{width:100%;height:330px;border:1px solid var(--line);border-radius:14px;background:#fff}.swatch{width:22px;height:22px;border-radius:999px;border:1px solid rgba(0,0,0,.18);display:inline-block;vertical-align:middle;margin-right:8px;box-shadow:inset 0 -5px 12px rgba(0,0,0,.12)}@media(max-width:1000px){.grid,.grid3{grid-template-columns:1fr}header{flex-direction:column}.state-main{flex-direction:column;align-items:flex-start}}
  </style>
</head>
<body>
<header><div><h1>IPET 控制台</h1><div class="subtitle">状态监控、CSV 可视化、手动录制、远程 OTA、LED 状态</div></div><div class="row"><div class="pill"><span id="onlineDot" class="dot"></span><span id="onlineText">连接状态检查中</span></div><div class="pill">服务器：<span id="serverAddr">-</span></div></div></header>
<main>
  <div class="tabs"><button class="tab active" onclick="showPage('overview')">总览</button><button class="tab" onclick="showPage('data')">CSV 数据</button><button class="tab" onclick="showPage('record')">录制</button><button class="tab" onclick="showPage('sd')">SD卡</button><button class="tab" onclick="showPage('ota')">OTA</button></div>

  <section id="page-overview" class="page active">
    <div class="grid">
      <section class="card"><h2>当前稳定状态</h2><div class="state-main"><div id="stateBadge" class="state-badge">--</div><div><p class="big-number" id="stateText">等待设备数据</p><div class="muted" id="stateMeta">ESP32 POST /pet 后更新</div></div></div><div class="grid3" style="margin-top:18px"><div class="metric"><div class="label">状态持续时间</div><div class="value" id="durationText">-</div></div><div class="metric"><div class="label">最后上报</div><div class="value" id="lastTimeText">-</div></div><div class="metric"><div class="label">Boot ID</div><div class="value" id="bootText">-</div></div></div><div class="grid3" style="margin-top:12px"><div class="metric"><div class="label">第一颗 LED：行为状态</div><div class="value"><span id="stateLedSwatch" class="swatch"></span><span id="stateLedText">-</span></div><div class="help" id="stateLedMeta">显示 REST / WALK / RUN / NOT_WORN 等状态</div></div><div class="metric"><div class="label">第二颗 LED：系统状态</div><div class="value"><span id="systemLedSwatch" class="swatch"></span><span id="systemLedText">-</span></div><div class="help" id="systemLedMeta">在线白色呼吸；离线红色慢闪；录制蓝色快闪；OTA 紫色快闪；电源键红色渐灭</div></div><div class="metric"><div class="label">系统标志</div><div class="value" id="systemFlagsText">-</div><div class="help" id="systemFlagsMeta">network / recording / ota / power-key</div></div><div class="metric"><div class="label">SD 卡状态</div><div class="value" id="sdCardText">等待设备上报</div><div class="help" id="sdCardMeta">未挂载时不会生成或上传 R/S/E CSV 文件</div></div></div></section>
      <section class="card"><h2>事件日志</h2><div id="events" class="events"></div></section>
    </div>
  </section>

  <section id="page-data" class="page">
    <div class="grid">
      <section class="card"><h2>CSV 文件</h2><div class="row" style="margin-bottom:10px"><button class="secondary" onclick="loadFiles()">刷新列表</button><select id="kindFilter" onchange="renderFiles()"><option value="">全部类型</option><option>高频RAW</option><option>录制状态</option><option>状态CSV</option><option>事件CSV</option><option>实时状态</option></select></div><div id="fileList" class="events"></div></section>
      <section class="card"><h2>折线图</h2><div class="grid3"><div><div class="help" style="margin:0 0 6px">X 轴</div><select id="xCol" onchange="drawChart()"></select></div><div><div class="help" style="margin:0 0 6px">读取最近 N 行</div><select id="recentRows" onchange="reloadCsv()"><option value="500">500</option><option value="1000" selected>1000</option><option value="3000">3000</option><option value="10000">10000</option><option value="20000">20000</option></select></div><div><div class="help" style="margin:0 0 6px">操作</div><button class="secondary" onclick="downloadCurrentFile()">下载当前 CSV</button></div></div><div class="help">Y 轴可多选：建议 raw 文件选 gx_dps/gy_dps/gz_dps、ax_g/ay_g/az_g、acc/gyro；状态文件选 activity_score/run_score/vote_run。</div><div id="yChecks" class="checks" style="margin:10px 0"></div><canvas id="chart" width="1100" height="360"></canvas></section>
    </div>
    <section class="card" style="margin-top:18px"><h2>表格预览</h2><div id="csvInfo" class="help">请选择一个 CSV 文件。</div><div id="stats" class="help"></div><div id="tableWrap" class="tablewrap" style="margin-top:10px"></div></section>
  </section>

  <section id="page-record" class="page">
    <div class="grid">
      <section class="card"><h2>手动录制数据</h2><div class="metric" style="margin-bottom:12px"><div class="label">录制状态</div><div class="value" id="recordStatus">未录制</div><div class="help" id="recordDetail">点击开始后，后续 /pet 数据会写入独立文件夹。</div></div><div class="grid3"><div><div class="help" style="margin:0 0 6px">场景标签</div><select id="recordLabel"><option value="walk">走路 walk</option><option value="run">跑步 run</option><option value="rest">休息 rest</option><option value="sleep">睡觉 sleep</option><option value="not_worn">未佩戴 not_worn</option><option value="play">玩耍 play</option><option value="custom">自定义 custom</option></select></div><div><div class="help" style="margin:0 0 6px">自定义名称</div><input id="recordCustom" placeholder="例如 dog_walk_outdoor_01" /></div><div><div class="help" style="margin:0 0 6px">操作</div><div class="row"><button onclick="startRecord()">开始录制</button><button class="danger" onclick="stopRecord()">结束录制</button></div></div></div><div class="grid3" style="margin-top:10px"><div><div class="help" style="margin:0 0 6px">采样间隔</div><select id="recordSamplePeriod"><option value="5" selected>最高：5ms / 200Hz</option><option value="10">稳定：10ms / 100Hz</option><option value="20">普通：20ms / 50Hz</option></select></div><label class="metric" style="display:flex;align-items:center;gap:10px"><input id="recordRaw" type="checkbox" checked style="width:auto" />记录每个 sample 的 raw acc/gyro</label><div class="metric"><div class="label">重要</div><div class="small muted">结束录制后，设备会关闭 Rxxxxxx.CSV 并上传；请等 10~60 秒后再下载 zip。</div></div></div><div style="margin-top:10px"><textarea id="recordNote" placeholder="备注：例如 10m 范围往返走路、楼下小跑、取下放地上等"></textarea></div></section>
      <section class="card"><h2>录制会话</h2><div id="recordings" class="events"></div></section>
    </div>
  </section>

  <section id="page-sd" class="page">
    <div class="grid">
      <section class="card"><h2>SD 卡文件浏览器</h2>
        <div class="metric" style="margin-bottom:12px"><div class="label">设备端 SD 列表状态</div><div class="value" id="sdStatus">等待刷新</div><div class="help" id="sdDetail">点击刷新后，ESP32 只扫描当前目录，不再递归扫描整张卡，避免 SDMMC 超时。</div></div>
        <div class="row" style="margin-bottom:10px"><input id="sdPath" value="/sdcard" placeholder="/sdcard 或 /sdcard/D260523" style="flex:1;min-width:220px"/><button onclick="sdRequestList()">刷新当前目录</button><button class="secondary" onclick="sdRefreshOnly()">只刷新网页状态</button></div>
        <div class="help">可以勾选文件后让设备上传到电脑，也可以直接删除 SD 卡上的文件。为安全起见，CONFIG.TXT 会被固件保护，不会删除；目录只允许删除空目录。</div>
        <div class="row" style="margin:10px 0"><button class="purple" onclick="sdUploadSelected()">上传选中文件到电脑</button><button class="danger" onclick="sdDeleteSelected()">删除选中文件</button><button class="secondary" onclick="sdSelectRaw()">只选 R*.CSV</button><button class="secondary" onclick="sdSelectNone()">清空选择</button></div>
        <div id="sdFileList" class="events" style="max-height:520px"></div>
      </section>
      <section class="card"><h2>SD 操作说明</h2>
        <div class="event"><b>刷新列表</b><div class="small muted">网页排队 list 命令，ESP32 轮询后扫描 SD 卡。文件多时最多返回约 240 个，避免 JSON 太大。</div></div>
        <div class="event"><b>上传文件</b><div class="small muted">设备会把选中的文件 POST 到 /upload，网页随后可在 “CSV 数据” 或 “最近上传文件” 里看到。</div></div>
        <div class="event"><b>删除文件</b><div class="small muted">删除是直接发生在 SD 卡上，建议先上传确认。不要删除正在录制的当前 R/S/E 文件。</div></div>
        <div class="event"><b>推荐流程</b><div class="small muted">录制结束 → SD卡页刷新 → 勾选 R000xxx.CSV → 上传选中文件到电脑 → CSV 数据页查看折线图。</div></div>
      </section>
    </div>
  </section>

  <section id="page-ota" class="page">
    <div class="grid"><section class="card"><h2>远程 OTA</h2><div class="metric" style="margin-bottom:12px"><div class="label">OTA 队列</div><div class="value" id="otaStatus">无待执行 OTA</div><div class="help" id="otaDetail">设备轮询 /ota_cmd 后会下载固件。</div></div><input id="binFile" type="file" accept=".bin,application/octet-stream" /><div class="row" style="margin-top:10px"><button onclick="queueUpload()">上传 .bin 并发送 OTA</button><button class="secondary" onclick="refreshStatus()">刷新</button><button class="danger" onclick="clearOta()">清除 OTA</button></div></section><section class="card"><h2>最近上传文件</h2><div id="uploads" class="events"></div></section></div>
  </section>
</main>
<div id="toast" class="toast"></div>
<script>
const STATE_LABEL_CN={REST:'休息',SLEEP:'睡觉',WALK:'走动',TROT:'快走',RUN:'跑步',PLAY:'玩耍',NOT_WORN:'未佩戴',UNKNOWN:'未知'};
const STATE_CLASS={REST:'rest',SLEEP:'sleep',WALK:'walk',TROT:'run',RUN:'run',PLAY:'run',NOT_WORN:'notworn',UNKNOWN:'unknown'};
let allFiles=[],currentFile=null,currentCsv=null;
function stateName(s){s=(s||'').toUpperCase();return STATE_LABEL_CN[s]||s||'未知'}
function fmtDuration(ms){if(ms===undefined||ms===null||ms==='')return'-';let sec=Math.max(0,Math.floor(Number(ms)/1000));let h=Math.floor(sec/3600),m=Math.floor((sec%3600)/60),s=sec%60;return h?`${h}小时 ${m}分 ${s}秒`:m?`${m}分 ${s}秒`:`${s}秒`}
function escapeHtml(s){return String(s??'').replace(/[&<>'"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c]))}
function toast(msg){const el=document.getElementById('toast');el.textContent=msg;el.style.display='block';clearTimeout(window.__toastTimer);window.__toastTimer=setTimeout(()=>el.style.display='none',3600)}
async function api(path,opt={}){const r=await fetch(path,opt);const t=await r.text();let d={};try{d=t?JSON.parse(t):{}}catch(e){d={ok:false,error:t}}if(!r.ok||d.ok===false)throw new Error(d.error||`HTTP ${r.status}`);return d}
function showPage(name){document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));document.querySelectorAll('.tab').forEach(p=>p.classList.remove('active'));document.getElementById('page-'+name).classList.add('active');[...document.querySelectorAll('.tab')].find(b=>b.textContent.includes({overview:'总览',data:'CSV',record:'录制',sd:'SD卡',ota:'OTA'}[name])).classList.add('active');if(name==='data')loadFiles()}

function safeColor(c){return /^#[0-9a-fA-F]{6}$/.test(String(c||''))?c:'#d1d5db'}
function renderStorageStatus(p){
  p=p||{};
  const has=Object.prototype.hasOwnProperty.call(p,'sd_mounted');
  const mounted=Number(p.sd_mounted||0)===1;
  const text=document.getElementById('sdCardText');
  const meta=document.getElementById('sdCardMeta');
  if(!has){ if(text)text.textContent='等待新版固件上报'; if(meta)meta.textContent='当前固件还没有 sd_mounted 字段，更新固件后可显示 SD 状态。'; return; }
  if(text) text.textContent=mounted?'已挂载 · SD OK':'未挂载 · SD 异常';
  if(meta) meta.textContent=mounted?`挂载点：${p.sd_mount_point||'/sdcard'}；可以记录/上传 R/S/E CSV`:'SD 卡未挂载：raw 文件不会写入，也不能从网页浏览/删除 SD 文件；请检查插卡、供电、卡座、4-bit 线和上拉。';
}
function renderLedStatus(p){
  p=p||{};
  const stateRgb=safeColor(p.state_led_rgb);
  const sysRgb=safeColor(p.system_led_rgb||p.battery_led_rgb);
  let a=document.getElementById('stateLedSwatch'); if(a)a.style.background=stateRgb;
  let b=document.getElementById('systemLedSwatch'); if(b)b.style.background=sysRgb;
  let stateNameText=p.state_led_name||((p.state?stateName(p.state)+' · LED1':'-'));
  let st=document.getElementById('stateLedText'); if(st)st.textContent=stateNameText;
  let sm=document.getElementById('stateLedMeta'); if(sm)sm.textContent=`LED1 ${stateRgb}`;
  let sysName=p.system_led_name||p.system_led_mode||'-';
  let bt=document.getElementById('systemLedText'); if(bt)bt.textContent=sysName;
  let bm=document.getElementById('systemLedMeta'); if(bm)bm.textContent=`LED2 ${sysRgb}${Number(p.power_key_led_override||0)?' · 电源键红色覆盖中':''}`;
  let flags=[];
  flags.push(Number(p.network_online||0)?'网络在线':'网络离线');
  if(Number(p.recording_active||0)) flags.push('录制中');
  if(Number(p.ota_active||0)) flags.push('OTA中');
  if(Number(p.power_key_led_override||0)) flags.push('电源键按住');
  if(Object.prototype.hasOwnProperty.call(p,'sd_mounted')) flags.push(Number(p.sd_mounted||0)?'SD已挂载':'SD未挂载');
  let fv=document.getElementById('systemFlagsText'); if(fv)fv.textContent=flags.join(' / ');
  let fm=document.getElementById('systemFlagsMeta'); if(fm)fm.textContent=`mode=${p.system_led_mode||'-'}`;
}
function setBadge(state){let s=(state||'--').toUpperCase();let b=document.getElementById('stateBadge');b.textContent=stateName(s);b.className='state-badge '+(STATE_CLASS[s]||'unknown')}
function connectionText(d){let age=d.last_pet_age_sec;if(age==null)return d.device_connection_text||'等待设备数据';let ageText=age.toFixed(1)+' 秒前';return `${d.device_connection_text||'设备状态'} · ${ageText}`}
async function refreshStatus(){try{let d=await api('/api/status');let conn=d.device_connection_state||'offline';document.getElementById('onlineDot').className='dot '+conn;document.getElementById('onlineText').textContent=connectionText(d);document.getElementById('serverAddr').textContent=`${d.server_ip}:${d.port}`;let p=d.last_pet||{},st=p.state||'';window.__lastPet=p;setBadge(st);document.getElementById('stateText').textContent=st?`${stateName(st)} · ${String(st).toUpperCase()}`:'等待设备数据';document.getElementById('stateMeta').textContent=st?`最近上报：${d.last_pet_age_sec==null?'-':d.last_pet_age_sec.toFixed(1)+' 秒前'}；状态上报目标约 1 秒一次`:'ESP32 POST /pet 后更新';document.getElementById('durationText').textContent=fmtDuration(p.state_duration_ms);document.getElementById('lastTimeText').textContent=p.time_str||p.received_at||'-';document.getElementById('bootText').textContent=p.boot_id||'-';renderLedStatus(p);renderStorageStatus(p);renderRecord(d.active_recording,d.recordings||[]);renderOta(d.ota_command);renderUploads(d.recent_uploads||[]);renderSd(d.sd||{});renderEvents(d.events||[])}catch(e){document.getElementById('onlineText').textContent='服务器连接失败'}}
function renderRecord(active,sessions){document.getElementById('recordStatus').textContent=active?`录制中：${active.label}`:'未录制';document.getElementById('recordDetail').textContent=active?`folder=${active.folder}，${Math.floor(active.duration_sec)}秒，采样=${active.sample_period_ms}ms，raw=${active.raw_sample_log?'on':'off'}，PET=${active.pet_rows}行，上传=${active.uploaded_files}个`:'结束录制后，设备上传的 R/S/E CSV 也会继续归入刚结束的会话。';let el=document.getElementById('recordings');if(!sessions.length){el.innerHTML='<div class="muted small">暂无录制会话。</div>';return}el.innerHTML=sessions.map(s=>{let running=s.is_active||(!s.ended_at_text&&!s.ended_at);let title=running?'录制中':'已结束';return `<div class="event"><div class="time">${escapeHtml(s.started_at_text)} ${running?'· 录制中':'→ '+escapeHtml(s.ended_at_text||'已结束')}</div><b>${escapeHtml(s.label)} · ${title}</b><div class="small muted">PET=${s.pet_rows||0} 行，上传=${s.uploaded_files||0} 个</div><div class="small">${escapeHtml(s.folder)}</div><div class="row" style="margin-top:8px"><button class="secondary" onclick="downloadRecord('${encodeURIComponent(s.id)}')">下载 zip</button></div></div>`}).join('')}
function renderOta(o){if(!o){document.getElementById('otaStatus').textContent='无待执行 OTA';document.getElementById('otaDetail').textContent='选择 .bin 后上传即可。';return}let name=o.firmware_name||o.external_url||o.firmware_path||'-';let st=o.consumed?'已下发给设备':'等待设备轮询';if(o.downloaded_count>0)st='设备已下载固件';document.getElementById('otaStatus').textContent=`${st}：${name}`;document.getElementById('otaDetail').textContent=`id=${o.id} served=${o.served_count} downloaded=${o.downloaded_count}`}
function renderUploads(files){let el=document.getElementById('uploads');if(!files.length){el.innerHTML='<div class="muted small">暂无上传文件。</div>';return}el.innerHTML=files.map(f=>`<div class="event"><div class="time">${f.mtime_text} · ${(f.size/1024).toFixed(1)} KB</div>${escapeHtml(f.path)}</div>`).join('')}
function renderEvents(events){let el=document.getElementById('events');let ev=events.slice().reverse();if(!ev.length){el.innerHTML='<div class="muted small">暂无事件。</div>';return}el.innerHTML=ev.map(e=>`<div class="event"><div class="time">${e.time}</div><b>${escapeHtml(e.kind)}</b> ${escapeHtml(e.message)}</div>`).join('')}
async function startRecord(){let label=document.getElementById('recordLabel').value;if(label==='custom')label=document.getElementById('recordCustom').value.trim()||'custom';let note=document.getElementById('recordNote').value;let sample_period_ms=Number(document.getElementById('recordSamplePeriod').value||5);let raw_sample_log=document.getElementById('recordRaw').checked;try{let d=await api('/api/record/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({label,note,sample_period_ms,raw_sample_log})});toast('开始高频录制：'+d.session.label);await refreshStatus()}catch(e){toast('开始失败：'+e.message)}}
async function stopRecord(){try{let d=await api('/api/record/stop',{method:'POST'});toast('结束录制，等待设备上传 CSV：'+d.session.label);await refreshStatus();setTimeout(loadFiles,3000)}catch(e){toast('结束失败：'+e.message)}}
function downloadRecord(id){window.open('/api/record/download?id='+id,'_blank')}
async function queueUpload(){let file=document.getElementById('binFile').files[0];if(!file)return toast('请先选择 .bin 文件');try{toast('正在上传固件...');let d=await api('/api/ota_upload',{method:'POST',headers:{'X-File-Name':encodeURIComponent(file.name)},body:file});toast('OTA 已发送：'+d.id);await refreshStatus()}catch(e){toast('OTA 失败：'+e.message)}}
async function clearOta(){try{await api('/api/clear_ota',{method:'POST'});toast('OTA 已清除');await refreshStatus()}catch(e){toast('清除失败：'+e.message)}}
async function loadFiles(){try{let d=await api('/api/files');allFiles=d.files||[];renderFiles()}catch(e){toast('文件列表失败：'+e.message)}}
function renderFiles(){let kind=document.getElementById('kindFilter').value;let files=kind?allFiles.filter(f=>f.kind===kind):allFiles;let el=document.getElementById('fileList');if(!files.length){el.innerHTML='<div class="muted small">暂无 CSV 文件。</div>';return}el.innerHTML=files.map(f=>`<div class="event fileitem" onclick="openCsv('${encodeURIComponent(f.token)}')"><div class="time">${f.mtime_text} · ${f.size_kb} KB · ${escapeHtml(f.kind)}</div><b>${escapeHtml(f.name)}</b><div class="small muted">${escapeHtml(f.path)}</div></div>`).join('')}
async function openCsv(token){currentFile=decodeURIComponent(token);await reloadCsv()}
async function reloadCsv(){if(!currentFile)return;try{let recent=document.getElementById('recentRows').value;let d=await api('/api/csv?path='+encodeURIComponent(currentFile)+'&recent='+recent);currentCsv=d;renderCsv(d)}catch(e){toast('读取 CSV 失败：'+e.message)}}
function renderCsv(d){document.getElementById('csvInfo').textContent=`${d.file.kind} · ${d.file.name} · 总行数 ${d.total_rows}，显示最近 ${d.returned_rows} 行`;let x=document.getElementById('xCol');x.innerHTML=d.columns.map(c=>`<option value="${escapeHtml(c)}">${escapeHtml(c)}</option>`).join('');let preferred=['time_ms','epoch_ms','time_str','received_at'];let found=preferred.find(c=>d.columns.includes(c));if(found)x.value=found;let y=document.getElementById('yChecks');let cols=d.numeric_columns.filter(c=>!['epoch_ms','time_ms'].includes(c));let defaults=cols.filter(c=>['gyro','acc','gx_dps','gy_dps','gz_dps','ax_g','ay_g','az_g','run_score','activity_score'].includes(c)).slice(0,4);if(!defaults.length)defaults=cols.slice(0,3);y.innerHTML=cols.map(c=>`<label><input type="checkbox" value="${escapeHtml(c)}" ${defaults.includes(c)?'checked':''} onchange="drawChart()"/>${escapeHtml(c)}</label>`).join('');renderStats(d);renderTable(d);drawChart()}
function selectedY(){return [...document.querySelectorAll('#yChecks input:checked')].map(i=>i.value)}
function renderStats(d){let ys=selectedY();let text=ys.map(c=>{let s=d.stats[c];return s?`${c}: min=${s.min.toFixed(3)} max=${s.max.toFixed(3)} avg=${s.avg.toFixed(3)} count=${s.count}`:''}).filter(Boolean).join(' ｜ ');document.getElementById('stats').textContent=text||'勾选 Y 轴字段后显示统计。'}
function renderTable(d){let rows=d.rows.slice(-200);let cols=d.columns;let html='<table><thead><tr>'+cols.map(c=>`<th>${escapeHtml(c)}</th>`).join('')+'</tr></thead><tbody>'+rows.map(r=>'<tr>'+cols.map(c=>`<td>${escapeHtml(r[c]??'')}</td>`).join('')+'</tr>').join('')+'</tbody></table>';document.getElementById('tableWrap').innerHTML=html}
function drawChart(){if(!currentCsv)return;renderStats(currentCsv);let canvas=document.getElementById('chart'),ctx=canvas.getContext('2d');let W=canvas.width,H=canvas.height;ctx.clearRect(0,0,W,H);ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);let rows=currentCsv.rows,ys=selectedY(),xcol=document.getElementById('xCol').value;if(!rows.length||!ys.length){ctx.fillStyle='#667085';ctx.fillText('请选择 Y 轴字段',24,32);return}let padL=55,padR=15,padT=20,padB=38;let vals=[];for(let y of ys){for(let r of rows){let v=parseFloat(r[y]);if(Number.isFinite(v))vals.push(v)}}if(!vals.length)return;let min=Math.min(...vals),max=Math.max(...vals);if(max===min){max+=1;min-=1}ctx.strokeStyle='#e5e9f2';ctx.lineWidth=1;for(let i=0;i<5;i++){let y=padT+(H-padT-padB)*i/4;ctx.beginPath();ctx.moveTo(padL,y);ctx.lineTo(W-padR,y);ctx.stroke();let val=max-(max-min)*i/4;ctx.fillStyle='#667085';ctx.fillText(val.toFixed(2),6,y+4)}let colors=['#2563eb','#dc2626','#0f9f6e','#f97316','#7c3aed','#111827'];ys.forEach((col,idx)=>{ctx.strokeStyle=colors[idx%colors.length];ctx.lineWidth=1.8;ctx.beginPath();let started=false;rows.forEach((r,i)=>{let v=parseFloat(r[col]);if(!Number.isFinite(v))return;let x=padL+(W-padL-padR)*(rows.length===1?0:i/(rows.length-1));let y=padT+(H-padT-padB)*(1-(v-min)/(max-min));if(!started){ctx.moveTo(x,y);started=true}else ctx.lineTo(x,y)});ctx.stroke();ctx.fillStyle=colors[idx%colors.length];ctx.fillText(col,padL+idx*130,14)});ctx.fillStyle='#667085';ctx.fillText(`${xcol} · ${rows.length} points`,padL,H-10)}
function downloadCurrentFile(){if(!currentFile)return toast('请先选择 CSV');window.open('/api/file/download?path='+encodeURIComponent(currentFile),'_blank')}

function sdFiles(){return (window.__sdFiles||[])}
async function sdRefreshOnly(){try{let d=await api('/api/sd/status');renderSd(d)}catch(e){toast('SD 状态刷新失败：'+e.message)}}
async function sdRequestList(){let path=document.getElementById('sdPath').value||'/sdcard';try{let d=await api('/api/sd/list',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path})});toast('已请求 ESP32 扫描当前目录，请等 1-3 秒');renderSd(d);setTimeout(sdRefreshOnly,2200);setTimeout(sdRefreshOnly,5000)}catch(e){toast('SD 刷新失败：'+e.message)}}
function selectedSdPaths(){return [...document.querySelectorAll('#sdFileList input.sdcheck:checked')].map(i=>i.value)}
async function sdUploadSelected(){let files=selectedSdPaths();if(!files.length)return toast('请先勾选文件');try{let d=await api('/api/sd/upload',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({files})});toast('已请求设备上传 '+files.length+' 个文件');renderSd(d);setTimeout(()=>{sdRefreshOnly();loadFiles()},2500)}catch(e){toast('上传请求失败：'+e.message)}}
async function sdDeleteSelected(){let files=selectedSdPaths();if(!files.length)return toast('请先勾选文件');if(!confirm('确定要删除 SD 卡上的 '+files.length+' 个文件？删除后不能恢复。'))return;try{let d=await api('/api/sd/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({files})});toast('已请求设备删除 '+files.length+' 个文件');renderSd(d);setTimeout(sdRefreshOnly,2500)}catch(e){toast('删除请求失败：'+e.message)}}
function sdSelectNone(){document.querySelectorAll('#sdFileList input.sdcheck').forEach(i=>i.checked=false)}
function sdSelectRaw(){document.querySelectorAll('#sdFileList input.sdcheck').forEach(i=>{let p=i.value.toUpperCase();i.checked=/\/R\d{6}\.CSV$/.test(p)||/R\d{6}\.CSV$/.test(p)})}
function sdEnterDir(path){document.getElementById('sdPath').value=path;sdRequestList()}
function renderSd(d){let sd=d.sd||d;let res=sd.last_result||{};let files=sd.files||[];window.__sdFiles=files;let status=document.getElementById('sdStatus');let detail=document.getElementById('sdDetail');if(status){let cmd=(sd.command&&sd.command.cmd)||'-';let pet=(window.__lastPet||{});let has=Object.prototype.hasOwnProperty.call(pet,'sd_mounted');let sdOk=Number(pet.sd_mounted||0)===1;status.textContent=`${has?(sdOk?'SD已挂载':'SD未挂载'):'等待SD状态'} · 当前目录文件 ${files.length} 个 · 最近命令 ${cmd}`;}if(detail){let current=(res.path||document.getElementById('sdPath')?.value||'/sdcard');detail.textContent=`当前目录：${current}；最后结果：${sd.last_result_at_text||'-'}；${res.message||''}${res.truncated?'；列表被截断，请进入更具体目录':''}`;}let el=document.getElementById('sdFileList');if(!el)return;if(!files.length){el.innerHTML='<div class="muted small">暂无 SD 文件列表。点击“刷新当前目录”。</div>';return}el.innerHTML=files.map(f=>{let isDir=Number(f.is_dir||0)===1;let size=isDir?'目录':((Number(f.size||0)/1024).toFixed(1)+' KB');let kind=isDir?'目录':(f.kind||'file');let enter=isDir?`<button class="secondary" onclick="sdEnterDir('${escapeHtml(f.path||'')}')">进入目录</button>`:'';return `<div class="event"><label style="display:flex;gap:10px;align-items:flex-start"><input class="sdcheck" type="checkbox" value="${escapeHtml(f.path)}" style="width:auto;margin-top:4px"/><span style="flex:1"><div class="time">${escapeHtml(kind)} · ${size}</div><b>${escapeHtml(f.name||'')}</b><div class="small muted">${escapeHtml(f.path||'')}</div><div class="row" style="margin-top:8px">${enter}</div></span></label></div>`}).join('')}
refreshStatus();loadFiles();setInterval(refreshStatus,1000);
</script>
</body></html>"""

class PetHandler(BaseHTTPRequestHandler):
    server_version = "IPETRecordConsole/1.0"

    def log_message(self, fmt: str, *args) -> None:
        if console_options.get("http_log", False):
            print("[%s] %s" % (time.strftime("%H:%M:%S"), fmt % args), flush=True)

    def end_headers(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-File-Name, X-Session")
        super().end_headers()

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.end_headers()

    def send_json(self, obj: dict, status: int = 200) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_html(self, html: str) -> None:
        body = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_json(self) -> dict:
        n = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(n) if n > 0 else b"{}"
        data = json.loads(body.decode("utf-8", errors="replace") or "{}")
        if not isinstance(data, dict):
            raise ValueError("JSON body must be object")
        return data

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        if path == "/":
            self.send_html(INDEX_HTML)
            return
        if path in ("/api/status", "/status"):
            self.send_json(status_payload())
            return
        if path == "/api/files":
            self.send_json({"ok": True, "files": list_csv_files()})
            return
        if path == "/api/sd/status":
            self.send_json(sd_status_payload())
            return
        if path == "/api/csv":
            params = parse_qs(parsed.query)
            p = resolve_data_path(params.get("path", [""])[0])
            recent = int(params.get("recent", ["1000"])[0] or 1000)
            self.send_json(read_csv_preview(p, recent))
            return
        if path == "/api/file/download":
            params = parse_qs(parsed.query)
            p = resolve_data_path(params.get("path", [""])[0])
            self.handle_file_download(p)
            return
        if path == "/api/record/download":
            params = parse_qs(parsed.query)
            sid = params.get("id", [""])[0]
            self.handle_record_download(sid)
            return
        if path == "/ota_cmd":
            self.handle_ota_cmd()
            return
        if path == "/record_cmd":
            self.handle_record_cmd()
            return
        if path == "/sd_cmd":
            self.handle_sd_cmd()
            return
        if path == "/firmware.bin":
            self.handle_firmware_download(parsed.query)
            return
        self.send_error(404, "Not found")

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        try:
            if path == "/pet":
                self.handle_pet_json()
                return
            if path == "/upload":
                self.handle_file_upload()
                return
            if path == "/api/record/start":
                data = self.read_json()
                session = start_recording(data.get("label", "manual"),
                                          data.get("note", ""),
                                          int(data.get("sample_period_ms", 5) or 5),
                                          bool(data.get("raw_sample_log", True)))
                self.send_json({"ok": True, "session": active_recording_to_dict()})
                return
            if path == "/api/record/stop":
                session = stop_recording()
                self.send_json({"ok": True, "session": {"id": session.session_id, "label": session.label, "folder": str(session.folder)}})
                return
            if path == "/api/ota_upload":
                self.handle_api_ota_upload()
                return
            if path == "/api/sd/list":
                data = self.read_json()
                self.send_json(queue_sd_command("list", data.get("path", "/sdcard")))
                return
            if path == "/api/sd/upload":
                data = self.read_json()
                self.send_json(queue_sd_command("upload", files=list(data.get("files", []))))
                return
            if path == "/api/sd/delete":
                data = self.read_json()
                self.send_json(queue_sd_command("delete", files=list(data.get("files", []))))
                return
            if path == "/sd_result":
                self.handle_sd_result()
                return
            if path == "/api/clear_ota":
                self.handle_api_clear_ota()
                return
        except Exception as e:
            add_event("error", str(e))
            self.send_json({"ok": False, "error": str(e)}, status=400)
            return
        self.send_error(404, "Not found")

    def handle_pet_json(self) -> None:
        global last_pet, last_pet_received_at
        data = self.read_json()
        data.setdefault("received_at", time.strftime("%Y-%m-%d %H:%M:%S"))
        append_json_row(data)
        record_pet_row(data)
        with state_lock:
            last_pet = dict(data)
            last_pet_received_at = time.time()
        if console_options.get("watch_pet", False):
            print(f"[PET] {data.get('time_str','')} state={data.get('state','')} duration={data.get('state_duration_ms','')}", flush=True)
        self.send_json({"ok": True})

    def handle_file_upload(self) -> None:
        n = int(self.headers.get("Content-Length", "0") or "0")
        filename = safe_filename(self.headers.get("X-File-Name", "upload.csv"), "upload.csv")
        session = self.headers.get("X-Session", "unknown").replace("\\", "/")
        session = posixpath.normpath("/" + session).lstrip("/")
        parts = [safe_filename(part, "_") for part in session.split("/") if part not in ("", ".", "..")]
        target_dir = UPLOAD_DIR.joinpath(*parts) if parts else UPLOAD_DIR
        target_dir.mkdir(parents=True, exist_ok=True)
        target = target_dir / filename
        data = self.rfile.read(n) if n > 0 else b""
        target.write_bytes(data)
        record_uploaded_file(target, filename)
        add_event("upload", f"收到文件：{filename}", path=str(target), size=len(data))
        print(f"[UPLOAD] {target} ({len(data)} bytes)", flush=True)
        self.send_json({"ok": True, "file": str(target)})

    def handle_api_ota_upload(self) -> None:
        filename = safe_filename(self.headers.get("X-File-Name", "firmware.bin"), "firmware.bin")
        if not filename.lower().endswith(".bin"):
            filename += ".bin"
        n = int(self.headers.get("Content-Length", "0") or "0")
        if n <= 0:
            raise ValueError("empty firmware")
        OTA_DIR.mkdir(parents=True, exist_ok=True)
        target = OTA_DIR / (time.strftime("%Y%m%d-%H%M%S") + "_" + filename)
        with target.open("wb") as f:
            remaining = n
            while remaining > 0:
                chunk = self.rfile.read(min(131072, remaining))
                if not chunk:
                    break
                f.write(chunk)
                remaining -= len(chunk)
        cmd = queue_ota_file(target)
        self.send_json({"ok": True, "id": cmd.command_id, "path": str(target)})

    def handle_api_clear_ota(self) -> None:
        global ota_command
        with state_lock:
            ota_command = None
        add_event("ota", "OTA 队列已清除")
        self.send_json({"ok": True})

    def handle_file_download(self, path: Path) -> None:
        size = path.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", "text/csv; charset=utf-8" if path.suffix.lower() == ".csv" else "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Content-Disposition", f'attachment; filename="{path.name}"')
        self.end_headers()
        with path.open("rb") as f:
            shutil.copyfileobj(f, self.wfile)

    def handle_record_download(self, session_id: str) -> None:
        zip_path = zip_recording(session_id)
        body_size = zip_path.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", "application/zip")
        self.send_header("Content-Length", str(body_size))
        self.send_header("Content-Disposition", f'attachment; filename="{zip_path.name}"')
        self.end_headers()
        with zip_path.open("rb") as f:
            shutil.copyfileobj(f, self.wfile)

    def handle_record_cmd(self) -> None:
        # 录制结束后的一小段时间内，持续告诉 ESP32 立即扫描并上传 CSV。
        # 这样 R000xxx.CSV 不需要等默认扫描周期。
        with state_lock:
            payload = dict(record_command_payload)
            if upload_capture_session is not None and time.time() <= upload_capture_until:
                elapsed_after_stop = max(0.0, time.time() - (upload_capture_session.ended_at or time.time()))
                if elapsed_after_stop <= UPLOAD_COMMAND_GRACE_SEC:
                    payload["upload_now"] = 1
                    payload["upload_record_id"] = upload_capture_session.session_id
                    payload["upload_grace_sec"] = UPLOAD_COMMAND_GRACE_SEC
        self.send_json(payload)

    def handle_sd_cmd(self) -> None:
        host = self.headers.get("Host") or f"{local_ip_guess()}:{PORT}"
        with state_lock:
            payload = dict(sd_command_payload)
            if not payload.get("sd"):
                self.send_json({"sd": 0})
                return
            # 下发一次后清空，避免设备反复删除/上传同一批文件。
            globals()["sd_command_payload"] = {"sd": 0, "id": payload.get("id", "done") + "_done"}
        payload.setdefault("result_url", f"http://{host}/sd_result")
        payload.setdefault("upload_url", f"http://{host}/upload")
        add_event("sd", f"SD 命令已下发给 {self.client_address[0]}", id=payload.get("id"), cmd=payload.get("cmd"))
        self.send_json(payload)

    def handle_sd_result(self) -> None:
        global sd_last_result, sd_last_result_at, sd_files_cache
        data = self.read_json()
        data.setdefault("received_at", time.strftime("%Y-%m-%d %H:%M:%S"))
        with state_lock:
            sd_last_result = dict(data)
            sd_last_result_at = time.time()
            if data.get("cmd") == "list" and isinstance(data.get("files"), list):
                sd_files_cache = data.get("files", [])
        add_event("sd", f"SD 结果：{data.get('cmd','')} {data.get('message','')}", id=data.get("id"), count=data.get("count"), affected=data.get("affected"))
        self.send_json({"ok": True})

    def handle_ota_cmd(self) -> None:
        host = self.headers.get("Host") or f"{local_ip_guess()}:{PORT}"
        with state_lock:
            cmd = ota_command
            if cmd is None or cmd.consumed:
                self.send_json({"ota": 0})
                return
            cmd.served_count += 1
            cmd.consumed = True
            url = cmd.external_url if cmd.external_url else f"http://{host}/firmware.bin?id={cmd.command_id}"
            payload = {"ota": 1, "id": cmd.command_id, "url": url}
        add_event("ota", f"OTA 命令已下发给 {self.client_address[0]}", id=payload["id"])
        self.send_json(payload)

    def handle_firmware_download(self, query: str) -> None:
        params = parse_qs(query)
        requested_id = params.get("id", [""])[0]
        with state_lock:
            cmd = ota_command
            if cmd is None or cmd.firmware_path is None:
                self.send_error(404, "No firmware")
                return
            if requested_id and requested_id != cmd.command_id:
                self.send_error(404, "OTA id not found")
                return
            firmware_path = cmd.firmware_path
        size = firmware_path.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Content-Disposition", f'attachment; filename="{firmware_path.name}"')
        self.end_headers()
        with firmware_path.open("rb") as f:
            shutil.copyfileobj(f, self.wfile)
        with state_lock:
            cmd = ota_command
            if cmd and cmd.command_id == requested_id:
                cmd.downloaded_count += 1
                cmd.last_download_at = time.time()
                cmd.last_download_ip = self.client_address[0]
        add_event("ota", f"固件已被 {self.client_address[0]} 下载", path=str(firmware_path), size=size)


def console_loop(httpd: ThreadingHTTPServer) -> None:
    print("Commands: status | record start <label> | record stop | quit", flush=True)
    while True:
        try:
            line = input("pet-rec> ").strip()
        except (EOFError, KeyboardInterrupt):
            line = "quit"
        if not line:
            continue
        try:
            if line == "status":
                print(json.dumps(status_payload(), ensure_ascii=False, indent=2), flush=True)
            elif line.startswith("record start"):
                label = line[len("record start"):].strip() or "manual"
                s = start_recording(label, sample_period_ms=5, raw_sample_log=True)
                print(f"recording started: {s.folder}, sample={s.sample_period_ms}ms, raw={s.raw_sample_log}", flush=True)
            elif line == "record stop":
                s = stop_recording()
                print(f"recording stopped: {s.folder}", flush=True)
            elif line in ("quit", "exit"):
                httpd.shutdown()
                break
            else:
                print("Unknown command", flush=True)
        except Exception as e:
            print(f"[ERROR] {e}", flush=True)


def main() -> int:
    for d in (UPLOAD_DIR, OTA_DIR, RECORD_DIR):
        d.mkdir(parents=True, exist_ok=True)
    ip = local_ip_guess()
    httpd = ThreadingHTTPServer((HOST, PORT), PetHandler)
    add_event("server", "Recording console started")
    print(f"IPET recording console listening on http://{HOST}:{PORT}", flush=True)
    print(f"Open dashboard:  http://{ip}:{PORT}/", flush=True)
    print(f"ESP32 JSON URL:  http://{ip}:{PORT}/pet", flush=True)
    print(f"ESP32 FILE URL:  http://{ip}:{PORT}/upload", flush=True)
    print(f"ESP32 OTA CMD:   http://{ip}:{PORT}/ota_cmd", flush=True)
    print(f"ESP32 RECORD CMD:http://{ip}:{PORT}/record_cmd", flush=True)
    print(f"ESP32 SD CMD:    http://{ip}:{PORT}/sd_cmd", flush=True)
    print(f"Recording dir:   {RECORD_DIR.resolve()}", flush=True)
    t = threading.Thread(target=console_loop, args=(httpd,), daemon=True)
    t.start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
