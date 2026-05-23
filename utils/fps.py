import time


class FPS:

    def __init__(self):

        self.prev_time = 0

    def calculate_fps(self):

        current_time = time.time()

        fps = 1 / (current_time - self.prev_time)

        self.prev_time = current_time

        return int(fps)