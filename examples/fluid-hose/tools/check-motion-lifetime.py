#!/usr/bin/env python3
"""Verify the reader exits when its owning process is killed. No sensors opened."""
from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
import time

project = Path(__file__).resolve().parents[1]
helper = project / "build" / "probe-motion"
parent_script = r'''
import subprocess, sys, time
with open(sys.argv[2], "w") as log:
    reader = subprocess.Popen([sys.argv[1], "--stream-mock"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, start_new_session=True)
    packet = reader.stdout.read(48)
    if len(packet) != 48:
        raise SystemExit("No motion packet")
    print(reader.pid, flush=True)
    while True:
        time.sleep(0.1)
'''
with tempfile.TemporaryDirectory(prefix="motion-lifetime-", dir=project / "build") as directory:
    log = Path(directory) / "reader.log"
    parent = subprocess.Popen([sys.executable, "-c", parent_script, str(helper), str(log)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                              start_new_session=True)
    reader_pid = None
    try:
        reader_pid = int(parent.stdout.readline().strip())
        assert os.getpgid(reader_pid) == reader_pid, "Reader must have a separate process group"
        os.kill(parent.pid, signal.SIGKILL)
        parent.wait(timeout=2)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if "stopped after owner shutdown" in log.read_text():
                break
            time.sleep(0.02)
        else:
            raise AssertionError("Reader kept running after its owner was force-killed")
        print("PASS: force-killing the owner closes its control pipe and stops the reader.")
    finally:
        if parent.poll() is None:
            parent.kill()
            parent.wait()
        if reader_pid:
            try:
                os.kill(reader_pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
