import time


class FPS:
    """Rolling instantaneous FPS counter.

    On the very first call to calculate_fps() the result is 0 (no previous
    frame exists yet).  All subsequent calls return the inter-frame rate.
    """

    def __init__(self) -> None:
        self._prev: float | None = None   # None until the first frame

    def calculate_fps(self) -> int:
        now = time.perf_counter()
        if self._prev is None:
            self._prev = now
            return 0                      # no previous frame – return 0
        delta = now - self._prev
        self._prev = now
        if delta <= 0:
            return 0
        return int(1.0 / delta)