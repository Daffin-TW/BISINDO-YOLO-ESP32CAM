import time


class FPS:
    """
    Rolling FPS counter with exponential moving average (EMA) smoothing.

    ``smoothing`` controls how aggressively transient spikes are suppressed.
    A value of 0.93 means each new sample contributes only 7 % to the
    running average, keeping the display stable even when individual frame
    times vary by 10×.
    """

    def __init__(self, smoothing: float = 0.93) -> None:
        self._prev: float | None = None
        self._ema: float = 0.0
        self._alpha: float = 1.0 - smoothing   # weight for the newest sample

    def calculate_fps(self) -> int:
        now = time.perf_counter()
        if self._prev is None:
            self._prev = now
            return 0
        delta = now - self._prev
        self._prev = now
        if delta <= 0:
            return 0
        instant = 1.0 / delta
        # Seed EMA on first real measurement; blend thereafter
        self._ema = instant if self._ema == 0.0 else (
            self._alpha * instant + (1.0 - self._alpha) * self._ema
        )
        return max(0, int(round(self._ema)))