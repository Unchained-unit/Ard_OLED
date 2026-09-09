import argparse
import time

import serial
from serial.tools import list_ports


def list_ports_cli():
    ports = list(list_ports.comports())
    if not ports:
        print("COM-порты не найдены")
        return
    print("Доступные COM-порты:")
    for port in ports:
        print(f"  {port.device:8} {port.description}")


def transact(ser, board, command, timeout=1.2, until_end=False):
    full = f"{board},{command}"
    print(f"TX: {full}")
    ser.write((full + "\n").encode("ascii"))
    ser.flush()
    end = time.monotonic() + timeout
    lines = []
    while time.monotonic() < end:
        raw = ser.readline()
        if not raw:
            continue
        text = raw.decode("ascii", errors="replace").strip()
        if text:
            lines.append(text)
            print(f"RX: {text}")
            if until_end and text == f"{board},END":
                break
    if not lines:
        print("RX: нет ответа")
    return lines


def main():
    parser = argparse.ArgumentParser(description="Быстрый тест NMSE OLEDer RS-485")
    parser.add_argument("port", nargs="?", help="COM-порт USB-RS485, например COM3")
    parser.add_argument("--boards", default="1,2", help="Адреса плат через запятую, например 1,2,3")
    args = parser.parse_args()
    if not args.port:
        list_ports_cli()
        print("\nПример: python tools\\rs485_multi_board_test.py COM3 --boards 1,2")
        return 2
    boards = [int(x) for x in args.boards.replace(";", ",").split(",") if x.strip()]
    with serial.Serial(args.port, 9600, timeout=0.1, write_timeout=1) as ser:
        time.sleep(0.4)
        ser.reset_input_buffer()
        for board in boards:
            print(f"\n--- board {board} ---")
            transact(ser, board, "PING", timeout=0.8)
            transact(ser, board, "STATUS", timeout=1.0)
            transact(ser, board, "READ", timeout=3.5, until_end=True)
            transact(ser, board, "PING", timeout=0.8)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
