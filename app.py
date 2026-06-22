"""
=============================================================================
BISINDO Sign Language Translator  -  Flask Dashboard
=============================================================================
Two-thread architecture:
  • _reader_loop  : reads camera frames as fast as possible (no inference),
                    stores the latest raw frame in _raw_frame.
  • _inference_loop: picks up the latest raw frame, runs YOLO, annotates,
                    stores the output JPEG in _output_jpeg.
  • /video_feed   : polls _output_jpeg and emits it as an MJPEG stream.

This separation ensures frame reading is never blocked by inference time,
eliminating the "stutter at stream start" and "stale frame" problems.
=============================================================================
"""

import json
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
    DETECTION_DWELL_TIME,
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
_state: dict = {
    "streaming":      False,
    "connected":      False,
    "fps":            0,
    "latency_ms":     0,
    "active_seconds": 0,
    "battery":        "--",
    "last_label":     "",
    "last_label_id":  -1,
}

# ── Inter-thread frame buffers ───────────────────────────────────────────────
# Reader  → Inference : latest raw frame
_raw_frame:       np.ndarray | None = None
_raw_frame_lock   = threading.Lock()
_new_raw_event    = threading.Event()   # set when reader stores a new raw frame

# Inference → video_feed : latest annotated JPEG (single-slot, always fresh)
_output_jpeg:     bytes | None = None
_output_jpeg_lock = threading.Lock()
_output_seq:      int = 0               # monotonic counter; video_feed compares

# SSE event queue for sidebar stats
_sse_queue: queue.Queue = queue.Queue(maxsize=50)

# Thread handles
_reader_thread:    threading.Thread | None = None
_inference_thread: threading.Thread | None = None
_detector:         Detector | None = None
_start_time:       float = 0.0
_last_label_time:  float = 0.0

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def _push_sse(event: str, data: str) -> None:
    """Push an SSE message, dropping the oldest if the queue is full."""
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
    """POST the label id to the ESP32-CAM asynchronously (rate-limited)."""
    global _last_label_time
    now = time.time()
    if now - _last_label_time < LABEL_SEND_COOLDOWN:
        return
    _last_label_time = now

    def _post() -> None:
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


def _is_running() -> bool:
    with _lock:
        return _state["streaming"]


# ─────────────────────────────────────────────────────────────────────────────
# Thread 1 — Camera reader
# Reads from the ESP32-CAM MJPEG stream as fast as possible and stores
# only the most recent raw frame.  No inference is done here.
# ─────────────────────────────────────────────────────────────────────────────

def _reader_loop() -> None:
    global _raw_frame

    cap = cv2.VideoCapture(ESP32_STREAM_URL)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)  # discard all but the latest buffered frame

    if not cap.isOpened():
        with _lock:
            _state["connected"] = False
            _state["streaming"] = False
        _push_sse("status", '{"connected":false,"streaming":false}')
        _new_raw_event.set()  # wake inference thread so it exits cleanly
        return

    with _lock:
        _state["connected"] = True
    _push_sse("status", '{"connected":true}')

    while _is_running():
        ret, frame = cap.read()
        if not ret or frame is None:
            # Brief pause before retry to avoid busy-spin on transient errors
            time.sleep(0.02)
            continue
        with _raw_frame_lock:
            _raw_frame = frame          # replace; old frame is discarded
        _new_raw_event.set()            # notify inference thread

    cap.release()
    with _lock:
        _state["connected"] = False
    _new_raw_event.set()                # wake inference thread so it can exit


# ─────────────────────────────────────────────────────────────────────────────
# Thread 2 — Inference + annotation
# Picks up the latest raw frame, runs YOLO (on every Nth frame),
# draws bounding boxes using the most recent detections, and stores
# the annotated JPEG for the video feed to serve.
# ─────────────────────────────────────────────────────────────────────────────

