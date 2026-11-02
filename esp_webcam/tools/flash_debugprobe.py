"""Keep CDC UART line coding stable while esptool adjusts read timeouts."""
import argparse
from pathlib import Path
import time
import serial
import esptool
from esptool.targets.esp32s3 import ESP32S3ROM


class StableTimeoutPort:
    def __init__(self, port):
        object.__setattr__(self, '_serial', port)
        object.__setattr__(self, '_timeout', 1.0)

    def __getattr__(self, name):
        return getattr(self._serial, name)

    def __setattr__(self, name, value):
        if name == 'timeout':
            object.__setattr__(self, '_timeout', value)
        elif name in ('baudrate', 'write_timeout') and getattr(self._serial, name) == value:
            return
        else:
            setattr(self._serial, name, value)

    @property
    def timeout(self):
        return self._timeout

    def read(self, size=1):
        result = bytearray()
        end = None if self._timeout is None else time.monotonic() + self._timeout
        while len(result) < size:
            result.extend(self._serial.read(size - len(result)))
            if end is not None and time.monotonic() >= end:
                break
        return bytes(result)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--build-dir', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'build',
                        help='Image directory (default: firmware build; use build-fast for EAP-FAST)')
    args = parser.parse_args()
    raw = serial.Serial()
    raw.port = args.port
    raw.baudrate = 115200
    raw.timeout = 0.02
    raw.write_timeout = 3
    raw.dtr = True
    raw.rts = False
    raw.open()
    try:
        port = StableTimeoutPort(raw)
        esp = ESP32S3ROM(port, baud=115200)
        raw.dtr = True
        time.sleep(0.3)
        esp.connect(mode='no-reset', attempts=3)
        print('TARGET:', esp.CHIP_NAME, esp.get_chip_description(), flush=True)
        if args.check:
            return
        build = args.build_dir.resolve()
        files = [(0, build / 'bootloader/bootloader.bin'),
                 (0x8000, build / 'partition_table/partition-table.bin'),
                 (0x10000, build / 'esp_webcam.bin')]
        for _, path in files:
            if not path.is_file():
                raise FileNotFoundError(path)
        argv = ['--chip', 'esp32s3', '--port', args.port, '--baud', '115200',
                '--before', 'no-reset', '--after', 'no-reset', 'write-flash',
                '--flash-mode', 'dio', '--flash-freq', '80m', '--flash-size', '8MB']
        for address, path in files:
            argv.extend([hex(address), str(path)])
        esptool.main(argv, esp=esp)
        print('FLASH_COMPLETE', flush=True)
    finally:
        if raw.is_open:
            raw.close()


if __name__ == '__main__':
    main()
