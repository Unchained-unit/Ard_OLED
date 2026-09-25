"""Slow PC-side current feedback; measurements remain unchanged."""
from dataclasses import dataclass
import math
import time

DAC_STEP_V = 5.0 / 4095.0


@dataclass
class CurrentControl:
    target_ma: float
    slope_ma_v: float
    dac_v: float
    tolerance_ma: float
    enabled: bool = True
    monitoring: bool = True
    status: str = "Подстройка"
    error_ma: float | None = None
    settled: int = 0
    started: float = 0.0
    last_millis: int | None = None
    voltage_v: float = 0.0

    def __post_init__(self):
        self.started = time.monotonic()

    def stop(self, reason):
        self.enabled = False
        self.monitoring = False
        self.status = reason

    def update(self, measured_ma, device_millis):
        if not self.enabled:
            return None
        if not math.isfinite(measured_ma):
            raise ValueError("Некорректный ток АЦП")
        if self.last_millis is not None and device_millis <= self.last_millis:
            raise ValueError("Повтор данных или перезапуск платы; примените уставку заново")
        self.last_millis = device_millis
        self.error_ma = self.target_ma - measured_ma
        if abs(self.error_ma) <= self.tolerance_ma:
            self.settled += 1
            self.status = "ЦАП зафиксирован" if self.settled >= 3 else "Проверка стабилизации"
            if self.settled >= 3:
                self.enabled = False
            self.started = time.monotonic()
            return None
        self.settled = 0
        if time.monotonic() - self.started > 120:
            raise ValueError("Ток не достигнут за 120 с: проверьте запас напряжения и схему")
        # Damped integral correction, bounded to 20 mV per fresh measurement.
        delta = max(-0.02, min(0.02, 0.35 * self.error_ma / self.slope_ma_v))
        steps = max(1, round(abs(delta) / DAC_STEP_V))
        candidate = max(0.0, min(5.0, self.dac_v + math.copysign(steps * DAC_STEP_V, delta)))
        if abs(candidate - self.dac_v) < DAC_STEP_V / 2:
            raise ValueError("Достигнут предел ЦАП: заданный ток недостижим")
        self.status = "Подстройка"
        return candidate
