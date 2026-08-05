"""
Bird Feeder Video Server
Receives MJPEG uploads from the ESP32 and saves them to disk.
"""

from datetime import datetime
import os
import socket
import struct

from flask import Flask, jsonify, request
from werkzeug.exceptions import BadRequest

app = Flask(__name__)

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UPLOAD_FOLDER = os.environ.get(
    "BIRD_VIDEO_FOLDER",
    os.path.join(PROJECT_ROOT, "bird_videos"),
)
os.makedirs(UPLOAD_FOLDER, exist_ok=True)


def count_jpeg_frames(filepath):
    """Count JPEG SOI markers inside an MJPEG file."""
    try:
        with open(filepath, "rb") as infile:
            data = infile.read()
        return data.count(b"\xff\xd8")
    except Exception:
        return 0


def extract_jpeg_frames(data):
    """Split a raw MJPEG byte stream into complete JPEG frames."""
    frames = []
    cursor = 0

    while True:
        start = data.find(b"\xff\xd8", cursor)
        if start < 0:
            break

        end = data.find(b"\xff\xd9", start + 2)
        if end < 0:
            break

        frames.append(data[start : end + 2])
        cursor = end + 2

    return frames


def get_jpeg_dimensions(frame_bytes):
    """Read width and height from a JPEG SOF marker."""
    sof_markers = {
        0xC0,
        0xC1,
        0xC2,
        0xC3,
        0xC5,
        0xC6,
        0xC7,
        0xC9,
        0xCA,
        0xCB,
        0xCD,
        0xCE,
        0xCF,
    }
    standalone_markers = {0x01, 0xD8, 0xD9} | set(range(0xD0, 0xD8))

    pos = 2
    while pos + 8 < len(frame_bytes):
        if frame_bytes[pos] != 0xFF:
            pos += 1
            continue

        while pos < len(frame_bytes) and frame_bytes[pos] == 0xFF:
            pos += 1

        if pos >= len(frame_bytes):
            break

        marker = frame_bytes[pos]
        pos += 1

        if marker in standalone_markers:
            continue

        if pos + 1 >= len(frame_bytes):
            break

        segment_length = struct.unpack(">H", frame_bytes[pos : pos + 2])[0]
        if segment_length < 2 or pos + segment_length > len(frame_bytes):
            break

        if marker in sof_markers:
            height = struct.unpack(">H", frame_bytes[pos + 3 : pos + 5])[0]
            width = struct.unpack(">H", frame_bytes[pos + 5 : pos + 7])[0]
            return width, height

        pos += segment_length

    raise ValueError("Could not determine JPEG dimensions")


def riff_chunk(chunk_id, payload):
    chunk = chunk_id + struct.pack("<I", len(payload)) + payload
    if len(payload) & 1:
        chunk += b"\x00"
    return chunk


def list_chunk(list_type, payload):
    chunk_payload = list_type + payload
    chunk = b"LIST" + struct.pack("<I", len(chunk_payload)) + chunk_payload
    if len(chunk_payload) & 1:
        chunk += b"\x00"
    return chunk


def write_mjpeg_avi(frames, avi_path, fps):
    """Wrap JPEG frames in a simple MJPEG AVI container."""
    if not frames:
        raise ValueError("No frames available for AVI conversion")

    width, height = get_jpeg_dimensions(frames[0])
    frame_count = len(frames)
    max_frame_size = max(len(frame) for frame in frames)
    fps_int = max(1, int(round(fps)))
    microseconds_per_frame = int(round(1_000_000 / fps_int))
    max_bytes_per_second = max_frame_size * fps_int

    avih_payload = struct.pack(
        "<IIIIIIIIII4I",
        microseconds_per_frame,
        max_bytes_per_second,
        0,
        0x10,
        frame_count,
        0,
        1,
        max_frame_size,
        width,
        height,
        0,
        0,
        0,
        0,
    )
    avih_chunk = riff_chunk(b"avih", avih_payload)

    strh_payload = struct.pack(
        "<4s4sIHHIIIIIIIIhhhh",
        b"vids",
        b"MJPG",
        0,
        0,
        0,
        0,
        1,
        fps_int,
        0,
        frame_count,
        max_frame_size,
        0xFFFFFFFF,
        0,
        0,
        0,
        width,
        height,
    )
    strh_chunk = riff_chunk(b"strh", strh_payload)

    strf_payload = struct.pack(
        "<IiiHH4sIiiII",
        40,
        width,
        height,
        1,
        24,
        b"MJPG",
        max_frame_size,
        0,
        0,
        0,
        0,
    )
    strf_chunk = riff_chunk(b"strf", strf_payload)

    strl_list = list_chunk(b"strl", strh_chunk + strf_chunk)
    hdrl_list = list_chunk(b"hdrl", avih_chunk + strl_list)

    movi_payload = bytearray(b"movi")
    idx1_entries = bytearray()
    movi_offset = 4

    for frame in frames:
        frame_chunk = b"00dc" + struct.pack("<I", len(frame)) + frame
        if len(frame) & 1:
            frame_chunk += b"\x00"

        movi_payload.extend(frame_chunk)
        idx1_entries.extend(b"00dc")
        idx1_entries.extend(struct.pack("<III", 0x10, movi_offset, len(frame)))
        movi_offset += len(frame_chunk)

    movi_list = b"LIST" + struct.pack("<I", len(movi_payload)) + movi_payload
    idx1_chunk = riff_chunk(b"idx1", bytes(idx1_entries))

    riff_payload = b"AVI " + hdrl_list + movi_list + idx1_chunk
    avi_data = b"RIFF" + struct.pack("<I", len(riff_payload)) + riff_payload

    with open(avi_path, "wb") as outfile:
        outfile.write(avi_data)

    return width, height


