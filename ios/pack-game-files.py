#!/usr/bin/env python3
"""Packs the files the iOS cloud build needs into one encrypted archive.

    python3 ios/pack-game-files.py --iso "Skate 3.iso" \
        --tu TU_12K2276_000000C000000.00000000000O3 -o skate3-build-inputs.enc

Only default.xex, data/webkit/EAWebkit.xex (read straight out of your ISO) and
the title update package are included - a few tens of MB, not the whole disc.
The archive is encrypted with AES-256 (openssl, PBKDF2) using a password you
choose. Upload the .enc file somewhere only you control, then store its direct
download link and the password as repository secrets (see README, "iOS cloud
build").
"""

import argparse
import getpass
import io
import os
import struct
import subprocess
import sys
import tarfile

SECTOR = 2048
GAME_OFFSETS = (0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000)
MAGIC = b"MICROSOFT*XBOX*MEDIA"
WANTED = ("default.xex", "data/webkit/EAWebkit.xex")
TU_NAME = "TU_12K2276_000000C000000.00000000000O3"


def read_at(f, offset, size):
    f.seek(offset)
    data = f.read(size)
    if len(data) != size:
        raise SystemExit("error: unexpected end of ISO")
    return data


def find_files(iso_path):
    """Walks the XDVDFS directory trees (same layout the app's installer reads)."""
    with open(iso_path, "rb") as f:
        game_offset = None
        for candidate in GAME_OFFSETS:
            f.seek(candidate + 32 * SECTOR)
            if f.read(len(MAGIC)) == MAGIC:
                game_offset = candidate
                break
        if game_offset is None:
            raise SystemExit("error: not a recognized Xbox 360 game ISO")
        root_sector, _root_size = struct.unpack(
            "<II", read_at(f, game_offset + 32 * SECTOR + 20, 8))

        wanted = {w.lower(): w for w in WANTED}
        found = {}
        pending = [(game_offset + root_sector * SECTOR, 0, "")]
        visited = 0
        while pending and len(found) < len(wanted):
            directory, node, prefix = pending.pop()
            visited += 1
            if visited > 500000:
                raise SystemExit("error: ISO directory tree is unexpectedly large")
            header = read_at(f, directory + node, 14)
            left, right, sector, length, attributes, name_len = struct.unpack(
                "<HHIIBB", header)
            name = read_at(f, directory + node + 14, name_len).decode("ascii", "replace")
            if left:
                pending.append((directory, left * 4, prefix))
            if right:
                pending.append((directory, right * 4, prefix))
            path = prefix + name
            if attributes & 0x10:
                if length:
                    pending.append((game_offset + sector * SECTOR, 0, path + "/"))
            elif path.lower() in wanted:
                found[wanted[path.lower()]] = read_at(f, game_offset + sector * SECTOR, length)
        missing = [w for w in WANTED if w not in found]
        if missing:
            raise SystemExit("error: ISO is missing " + ", ".join(missing))
        return found


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True, help="your Skate 3 Xbox 360 ISO")
    parser.add_argument("--tu", required=True, help=f"the title update package ({TU_NAME})")
    parser.add_argument("-o", "--output", default="skate3-build-inputs.enc")
    args = parser.parse_args()

    files = find_files(args.iso)
    with open(args.tu, "rb") as tu:
        files[TU_NAME] = tu.read()

    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode="w:gz") as tar:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
            print(f"  + {name} ({len(data) / 1e6:.1f} MB)")

    password = os.environ.get("SKATE3_FILES_PASSWORD") or getpass.getpass(
        "Choose an encryption password (you'll store it as a GitHub secret): ")
    if len(password) < 12:
        raise SystemExit("error: use a password of at least 12 characters")
    env = dict(os.environ, SKATE3_FILES_PASSWORD=password)
    subprocess.run(
        ["openssl", "enc", "-aes-256-cbc", "-pbkdf2", "-iter", "200000", "-md", "sha256",
         "-salt", "-pass", "env:SKATE3_FILES_PASSWORD", "-out", args.output],
        input=archive.getvalue(), env=env, check=True)
    print(f"Wrote {args.output}. Upload it somewhere private and add the secrets "
          "SKATE3_FILES_URL and SKATE3_FILES_PASSWORD (see README).")


if __name__ == "__main__":
    sys.exit(main())
