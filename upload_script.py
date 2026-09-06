"""
Pre-upload script for ESP8266 dual-slot (A/B) OTA.

On first USB upload, flashes the firmware to BOTH slots (app0 and app1)
so the device boots from either one. After that, OTA updates only the
inactive slot, providing automatic rollback on failure.

Usage:
  First USB flash:  pio run --target upload
  OTA flash:        pio run --target upload --upload-port <IP>
"""

import subprocess
import sys
import os
from os.path import join, exists, dirname

Import("env")

def before_upload(source, target, env):
    """Before USB upload, flash the same firmware to both app0 and app1."""
    
    upload_protocol = env.get("UPLOAD_PROTOCOL", "")
    upload_port = env.get("UPLOAD_PORT", "")
    
    # Only do dual flash for serial (USB) uploads
    is_ota = (upload_protocol == "espota" or 
              upload_port.startswith("network:") or
              "." in upload_port or
              ".local" in upload_port)
    
    if is_ota:
        print("[upload_script] OTA upload detected: single slot")
        return
    
    print("[upload_script] USB upload detected: flashing BOTH slots (A/B)")
    
    firmware_path = str(source[0])
    
    if not upload_port:
        print("[upload_script] WARNING: No upload port specified!")
        return
    
    # Find esptool in PlatformIO packages using the uploader path
    uploader_path = env.subst("$UPLOADER")
    if uploader_path and exists(uploader_path):
        esptool = uploader_path
    else:
        # Try to find esptool.py in tool-esptoolpy package
        for root, dirs, files in os.walk(env.subst("$PROJECT_PACKAGES_DIR")):
            for f in files:
                if f == "esptool.py":
                    esptool = os.path.join(root, f)
                    break
            if 'esptool' in locals():
                break
    
    if not esptool or not exists(esptool):
        print("[upload_script] WARNING: esptool not found, skipping dual flash")
        print("[upload_script] Device will only have firmware in slot 0")
        print("[upload_script] To fill slot 1 later, run: pio run --target upload")
        return
    
    python = env.subst("$PYTHONEXE")
    
    # Flash to app0 (0x10000) and app1 (0x101000)
    cmd = [
        python, esptool,
        "--port", upload_port,
        "--baud", "460800",
        "write_flash",
        "0x10000", firmware_path,
        "0x101000", firmware_path
    ]
    
    print(f"[upload_script] Running: {' '.join(cmd)}")
    try:
        subprocess.check_call(cmd)
        print("[upload_script] Both slots flashed successfully!")
        # Prevent default upload (we already did it)
        env["UPLOAD_PROTOCOL"] = "custom"
    except subprocess.CalledProcessError as e:
        print(f"[upload_script] ERROR: {e}")
        sys.exit(1)

env.AddPreAction("upload", before_upload)