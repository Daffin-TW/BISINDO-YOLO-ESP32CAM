"""
=============================================================================
BISINDO Sign Language Translator  -  Flask Dashboard
=============================================================================
Backend responsibilities
  - Proxy + decode the ESP32-CAM MJPEG stream (port 81)
  - Run YOLO inference on every frame
  - Draw bounding boxes and emit the annotated frame as a new MJPEG feed
  - Push the highest-confidence detection label to the ESP32-CAM via POST /label
  - Provide a Server-Sent Events (SSE) endpoint for live sidebar stats
  - Expose REST controls:  /api/start  /api/stop  /api/status

All tuneable constants live at the top of this file for easy modification.
=============================================================================
"""

import time
import threading
import queue
import requests
import cv2
import numpy as np
from flask import Flask, Response, render_template, jsonify, stream_with_context

from utils.config import (
    ESP32_STREAM_URL,
    ESP32_BASE_URL,
    MODEL_PATH,
    CONFIDENCE_THRESHOLD,
    LABEL_SEND_COOLDOWN,
    INFERENCE_SKIP_FRAMES,
    FLASK_HOST,
    FLASK_PORT,
    FLASK_DEBUG,
)
from utils.detect import Detector
from utils.fps import FPS

# ─────────────────────────────────────────────────────────────────────────────
# Flask app
# ─────────────────────────────────────────────────────────────────────────────
app = Flask(__name__)

# ─────────────────────────────────────────────────────────────────────────────
# Shared state  (guarded by a lock where needed)
# ─────────────────────────────────────────────────────────────────────────────
_lock = threading.Lock()
_state = {
    "streaming":      False,   # True while the capture thread is running
    "connected":      False,   # True once the first frame is received
    "fps":            0,
    "latency_ms":     0,
    "active_seconds": 0,
    "battery":        "--",    # polled from ESP32 /battery (if implemented)
    "last_label":     "",
    "last_label_id":  -1,
}

# MJPEG output queue: the latest annotated JPEG bytes (max 2 frames buffered)
_frame_queue: queue.Queue = queue.Queue(maxsize=2)

# SSE event queue for sidebar updates
_sse_queue: queue.Queue = queue.Queue(maxsize=50)

_capture_thread: threading.Thread | None = None
_detector: Detector | None = None
_start_time: float = 0.0
_last_label_time: float = 0.0

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _push_sse(event: str, data: str) -> None:
    """Push an SSE message, dropping oldest if the queue is full."""
    msg = f"event: {event}\ndata: {data}\n\n"
    try:
        _sse_queue.put_nowait(msg)
    except queue.Full:
        try:
            _sse_queue.get_nowait()
        except queue.Empty:
            pass
        _sse_queue.put_nowait(msg)


def _send_label_to_esp32(label_id: int) -> None:
    """POST the label id to the ESP32-CAM asynchronously."""
    global _last_label_time
    now = time.time()
    if now - _last_label_time < LABEL_SEND_COOLDOWN:
        return
    _last_label_time = now

    def _post():
        try:
            requests.post(
                f"{ESP32_BASE_URL}/label",
                data={"id": label_id},
                timeout=1.0,
            )
        except Exception:
            pass

    threading.Thread(target=_post, daemon=True).start()


def _build_status_json() -> dict:
    with _lock:
        s = dict(_state)
    elapsed = int(time.time() - _start_time) if s["streaming"] else 0
    s["active_seconds"] = elapsed
    return s


# ─────────────────────────────────────────────────────────────────────────────
# Capture + inference thread
# ─────────────────────────────────────────────────────────────────────────────

