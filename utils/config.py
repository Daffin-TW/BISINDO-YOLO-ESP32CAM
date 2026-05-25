# =============================================================================
#  Configuration  –  edit these values to customise the dashboard behaviour
# =============================================================================

# ── ESP32-CAM endpoints ───────────────────────────────────────────────────────
# Base HTTP address of the ESP32-CAM control server (port 80)
ESP32_BASE_URL = "http://10.149.241.19"

# Raw MJPEG stream served by the ESP32-CAM on port 81
ESP32_STREAM_URL = f"{ESP32_BASE_URL}:81"

# ── YOLO model ────────────────────────────────────────────────────────────────
MODEL_PATH = "model/yolo_model.pt"

# Minimum confidence to accept a detection (0.0 – 1.0)
CONFIDENCE_THRESHOLD = 0.5

# ── Performance tuning ────────────────────────────────────────────────────────
# Run inference only on every Nth frame (1 = every frame, 2 = every other, …)
INFERENCE_SKIP_FRAMES = 3

# Minimum seconds between successive label POSTs to the ESP32-CAM
# (prevents flooding when the same sign stays in frame)
LABEL_SEND_COOLDOWN = 3.0

# ── Flask server ──────────────────────────────────────────────────────────────
FLASK_HOST  = "0.0.0.0"
FLASK_PORT  = 5000
FLASK_DEBUG = False