import asyncio
import copy
import ctypes
import csv
import io
import json
import math
import os
import socket
import sys
import threading
import time
import webbrowser
import zipfile
from collections import deque
from datetime import datetime
from pathlib import Path
from typing import Any

if sys.stdout is None:
    sys.stdout = open(os.devnull, "w", encoding="utf-8")
if sys.stderr is None:
    sys.stderr = open(os.devnull, "w", encoding="utf-8")

if sys.platform == "win32" and getattr(sys, "frozen", False) and os.environ.get("NMSE_SHOW_CONSOLE") != "1":
    console_window = ctypes.windll.kernel32.GetConsoleWindow()
    if console_window:
        ctypes.windll.user32.ShowWindow(console_window, 0)

import serial
import uvicorn
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
from serial.tools import list_ports


APP_NAME = "NMSE OLEDer RS-485"
DATA_DIR = Path.home() / ".nmse_oleder_rs485"
CONFIG_FILE = DATA_DIR / "config.json"
CONFIG_BACKUP_FILE = DATA_DIR / "config.backup.json"
RECORDINGS_DIR = DATA_DIR / "recordings"
SAMPLES_PER_BOARD = 4
DEFAULT_BOARD_IDS = [1, 2]
DEFAULT_SHUNT_OHM = 25.0
DEFAULT_CURRENT_GAIN = 10.0
DEFAULT_PHOTO_ADC_GAIN = "2/3"
DEFAULT_CURRENT_ADC_GAIN = "2/3"

ADS_GAIN_OPTIONS = {
    "2/3": {"label": "2/3x · до 6.144 В · 187.5 мкВ/бит", "full_scale_v": 6.144, "lsb_uv": 187.5},
    "1": {"label": "1x · до 4.096 В · 125 мкВ/бит", "full_scale_v": 4.096, "lsb_uv": 125.0},
    "2": {"label": "2x · до 2.048 В · 62.5 мкВ/бит", "full_scale_v": 2.048, "lsb_uv": 62.5},
    "4": {"label": "4x · до 1.024 В · 31.25 мкВ/бит", "full_scale_v": 1.024, "lsb_uv": 31.25},
    "8": {"label": "8x · до 0.512 В · 15.625 мкВ/бит", "full_scale_v": 0.512, "lsb_uv": 15.625},
    "16": {"label": "16x · до 0.256 В · 7.8125 мкВ/бит", "full_scale_v": 0.256, "lsb_uv": 7.8125},
}

NUMERIC_CALIBRATION_FIELDS = [
    "voltage_dac_scale",
    "voltage_output_correction",
    "current_dac_scale",
    "current_output_correction",
    "voltage_measure_scale",
    "voltage_measure_offset",
    "current_measure_scale",
    "current_measure_offset",
    "voltage_device_scale",
    "voltage_device_offset",
    "current_device_scale",
    "current_device_offset",
    "current_offset_u1_v",
    "current_offset_i1_ma",
    "current_offset_u2_v",
    "current_offset_i2_ma",
    "photo_measure_scale",
    "photo_measure_offset",
]


def resource_path(name: str) -> Path:
    base = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    return base / name


STATIC_DIR = resource_path("static")


def current_scale_from_hardware(shunt_ohm: float, gain: float) -> float:
    return 1000.0 / (shunt_ohm * gain)


def current_offset_from_voltage(calibration: dict[str, Any], voltage_v: float) -> float:
    """Return the current zero offset in mA for the measured sample voltage.

    The user specifies two calibration points. Between the points the offset is
    linearly interpolated. Outside the interval the nearest endpoint is used;
    this avoids dangerous extrapolation if the sample voltage leaves the
    calibration range.
    """
    u1 = float(calibration.get("current_offset_u1_v", 0.0))
    i1 = float(calibration.get("current_offset_i1_ma", 0.0))
    u2 = float(calibration.get("current_offset_u2_v", 12.0))
    i2 = float(calibration.get("current_offset_i2_ma", 0.0))
    if not all(math.isfinite(value) for value in (u1, i1, u2, i2, voltage_v)):
        return 0.0
    if u2 <= u1:
        return i1
    clamped_voltage = min(max(voltage_v, u1), u2)
    position = (clamped_voltage - u1) / (u2 - u1)
    return i1 + (i2 - i1) * position


def downsample_recording_points(points: list[dict[str, Any]], max_points: int) -> list[dict[str, Any]]:
    """Reduce a long display series while preserving its first and last point."""
    if max_points < 2 or len(points) <= max_points:
        return points
    step = (len(points) - 1) / (max_points - 1)
    indexes = [round(index * step) for index in range(max_points)]
    return [points[index] for index in indexes]


def clean_photo_adc_gain(value: Any) -> str:
    text = str(value).strip().upper().replace("X", "")
    aliases = {
        "0": "2/3",
        "2/3": "2/3",
        "TWOTHIRDS": "2/3",
        "TWO_THIRDS": "2/3",
        "6.144": "2/3",
        "6144": "2/3",
        "GAIN_TWOTHIRDS": "2/3",
        "1": "1",
        "ONE": "1",
        "4.096": "1",
        "4096": "1",
        "GAIN_ONE": "1",
        "2": "2",
        "TWO": "2",
        "2.048": "2",
        "2048": "2",
        "GAIN_TWO": "2",
        "4": "4",
        "FOUR": "4",
        "1.024": "4",
        "1024": "4",
        "GAIN_FOUR": "4",
        "8": "8",
        "EIGHT": "8",
        "0.512": "8",
        "512": "8",
        "GAIN_EIGHT": "8",
        "16": "16",
        "SIXTEEN": "16",
        "0.256": "16",
        "256": "16",
        "GAIN_SIXTEEN": "16",
    }
    result = aliases.get(text)
    if result not in ADS_GAIN_OPTIONS:
        raise ValueError("Неверный gain яркости. Допустимо: 2/3, 1, 2, 4, 8, 16")
    return result


