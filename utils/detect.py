from ultralytics import YOLO
from utils.config import MODEL_PATH, CONFIDENCE_THRESHOLD


class Detector:
    """Thin wrapper around a YOLO model. Instantiated once and reused."""

    def __init__(self, model_path: str = MODEL_PATH,
                 confidence: float = CONFIDENCE_THRESHOLD) -> None:
        self._model = YOLO(model_path)
        self._conf  = confidence

    def detect(self, frame) -> list[dict]:
        """
        Run inference on *frame* and return a list of detections.

        Each detection dict contains:
          label      (str)   – human-readable class name
          label_id   (int)   – 1-based id matching label_info.txt / ESP32 table
          confidence (float) – 0.0 – 1.0
          bbox       (tuple) – (x1, y1, x2, y2) in pixels
        """
        results = self._model(frame, conf=self._conf, verbose=False)
        detections = []
        for result in results:
            for box in result.boxes:
                class_id   = int(box.cls[0])
                confidence = float(box.conf[0])
                label      = self._model.names[class_id]
                x1, y1, x2, y2 = map(int, box.xyxy[0])
                detections.append({
                    "label":      label,
                    "label_id":   class_id + 1,   # YOLO classes are 0-based; ESP32 table is 1-based
                    "confidence": confidence,
                    "bbox":       (x1, y1, x2, y2),
                })
        return detections


# ── Legacy function kept for backward compatibility ───────────────────────────
_default_detector: Detector | None = None

def detect_objects(frame) -> list[dict]:
    global _default_detector
    if _default_detector is None:
        _default_detector = Detector()
    return _default_detector.detect(frame)