import csv
import tempfile
import time
import unittest
from collections import deque
from pathlib import Path
from unittest.mock import patch

import web_station.main as main_module
from web_station.main import (
    ConfigRequest,
    StationState,
    current_offset_from_voltage,
    downsample_recording_points,
    ensure_config_shape,
)


def make_station():
    station = StationState.__new__(StationState)
    station.config = ensure_config_shape({
        "board_ids": [5],
        "hardware": {"shunt_ohm": 25.0, "current_gain": 10.0},
        "calibration": {},
    })
    station.started = time.monotonic()
    station.latest = {}
    station.history = {"5:1": deque(maxlen=10)}
    return station


class DataProtocolTests(unittest.TestCase):
    def test_calibration_is_restored_from_disk_and_backup(self):
        station = make_station()
        station.config["calibration"]["5"][2]["voltage_device_scale"] = 1.2345
        with tempfile.TemporaryDirectory() as directory:
            data_dir = Path(directory)
            config_file = data_dir / "config.json"
            backup_file = data_dir / "config.backup.json"
            with (
                patch.object(main_module, "DATA_DIR", data_dir),
                patch.object(main_module, "CONFIG_FILE", config_file),
                patch.object(main_module, "CONFIG_BACKUP_FILE", backup_file),
            ):
                station.save_config()
                self.assertTrue(config_file.exists())
                self.assertTrue(backup_file.exists())
                config_file.write_text("повреждённый json", encoding="utf-8")
                restored = main_module.load_config()

        self.assertAlmostEqual(
            restored["calibration"]["5"][2]["voltage_device_scale"], 1.2345
        )

    def test_calibration_survives_board_removal_and_readdition(self):
        station = make_station()
        station.config["calibration"]["5"][0]["current_device_offset"] = 0.4321
        station.board_status = {5: {"online": False, "status": "", "last_seen": None}}
        station.ensure_runtime_maps = lambda: None
        station.save_config = lambda: None

        station.apply_config(ConfigRequest(board_ids=[6], shunt_ohm=25.0, current_gain=10.0))
        self.assertIn("5", station.config["calibration"])
        station.apply_config(ConfigRequest(board_ids=[5], shunt_ohm=25.0, current_gain=10.0))

        self.assertAlmostEqual(
            station.config["calibration"]["5"][0]["current_device_offset"], 0.4321
        )

    def test_device_converted_values_are_authoritative(self):
        station = make_station()
        parsed = station.parse_data_line(
            "5,DATA,1,2042,1.590562,1.237500,0.251063,6.362250,6.187500"
        )
        self.assertIsNotNone(parsed)
        _, point = parsed
        self.assertAlmostEqual(point["u"], 6.362250)
        self.assertAlmostEqual(point["i"], 6.187500)
        self.assertEqual(point["measurement_source"], "device_converted")
        self.assertAlmostEqual(point["raw_u"], 1.590562)
        self.assertAlmostEqual(point["raw_i"], 1.237500)

    def test_optional_device_correction_is_applied_once(self):
        station = make_station()
        calibration = station.config["calibration"]["5"][0]
        calibration["voltage_device_scale"] = 1.01
        calibration["voltage_device_offset"] = -0.02
        calibration["current_device_scale"] = 0.99
        calibration["current_device_offset"] = 0.1
        _, point = station.parse_data_line(
            "5,DATA,1,2042,1.590562,1.237500,0.251063,6.362250,6.187500"
        )
        self.assertAlmostEqual(point["u"], 6.362250 * 1.01 - 0.02)
        self.assertAlmostEqual(point["i"], 6.187500 * 0.99 + 0.1)

    def test_old_firmware_falls_back_to_raw_conversion(self):
        station = make_station()
        _, point = station.parse_data_line(
            "5,DATA,1,2042,1.500000,1.250000,0.250000"
        )
        self.assertAlmostEqual(point["u"], 4.5)
        self.assertAlmostEqual(point["i"], 5.0)
        self.assertEqual(point["measurement_source"], "pc_legacy_raw")

    def test_voltage_dependent_current_offset_is_interpolated_and_subtracted(self):
        station = make_station()
        calibration = station.config["calibration"]["5"][0]
        calibration["current_offset_u1_v"] = 2.0
        calibration["current_offset_i1_ma"] = 0.5
        calibration["current_offset_u2_v"] = 8.0
        calibration["current_offset_i2_ma"] = 0.0
        _, point = station.parse_data_line(
            "5,DATA,1,2042,1.250000,1.237500,0.251063,5.000000,7.000000"
        )
        expected_offset = 0.25
        self.assertAlmostEqual(point["current_dynamic_offset_ma"], expected_offset)
        self.assertAlmostEqual(point["current_before_dynamic_offset_ma"], 7.0)
        self.assertAlmostEqual(point["i"], 7.0 - expected_offset)

    def test_voltage_dependent_offset_is_clamped_outside_range(self):
        calibration = {
            "current_offset_u1_v": 2.0,
            "current_offset_i1_ma": 0.5,
            "current_offset_u2_v": 8.0,
            "current_offset_i2_ma": 0.0,
        }
        self.assertAlmostEqual(current_offset_from_voltage(calibration, 0.0), 0.5)
        self.assertAlmostEqual(current_offset_from_voltage(calibration, 12.0), 0.0)

    def test_recording_downsampling_preserves_range(self):
        points = [{"x": index} for index in range(1000)]
        displayed = downsample_recording_points(points, 100)
        self.assertEqual(len(displayed), 100)
        self.assertEqual(displayed[0]["x"], 0)
        self.assertEqual(displayed[-1]["x"], 999)

    def test_board_recording_history_reads_all_four_channels(self):
        station = make_station()
        with tempfile.TemporaryDirectory() as directory:
            run_dir = Path(directory) / "run_2026-08-18_10-00-00"
            board_dir = run_dir / "board_05"
            board_dir.mkdir(parents=True)
            fields = ["human_time", "computer_time", "elapsed_s", "voltage_v", "current_ma", "brightness"]
            for sample in range(1, 5):
                with (board_dir / f"sample_{sample}.csv").open("w", encoding="utf-8-sig", newline="") as file:
                    writer = csv.DictWriter(file, fieldnames=fields, delimiter=";")
                    writer.writeheader()
                    for point in range(3):
                        writer.writerow({
                            "human_time": f"2026-08-18 10:00:0{point}",
                            "computer_time": 1_776_500_000 + point,
                            "elapsed_s": point,
                            "voltage_v": sample + point / 10,
                            "current_ma": sample * 2 + point / 10,
                            "brightness": sample * 3 + point / 10,
                        })
            station.recording_dir = run_dir
            station.recording_active = False
            payload = station.recording_board_history(5, 100)
            self.assertEqual(payload["board"], 5)
            self.assertEqual(len(payload["channels"]), 4)
            self.assertEqual(payload["source_points"], 12)
            self.assertEqual(payload["channels"][3]["points"][2]["i"], 8.2)


if __name__ == "__main__":
    unittest.main()