def clean_current_adc_gain(value: Any) -> str:
    try:
        return clean_photo_adc_gain(value)
    except ValueError as exc:
        raise ValueError("Неверный gain измерения тока. Допустимо: 2/3, 1, 2, 4, 8, 16") from exc


def default_sample_calibration(current_scale: float):
    return {
        "voltage_dac_scale": 3.0,
        "voltage_output_correction": 0.95,
        "current_dac_scale": current_scale,
        "current_output_correction": 1.0,
        "voltage_measure_scale": 3.0,
        "voltage_measure_offset": 0.0,
        "current_measure_scale": current_scale,
        "current_measure_offset": 0.0,
        # New firmware sends ready physical values Uapprox_V and Iapprox_mA.
        # These four values are optional final corrections for those values.
        # Keeping them separate from *_measure_* prevents a second raw-to-
        # physical conversion in the PC application.
        "voltage_device_scale": 1.0,
        "voltage_device_offset": 0.0,
        "current_device_scale": 1.0,
        "current_device_offset": 0.0,
        # Voltage-dependent current zero offset. The interpolated value is
        # subtracted from the current sent by Arduino.
        "current_offset_u1_v": 0.0,
        "current_offset_i1_ma": 0.0,
        "current_offset_u2_v": 12.0,
        "current_offset_i2_ma": 0.0,
        "photo_measure_scale": 1.0,
        "photo_measure_offset": 0.0,
        "photo_adc_gain": DEFAULT_PHOTO_ADC_GAIN,
        "current_adc_gain": DEFAULT_CURRENT_ADC_GAIN,
    }


def default_config():
    scale = current_scale_from_hardware(DEFAULT_SHUNT_OHM, DEFAULT_CURRENT_GAIN)
    return {
        "schema_version": 4,
        "board_ids": DEFAULT_BOARD_IDS[:],
        "hardware": {"shunt_ohm": DEFAULT_SHUNT_OHM, "current_gain": DEFAULT_CURRENT_GAIN},
        "calibration": {
            str(board_id): [default_sample_calibration(scale) for _ in range(SAMPLES_PER_BOARD)]
            for board_id in DEFAULT_BOARD_IDS
        },
    }


def clean_board_ids(values):
    ids = []
    for value in values:
        board_id = int(value)
        if not 1 <= board_id <= 250:
            raise ValueError("Адреса плат должны быть от 1 до 250")
        if board_id not in ids:
            ids.append(board_id)
    if not ids:
        raise ValueError("Нужен хотя бы один адрес платы")
    return ids


def ensure_config_shape(config):
    shunt = float(config.get("hardware", {}).get("shunt_ohm", DEFAULT_SHUNT_OHM))
    gain = float(config.get("hardware", {}).get("current_gain", DEFAULT_CURRENT_GAIN))
    if not math.isfinite(shunt) or shunt <= 0:
        shunt = DEFAULT_SHUNT_OHM
    if not math.isfinite(gain) or gain <= 0:
        gain = DEFAULT_CURRENT_GAIN
    scale = current_scale_from_hardware(shunt, gain)
    board_ids = clean_board_ids(config.get("board_ids", DEFAULT_BOARD_IDS))
    calibration = config.get("calibration", {})
    result = {
        "schema_version": 4,
        "board_ids": board_ids,
        "hardware": {"shunt_ohm": shunt, "current_gain": gain},
        "calibration": {},
    }

    # Calibration is a persistent library keyed by the RS-485 address. Keep
    # inactive boards too: removing an address from board_ids must not erase
    # the calibration that belongs to that physical board.
    calibration_board_ids = []
    if isinstance(calibration, dict):
        for raw_board_id in calibration:
            try:
                board_id = int(raw_board_id)
            except (TypeError, ValueError):
                continue
            if 1 <= board_id <= 250 and board_id not in calibration_board_ids:
                calibration_board_ids.append(board_id)
    for board_id in board_ids:
        if board_id not in calibration_board_ids:
            calibration_board_ids.append(board_id)

    for board_id in calibration_board_ids:
        existing = calibration.get(str(board_id), calibration.get(board_id, [])) if isinstance(calibration, dict) else []
        if not isinstance(existing, list):
            existing = []
        samples = []
        for index in range(SAMPLES_PER_BOARD):
            sample = default_sample_calibration(scale)
            if index < len(existing) and isinstance(existing[index], dict):
                sample.update(existing[index])
            try:
                sample["photo_adc_gain"] = clean_photo_adc_gain(sample.get("photo_adc_gain", DEFAULT_PHOTO_ADC_GAIN))
            except ValueError:
                sample["photo_adc_gain"] = DEFAULT_PHOTO_ADC_GAIN
            try:
                sample["current_adc_gain"] = clean_current_adc_gain(sample.get("current_adc_gain", DEFAULT_CURRENT_ADC_GAIN))
            except ValueError:
                sample["current_adc_gain"] = DEFAULT_CURRENT_ADC_GAIN
            samples.append(sample)
        result["calibration"][str(board_id)] = samples
    return result


def load_config():
    for path in (CONFIG_FILE, CONFIG_BACKUP_FILE):
        try:
            return ensure_config_shape(json.loads(path.read_text(encoding="utf-8")))
        except Exception:
            continue
    return default_config()


class ConnectRequest(BaseModel):
    port: str
    board_ids: list[int] | None = None


class ConfigRequest(BaseModel):
    board_ids: list[int]
    shunt_ohm: float = DEFAULT_SHUNT_OHM
    current_gain: float = DEFAULT_CURRENT_GAIN


class SetRequest(BaseModel):
    board: int
    sample: int
    voltage: float
    current: float
    current_limit: float


class PhotoGainRequest(BaseModel):
    board: int
    sample: int
    gain: str


class CurrentAdcGainRequest(BaseModel):
    board: int
    sample: int
    gain: str


