import cv2
from utils.config import ESP32_STREAM_URL


def get_camera():

    cap = cv2.VideoCapture(ESP32_STREAM_URL)
    # cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    
    if not cap.isOpened():
        print("Error: Could not open the camera or stream.")
    else:
        print("Stream status: Connected.")

    return cap