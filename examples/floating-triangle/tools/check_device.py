#!/usr/bin/env python3
"""Save hardware facts and the result of a real Metal device probe, without IDs."""
import json
import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parent.parent


def output(*arguments):
    return subprocess.check_output(arguments, text=True).strip()


hardware = json.loads(output("system_profiler", "SPHardwareDataType", "-json"))["SPHardwareDataType"][0]
displays = json.loads(output("system_profiler", "SPDisplaysDataType", "-json"))["SPDisplaysDataType"]
report = {
    "model": hardware.get("machine_name"),
    "model_identifier": hardware.get("machine_model"),
    "chip": hardware.get("chip_type"),
    "memory": hardware.get("physical_memory"),
    "architecture": output("uname", "-m"),
    "macos": output("sw_vers", "-productVersion"),
    "gpus": [{
        "name": display.get("sppci_model"),
        "cores": display.get("sppci_cores"),
        "metal_reported_by_macos": display.get("spdisplays_metal") == "spdisplays_supported",
    } for display in displays],
}
probe = subprocess.run([str(root / "build/metal-capabilities")], text=True, capture_output=True)
report["metal_device_accessible_in_this_process"] = probe.returncode == 0
if probe.returncode == 0:
    report["metal_runtime_capabilities"] = json.loads(probe.stdout)
else:
    report["metal_probe_error"] = probe.stderr.strip()
    report["note"] = "macOS reports Metal support; this process could not access a Metal device."
destination = root / "build/device-capabilities.json"
destination.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
print(f"Saved {destination}")
