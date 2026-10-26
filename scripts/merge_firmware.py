"""Merge PlatformIO build output into a single flashable image.

Usage: python scripts/merge_firmware.py <env> <output.bin>
The image is written for flash offset 0x0 (bootloader, partitions, boot_app0, app).
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BOOTLOADER_OFFSET = {"featherv1": 0x0, "featherv2": 0x1000}


def find_boot_app0():
    for base in (Path.home() / ".platformio" / "packages").glob("framework-arduinoespressif32*"):
        candidate = base / "tools" / "partitions" / "boot_app0.bin"
        if candidate.exists():
            return candidate
    raise SystemExit("boot_app0.bin not found in PlatformIO packages")


def main():
    env, out = sys.argv[1], Path(sys.argv[2])
    build = ROOT / "firmware" / ".pio" / "build" / env
    parts = [
        (BOOTLOADER_OFFSET[env], build / "bootloader.bin"),
        (0x8000, build / "partitions.bin"),
        (0xE000, find_boot_app0()),
        (0x10000, build / "firmware.bin"),
    ]
    for _, path in parts:
        if not path.exists():
            raise SystemExit(f"missing {path}")
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, "-m", "esptool"]
    if env == "featherv1":
        cmd += ["--chip", "esp32s3"]
    else:
        cmd += ["--chip", "esp32"]
    cmd += ["merge-bin", "-o", str(out)]
    for offset, path in parts:
        cmd += [hex(offset), str(path)]
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
