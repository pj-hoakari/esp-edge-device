"""Flash a built ESP32-S3 image over a normal COM/USB serial port.

Use this for a USB-UART bridge or the USB Serial/JTAG port. For the Pico
debugprobe UART, keep using flash_debugprobe.py (it keeps the CDC line coding
stable and uses no-reset).

Run inside the ESP-IDF Python environment, from esp_webcam/:

    python tools/flash_serial.py --port COM5 --check
    python tools/flash_serial.py --port COM5
    python tools/flash_serial.py --port COM5 --erase
    python tools/flash_serial.py --port COM5 --build-dir build-fast
"""

import argparse
import json
import sys
from pathlib import Path

DEFAULT_FILES = {
    "0x0": "bootloader/bootloader.bin",
    "0x8000": "partition_table/partition-table.bin",
    "0x10000": "esp_webcam.bin",
}


def load_flasher_args(build: Path):
    path = build / "flasher_args.json"
    if not path.is_file():
        return {}, {}, {}
    data = json.loads(path.read_text(encoding="utf-8"))
    return (
        data.get("extra_esptool_args", {}),
        data.get("flash_settings", {}),
        data.get("flash_files", {}),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="COM5 or /dev/cu.usbserial-...")
    parser.add_argument("--baud", type=int, default=460800)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "build",
        help="Image directory (default: build; use build-fast for EAP-FAST)",
    )
    parser.add_argument(
        "--before",
        default=None,
        choices=("default_reset", "no_reset", "no_reset_no_sync"),
    )
    parser.add_argument(
        "--after",
        default=None,
        choices=("hard_reset", "soft_reset", "no_reset", "no_reset_no_sync"),
    )
    parser.add_argument("--check", action="store_true", help="Connect and print the chip/flash ID only")
    parser.add_argument("--erase", action="store_true", help="Erase the flash before writing")
    args = parser.parse_args()

    try:
        import esptool
    except ImportError:
        print(
            "esptool is not importable. Run this with the ESP-IDF Python "
            "(export.sh / export.bat) or use idf.py -p <port> flash.",
            file=sys.stderr,
        )
        return 2

    build = args.build_dir.resolve()
    extra, settings, files = load_flasher_args(build)
    if not files:
        files = dict(DEFAULT_FILES)

    missing = [str(build / name) for name in files.values() if not (build / name).is_file()]
    if missing:
        print("Missing build output. Run idf.py build first:", file=sys.stderr)
        for name in missing:
            print(f"  {name}", file=sys.stderr)
        return 1

    chip = extra.get("chip", "esp32s3")
    before = args.before or extra.get("before", "default_reset")
    after = args.after or extra.get("after", "hard_reset")
    flash_mode = settings.get("flash_mode", "dio")
    flash_freq = settings.get("flash_freq", "80m")
    flash_size = settings.get("flash_size", "8MB")

    base = [
        "--chip", chip,
        "--port", args.port,
        "--baud", str(args.baud),
        "--before", before,
        "--after", after,
    ]

    if args.check:
        esptool.main(base + ["flash-id"])
        return 0

    if args.erase:
        print("Erasing flash...", flush=True)
        esptool.main(base + ["erase-flash"])

    argv = base + [
        "write-flash",
        "--flash-mode", flash_mode,
        "--flash-freq", flash_freq,
        "--flash-size", flash_size,
    ]
    for offset, name in files.items():
        argv.extend([offset, str(build / name)])

    print("Writing:", " ".join(argv), flush=True)
    esptool.main(argv)
    print("FLASH_COMPLETE", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
