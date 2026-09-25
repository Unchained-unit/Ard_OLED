"""Launch a packaged EXE without connecting hardware and check HTTP/WebSocket."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
from urllib.request import Request, urlopen

from websockets.sync.client import connect


def main():
    executable = Path(sys.argv[1]).resolve()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, NMSE_NO_BROWSER="1", NMSE_PORT=str(port))
    process = subprocess.Popen([str(executable)], env=env,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    base = f"http://127.0.0.1:{port}"
    try:
        deadline = time.monotonic() + 45
        while True:
            try:
                with urlopen(base + "/openapi.json", timeout=2) as response:
                    schema = json.load(response)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("Packaged application failed to start")
                time.sleep(.3)
        fields = schema["components"]["schemas"]["SetRequest"]["properties"]
        assert fields["current_feedback"]["default"] is True
        assert fields["current_tolerance"]["default"] == .01
        with urlopen(base, timeout=3) as response:
            html = response.read().decode("utf-8")
        assert "Подстроить ток по АЦП и зафиксировать" in html
        with connect(f"ws://127.0.0.1:{port}/ws", open_timeout=5) as ws:
            snapshot = json.loads(ws.recv(timeout=5))
            assert not snapshot["connected"]
            assert all(ch["current_control"] is None for ch in snapshot["channels"])
        print("EXE OK: HTTP, new API fields, UI, WebSocket; no hardware connection")
        with urlopen(Request(base + "/api/shutdown", data=b"{}",
                             headers={"Content-Type": "application/json"}), timeout=3):
            pass
        process.wait(timeout=8)
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=8)


if __name__ == "__main__":
    main()
