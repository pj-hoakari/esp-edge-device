"""Import an EAP CA/client certificate, private key or binary PAC over UART.

The terminal must be disconnected. File bytes and private keys are never printed.
Configure the EAP profile first, especially the password for an encrypted key.
"""
import argparse
import base64
from pathlib import Path
import re
import time
import serial


def wait_for(port, token, timeout=15):
    end = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < end:
        data.extend(port.read(min(max(port.in_waiting, 1), 4096)))
        if b'CREDENTIAL_ERROR' in data:
            raise RuntimeError('Device rejected credential; check PEM/key password/PAC length and free NVS space.')
        if token in data:
            return
        if len(data) > 16384:
            del data[:-4096]
    raise TimeoutError('Device did not reach the credential upload prompt. Check firmware and normal boot.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=('ca', 'client', 'key', 'pac'))
    parser.add_argument('file', type=Path)
    parser.add_argument('--port', default='COM3')
    args = parser.parse_args()
    data = args.file.read_bytes()
    if args.kind in ('ca', 'client'):
        # openssl x509 -text exports often include several KiB of readable
        # metadata before the PEM. Transfer only certificate blocks, keeping
        # all members of a chain; the ESP still validates the certificates.
        certificates = re.findall(rb'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', data, re.DOTALL)
        if not certificates:
            parser.error('CA/client files must contain PEM CERTIFICATE blocks.')
        data = b'\n'.join(certificates) + b'\n'
    limit = 4096 if args.kind == 'pac' else 4095
    if not data or len(data) > limit:
        parser.error(f'File must be 1-{limit} bytes.')
    if args.kind == 'pac':
        if len(data) < 512:
            parser.error('The IDF EAP-FAST API requires a pre-provisioned binary PAC of at least 512 bytes.')
    elif b'\x00' in data or not data.startswith(b'-----BEGIN '):
        parser.error('CA/client/key files must be PEM text with no NUL bytes.')
    port = serial.Serial()
    port.port = args.port
    port.baudrate = 115200
    port.timeout = 0.1
    port.write_timeout = 3
    port.dtr = True
    port.rts = False
    port.open()
    complete = False
    try:
        # Cancel any unfinished input before entering the upload transaction.
        port.write(b'\x03\r\n')
        time.sleep(0.2)
        port.reset_input_buffer()
        port.write(f'credential {args.kind}\n'.encode('ascii'))
        wait_for(port, b'data> ')
        for offset in range(0, len(data), 48):
            port.write(base64.b64encode(data[offset:offset + 48]) + b'\n')
            wait_for(port, b'data> ')
        port.write(b'.\n')
        wait_for(port, f'CREDENTIAL_SAVED {args.kind}'.encode('ascii'))
        complete = True
        print(f'Stored {args.kind}: {len(data)} bytes. Reboot the ESP to apply.')
    finally:
        try:
            if not complete:
                port.write(b'\x03')
        finally:
            port.close()


if __name__ == '__main__':
    main()
