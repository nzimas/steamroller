"""Offline startup, static GPIO-record and DSP comparison of official images.

Run with .venv/bin/python tools/compare_platform.py. On macOS, Unicorn JIT
needs an execution context allowing JIT. No MIDI/USB APIs are used here.
"""

import hashlib
import json
import struct
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_CPU_ARM_CORTEX_M7, UC_ARM_REG_SP, UC_ARM_REG_PC

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x08010000
IMAGES = {
    "tracks": ("circuittracks-firmware-4486.bin", 0x0801D3C8, 0x08029700,
               "b897e0f01c403974eafae32074876234f2da7ef834796a485993acf01423f8da"),
    "rhythm": ("circuitrhythm-firmware-5706.bin", 0x08025688, 0x08031658,
               "d5b9ff5dadf2e284f80d69ecb7087486cf0df315a697bed8272af9de0e90df15"),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def initialize_ram(image, entry, *, return_cpu=False):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
    # Simulated MCU memory only. RCC/SCB writes affect these local buffers.
    for address, size in [(0x08000000, 0x80000), (0x20000000, 0x50000),
                          (0xE0000000, 0x100000), (0x40023000, 0x1000)]:
        cpu.mem_map(address, size)
    cpu.mem_write(BASE, image)
    sp, reset = struct.unpack_from("<II", image)
    cpu.reg_write(UC_ARM_REG_SP, sp)
    reached = []

    def stop(uc, address, size, context):
        reached.append(address)
        uc.emu_stop()

    stop_hook=cpu.hook_add(UC_HOOK_CODE, stop, begin=entry, end=entry)
    try:
        cpu.emu_start(reset, 0, count=3_000_000, timeout=10_000_000)
    except UcError as error:
        raise RuntimeError(f"Startup failed at {cpu.reg_read(UC_ARM_REG_PC):#x}") from error
    if not reached:
        raise RuntimeError("Startup did not reach the expected application entry")
    cpu.hook_del(stop_hook)
    if return_cpu:return cpu
    return bytes(cpu.mem_read(0x20000000, 0x50000))


def gpio_records(ram):
    # Candidate records: aligned GPIOA..GPIOE base followed by seven words.
    # We compare raw records; we do not assume that every record is used or
    # that static initialization proves identical physical board wiring.
    records = []
    for offset in range(0, len(ram) - 31, 4):
        fields = struct.unpack_from("<8I", ram, offset)
        if fields[0] in range(0x40020000, 0x40021001, 0x400):
            records.append({"address": hex(0x20000000 + offset),
                            "words": [hex(value) for value in fields]})
    return records


def dsp_blocks(image, table):
    blocks = []
    for pointer in struct.unpack_from("<4I", image, table - BASE):
        offset = pointer - BASE
        if not 0 <= offset <= len(image) - 6:
            raise ValueError("DSP pointer outside application")
        words = int.from_bytes(image[offset:offset + 3], "big")
        size = 6 + 3 * words
        if offset + size > len(image):
            raise ValueError("DSP block extends beyond application")
        blocks.append({"address": hex(pointer), "words": words,
                       "load_address": hex(int.from_bytes(image[offset + 3:offset + 6], "big")),
                       "sha256": digest(image[offset:offset + size])})
    return blocks


def main():
    result = {"scope": "Offline pre-main startup only; no peripheral or audio emulation", "images": {}}
    buffers = {}
    for label, (filename, entry, table, expected_hash) in IMAGES.items():
        image = (ROOT / "firmware/official" / filename).read_bytes()
        if digest(image) != expected_hash:
            raise ValueError(f"Unexpected image: {filename}")
        buffers[label] = image
        ram = initialize_ram(image, entry)
        result["images"][label] = {
            "image_sha256": digest(image), "pre_main_entry": hex(entry),
            "pre_main_entry_reached": True,
            "gpio_records": gpio_records(ram), "dsp_blocks": dsp_blocks(image, table),
        }
    tracks, rhythm = (result["images"][key] for key in ("tracks", "rhythm"))
    result["gpio_record_sequences_identical"] = (
        [r["words"] for r in tracks["gpio_records"]] ==
        [r["words"] for r in rhythm["gpio_records"]]
    )
    result["alternate_dsp_blocks_identical"] = all(
        tracks["dsp_blocks"][index]["sha256"] == rhythm["dsp_blocks"][index]["sha256"]
        for index in (1, 3)
    )
    # Complete SystemInit function through BX LR: 60 instruction bytes.
    ta, ra = 0x0801BC78 - BASE, 0x08023F50 - BASE
    result["system_init_60_bytes_identical"] = buffers["tracks"][ta:ta + 60] == buffers["rhythm"][ra:ra + 60]
    output = ROOT / "docs/platform-comparison.json"
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({
        "report": str(output),
        "gpio_record_counts": {k: len(v["gpio_records"]) for k, v in result["images"].items()},
        **{k: v for k, v in result.items() if k not in ("scope", "images")},
    }, indent=2))


if __name__ == "__main__":
    main()