def _inference_loop() -> None:
    global _detector, _start_time, _output_jpeg, _output_seq

    fps_counter    = FPS()
    frame_index    = 0
    last_detections: list[dict] = []   # persisted between inference frames
    _last_sse_t:    float = 0.0        # perf_counter timestamp of last SSE push
    _sse_prev_label: str  = ""         # last label sent over SSE (detect changes)
    
    # Dwell time tracking
    _candidate_id: int = 0
    _candidate_start: float = 0.0

    # Lazy-load YOLO detector (heavy; only once per process lifetime)
    if _detector is None:
        _detector = Detector(MODEL_PATH, CONFIDENCE_THRESHOLD)

    _start_time = time.time()

    while _is_running():
        # Block until reader signals a new frame (or stop is requested)
        if not _new_raw_event.wait(timeout=1.0):
            continue
        _new_raw_event.clear()

        if not _is_running():
            break

        # Grab the latest raw frame
        with _raw_frame_lock:
            if _raw_frame is None:
                continue
            frame = _raw_frame.copy()

        t0 = time.perf_counter()

        # ── Inference every Nth frame; reuse last_detections otherwise ──────
        # This avoids bbox flickering on skipped frames.
        if frame_index % max(1, INFERENCE_SKIP_FRAMES) == 0:
            last_detections = _detector.detect(frame)

        latency_ms = int((time.perf_counter() - t0) * 1000)
        fps        = fps_counter.calculate_fps()

        # ── Draw bounding boxes from persisted detections ───────────────────
        h, w      = frame.shape[:2]
        best_id   = -1
        best_conf = 0.0

        for det in last_detections:
            lbl  = det["label"]
            lid  = det["label_id"]
            conf = det["confidence"]
            x1, y1, x2, y2 = det["bbox"]

            # Clamp to frame bounds to prevent out-of-range drawing
            x1 = max(0, min(x1, w - 1))
            y1 = max(0, min(y1, h - 1))
            x2 = max(0, min(x2, w - 1))
            y2 = max(0, min(y2, h - 1))
            if x2 <= x1 or y2 <= y1:
                continue

            # Box
            cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 220, 120), 2)

            # Label tag
            tag_txt = f"{lbl}  {conf:.0%}"
            (tw, th), _ = cv2.getTextSize(
                tag_txt, cv2.FONT_HERSHEY_SIMPLEX, 0.60, 2
            )
            tag_y0 = max(y1 - th - 10, 0)
            tag_y1 = min(tag_y0 + th + 10, h - 1)
            cv2.rectangle(
                frame, (x1, tag_y0), (min(x1 + tw + 8, w - 1), tag_y1),
                (0, 220, 120), cv2.FILLED
            )
            cv2.putText(
                frame, tag_txt,
                (x1 + 4, tag_y1 - 3),
                cv2.FONT_HERSHEY_SIMPLEX, 0.60, (15, 15, 15), 2,
                cv2.LINE_AA
            )

            if conf > best_conf:
                best_conf = conf
                best_id   = lid

        # ── Send label to ESP32-CAM ─────────────────────────────────────────
        if best_id > 0:
            if best_id != _candidate_id:
                # New gesture detected, start timer
                _candidate_id = best_id
                _candidate_start = time.time()
            elif time.time() - _candidate_start >= DETECTION_DWELL_TIME:
                # Gesture has been held long enough, accept it
                label_text = next(
                    (d["label"] for d in last_detections if d["label_id"] == best_id),
                    "",
                )
                with _lock:
                    _state["last_label"]    = label_text
                    _state["last_label_id"] = best_id
                _send_label_to_esp32(best_id)
        else:
            # No detection, reset candidate timer
            _candidate_id = 0

        # ── Update shared state ─────────────────────────────────────────────
        with _lock:
            _state["fps"]        = fps
            _state["latency_ms"] = latency_ms

        # ── Rate-limited SSE push (max 5 Hz) ───────────────────────────────
        _now_t = time.perf_counter()
        if _now_t - _last_sse_t >= 0.2:
            _last_sse_t = _now_t
            cur_label = _state.get("last_label", "")
            payload: dict = {
                "fps":        fps,
                "latency_ms": latency_ms,
                "connected":  True,
            }
            if cur_label != _sse_prev_label:  # only include when changed
                payload["last_label"] = cur_label
                _sse_prev_label = cur_label
            _push_sse("stats", json.dumps(payload))

        # ── Encode and publish annotated frame ──────────────────────────────
        ok, buf = cv2.imencode(
            ".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 78]
        )
        if ok:
            with _output_jpeg_lock:
                _output_jpeg = buf.tobytes()
                _output_seq += 1

        frame_index += 1

    # Clean up
    with _lock:
        _state["streaming"]  = False
        _state["connected"]  = False
        _state["fps"]        = 0
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
    """
    MJPEG stream of annotated frames.
    Uses a lightweight polling loop (5 ms) so multiple browser tabs can
    connect without competing over a single threading.Event.
    """
    def _gen():
        blank      = _make_blank_frame()
        last_seq   = -1

        while True:
            if not _is_running():
                yield _mjpeg_part(blank)
                time.sleep(0.05)
                last_seq = -1
                continue

            with _output_jpeg_lock:
                cur_seq  = _output_seq
                cur_jpeg = _output_jpeg

            if cur_jpeg is not None and cur_seq != last_seq:
                last_seq = cur_seq
                yield _mjpeg_part(cur_jpeg)
            else:
                time.sleep(0.005)   # 5 ms poll — negligible latency overhead

    return Response(
        stream_with_context(_gen()),
        mimetype="multipart/x-mixed-replace; boundary=frame",
        headers={"Cache-Control": "no-store"},
    )


