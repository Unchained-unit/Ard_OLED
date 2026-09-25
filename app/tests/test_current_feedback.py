import math
import threading
import time
import unittest
from unittest.mock import patch

from web_station import main
from web_station.current_control import CurrentControl


class FakeBus:
    connected = True
    port = "test"

    def __init__(self):
        self.commands = []
        self.logs = []
        self.reject = False

    def log(self, *args):
        self.logs.append(args)

    def disconnect(self):
        self.connected = False

    def transact(self, board, command, **kwargs):
        self.commands.append((board, command))
        if self.reject:
            return []
        parts = command.split(",")
        if parts[0] == "ZERO":
            return [f"{board},OK,ZERO"]
        if parts[0] == "SET":
            return [f"{board},OK,DAC,{parts[1]},{mode},0,0" for mode in "VI"]
        if parts[0] == "IDAC":
            return [f"{board},OK,DAC,{parts[1]},I,{parts[2]},0"]
        return []


def station():
    with patch.object(main, "load_config", main.default_config), patch.object(threading.Thread, "start"):
        result = main.StationState()
    result.bus = FakeBus()
    return result


def request(**kwargs):
    values = dict(board=1, sample=1, voltage=3, current=0.3, current_limit=2)
    values.update(kwargs)
    return main.SetRequest(**values)


def point(current, tick=1, sample=1, board=1, raw=None):
    return {f"{board}:{sample}": dict(board=board, sample=sample, i=current,
            raw_i=current / 4 if raw is None else raw, device_millis=tick)}


class CurrentFeedbackTests(unittest.TestCase):
    def test_converges_with_offset_then_freezes_despite_drift(self):
        s = station()
        s.set_sample(request())
        c = s.current_controls["1:1"]
        for tick in range(1, 80):
            actual = 4 * c.dac_v + 0.2
            s.regulate_board(1, point(actual, tick))
            if not c.enabled:
                break
        self.assertEqual(c.status, "ЦАП зафиксирован")
        self.assertTrue(c.monitoring)
        self.assertLessEqual(abs(actual - .3), c.tolerance_ma)
        count = len(s.bus.commands)
        for tick in range(100, 110):
            s.regulate_board(1, point(.5, tick))
        self.assertEqual(len(s.bus.commands), count)
        s.set_sample(request())
        self.assertTrue(s.current_controls["1:1"].enabled)

    def test_requires_three_consecutive_measurements(self):
        c = CurrentControl(.3, 4, .075, .01)
        for tick, measured in enumerate([.3, .3, .4, .3, .3], 1):
            c.update(measured, tick)
            self.assertTrue(c.enabled)
        c.update(.3, 6)
        self.assertFalse(c.enabled)

    def test_step_is_bounded_and_direction_correct(self):
        c = CurrentControl(1, 4, .25, .01)
        self.assertLess(c.update(2, 1), c.dac_v)
        self.assertLessEqual(abs(c.update(0, 2) - c.dac_v), .021)

    def test_zero_and_manual_never_autocorrect(self):
        for req in [request(current=0), request(current_feedback=False)]:
            s = station()
            s.set_sample(req)
            count = len(s.bus.commands)
            s.regulate_board(1, point(.5))
            self.assertEqual(len(s.bus.commands), count)
            self.assertFalse(s.current_controls["1:1"].enabled)
        s = station()
        s.set_sample(request(current=0))
        self.assertEqual(s.bus.commands[-1][1], "SET,1,0.000000,0.000000")

    def test_faults_disable_feedback_and_zero_board(self):
        for data, frame_error in [(point(3), False), ({}, False),
                                  (point(.2, raw=5), False), (point(.2), True)]:
            s = station()
            s.set_sample(request())
            s.regulate_board(1, data, frame_error)
            self.assertFalse(s.current_controls["1:1"].enabled)
            self.assertEqual(s.bus.commands[-1], (1, "ZERO"))

    def test_frozen_target_still_has_emergency_limit(self):
        s = station()
        s.set_sample(request())
        for tick in range(1, 4):
            s.regulate_board(1, point(.3, tick))
        s.regulate_board(1, point(3, 4))
        self.assertEqual(s.bus.commands[-1], (1, "ZERO"))

    def test_repeated_data_and_timeout_stop(self):
        c = CurrentControl(.3, 4, .075, .01)
        c.update(.4, 2)
        with self.assertRaises(ValueError):
            c.update(.4, 2)
        c.started = time.monotonic() - 121
        with self.assertRaises(ValueError):
            c.update(.4, 3)

    def test_unreachable_low_current_stops_at_zero_dac(self):
        s = station()
        s.set_sample(request())
        for tick in range(1, 30):
            s.regulate_board(1, point(.5, tick))
            if not s.current_controls["1:1"].monitoring:
                break
        self.assertFalse(s.current_controls["1:1"].monitoring)
        self.assertEqual(s.bus.commands[-1], (1, "ZERO"))

    def test_no_ack_does_not_arm_and_reports_failed_zero(self):
        s = station()
        s.set_sample(request())
        s.bus.reject = True
        s.regulate_board(1, point(.5))
        c = s.current_controls["1:1"]
        self.assertFalse(c.enabled)
        self.assertIn("ZERO НЕ подтверждён", c.status)

    def test_nonfinite_data_rejected_without_legacy_fallback(self):
        s = station()
        for line in ["1,DATA,1,42,.1,nan,.1,1,.5", "1,DATA,1,42,.1,.1,.1,1,nan"]:
            self.assertIsNone(s.parse_data_line(line))

    def test_channels_and_boards_are_independent(self):
        s = station()
        s.set_sample(request())
        s.set_sample(request(board=2, sample=2))
        s.regulate_board(1, point(.5))
        self.assertEqual(s.bus.commands[-1][0], 1)
        self.assertEqual(s.current_controls["2:2"].dac_v, .075)
        s.zero_board(1)
        self.assertFalse(s.current_controls["1:1"].enabled)
        self.assertTrue(s.current_controls["2:2"].enabled)

    def test_bad_tolerance_and_limit_rejected(self):
        for req in [request(current_tolerance=math.nan), request(current_limit=.3)]:
            with self.assertRaises(ValueError):
                station().set_sample(req)

    def test_recording_long_interval_keeps_polling_feedback(self):
        s = station()
        s.set_sample(request())
        s.recording_interval = 100
        s.recording_counts = {}
        calls = []
        def read():
            calls.append(1)
            if len(calls) == 3:
                s.recording_cancel.set()
            return point(.3, len(calls))
        s.read_all_boards = read
        s.append_recording_point = lambda p: None
        with patch.object(s.recording_cancel, "wait"):
            s._recording_worker()
        self.assertEqual(len(calls), 3)


if __name__ == "__main__":
    unittest.main()
