#!/usr/bin/env python3
"""Compile the real Murphy UC8253 driver with a recording bus and host Arduino shims."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / "src"
STUBS = HERE / "pro_stubs"
with tempfile.TemporaryDirectory(prefix="uc8253_murphy-test-") as directory:
    root = Path(directory)
    for folder in ("driver", "bus", "lut"):
        (root / folder).mkdir()
    for name in ("Uc8253MurphyDriver.cpp", "Uc8253MurphyDriver.h", "PanelDriver.h", "RestTextCodes.h"):
        shutil.copy2(SOURCE / "driver" / name, root / "driver" / name)
    shutil.copy2(SOURCE / "lut/Uc8253MurphyLuts.h", root / "lut/Uc8253MurphyLuts.h")
    shutil.copy2(SOURCE.parent / "include/GrayscaleCapabilities.h", root / "GrayscaleCapabilities.h")
    panel = root / "driver/PanelDriver.h"
    panel.write_text(panel.read_text().replace("../../include/GrayscaleCapabilities.h", "../GrayscaleCapabilities.h"))
    shutil.copy2(STUBS / "EpdBus.h", root / "bus/EpdBus.h")
    for name in ("Arduino.h", "esp_heap_caps.h", "BoardConfig.h"):
        shutil.copy2(STUBS / name, root / name)
    subprocess.check_call(
        [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wno-unused-parameter", f"-I{root}",
         str(HERE / "test_uc8253_murphy.cpp"), str(root / "driver/Uc8253MurphyDriver.cpp"),
         "-o", str(root / "test")]
    )
    subprocess.check_call([root / "test"])

# The display facade over the real driver: a pending refresh against the end-of-frame power off.
LIB = HERE.parents[1]
with tempfile.TemporaryDirectory(prefix="uc8253_murphy-facade-test-") as directory:
    root = Path(directory)
    for folder in ("src/driver", "src/bus", "src/lut", "include"):
        (root / folder).mkdir(parents=True)
    for name in ("Uc8253MurphyDriver.cpp", "Uc8253MurphyDriver.h", "PanelDriver.h", "RestTextCodes.h"):
        shutil.copy2(SOURCE / "driver" / name, root / "src/driver" / name)
    shutil.copy2(SOURCE / "lut/Uc8253MurphyLuts.h", root / "src/lut/Uc8253MurphyLuts.h")
    shutil.copy2(SOURCE / "FreeInkDisplay.cpp", root / "src/FreeInkDisplay.cpp")
    for name in ("FreeInkDisplay.h", "GrayscaleCapabilities.h"):
        shutil.copy2(LIB / "include" / name, root / "include" / name)
    shutil.copy2(STUBS / "EpdBus.h", root / "src/bus/EpdBus.h")
    for name in ("Arduino.h", "BoardConfig.h", "SPI.h", "esp_heap_caps.h", "sdkconfig.h"):
        shutil.copy2(STUBS / name, root / name)
    subprocess.check_call(
        [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wno-unused-parameter", "-Wno-unused-variable", "-DARDUINO=1",
         "-DFREEINK_DRIVER_UC8253_MURPHY=1", "-DFREEINK_MURPHY_GRAY=1", f"-I{root}", f"-I{root / 'include'}",
         f"-I{root / 'src'}", str(HERE / "test_uc8253_murphy_facade.cpp"), str(root / "src/FreeInkDisplay.cpp"),
         str(root / "src/driver/Uc8253MurphyDriver.cpp"), "-o", str(root / "test")]
    )
    subprocess.check_call([root / "test"])