def save_request_body_to_file(filepath):
    """Stream the request body to disk using the declared Content-Length."""
    content_length = request.content_length
    raw_input = request.environ["wsgi.input"]
    total_written = 0

    with open(filepath, "wb") as outfile:
        if content_length is None:
            while True:
                chunk = raw_input.read(8192)
                if not chunk:
                    break
                outfile.write(chunk)
                total_written += len(chunk)
        else:
            remaining = content_length
            while remaining > 0:
                chunk = raw_input.read(min(8192, remaining))
                if not chunk:
                    raise ValueError(
                        f"Upload ended early: received {total_written} of {content_length} bytes"
                    )
                outfile.write(chunk)
                chunk_len = len(chunk)
                total_written += chunk_len
                remaining -= chunk_len

    return total_written


def finalize_raw_mjpeg_upload(raw_path, avi_path, frame_interval_ms, video_duration_ms, expected_frame_count=None):
    """Convert a raw MJPEG upload on disk into AVI."""
    with open(raw_path, "rb") as infile:
        payload = infile.read()

    frames = extract_jpeg_frames(payload)
    if not frames:
        raise ValueError("Upload did not contain any complete JPEG frames")

    if expected_frame_count is not None and expected_frame_count > 0:
        if len(frames) < expected_frame_count:
            raise ValueError(
                f"Upload only contained {len(frames)} JPEG frames, expected {expected_frame_count}"
            )
        if len(frames) > expected_frame_count:
            frames = frames[:expected_frame_count]

    if video_duration_ms > 0:
        fps = max(1.0, len(frames) / (video_duration_ms / 1000.0))
    else:
        fps = 1000.0 / max(1, frame_interval_ms)

    width, height = write_mjpeg_avi(frames, avi_path, fps)
    return len(frames), fps, width, height


