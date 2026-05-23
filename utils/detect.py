from ultralytics import YOLO
from utils.config import MODEL_PATH


model = YOLO(MODEL_PATH)

def detect_objects(frame):

    results = model(frame)

    detections = []

    for result in results:

        boxes = result.boxes

        for box in boxes:

            class_id = int(box.cls[0])
            confidence = float(box.conf[0])

            label = model.names[class_id]

            x1, y1, x2, y2 = map(int, box.xyxy[0])

            detections.append({
                "label": label,
                "confidence": confidence,
                "bbox": (x1, y1, x2, y2)
            })

    return detections