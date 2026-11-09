"""Writes the signed update manifest the firmware's updater fetches.

    HTRAM_SIGNING_KEY=<64 hex chars> python tools/sign_release.py htram-fw-1.4.0-ota.bin 1.4.0 <url> > htram-fw-ota.json
    python tools/sign_release.py --key-file ~/.htram/update-signing-key.txt ...

The key is the 32-byte Ed25519 seed, in hex. CI takes it from the repository
secret HTRAM_SIGNING_KEY; its public half is src/update_key.h. The signed
message must match signed_message() in src/updater.cpp.
"""
import argparse, hashlib, json, os, sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ap = argparse.ArgumentParser()
ap.add_argument("image")
ap.add_argument("version")
ap.add_argument("url", help="where the device downloads the image from")
ap.add_argument("--key-file")
a = ap.parse_args()

seed_hex = open(os.path.expanduser(a.key_file)).read().strip() if a.key_file else os.environ.get("HTRAM_SIGNING_KEY", "").strip()
if len(seed_hex) != 64:
    sys.exit("No signing key: set HTRAM_SIGNING_KEY or pass --key-file.")
key = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(seed_hex))

data = open(a.image, "rb").read()
sha = hashlib.sha256(data).hexdigest()
message = f"htram-fw-ota\n{a.version}\n{len(data)}\n{sha}".encode()
print(json.dumps({"version": a.version, "size": len(data), "sha256": sha,
                  "sig": key.sign(message).hex(), "url": a.url}, indent=1))
