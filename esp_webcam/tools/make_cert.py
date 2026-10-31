"""Create a unique local HTTPS certificate. Never overwrite an existing key."""
from datetime import datetime, timedelta, timezone
from pathlib import Path
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa

folder = Path(__file__).resolve().parents[1] / "certs"
folder.mkdir(exist_ok=True)
key_path, cert_path = folder / "server.key", folder / "server.crt"
if key_path.exists() or cert_path.exists():
    raise SystemExit("certs already exists; preserving the existing certificate/key")
key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "esp-webcam.local")])
now = datetime.now(timezone.utc)
cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
        .public_key(key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(days=1)).not_valid_after(now + timedelta(days=365))
        .add_extension(x509.SubjectAlternativeName([x509.DNSName("esp-webcam.local")]), critical=False)
        .sign(key, hashes.SHA256()))
key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                      serialization.NoEncryption()))
cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
print("Created local certificate and private key in certs/ (do not share or commit)")
