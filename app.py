from flask import Flask, render_template, Response
import cv2

from utils import get_camera, detect_objects, FPS


app = Flask(__name__)

def generate_frames():

    cap = get_camera()

    fps_counter = FPS()

    while True:

        success, frame = cap.read()

        if not success:
            break

        detections = detect_objects(frame)

        for detection in detections:

            label = detection["label"]

            confidence = detection["confidence"]

            x1, y1, x2, y2 = detection["bbox"]

            cv2.rectangle(
                frame,
                (x1, y1),
                (x2, y2),
                (0, 255, 0),
                2
            )

            cv2.putText(
                frame,
                f"{label} {confidence:.2f}",
                (x1, y1 - 10),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.8,
                (0, 255, 0),
                2
            )

        fps = fps_counter.calculate_fps()

        cv2.putText(
            frame,
            f"FPS: {fps}",
            (20, 40),
            cv2.FONT_HERSHEY_SIMPLEX,
            1,
            (255, 0, 0),
            2
        )

        ret, buffer = cv2.imencode('.jpg', frame)

        frame = buffer.tobytes()

        yield (
            b'--frame\r\n'
            b'Content-Type: image/jpeg\r\n\r\n' +
            frame +
            b'\r\n'
        )

@app.route('/')
def index():

    return render_template('index.html')

@app.route('/video_feed')
def video_feed():

    return Response(
        generate_frames(),
        mimetype='multipart/x-mixed-replace; boundary=frame'
    )

if __name__ == '__main__':

    app.run(debug=True)