#!/usr/bin/env python3
"""Generate a self-signed certificate/key pair using cryptography."""
from __future__ import annotations

import argparse
import ipaddress
from datetime import datetime, timedelta, timezone
from pathlib import Path

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
except ImportError as exc:  # pragma: no cover - dependency guard
    raise SystemExit(
        "error: missing dependency 'cryptography'. Install it via 'pip install cryptography'"
    ) from exc


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate a self-signed server certificate and private key",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("-d", "--days", type=int, default=365, help="Validity days")
    parser.add_argument("-C", "--country", default="CN", help="Country code")
    parser.add_argument("-ST", "--state", default="Beijing", help="State or province")
    parser.add_argument("-L", "--locality", default="Beijing", help="Locality or city")
    parser.add_argument("-O", "--org", default="co_wq", help="Organization name")
    parser.add_argument("-OU", "--org-unit", default="Dev", help="Organizational unit")
    parser.add_argument("-CN", "--common-name", default="localhost", help="Common name")
    parser.add_argument("-E", "--email", default="admin@example.com", help="Email address")
    parser.add_argument(
        "-o",
        "--outdir",
        type=Path,
        default=Path("certs"),
        help="Output directory for server.key/server.crt",
    )
    return parser


def build_subject(args: argparse.Namespace) -> x509.Name:
    attributes = [
        x509.NameAttribute(NameOID.COUNTRY_NAME, args.country),
        x509.NameAttribute(NameOID.STATE_OR_PROVINCE_NAME, args.state),
        x509.NameAttribute(NameOID.LOCALITY_NAME, args.locality),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, args.org),
        x509.NameAttribute(NameOID.ORGANIZATIONAL_UNIT_NAME, args.org_unit),
        x509.NameAttribute(NameOID.COMMON_NAME, args.common_name),
    ]
    if args.email:
        attributes.append(x509.NameAttribute(NameOID.EMAIL_ADDRESS, args.email))
    return x509.Name(attributes)


def build_subject_alt_name(common_name: str) -> x509.SubjectAlternativeName | None:
    if not common_name:
        return None
    try:
        entry = x509.IPAddress(ipaddress.ip_address(common_name))
    except ValueError:
        entry = x509.DNSName(common_name)
    return x509.SubjectAlternativeName([entry])


def generate_certificate(args: argparse.Namespace) -> tuple[bytes, bytes]:
    private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = build_subject(args)
    now = datetime.now(timezone.utc)
    builder = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(private_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - timedelta(minutes=1))
        .not_valid_after(now + timedelta(days=max(1, args.days)))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),
            critical=False,
        )
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(private_key.public_key()),
            critical=False,
        )
    )

    san = build_subject_alt_name(args.common_name)
    if san is not None:
        builder = builder.add_extension(san, critical=False)
    certificate = builder.sign(private_key=private_key, algorithm=hashes.SHA256())

    key_bytes = private_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.TraditionalOpenSSL,
        encryption_algorithm=serialization.NoEncryption(),
    )
    cert_bytes = certificate.public_bytes(serialization.Encoding.PEM)
    return key_bytes, cert_bytes


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    outdir: Path = args.outdir
    outdir.mkdir(parents=True, exist_ok=True)

    key_path = outdir / "server.key"
    crt_path = outdir / "server.crt"

    key_bytes, cert_bytes = generate_certificate(args)

    key_path.write_bytes(key_bytes)
    crt_path.write_bytes(cert_bytes)

    print("生成成功:")
    print(f"  私钥: {key_path}")
    print(f"  证书: {crt_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
