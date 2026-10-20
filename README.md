<div align="center">

# 🤟 BISINDO Sign Language Recognition System

**Real-time Indonesian Sign Language (BISINDO) detection using YOLOv8 & ESP32-CAM**

[![Python](https://img.shields.io/badge/Python-3.12%2B-blue?logo=python&logoColor=white)](https://www.python.org/)
[![Flask](https://img.shields.io/badge/Flask-3.1.3-black?logo=flask&logoColor=white)](https://flask.palletsprojects.com/)
[![YOLOv8](https://img.shields.io/badge/YOLOv8-Ultralytics-purple?logo=yolo&logoColor=white)](https://ultralytics.com/)
[![PyTorch](https://img.shields.io/badge/PyTorch-2.5.1-EE4C2C?logo=pytorch&logoColor=white)](https://pytorch.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![Platform](https://img.shields.io/badge/Platform-ESP32--CAM-green?logo=espressif&logoColor=white)](https://www.espressif.com/)

<br/>

*Undergraduate Thesis Project — Department of Data Science*

</div>

---

## 📖 Overview

This project is a real-time **Bahasa Isyarat Indonesia (BISINDO)** sign language recognition system that combines:

- 🔬 **YOLOv8** object detection model trained on 56 BISINDO gesture classes
- 📷 **ESP32-CAM** (AI Thinker / OV2640) as a wireless embedded camera module
- 🌐 **Flask** web dashboard for live MJPEG video streaming and detection visualization
- 🖥️ **OLED display** (SH1106G) on the ESP32-CAM device to render recognized gesture labels
- 🔊 **DFPlayer Mini** for audio feedback of recognized signs

The system uses a **dual-thread architecture** on the server side: one thread reads camera frames continuously (without blocking on inference), and a separate thread runs YOLO inference — ensuring smooth, low-latency streaming even on slower hardware.

---

## ✨ Features

| Feature | Description |
|---|---|
| 🏷️ **56 Gesture Classes** | Alphabet A–Z plus 30 common BISINDO words/phrases |
| ⚡ **Real-time Streaming** | MJPEG stream via Flask with Server-Sent Events (SSE) sidebar |
| 🔄 **Dual-thread Architecture** | Decoupled frame reader & inference threads for stutter-free video |
| 📡 **Wireless Camera** | ESP32-CAM streams over Wi-Fi; no USB camera required |
| 🖥️ **Embedded Feedback** | OLED display + DFPlayer Mini audio on the ESP32-CAM device |
| 🔋 **Battery Monitoring** | ADC-based voltage divider reads LiPo battery level on device |
| 🎛️ **Web Dashboard** | Live FPS, latency, session timer, and last detected sign |
| 🔧 **Configurable** | All thresholds and endpoints centralized in `utils/config.py` |

---

## 🏗️ System Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    ESP32-CAM Device                     │
│  ┌──────────┐   ┌──────────┐   ┌──────────────────────┐ │
│  │ OV2640   │──▶│  Wi-Fi   │   │ OLED (SH1106G 128×64)│ │
│  │ Camera   │   │  MJPEG   │   │ DFPlayer Mini (Audio) │ │
│  └──────────┘   └────┬─────┘   └──────────────────────┘ │
└───────────────────────┼─────────────────────────────────┘
                        │ HTTP MJPEG stream (:81)
                        ▼
┌─────────────────────────────────────────────────────────┐
│                  Flask Server (Python)                  │
│  ┌─────────────────┐       ┌──────────────────────────┐ │
│  │  CameraReader   │──────▶│    Inference Thread      │ │
│  │  Thread         │ frame │  (YOLOv8 · Ultralytics)  │ │
│  └─────────────────┘       └────────────┬─────────────┘ │
│                                         │ annotated JPEG │
│  ┌──────────────────────────────────────▼─────────────┐ │
│  │     /video_feed (MJPEG)  │  /events (SSE stats)    │ │
│  └──────────────────────────────────────┬─────────────┘ │
└─────────────────────────────────────────┼───────────────┘
                                          │
                                          ▼
                              ┌─────────────────────┐
                              │   Web Browser        │
                              │  (Live Dashboard)    │
                              └─────────────────────┘
```

---

## 🗂️ Project Structure

```
BISINDO-YOLO-ESP32CAM/
├── app.py                  # Flask application & dual-thread streaming logic
├── requirement.txt         # Python dependencies
├── label_info.txt          # Mapping of class IDs → gesture labels (56 classes)
│
├── model/
│   └── yolo_model.pt       # Trained YOLOv8 model weights
│
├── utils/
│   ├── config.py           # Centralized configuration (URLs, thresholds, ports)
│   ├── detect.py           # Detector class wrapping Ultralytics YOLO
│   ├── camera.py           # OpenCV camera helper
│   ├── fps.py              # EMA-smoothed FPS counter
│   └── __init__.py
│
├── templates/
│   └── index.html          # Web dashboard HTML template
│
├── static/
│   ├── css/style.css       # Dashboard stylesheet
│   └── js/
│       ├── script.js       # Core dashboard JS (stream control, SSE)
│       └── dashboard.js    # UI rendering & stats updates
│
├── backup/
│   └── esp32cam.ino        # Arduino firmware for ESP32-CAM (state machine)
│
└── src/
    └── sound/              # DFPlayer Mini audio assets (MP3)
        ├── 0057_startup.mp3
        ├── 0058_interact1.mp3
        └── ...
```

---

## 🏷️ Recognized Gesture Classes

The model detects **56 gesture classes** from the BISINDO vocabulary:

<details>
<summary>Click to expand full class list</summary>

| ID | Class | ID | Class | ID | Class | ID | Class |
|---|---|---|---|---|---|---|---|
| 1 | A | 15 | O | 29 | cinta | 43 | motor |
| 2 | B | 16 | P | 30 | halo | 44 | nenek |
| 3 | C | 17 | Q | 31 | hati hati | 45 | rumah |
| 4 | D | 18 | R | 32 | ibu | 46 | sahabat |
| 5 | E | 19 | S | 33 | jangan | 47 | sakit |
| 6 | F | 20 | T | 34 | kakak | 48 | saya |
| 7 | G | 21 | U | 35 | kakek | 49 | sedih |
| 8 | H | 22 | V | 36 | kamu | 50 | senang |
| 9 | I | 23 | W | 37 | kerja | 51 | siapa |
| 10 | J | 24 | X | 38 | maaf | 52 | takut |
| 11 | K | 25 | Y | 39 | makan | 53 | terima kasih |
| 12 | L | 26 | Z | 40 | marah | 54 | tidur |
| 13 | M | 27 | apa | 41 | minum | 55 | tolong |
| 14 | N | 28 | ayah | 42 | mobil | 56 | uang |

</details>

---

## 🛠️ Hardware Requirements

| Component | Specification |
|---|---|
| Microcontroller | AI Thinker ESP32-CAM |
| Camera | OV2640 (onboard) |
| Display | SH1106G OLED 128×64, I2C (SDA=GPIO15, SCL=GPIO2) |
| Audio | DFPlayer Mini (UART0: TX=GPIO1, RX=GPIO3) |
| Button | Tactile switch on GPIO13 (Active LOW, INPUT\_PULLUP) |
| Battery | LiPo with voltage divider on GPIO14 (ADC2), R1=R2=100kΩ |

---

## 💻 Software Requirements

- Python **3.12+**
- CUDA-capable GPU *(recommended for real-time inference; CPU mode also supported)*
- Arduino IDE with ESP32 board support (for flashing firmware)

---

## 🚀 Getting Started

### 1. Clone the Repository

```bash
git clone https://github.com/Daffin-TW/BISINDO-YOLO-ESP32CAM.git
cd BISINDO-YOLO-ESP32CAM
```

### 2. Install Python Dependencies

```bash
pip install -r requirement.txt
```

### 3. Configure the System

Edit [`utils/config.py`](./utils/config.py) to match your network setup:

```python
# IP address of your ESP32-CAM on the local network
ESP32_BASE_URL = "http://<YOUR_ESP32_IP>"

# Detection confidence threshold (0.0 – 1.0)
CONFIDENCE_THRESHOLD = 0.5

# Minimum seconds a gesture must be held before it is sent
DETECTION_DWELL_TIME = 0.1

# Minimum seconds between successive label POSTs to the ESP32-CAM
LABEL_SEND_COOLDOWN = 1.0
```

### 4. Flash the ESP32-CAM Firmware

1. Open [`backup/esp32cam.ino`](./backup/esp32cam.ino) in Arduino IDE.
2. Install required libraries:
   - `Adafruit GFX Library`
   - `Adafruit SH110X`
   - `DFRobotDFPlayerMini`
3. Set your Wi-Fi credentials in the sketch.
4. Select board: **AI Thinker ESP32-CAM** and flash.

### 5. Run the Flask Server

```bash
python app.py
```

Open your browser and navigate to **[http://localhost:5000](http://localhost:5000)**.

---

## 🌐 API Endpoints

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/` | Web dashboard |
| `GET` | `/video_feed` | MJPEG live stream |
| `GET` | `/events` | Server-Sent Events (SSE) for live stats |
| `POST` | `/api/start` | Start streaming & inference |
| `POST` | `/api/stop` | Stop streaming & inference |
| `GET` | `/api/status` | Current system status (JSON) |

---

## ⚙️ Configuration Reference

All tuneable parameters are in [`utils/config.py`](./utils/config.py):

| Parameter | Default | Description |
|---|---|---|
| `ESP32_BASE_URL` | `http://10.229.248.19` | ESP32-CAM base HTTP address |
| `ESP32_STREAM_URL` | `{BASE_URL}:81` | MJPEG stream endpoint |
| `MODEL_PATH` | `model/yolo_model.pt` | Path to YOLO weights |
| `CONFIDENCE_THRESHOLD` | `0.5` | Minimum detection confidence |
| `INFERENCE_SKIP_FRAMES` | `1` | Run inference every N frames |
| `LABEL_SEND_COOLDOWN` | `1.0 s` | Rate-limit for ESP32 label POSTs |
| `DETECTION_DWELL_TIME` | `0.1 s` | Gesture hold time before trigger |
| `FLASK_HOST` | `0.0.0.0` | Flask server bind address |
| `FLASK_PORT` | `5000` | Flask server port |

---

## 📦 Dependencies

```
flask==3.1.3
torch==2.5.1
torchvision==0.20.1
ultralytics==8.4.51
opencv-python==4.13.0.92
numpy==2.4.5
requests>=2.31.0
```

---

## 📄 License

This project is licensed under the **MIT License** — see the [LICENSE](./LICENSE) file for details.

---

## 🙏 Acknowledgements

- [Ultralytics YOLOv8](https://github.com/ultralytics/ultralytics) — Object detection framework
- [ESP32-CAM AI Thinker](https://github.com/espressif/esp32-camera) — Embedded camera platform
- [Flask](https://flask.palletsprojects.com/) — Lightweight Python web framework
- BISINDO community and Tuli Indonesia — For the sign language reference corpus

---

<div align="center">

*Made with ❤️ as an Undergraduate Thesis Project*

</div>
