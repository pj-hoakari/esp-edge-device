"""Test UART credential framing and cleanup using a local serial adapter."""
import base64
from contextlib import redirect_stdout
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('upload_wifi_credential', PROJECT / 'tools/upload_wifi_credential.py')
UPLOAD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UPLOAD)


class Port:
    def __init__(self, reject=False):
        self.response = bytearray()
        self.data = bytearray()
        self.reject = reject
        self.closed = False

    @property
    def in_waiting(self):
        return len(self.response)

    def open(self):
        pass

    def close(self):
        self.closed = True

    def reset_input_buffer(self):
        self.response.clear()

    def write(self, data):
        if data.startswith(b'\x03'):
            self.response.extend(b'Cancelled.\nesp> ')
        elif data.startswith(b'credential '):
            self.kind = data.split()[1]
            self.response.extend(b'CREDENTIAL_READY\ndata> ')
        elif data == b'.\n':
            self.response.extend(b'CREDENTIAL_ERROR invalid' if self.reject else b'CREDENTIAL_SAVED ' + self.kind)
        else:
            self.data.extend(base64.b64decode(data, validate=False))
            self.response.extend(b'\ndata> ')

    def read(self, size):
        size = min(size, 3)  # Split prompt markers across reads.
        result = bytes(self.response[:size])
        del self.response[:size]
        return result


class UploadTest(unittest.TestCase):
    def invoke(self, data, kind='key', reject=False, expected=None):
        root = (PROJECT / '.tools').resolve()
        root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='upload-test-', dir=root) as directory:
            temp = Path(directory)
            self.assertTrue(temp.resolve().is_relative_to(root))
            file = temp / 'credential'
            file.write_bytes(data)
            port = Port(reject)
            output = io.StringIO()
            with patch.object(sys, 'argv', ['upload', kind, str(file)]), \
                    patch.object(UPLOAD.serial, 'Serial', return_value=port), \
                    patch.object(UPLOAD.time, 'sleep'), redirect_stdout(output):
                if reject:
                    with self.assertRaises(RuntimeError):
                        UPLOAD.main()
                else:
                    UPLOAD.main()
            self.assertTrue(port.closed)
            self.assertEqual(port.data, data if expected is None else expected)
            self.assertNotIn(base64.b64encode(data).decode(), output.getvalue())
            return output.getvalue()

    def test_pem_key_roundtrip(self):
        data = b'-----BEGIN PRIVATE KEY-----\n' + b'SecretKeyMaterial' * 20 + b'\n-----END PRIVATE KEY-----\n'
        result = self.invoke(data)
        self.assertNotIn('SecretKeyMaterial', result)
        self.assertIn('Stored key:', result)

    def test_binary_pac_roundtrip(self):
        self.invoke(bytes(range(256)) * 4, 'pac')

    def test_certificate_chain_with_openssl_text(self):
        cert = b'-----BEGIN CERTIFICATE-----\nFakeCert\n-----END CERTIFICATE-----\n'
        data = b'Certificate:\n' + b'Readable metadata\n' * 500 + cert + b'Certificate:\n' + cert
        self.invoke(data, 'client', expected=cert + cert)

    def test_rejection_always_closes_port(self):
        self.invoke(b'-----BEGIN PRIVATE KEY-----\ninvalid\n', reject=True)


if __name__ == '__main__':
    unittest.main()