class CalibrationRequest(BaseModel):
    calibration: dict[str, list[dict[str, Any]]]


class CalibrationImportRequest(BaseModel):
    calibration: dict[str, list[dict[str, Any]]]


class RecordingRequest(BaseModel):
    interval: float = 10.0


class SerialBus:
    def __init__(self):
        self.ser = None
        self.port = ""
        self.lock = threading.RLock()
        self.logs = deque(maxlen=400)

    @property
    def connected(self):
        return bool(self.ser and self.ser.is_open)

    def log(self, direction, text):
        self.logs.append({"time": datetime.now().strftime("%H:%M:%S"), "direction": direction, "text": str(text)})

    def connect(self, port):
        with self.lock:
            self.disconnect()
            self.ser = serial.Serial(port, 9600, timeout=0.10, write_timeout=1)
            self.port = port
            time.sleep(0.4)
            self.ser.reset_input_buffer()
            self.log("info", f"connected {port}")

    def disconnect(self):
        with self.lock:
            if self.ser:
                try:
                    self.ser.close()
                except Exception:
                    pass
            self.ser = None
            self.port = ""

    def transact(self, board_id: int, command: str, timeout=1.2, until_end=False):
        if not self.connected:
            raise RuntimeError("COM-порт не подключён")
        full_command = f"{board_id},{command}"
        prefix = f"{board_id},"
        with self.lock:
            self.log("tx", full_command)
            self.ser.write((full_command + "\n").encode("ascii"))
            self.ser.flush()
            deadline = time.monotonic() + timeout
            lines = []
            while time.monotonic() < deadline:
                raw = self.ser.readline()
                if not raw:
                    continue
                line = raw.decode("ascii", errors="replace").strip()
                if not line:
                    continue
                self.log("rx", line)
                if line.startswith(prefix):
                    lines.append(line)
                    if until_end and line == f"{board_id},END":
                        break
                else:
                    self.log("skip", line)
            return lines


