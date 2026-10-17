"""Inspect official downloads offline; never opens a MIDI or USB device.

Requires the research/CIRCUITS-HACKS-RC checkout. Run from any directory.
"""

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "research/CIRCUITS-HACKS-RC/tools"))
import ct_fw


def inspect(path):
    raw = path.read_bytes()
    info = ct_fw.decode(raw)  # Includes CRC validation.
    image = info.pop("image")
    info.update(
        syx_sha256=ct_fw.sha256(raw),
        image_sha256=ct_fw.sha256(image),
        initial_sp=hex(struct.unpack_from("<I", image)[0]),
        reset_vector=hex(struct.unpack_from("<I", image, 4)[0]),
        version_block_hex=image[0x1C8:0x1EE].hex(),
        round_trip_identical=ct_fw.encode(image, info["proto"], info["product"]) == raw,
    )
    return info


if __name__ == "__main__":
    files = sorted((ROOT / "firmware/official").glob("*.syx"))
    if not files:
        sys.exit("No official .syx files found in firmware/official/")
    print(json.dumps({path.name: inspect(path) for path in files}, indent=2))