@app.route("/events")
def events():
    """Server-Sent Events stream for live sidebar stats."""
    def _gen():
        yield f"event: init\ndata: {json.dumps(_build_status_json())}\n\n"
        while True:
            try:
                yield _sse_queue.get(timeout=5)
            except queue.Empty:
                yield ": keepalive\n\n"

    return Response(
        stream_with_context(_gen()),
        mimetype="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )


@app.route("/api/start", methods=["POST"])
def api_start():
    global _reader_thread, _inference_thread
    with _lock:
        already = _state["streaming"]
    if already:
        return jsonify({"ok": True, "msg": "already running"})

    # Mark streaming = True *before* starting threads so _is_running() is True
    with _lock:
        _state["streaming"] = True

    _new_raw_event.clear()

    _reader_thread = threading.Thread(
        target=_reader_loop, daemon=True, name="CameraReader"
    )
    _inference_thread = threading.Thread(
        target=_inference_loop, daemon=True, name="Inference"
    )
    _reader_thread.start()
    _inference_thread.start()
    return jsonify({"ok": True, "msg": "started"})


@app.route("/api/stop", methods=["POST"])
def api_stop():
    with _lock:
        _state["streaming"] = False
    _new_raw_event.set()   # unblock reader/inference threads
    return jsonify({"ok": True, "msg": "stopped"})


@app.route("/api/status")
def api_status():
    return jsonify(_build_status_json())


# ─────────────────────────────────────────────────────────────────────────────
# Utilities
# ─────────────────────────────────────────────────────────────────────────────

def _mjpeg_part(jpeg: bytes) -> bytes:
    return b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n"


def _make_blank_frame(w: int = 640, h: int = 480) -> bytes:
    img = np.zeros((h, w, 3), dtype=np.uint8)
    img[:] = (18, 18, 28)
    cv2.putText(
        img, "Stream stopped",
        (w // 2 - 115, h // 2),
        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (70, 70, 90), 2, cv2.LINE_AA,
    )
    _, buf = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 60])
    return buf.tobytes()


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    app.run(host=FLASK_HOST, port=FLASK_PORT, debug=FLASK_DEBUG, threaded=True)