class StationState:
    def __init__(self):
        self.bus = SerialBus()
        self.config = load_config()
        self.board_status = {board_id: {"online": False, "status": "", "last_seen": None} for board_id in self.board_ids}
        self.latest = {}
        self.history = {}
        self.current_limits = {}
        self.running = True
        self.live = True
        self.recording_active = False
        self.recording_cancel = threading.Event()
        self.recording_interval = 10.0
        self.recording_started_at = None
        self.recording_dir = None
        self.recording_counts = {}
        self.recording_message = "Запись не запущена"
        self.started = time.monotonic()
        self.ensure_runtime_maps()
        self.worker = threading.Thread(target=self._poll_worker, daemon=True)
        self.worker.start()

    @property
    def board_ids(self):
        return list(self.config["board_ids"])

    def ensure_runtime_maps(self):
        for board_id in self.board_ids:
            self.board_status.setdefault(board_id, {"online": False, "status": "", "last_seen": None})
            for sample in range(1, SAMPLES_PER_BOARD + 1):
                key = self.key(board_id, sample)
                self.latest.setdefault(key, None)
                self.history.setdefault(key, deque(maxlen=1200))
                self.current_limits.setdefault(key, self.measurable_current_max(board_id, sample))
                self.recording_counts.setdefault(key, 0)

    def key(self, board_id, sample):
        return f"{board_id}:{sample}"

    def calibration_for(self, board_id, sample):
        board_key = str(board_id)
        if board_key not in self.config["calibration"]:
            self.config = ensure_config_shape(self.config)
        return self.config["calibration"][board_key][sample - 1]

    def measurable_current_max(self, board_id, sample):
        c = self.calibration_for(board_id, sample)
        gain = clean_current_adc_gain(c.get("current_adc_gain", DEFAULT_CURRENT_ADC_GAIN))
        # ADS1115 PGA may advertise 6.144 V, but a single-ended input must not
        # exceed the ADC supply. The board uses 5 V, therefore cap at 5 V.
        adc_limit_v = min(5.0, ADS_GAIN_OPTIONS[gain]["full_scale_v"])
        return max(0.0, adc_limit_v * c["current_measure_scale"] + c["current_measure_offset"])

    def save_config(self):
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        self.config = ensure_config_shape(self.config)
        payload = json.dumps(self.config, ensure_ascii=False, indent=2)
        for path in (CONFIG_FILE, CONFIG_BACKUP_FILE):
            temporary = path.with_suffix(path.suffix + ".tmp")
            temporary.write_text(payload, encoding="utf-8")
            os.replace(temporary, path)

    def apply_config(self, request: ConfigRequest):
        board_ids = clean_board_ids(request.board_ids)
        shunt = float(request.shunt_ohm)
        gain = float(request.current_gain)
        if not math.isfinite(shunt) or shunt <= 0:
            raise ValueError("Шунт должен быть больше нуля")
        if not math.isfinite(gain) or gain <= 0:
            raise ValueError("Усиление тока должно быть больше нуля")
        scale = current_scale_from_hardware(shunt, gain)
        old = ensure_config_shape(self.config)
        new_config = {
            "schema_version": 4,
            "board_ids": board_ids,
            "hardware": {"shunt_ohm": shunt, "current_gain": gain},
            "calibration": copy.deepcopy(old.get("calibration", {})),
        }
        for board_id in board_ids:
            samples = []
            old_samples = new_config["calibration"].get(str(board_id), [])
            for index in range(SAMPLES_PER_BOARD):
                sample = default_sample_calibration(scale)
                if index < len(old_samples):
                    sample.update(old_samples[index])
                sample["current_dac_scale"] = scale
                sample["current_measure_scale"] = scale
                try:
                    sample["photo_adc_gain"] = clean_photo_adc_gain(sample.get("photo_adc_gain", DEFAULT_PHOTO_ADC_GAIN))
                except ValueError:
                    sample["photo_adc_gain"] = DEFAULT_PHOTO_ADC_GAIN
                try:
                    sample["current_adc_gain"] = clean_current_adc_gain(sample.get("current_adc_gain", DEFAULT_CURRENT_ADC_GAIN))
                except ValueError:
                    sample["current_adc_gain"] = DEFAULT_CURRENT_ADC_GAIN
                samples.append(sample)
            new_config["calibration"][str(board_id)] = samples
        self.config = new_config
        self.board_status = {board_id: self.board_status.get(board_id, {"online": False, "status": "", "last_seen": None}) for board_id in board_ids}
        self.ensure_runtime_maps()
        self.save_config()

    def apply_calibration(self, request: CalibrationRequest):
        config = ensure_config_shape(self.config)
        incoming = request.calibration or {}
        for board_id in config["board_ids"]:
            board_key = str(board_id)
            incoming_samples = incoming.get(board_key, [])
            if not isinstance(incoming_samples, list):
                continue
            for index in range(min(SAMPLES_PER_BOARD, len(incoming_samples))):
                incoming_sample = incoming_samples[index]
                if not isinstance(incoming_sample, dict):
                    continue
                sample = config["calibration"][board_key][index]
                for field in NUMERIC_CALIBRATION_FIELDS:
                    if field not in incoming_sample:
                        continue
                    value = float(incoming_sample[field])
                    if not math.isfinite(value):
                        raise ValueError(f"Некорректное значение {field} для платы {board_id}, образец {index + 1}")
                    sample[field] = value
                if "photo_adc_gain" in incoming_sample:
                    sample["photo_adc_gain"] = clean_photo_adc_gain(incoming_sample["photo_adc_gain"])
                if "current_adc_gain" in incoming_sample:
                    sample["current_adc_gain"] = clean_current_adc_gain(incoming_sample["current_adc_gain"])
                if sample["current_offset_u2_v"] <= sample["current_offset_u1_v"]:
                    raise ValueError(
                        f"Для платы {board_id}, образец {index + 1}: "
                        "напряжение U2 токового offset должно быть больше U1"
                    )
        self.config = config
        self.ensure_runtime_maps()
        self.save_config()
        self.apply_all_photo_gains()
        self.apply_all_current_adc_gains()

    def import_calibration(self, request: CalibrationImportRequest):
        config = ensure_config_shape(self.config)
        incoming = request.calibration or {}
        scale = current_scale_from_hardware(
            config["hardware"]["shunt_ohm"], config["hardware"]["current_gain"]
        )
        imported_boards = []
        for raw_board_id, incoming_samples in incoming.items():
            try:
                board_id = int(raw_board_id)
            except (TypeError, ValueError) as exc:
                raise ValueError(f"Некорректный адрес платы: {raw_board_id}") from exc
            if not 1 <= board_id <= 250:
                raise ValueError(f"Адрес платы {board_id} должен быть от 1 до 250")
            if not isinstance(incoming_samples, list):
                raise ValueError(f"Калибровка платы {board_id} должна быть списком каналов")
            board_key = str(board_id)
            samples = config["calibration"].get(
                board_key, [default_sample_calibration(scale) for _ in range(SAMPLES_PER_BOARD)]
            )
            for index in range(min(SAMPLES_PER_BOARD, len(incoming_samples))):
                incoming_sample = incoming_samples[index]
                if not isinstance(incoming_sample, dict):
                    raise ValueError(f"Плата {board_id}, канал {index + 1}: ожидался объект")
                sample = samples[index]
                for field in NUMERIC_CALIBRATION_FIELDS:
                    if field not in incoming_sample:
                        continue
                    value = float(incoming_sample[field])
                    if not math.isfinite(value):
                        raise ValueError(f"Некорректное значение {field} для платы {board_id}, канал {index + 1}")
                    sample[field] = value
                if "photo_adc_gain" in incoming_sample:
                    sample["photo_adc_gain"] = clean_photo_adc_gain(incoming_sample["photo_adc_gain"])
                if "current_adc_gain" in incoming_sample:
                    sample["current_adc_gain"] = clean_current_adc_gain(incoming_sample["current_adc_gain"])
                if sample["current_offset_u2_v"] <= sample["current_offset_u1_v"]:
                    raise ValueError(
                        f"Для платы {board_id}, канал {index + 1}: "
                        "напряжение U2 токового offset должно быть больше U1"
                    )
            config["calibration"][board_key] = samples
            imported_boards.append(board_id)
        if not imported_boards:
            raise ValueError("Файл не содержит калибровок плат")
        self.config = config
        self.save_config()
        self.apply_all_photo_gains()
        self.apply_all_current_adc_gains()
        return imported_boards

    def connect(self, port, board_ids=None):
        if board_ids:
            hardware = self.config["hardware"]
            self.apply_config(ConfigRequest(board_ids=board_ids, shunt_ohm=hardware["shunt_ohm"], current_gain=hardware["current_gain"]))
        self.bus.connect(port)
        self.scan_boards()
        self.apply_all_photo_gains()
        self.apply_all_current_adc_gains()

    def scan_boards(self):
        for board_id in self.board_ids:
            try:
                lines = self.bus.transact(board_id, "PING", timeout=0.8)
                ok = any(line == f"{board_id},OK,PONG" for line in lines)
                self.board_status[board_id] = {
                    "online": ok,
                    "status": "OK,PONG" if ok else "нет ответа",
                    "last_seen": datetime.now().strftime("%H:%M:%S") if ok else self.board_status.get(board_id, {}).get("last_seen"),
                }
            except Exception as exc:
                self.board_status[board_id] = {"online": False, "status": str(exc), "last_seen": None}

    def status_board(self, board_id):
        lines = self.bus.transact(board_id, "STATUS", timeout=1.0)
        status = next((line for line in lines if line.startswith(f"{board_id},STATUS,")), None)
        self.board_status[board_id] = {
            "online": bool(status),
            "status": status.split(",", 1)[1] if status else "нет STATUS",
            "last_seen": datetime.now().strftime("%H:%M:%S") if status else self.board_status.get(board_id, {}).get("last_seen"),
        }
        return lines

    def send_photo_gain(self, board_id, sample, gain):
        return self.bus.transact(board_id, f"PHGAIN,{sample},{gain}", timeout=1.0)

    def set_photo_gain(self, req: PhotoGainRequest):
        if req.board not in self.board_ids:
            raise ValueError("Неизвестная плата")
        if not 1 <= req.sample <= SAMPLES_PER_BOARD:
            raise ValueError("Неверный номер образца")
        gain = clean_photo_adc_gain(req.gain)
        reply = []
        if self.bus.connected:
            reply = self.send_photo_gain(req.board, req.sample, gain)
            ok = any(line.startswith(f"{req.board},OK,PHGAIN,{req.sample},") for line in reply)
            if not ok:
                raise RuntimeError("Плата не подтвердила смену gain яркости. Проверьте, что загружена свежая прошивка")
        self.config["calibration"][str(req.board)][req.sample - 1]["photo_adc_gain"] = gain
        self.save_config()
        return reply

    def apply_all_photo_gains(self):
        if not self.bus.connected:
            return
        for board_id in self.board_ids:
            for sample in range(1, SAMPLES_PER_BOARD + 1):
                gain = self.calibration_for(board_id, sample).get("photo_adc_gain", DEFAULT_PHOTO_ADC_GAIN)
                try:
                    self.send_photo_gain(board_id, sample, gain)
                except Exception as exc:
                    self.bus.log("err", f"photo gain board {board_id} sample {sample}: {exc}")

    def send_current_adc_gain(self, board_id, sample, gain):
        return self.bus.transact(board_id, f"ICGAIN,{sample},{gain}", timeout=1.0)

    def set_current_adc_gain(self, req: CurrentAdcGainRequest):
        if req.board not in self.board_ids:
            raise ValueError("Неизвестная плата")
        if not 1 <= req.sample <= SAMPLES_PER_BOARD:
            raise ValueError("Неверный номер образца")
        gain = clean_current_adc_gain(req.gain)
        reply = []
        if self.bus.connected:
            reply = self.send_current_adc_gain(req.board, req.sample, gain)
            ok = any(line.startswith(f"{req.board},OK,ICGAIN,{req.sample},") for line in reply)
            if not ok:
                raise RuntimeError("Плата не подтвердила смену gain измерения тока. Загрузите свежую прошивку Arduino")
        self.config["calibration"][str(req.board)][req.sample - 1]["current_adc_gain"] = gain
        key = self.key(req.board, req.sample)
        self.current_limits[key] = min(
            self.current_limits.get(key, self.measurable_current_max(req.board, req.sample)),
            self.measurable_current_max(req.board, req.sample),
        )
        self.save_config()
        return reply

    def apply_all_current_adc_gains(self):
        if not self.bus.connected:
            return
        for board_id in self.board_ids:
            for sample in range(1, SAMPLES_PER_BOARD + 1):
                gain = self.calibration_for(board_id, sample).get("current_adc_gain", DEFAULT_CURRENT_ADC_GAIN)
                try:
                    self.send_current_adc_gain(board_id, sample, gain)
                except Exception as exc:
                    self.bus.log("err", f"current ADC gain board {board_id} sample {sample}: {exc}")

    def dac_values(self, board_id, sample, voltage, current):
        c = self.calibration_for(board_id, sample)
        u = voltage / (c["voltage_dac_scale"] * c["voltage_output_correction"])
        i = current / (c["current_dac_scale"] * c["current_output_correction"])
        if not math.isfinite(u) or not 0 <= u <= 5:
            raise ValueError(f"Плата {board_id}, образец {sample}: U DAC={u:.3f} В вне диапазона 0…5 В")
        if not math.isfinite(i) or not 0 <= i <= 5:
            max_current = 5 * c["current_dac_scale"] * c["current_output_correction"]
            raise ValueError(f"Плата {board_id}, образец {sample}: максимум уставки тока {max_current:.3f} мА")
        return u, i

    def set_sample(self, req: SetRequest):
        if req.board not in self.board_ids:
            raise ValueError("Неизвестная плата")
        if not 1 <= req.sample <= SAMPLES_PER_BOARD:
            raise ValueError("Неверный номер образца")
        values = (req.voltage, req.current, req.current_limit)
        if not all(math.isfinite(value) and value >= 0 for value in values):
            raise ValueError("Уставки и лимит должны быть неотрицательными числами")
        max_measured = self.measurable_current_max(req.board, req.sample)
        if req.current_limit <= 0 or req.current_limit > max_measured:
            raise ValueError(f"Аварийный лимит должен быть от 0 до {max_measured:.3f} мА")
        if req.current > req.current_limit:
            raise ValueError("Уставка тока выше аварийного лимита")
        u_dac, i_dac = self.dac_values(req.board, req.sample, req.voltage, req.current)
        self.current_limits[self.key(req.board, req.sample)] = req.current_limit
        return self.bus.transact(req.board, f"SET,{req.sample},{u_dac:.6f},{i_dac:.6f}", timeout=1.2)

    def zero_board(self, board_id):
        return self.bus.transact(board_id, "ZERO", timeout=2.0)

    def zero_all(self):
        replies = {}
        for board_id in self.board_ids:
            replies[str(board_id)] = self.zero_board(board_id)
        return replies

    def parse_data_line(self, line):
        parts = line.split(",")
        if len(parts) < 7 or parts[1] != "DATA":
            return None
        board_id = int(parts[0])
        sample = int(parts[2])
        raw_u = float(parts[4])
        raw_i = float(parts[5])
        raw_photo = float(parts[6])
        c = self.calibration_for(board_id, sample)

        # Current firmware protocol:
        # board,DATA,sample,millis,Uraw,Iraw,PhotoRaw,Uapprox,Iapprox_mA
        # Uapprox and Iapprox are already converted by Arduino using the real
        # divider, amplifier and shunt coefficients. They are authoritative.
        # Older firmware ended after PhotoRaw, so retain the old PC-side
        # conversion only as a backwards-compatible fallback.
        device_u = None
        device_i = None
        if len(parts) >= 9:
            parsed_device_u = float(parts[7])
            parsed_device_i = float(parts[8])
            if math.isfinite(parsed_device_u) and math.isfinite(parsed_device_i):
                device_u = parsed_device_u
                device_i = parsed_device_i

        if device_u is not None and device_i is not None:
            voltage = device_u * c.get("voltage_device_scale", 1.0) + c.get("voltage_device_offset", 0.0)
            current_before_dynamic_offset = (
                device_i * c.get("current_device_scale", 1.0)
                + c.get("current_device_offset", 0.0)
            )
            measurement_source = "device_converted"
        else:
            voltage = raw_u * c["voltage_measure_scale"] + c["voltage_measure_offset"]
            current_before_dynamic_offset = raw_i * c["current_measure_scale"] + c["current_measure_offset"]
            measurement_source = "pc_legacy_raw"

        dynamic_current_offset_ma = current_offset_from_voltage(c, voltage)
        current = current_before_dynamic_offset - dynamic_current_offset_ma

        point = {
            "t": round(time.monotonic() - self.started, 3),
            "board": board_id,
            "sample": sample,
            "u": voltage,
            "i": current,
            "photo": raw_photo * c["photo_measure_scale"] + c["photo_measure_offset"],
            "raw_u": raw_u,
            "raw_i": raw_i,
            "raw_photo": raw_photo,
            "device_u": device_u,
            "device_i": device_i,
            "measurement_source": measurement_source,
            "current_before_dynamic_offset_ma": current_before_dynamic_offset,
            "current_dynamic_offset_ma": dynamic_current_offset_ma,
            "device_millis": int(float(parts[3])),
        }
        key = self.key(board_id, sample)
        self.latest[key] = point
        self.history[key].append(point)
        return key, point

    def read_board(self, board_id):
        lines = self.bus.transact(board_id, "READ", timeout=3.5, until_end=True)
        data = {}
        online = bool(lines)
        errors = [line for line in lines if line.startswith(f"{board_id},ERR,")]
        for line in lines:
            if line.startswith(f"{board_id},DATA,"):
                parsed = self.parse_data_line(line)
                if parsed:
                    key, point = parsed
                    data[key] = point
        self.board_status[board_id] = {
            "online": online,
            "status": f"DATA {len(data)}" if data else ("; ".join(errors) if errors else "нет DATA"),
            "last_seen": datetime.now().strftime("%H:%M:%S") if online else self.board_status.get(board_id, {}).get("last_seen"),
        }
        return data

    def read_all_boards(self):
        points = {}
        for board_id in self.board_ids:
            try:
                points.update(self.read_board(board_id))
            except Exception as exc:
                self.board_status[board_id] = {"online": False, "status": str(exc), "last_seen": self.board_status.get(board_id, {}).get("last_seen")}
        return points

    def _poll_worker(self):
        while self.running:
            if self.live and self.bus.connected and not self.recording_active:
                try:
                    self.read_all_boards()
                except (serial.SerialException, OSError):
                    self.bus.disconnect()
                except Exception as exc:
                    self.bus.log("err", exc)
            time.sleep(0.8)

    def recording_status(self):
        return {
            "active": self.recording_active,
            "interval": self.recording_interval,
            "message": self.recording_message,
            "dir": str(self.recording_dir) if self.recording_dir else "",
            "started": datetime.fromtimestamp(self.recording_started_at).strftime("%Y-%m-%d %H:%M:%S") if self.recording_started_at else "",
            "counts": self.recording_counts,
        }

    def start_recording(self, interval):
        if not self.bus.connected:
            raise RuntimeError("Сначала подключите USB-RS485")
        interval = float(interval)
        if not math.isfinite(interval) or not 0.2 <= interval <= 86400:
            raise ValueError("Интервал записи должен быть от 0.2 секунды до 24 часов")
        if self.recording_active:
            raise RuntimeError("Запись уже идёт")
        RECORDINGS_DIR.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        self.recording_dir = RECORDINGS_DIR / f"run_{stamp}"
        self.recording_dir.mkdir(parents=True, exist_ok=True)
        self.recording_started_at = time.time()
        self.recording_interval = interval
        self.recording_cancel.clear()
        self.recording_counts = {}
        for board_id in self.board_ids:
            board_dir = self.recording_dir / f"board_{board_id:02d}"
            board_dir.mkdir(parents=True, exist_ok=True)
            for sample in range(1, SAMPLES_PER_BOARD + 1):
                key = self.key(board_id, sample)
                self.recording_counts[key] = 0
                with (board_dir / f"sample_{sample}.csv").open("w", encoding="utf-8-sig", newline="") as file:
                    writer = csv.writer(file, delimiter=";")
                    writer.writerow([
                        "human_time", "computer_time", "elapsed_s", "board", "sample",
                        "voltage_v", "current_ma", "brightness",
                        "current_adc_gain", "current_adc_range_v", "current_adc_max_ma",
                        "photo_adc_gain", "photo_adc_range_v",
                        "device_u", "device_i", "measurement_source",
                        "current_before_dynamic_offset_ma", "current_dynamic_offset_ma",
                        "raw_u", "raw_i", "raw_photo",
                    ])
        self.recording_active = True
        self.recording_message = "Запись запущена"
        threading.Thread(target=self._recording_worker, daemon=True).start()

    def append_recording_point(self, point):
        if not self.recording_dir:
            return
        board_id = point["board"]
        sample = point["sample"]
        key = self.key(board_id, sample)
        now = time.time()
        path = self.recording_dir / f"board_{board_id:02d}" / f"sample_{sample}.csv"
        calibration = self.calibration_for(board_id, sample)
        photo_gain = calibration.get("photo_adc_gain", DEFAULT_PHOTO_ADC_GAIN)
        current_adc_gain = calibration.get("current_adc_gain", DEFAULT_CURRENT_ADC_GAIN)
        with path.open("a", encoding="utf-8-sig", newline="") as file:
            writer = csv.writer(file, delimiter=";")
            writer.writerow([
                datetime.fromtimestamp(now).strftime("%Y-%m-%d %H:%M:%S"),
                f"{now:.6f}",
                f"{now - self.recording_started_at:.3f}",
                board_id,
                sample,
                f"{point['u']:.6f}",
                f"{point['i']:.6f}",
                f"{point['photo']:.6f}",
                current_adc_gain,
                f"{ADS_GAIN_OPTIONS[current_adc_gain]['full_scale_v']:.6f}",
                f"{self.measurable_current_max(board_id, sample):.6f}",
                photo_gain,
                f"{ADS_GAIN_OPTIONS[photo_gain]['full_scale_v']:.6f}",
                "" if point.get("device_u") is None else f"{point['device_u']:.6f}",
                "" if point.get("device_i") is None else f"{point['device_i']:.6f}",
                point.get("measurement_source", ""),
                f"{point.get('current_before_dynamic_offset_ma', point['i']):.6f}",
                f"{point.get('current_dynamic_offset_ma', 0.0):.6f}",
                f"{point['raw_u']:.6f}",
                f"{point['raw_i']:.6f}",
                f"{point['raw_photo']:.6f}",
            ])
        self.recording_counts[key] = self.recording_counts.get(key, 0) + 1

    def _recording_worker(self):
        next_tick = time.monotonic()
        try:
            while not self.recording_cancel.is_set() and self.bus.connected:
                now = time.monotonic()
                if now < next_tick:
                    self.recording_cancel.wait(min(0.2, next_tick - now))
                    continue
                points = self.read_all_boards()
                for point in points.values():
                    self.append_recording_point(point)
                total = sum(self.recording_counts.values())
                self.recording_message = f"Записано строк: {total}"
                next_tick += self.recording_interval
        except Exception as exc:
            self.recording_message = f"Ошибка записи: {exc}"
        finally:
            self.recording_active = False
            if not self.recording_cancel.is_set() and not self.bus.connected:
                self.recording_message = "Запись остановлена: COM-порт отключён"

    def stop_recording(self):
        self.recording_cancel.set()
        self.recording_active = False
        self.recording_message = "Запись остановлена"

    def recording_zip(self):
        if not self.recording_dir or not self.recording_dir.exists():
            raise RuntimeError("Нет папки записи")
        mem = io.BytesIO()
        with zipfile.ZipFile(mem, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in self.recording_dir.rglob("*.csv"):
                archive.write(path, path.relative_to(self.recording_dir))
        mem.seek(0)
        return mem.getvalue()

    def latest_recording_dir(self):
        if self.recording_dir and self.recording_dir.exists():
            return self.recording_dir
        if not RECORDINGS_DIR.exists():
            return None
        runs = sorted((path for path in RECORDINGS_DIR.glob("run_*") if path.is_dir()), reverse=True)
        return runs[0] if runs else None

    def recording_board_history(self, board_id: int, max_points: int = 6000):
        if board_id not in self.board_ids:
            raise ValueError(f"Плата {board_id} отсутствует в конфигурации")
        max_points = max(100, min(int(max_points), 20000))
        run_dir = self.latest_recording_dir()
        if not run_dir:
            raise FileNotFoundError("Записей пока нет")
        board_dir = run_dir / f"board_{board_id:02d}"
        if not board_dir.exists():
            raise FileNotFoundError(f"В записи нет данных платы {board_id}")

        channels = []
        total_source_points = 0
        first_time = None
        last_time = None
        for sample in range(1, SAMPLES_PER_BOARD + 1):
            path = board_dir / f"sample_{sample}.csv"
            points = []
            if path.exists():
                with path.open("r", encoding="utf-8-sig", newline="") as file:
                    for row in csv.DictReader(file, delimiter=";"):
                        try:
                            point = {
                                "elapsed_s": float(row["elapsed_s"]),
                                "computer_time": float(row["computer_time"]),
                                "human_time": row.get("human_time", ""),
                                "u": float(row["voltage_v"]),
                                "i": float(row["current_ma"]),
                                "photo": float(row["brightness"]),
                            }
                        except (KeyError, TypeError, ValueError):
                            continue
                        points.append(point)
            total_source_points += len(points)
            if points:
                if first_time is None or points[0]["computer_time"] < first_time:
                    first_time = points[0]["computer_time"]
                if last_time is None or points[-1]["computer_time"] > last_time:
                    last_time = points[-1]["computer_time"]
            displayed = downsample_recording_points(points, max_points)
            channels.append({
                "sample": sample,
                "source_points": len(points),
                "display_points": len(displayed),
                "points": displayed,
            })

        if not total_source_points:
            raise FileNotFoundError(f"CSV платы {board_id} пока пусты")
        return {
            "board": board_id,
            "run": run_dir.name,
            "directory": str(run_dir),
            "active": bool(self.recording_active and self.recording_dir == run_dir),
            "first_time": datetime.fromtimestamp(first_time).strftime("%Y-%m-%d %H:%M:%S") if first_time else "",
            "last_time": datetime.fromtimestamp(last_time).strftime("%Y-%m-%d %H:%M:%S") if last_time else "",
            "source_points": total_source_points,
            "channels": channels,
        }

    def snapshot(self):
        channels = []
        for board_id in self.board_ids:
            for sample in range(1, SAMPLES_PER_BOARD + 1):
                key = self.key(board_id, sample)
                c = self.calibration_for(board_id, sample)
                photo_gain = c.get("photo_adc_gain", DEFAULT_PHOTO_ADC_GAIN)
                photo_gain_info = ADS_GAIN_OPTIONS[photo_gain]
                current_adc_gain = c.get("current_adc_gain", DEFAULT_CURRENT_ADC_GAIN)
                current_gain_info = ADS_GAIN_OPTIONS[current_adc_gain]
                channels.append({
                    "key": key,
                    "board": board_id,
                    "sample": sample,
                    "latest": self.latest.get(key),
                    "history": list(self.history.get(key, []))[-240:],
                    "current_limit": self.current_limits.get(key, self.measurable_current_max(board_id, sample)),
                    "max_current": self.measurable_current_max(board_id, sample),
                    "current_adc_gain": current_adc_gain,
                    "current_adc_range_v": min(5.0, current_gain_info["full_scale_v"]),
                    "current_adc_lsb_uv": current_gain_info["lsb_uv"],
                    "photo_adc_gain": photo_gain,
                    "photo_adc_range_v": photo_gain_info["full_scale_v"],
                    "photo_adc_lsb_uv": photo_gain_info["lsb_uv"],
                })
        return {
            "connected": self.bus.connected,
            "port": self.bus.port,
            "config": self.config,
            "gain_options": ADS_GAIN_OPTIONS,
            "boards": self.board_status,
            "channels": channels,
            "recording": self.recording_status(),
            "logs": list(self.bus.logs)[-120:],
        }


state = StationState()
app = FastAPI(title=APP_NAME)
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/")
def index():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/api/ports")
def ports():
    return [{"device": p.device, "description": p.description} for p in list_ports.comports()]


@app.get("/api/config")
def get_config():
    return state.config


@app.put("/api/config")
def put_config(req: ConfigRequest):
    try:
        state.apply_config(req)
        return {"ok": True, "config": state.config}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.put("/api/calibration")
def put_calibration(req: CalibrationRequest):
    try:
        state.apply_calibration(req)
        return {"ok": True, "config": state.config}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.get("/api/calibration/export")
def export_calibration():
    exported = {
        "format": "NMSE OLEDer calibration",
        "schema_version": 1,
        "exported_at": datetime.now().astimezone().isoformat(timespec="seconds"),
        "active_board_ids": state.config["board_ids"],
        "hardware": state.config["hardware"],
        "calibration": state.config["calibration"],
    }
    filename = f"nmse_oleder_calibration_{datetime.now().strftime('%Y-%m-%d_%H-%M-%S')}.json"
    return Response(
        json.dumps(exported, ensure_ascii=False, indent=2),
        media_type="application/json",
        headers={"Content-Disposition": f'attachment; filename="{filename}"'},
    )


@app.post("/api/calibration/import")
def import_calibration(req: CalibrationImportRequest):
    try:
        boards = state.import_calibration(req)
        return {"ok": True, "boards": boards, "config": state.config}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/connect")
def connect(req: ConnectRequest):
    try:
        state.connect(req.port, req.board_ids)
        return {"ok": True, "boards": state.board_status}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/disconnect")
def disconnect():
    state.stop_recording()
    state.bus.disconnect()
    return {"ok": True}


@app.post("/api/shutdown")
def shutdown():
    def stop_process():
        state.running = False
        state.recording_cancel.set()
        state.bus.disconnect()
        os._exit(0)
    threading.Timer(0.35, stop_process).start()
    return {"ok": True}


@app.post("/api/scan")
def scan():
    try:
        state.scan_boards()
        return {"ok": True, "boards": state.board_status}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/status/{board_id}")
def board_status(board_id: int):
    try:
        return {"ok": True, "reply": state.status_board(board_id)}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/set")
def set_sample(req: SetRequest):
    try:
        return {"ok": True, "reply": state.set_sample(req)}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/photo-gain")
def photo_gain(req: PhotoGainRequest):
    try:
        return {"ok": True, "reply": state.set_photo_gain(req), "config": state.config}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/current-adc-gain")
def current_adc_gain(req: CurrentAdcGainRequest):
    try:
        return {"ok": True, "reply": state.set_current_adc_gain(req), "config": state.config}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/zero")
def zero_all():
    try:
        return {"ok": True, "reply": state.zero_all()}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/zero/{board_id}")
def zero_board(board_id: int):
    try:
        return {"ok": True, "reply": state.zero_board(board_id)}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/read")
def read_all():
    try:
        return {"ok": True, "points": state.read_all_boards()}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/recording/start")
def recording_start(req: RecordingRequest):
    try:
        state.start_recording(req.interval)
        return {"ok": True, "recording": state.recording_status()}
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.post("/api/recording/stop")
def recording_stop():
    state.stop_recording()
    return {"ok": True, "recording": state.recording_status()}


@app.get("/api/recording/zip")
def recording_zip():
    try:
        return Response(
            state.recording_zip(),
            media_type="application/zip",
            headers={"Content-Disposition": "attachment; filename=nmse_oleder_recording.zip"},
        )
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.get("/api/recording/board/{board_id}/history")
def recording_board_history(board_id: int, max_points: int = 6000):
    try:
        return state.recording_board_history(board_id, max_points)
    except FileNotFoundError as exc:
        raise HTTPException(404, str(exc))
    except Exception as exc:
        raise HTTPException(400, str(exc))


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await ws.accept()
    try:
        while True:
            await ws.send_json(state.snapshot())
            await asyncio.sleep(0.4)
    except (WebSocketDisconnect, RuntimeError):
        pass


def find_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run_server():
    """Точка запуска для exe, PowerShell-скрипта и PyCharm."""
    configured_port = os.environ.get("NMSE_PORT", "").strip()
    port = int(configured_port) if configured_port else find_port()
    if os.environ.get("NMSE_NO_BROWSER") != "1":
        threading.Timer(1.2, lambda: webbrowser.open(f"http://127.0.0.1:{port}")).start()
    uvicorn.run(app, host="127.0.0.1", port=port, log_level="warning", access_log=False)


if __name__ == "__main__":
    run_server()
