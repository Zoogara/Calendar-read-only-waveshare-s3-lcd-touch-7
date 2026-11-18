#!/usr/bin/env python3
"""Read or write the display's password-encrypted settings backup (gcal-settings.enc) without the display.

  gcal_settings.py decrypt gcal-settings.enc [-o settings.json]
  gcal_settings.py encrypt settings.json [-o gcal-settings.enc]

The passphrase comes from the GCAL_BACKUP_PASS environment variable, or is asked for (not echoed).
The decrypted file holds every password and key the display has, in the clear - treat it accordingly and
delete it when done. Needs the "cryptography" package (pip install cryptography).

Format (components/provisioning/include/settings_crypt.h): "GCALSET", version 1, PBKDF2-HMAC-SHA256
iterations (u32 big endian), 16-byte salt, 12-byte nonce, AES-256-GCM ciphertext and 16-byte tag, with the
first 40 bytes as the associated data.
"""
import argparse
import getpass
import hashlib
import os
import struct
import sys

MAGIC = b"GCALSET"
VERSION = 1
HEADER = 40
TAG = 16
MIN_ITER, MAX_ITER, DEFAULT_ITER = 10_000, 1_000_000, 100_000
MIN_PASSPHRASE = 12


class BackupError(Exception):
    pass


def _key(passphrase, salt, iterations):
    return hashlib.pbkdf2_hmac("sha256", passphrase.encode(), salt, iterations, 32)


def _aesgcm():
    try:
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    except ImportError:
        raise BackupError("this needs the 'cryptography' package: pip install cryptography")
    return AESGCM


def encrypt(passphrase, plain, iterations=DEFAULT_ITER, salt=None, nonce=None):
    if len(passphrase) < MIN_PASSPHRASE:
        raise BackupError(f"the passphrase must be at least {MIN_PASSPHRASE} characters")
    if not MIN_ITER <= iterations <= MAX_ITER:
        raise BackupError("iteration count out of range")
    salt = salt if salt is not None else os.urandom(16)
    nonce = nonce if nonce is not None else os.urandom(12)
    header = MAGIC + bytes([VERSION]) + struct.pack(">I", iterations) + salt + nonce
    # AESGCM.encrypt returns ciphertext || tag
    return header + _aesgcm()(_key(passphrase, salt, iterations)).encrypt(nonce, plain, header)


def decrypt(passphrase, blob):
    if len(blob) < HEADER + TAG or blob[:7] != MAGIC or blob[7] != VERSION:
        raise BackupError("not a gcal settings backup (or a newer format)")
    iterations = struct.unpack(">I", blob[8:12])[0]
    if not MIN_ITER <= iterations <= MAX_ITER:
        raise BackupError("not a gcal settings backup (iteration count out of range)")
    salt, nonce, header = blob[12:28], blob[28:40], blob[:HEADER]
    try:
        return _aesgcm()(_key(passphrase, salt, iterations)).decrypt(nonce, blob[HEADER:], header)
    except Exception as e:  # InvalidTag
        if isinstance(e, BackupError):
            raise
        raise BackupError("wrong passphrase, or the file has been changed")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("action", choices=["decrypt", "encrypt"])
    p.add_argument("file")
    p.add_argument("-o", "--output", help="write here instead of standard output (decrypt) / next to the input (encrypt)")
    a = p.parse_args()
    pw = os.environ.get("GCAL_BACKUP_PASS") or getpass.getpass("Backup passphrase: ")
    try:
        data = open(a.file, "rb").read()
        if a.action == "decrypt":
            out = decrypt(pw, data)
            if a.output:
                fd = os.open(a.output, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)   # owner-only
                with os.fdopen(fd, "wb") as f:
                    f.write(out)
                print(f"wrote {a.output} ({len(out)} bytes) - it holds your passwords in the clear", file=sys.stderr)
            else:
                sys.stdout.buffer.write(out)
        else:
            blob = encrypt(pw, data)
            dest = a.output or (os.path.splitext(a.file)[0] + ".enc")
            with open(dest, "wb") as f:
                f.write(blob)
            print(f"wrote {dest} ({len(blob)} bytes)", file=sys.stderr)
    except BackupError as e:
        sys.exit(f"error: {e}")
    except OSError as e:
        sys.exit(f"error: {e}")


if __name__ == "__main__":
    main()
