"""Capture Pico debugprobe UART logs with DTR enabled and Wi-Fi values redacted."""
import argparse
import json
from pathlib import Path
import re
import time
import serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--seconds', type=float, default=35)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--until-download', action='store_true')
    args = parser.parse_args()
    private = []
    config = Path(__file__).resolve().parents[1] / 'sdkconfig'
    if config.is_file():
        for line in config.read_text(encoding='utf-8').splitlines():
            if re.match(r'^CONFIG_WEBCAM_WIFI_(SSID|PASSWORD)=', line):
                value = json.loads(line.split('=', 1)[1])
                if value:
                    private.append(value)
    port = serial.Serial()
    port.port = args.port
    port.baudrate = 115200
    port.timeout = 0.1
    port.dtr = True
    port.rts = False
    port.open()
    print(f'{args.port} monitor ready, DTR=true ({args.seconds:g} seconds)', flush=True)
    data = bytearray()
    try:
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            data.extend(port.read(min(max(port.in_waiting, 1), 4096)))
            if args.until_download and b'waiting for download' in data:
                break
    finally:
        port.close()
    text = data.decode('utf-8', errors='replace')
    for value in private:
        text = text.replace(value, '[redacted]')
    if args.output:
        args.output.write_text(text, encoding='utf-8')
    selected = [line for line in text.splitlines() if re.search(
        r'ESP-ROM:|rst:|waiting for download|Project name:|App version:|ESP-IDF:|'
        r'esp_netif_handlers:|camera:|webcam:|mjpeg:|DTLS|Guru Meditation|'
        r'abort\(\)|Brownout|assert failed|Failed to', line)]
    print(json.dumps({'received_bytes': len(data), 'normal_boot': 'SPI_FAST_FLASH_BOOT' in text,
                      'lines': selected[-60:]}, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