def get_local_ip():
    """Best-effort LAN IP for console output."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


@app.route("/", methods=["GET"])
def index():
    return jsonify(
        {
            "status": "ok",
            "message": "Bird feeder server is running",
            "upload_endpoint": "/upload",
            "save_folder": UPLOAD_FOLDER,
        }
    ), 200


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok"}), 200


@app.route("/stream_upload", methods=["POST"])
def stream_upload():
    """Receive chunked MJPEG stream uploads from the ESP32."""
    try:
        filename = request.headers.get(
            "X-Filename",
            f"bird_{datetime.now().strftime('%Y%m%d_%H%M%S')}.mjpeg",
        )
        base_name = os.path.splitext(filename)[0]
        avi_filename = f"{base_name}.avi"
        avi_path = os.path.join(UPLOAD_FOLDER, avi_filename)
        raw_path = os.path.join(UPLOAD_FOLDER, filename)
        remote_ip = request.headers.get("X-Forwarded-For", request.remote_addr)
        frame_interval_ms = int(request.headers.get("X-Frame-Interval-Ms", "125"))
        video_duration_ms = int(request.headers.get("X-Video-Duration-Ms", "0"))
        frame_count_header = request.headers.get("X-Frame-Count")
        expected_frame_count = int(frame_count_header) if frame_count_header else None

        print(
            f"Incoming stream from {remote_ip} -> {filename} "
            f"(Transfer-Encoding={request.headers.get('Transfer-Encoding')}, "
            f"Content-Length={request.headers.get('Content-Length')}, "
            f"Frame-Count={frame_count_header or 'unknown'})"
        )

        bytes_written = save_request_body_to_file(raw_path)
        frame_count, fps, width, height = finalize_raw_mjpeg_upload(
            raw_path, avi_path, frame_interval_ms, video_duration_ms, expected_frame_count
        )
        size = os.path.getsize(avi_path)
        if os.path.exists(raw_path):
            os.remove(raw_path)

        print(
            f"OK Saved AVI: {avi_filename} ({size / (1024 * 1024):.2f} MB, "
            f"{frame_count} frames, {width}x{height}, {fps:.2f} fps, "
            f"raw bytes={bytes_written})"
        )

        return jsonify(
            {
                "status": "success",
                "filename": avi_filename,
                "size": size,
                "frames": frame_count,
                "width": width,
                "height": height,
                "fps": fps,
            }
        ), 200
    except BadRequest as exc:
        if "raw_path" in locals() and os.path.exists(raw_path):
            print(f"Saved raw MJPEG fallback: {raw_path}")
        print(f"ERROR receiving stream: malformed/incomplete request: {exc}")
        return jsonify({"status": "error", "message": f"Malformed upload: {exc}"}), 400
    except Exception as exc:
        if "raw_path" in locals() and os.path.exists(raw_path):
            print(f"Saved raw MJPEG fallback: {raw_path}")
        print(f"ERROR receiving stream: {exc}")
        return jsonify({"status": "error", "message": str(exc)}), 500


@app.route("/upload", methods=["POST"])
def upload_video():
    """Receive a fixed-length raw MJPEG upload from the ESP32."""
    try:
        filename = request.headers.get(
            "X-Filename",
            f"bird_{datetime.now().strftime('%Y%m%d_%H%M%S')}.mjpeg",
        )
        base_name = os.path.splitext(filename)[0]
        avi_filename = f"{base_name}.avi"
        raw_path = os.path.join(UPLOAD_FOLDER, filename)
        avi_path = os.path.join(UPLOAD_FOLDER, avi_filename)
        remote_ip = request.headers.get("X-Forwarded-For", request.remote_addr)
        frame_interval_ms = int(request.headers.get("X-Frame-Interval-Ms", "200"))
        video_duration_ms = int(request.headers.get("X-Video-Duration-Ms", "0"))
        frame_count_header = request.headers.get("X-Frame-Count", "unknown")
        expected_frame_count = None if frame_count_header == "unknown" else int(frame_count_header)

        print(
            f"Incoming upload from {remote_ip} -> {filename} "
            f"(Content-Length={request.headers.get('Content-Length')}, "
            f"Frame-Count={frame_count_header})"
        )

        bytes_written = save_request_body_to_file(raw_path)
        frame_count, fps, width, height = finalize_raw_mjpeg_upload(
            raw_path, avi_path, frame_interval_ms, video_duration_ms, expected_frame_count
        )
        size = os.path.getsize(avi_path)
        if os.path.exists(raw_path):
            os.remove(raw_path)

        print(
            f"OK Saved AVI: {avi_filename} ({size / (1024 * 1024):.2f} MB, "
            f"{frame_count} frames, {width}x{height}, {fps:.2f} fps, "
            f"raw bytes={bytes_written})"
        )

        return jsonify(
            {
                "status": "success",
                "filename": avi_filename,
                "size": size,
                "frames": frame_count,
                "width": width,
                "height": height,
                "fps": fps,
            }
        ), 200
    except BadRequest as exc:
        if "raw_path" in locals() and os.path.exists(raw_path):
            print(f"Saved raw MJPEG fallback: {raw_path}")
        print(f"ERROR receiving upload: malformed/incomplete request: {exc}")
        return jsonify({"status": "error", "message": f"Malformed upload: {exc}"}), 400
    except Exception as exc:
        if "raw_path" in locals() and os.path.exists(raw_path):
            print(f"Saved raw MJPEG fallback: {raw_path}")
        print(f"ERROR receiving upload: {exc}")
        return jsonify({"status": "error", "message": str(exc)}), 500


if __name__ == "__main__":
    local_ip = get_local_ip()

    print("=" * 60)
    print("Bird Feeder Video Server")
    print("=" * 60)
    print(f"Videos saved to: {UPLOAD_FOLDER}")
    print(f"Health URL (local PC only): http://127.0.0.1:5000/health")
    print(f"Health URL (LAN / ESP32 should use this IP): http://{local_ip}:5000/health")
    print(f"Upload URL for ESP32: http://{local_ip}:5000/upload")
    print("Listening on all interfaces: 0.0.0.0:5000")
    print("Press Ctrl+C to stop")
    print("=" * 60)

    app.run(host="0.0.0.0", port=5000, debug=False, threaded=True)