def _capture_loop() -> None:
    global _detector, _start_time

    fps_counter = FPS()
    frame_index = 0

    # Lazy-load the YOLO detector (heavy, done once)
    if _detector is None:
        _detector = Detector(MODEL_PATH, CONFIDENCE_THRESHOLD)

    # Open the ESP32-CAM MJPEG stream via OpenCV
    cap = cv2.VideoCapture(ESP32_STREAM_URL)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

    if not cap.isOpened():
        with _lock:
            _state["connected"] = False
        _push_sse("status", '{"connected":false}')
        return

    with _lock:
        _state["connected"] = True
        _state["streaming"] = True
    _start_time = time.time()
    _push_sse("status", '{"connected":true}')

    while True:
        with _lock:
            running = _state["streaming"]
        if not running:
            break

        t0 = time.perf_counter()
        ret, frame = cap.read()
        if not ret or frame is None:
            with _lock:
                _state["connected"] = False
            _push_sse("status", '{"connected":false}')
            break

        latency_ms = int((time.perf_counter() - t0) * 1000)
        fps = fps_counter.calculate_fps()

        # ── Inference (every Nth frame to reduce CPU load) ──────────────────
        detections = []
        if frame_index % max(1, INFERENCE_SKIP_FRAMES) == 0:
            detections = _detector.detect(frame)

        # ── Draw bounding boxes ─────────────────────────────────────────────
        best_label_id = -1
        best_conf = 0.0

        for det in detections:
            label       = det["label"]
            label_id    = det["label_id"]
            confidence  = det["confidence"]
            x1, y1, x2, y2 = det["bbox"]

            # Box
            cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 220, 120), 2)
            # Tag background
            text = f"{label}  {confidence:.0%}"
            (tw, th), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.65, 2)
            cv2.rectangle(frame, (x1, y1 - th - 10), (x1 + tw + 6, y1), (0, 220, 120), -1)
            cv2.putText(frame, text, (x1 + 3, y1 - 5),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.65, (15, 15, 15), 2)

            if confidence > best_conf:
                best_conf = confidence
                best_label_id = label_id

        # ── Send label to ESP32-CAM ─────────────────────────────────────────
        if best_label_id > 0:
            with _lock:
                _state["last_label"]    = detections[[d["label_id"] for d in detections].index(best_label_id)]["label"]
                _state["last_label_id"] = best_label_id
            _send_label_to_esp32(best_label_id)

        # ── Update shared state ─────────────────────────────────────────────
        with _lock:
            _state["fps"]        = fps
            _state["latency_ms"] = latency_ms

        # ── Push SSE stats ──────────────────────────────────────────────────
        import json
        _push_sse("stats", json.dumps({
            "fps":        fps,
            "latency_ms": latency_ms,
            "connected":  True,
            "last_label": _state.get("last_label", ""),
        }))

        # ── Encode and enqueue frame ────────────────────────────────────────
        ret2, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
        if ret2:
            jpeg = buf.tobytes()
            try:
                _frame_queue.put_nowait(jpeg)
            except queue.Full:
                try:
                    _frame_queue.get_nowait()
                except queue.Empty:
                    pass
                _frame_queue.put_nowait(jpeg)

        frame_index += 1

    cap.release()
    with _lock:
        _state["streaming"] = False
        _state["connected"] = False
        _state["fps"]       = 0
        _state["latency_ms"] = 0
    _push_sse("status", '{"connected":false,"streaming":false}')


# ─────────────────────────────────────────────────────────────────────────────
# Routes
# ─────────────────────────────────────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/video_feed")
def video_feed():
    """MJPEG feed of annotated frames."""
    def _gen():
        blank = _make_blank_frame()
        while True:
            with _lock:
                streaming = _state["streaming"]
            if not streaming:
                yield (b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + blank + b"\r\n")
                time.sleep(0.1)
                continue
            try:
                jpeg = _frame_queue.get(timeout=0.5)
                yield (b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n")
            except queue.Empty:
                yield (b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + blank + b"\r\n")

    return Response(
        stream_with_context(_gen()),
        mimetype="multipart/x-mixed-replace; boundary=frame",
    )


@app.route("/events")
def events():
    """Server-Sent Events stream for sidebar stats."""
    def _gen():
        # Send current state immediately on connect
        import json
        status = _build_status_json()
        yield f"event: init\ndata: {json.dumps(status)}\n\n"
        while True:
            try:
                msg = _sse_queue.get(timeout=5)
                yield msg
            except queue.Empty:
                yield ": keepalive\n\n"

    return Response(
        stream_with_context(_gen()),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


@app.route("/api/start", methods=["POST"])
def api_start():
    global _capture_thread
    with _lock:
        already = _state["streaming"]
    if already:
        return jsonify({"ok": True, "msg": "already running"})

    _capture_thread = threading.Thread(target=_capture_loop, daemon=True)
    _capture_thread.start()
    return jsonify({"ok": True, "msg": "started"})


@app.route("/api/stop", methods=["POST"])
def api_stop():
    with _lock:
        _state["streaming"] = False
    return jsonify({"ok": True, "msg": "stopped"})


@app.route("/api/status")
def api_status():
    return jsonify(_build_status_json())


# ─────────────────────────────────────────────────────────────────────────────
# Utilities
# ─────────────────────────────────────────────────────────────────────────────

def _make_blank_frame(w: int = 640, h: int = 480) -> bytes:
    """Return a JPEG of a dark placeholder frame."""
    img = np.zeros((h, w, 3), dtype=np.uint8)
    img[:] = (20, 20, 30)
    cv2.putText(img, "Stream stopped", (w // 2 - 110, h // 2),
                cv2.FONT_HERSHEY_SIMPLEX, 1.0, (80, 80, 100), 2)
    _, buf = cv2.imencode(".jpg", img)
    return buf.tobytes()


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    app.run(host=FLASK_HOST, port=FLASK_PORT, debug=FLASK_DEBUG, threaded=True)