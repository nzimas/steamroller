"""Execute specific official update-receiver functions in simulated RAM only.

No MIDI, USB, firmware-image patching, or filesystem writes outside reports.
This examines application-side behavior, not bootloader acceptance.
"""

import json
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M7, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_PC)

from compare_platform import ROOT, BASE, IMAGES, digest
from inspect_firmware import ct_fw

CONFIG = {
    "tracks": {"gate": 0x08038CFC, "state": 0x20006934,
               "accepted_state": 0x20003800, "parser": 0x08024F70},
    "rhythm": {"gate": 0x08043350, "state": 0x20006E38,
               "accepted_state": 0x20003900, "parser": 0x0802D1D4},
}


def machine(image):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
    cpu.mem_map(0x08000000, 0x80000)
    cpu.mem_map(0x20000000, 0x50000)
    cpu.mem_write(BASE, image)
    cpu.reg_write(UC_ARM_REG_SP, 0x2004D000)
    cpu.reg_write(UC_ARM_REG_LR, 0x08000001)
    return cpu


def call(cpu, address, r0, r1):
    cpu.reg_write(UC_ARM_REG_R0, r0)
    cpu.reg_write(UC_ARM_REG_R1, r1)
    cpu.emu_start(address | 1, 0x08000000, count=10000, timeout=1_000_000)
    if cpu.reg_read(UC_ARM_REG_PC) != 0x08000000:
        raise RuntimeError("Function did not return within the execution limit")


def gate_case(image, config, message, length=None):
    cpu = machine(image)
    cpu.mem_write(config["state"], struct.pack("<I", 0xDEADBEEF))
    cpu.mem_write(0x2004E000, message)
    cpu.mem_write(0x2004E100, struct.pack("<II", 0x2004E000, len(message) if length is None else length))
    call(cpu, config["gate"], 0, 0x2004E100)
    value = struct.unpack("<I", cpu.mem_read(config["state"], 4))[0]
    if value not in (0xDEADBEEF, config["accepted_state"]):
        raise RuntimeError("Unexpected updater state")
    return value == config["accepted_state"]


def main():
    report = {"scope": "Local emulation of application functions only; no device messages", "images": {}}
    for label, (filename, _, _, expected_hash) in IMAGES.items():
        image = (ROOT / "firmware/official" / filename).read_bytes()
        if digest(image) != expected_hash:
            raise ValueError(f"Unexpected image: {filename}")
        syx = (ROOT / "firmware/official" / filename).with_suffix(".syx").read_bytes()
        decoded = ct_fw.decode(syx)
        packets = ct_fw.split(syx)
        init = ct_fw.HDR + bytes([ct_fw.UPDATE_INIT]) + packets[0][1] + b"\xf7"
        config = CONFIG[label]
        cases = {}
        for product in (0x63, 0x64):
            candidate = bytearray(init)
            candidate[7] = product
            cases[f"product_{product:02x}"] = gate_case(image, config, bytes(candidate))
        candidate = bytearray(init)
        candidate[6] = 0x11
        cases["wrong_family_11"] = gate_case(image, config, bytes(candidate))
        cases["short_length_8"] = gate_case(image, config, init, length=8)
        candidate = bytearray(init)
        candidate[5] = 0x70
        cases["wrong_command_70"] = gate_case(image, config, bytes(candidate))

        cpu = machine(image)
        cpu.mem_write(0x2004E000, packets[1][1])
        call(cpu, config["parser"], 0x2004E000, 0x2004E100)
        fields = struct.unpack("<4I", cpu.mem_read(0x2004E100, 16))
        # Unlike the size/CRC nibble fields, this helper accumulates the six
        # version digits with powers of ten (572 / 731), not powers of 16.
        version_digits = packets[1][1][1:7]
        if any(digit > 9 for digit in version_digits):
            raise ValueError("Official version field is not decimal digits")
        decimal_version = int("".join(str(digit) for digit in version_digits))
        expected = (0, decimal_version, decoded["size"], decoded["crc"])
        if fields != expected:
            raise RuntimeError(f"Header parser disagrees with container decoder: {label}: {fields!r} vs {expected!r}")
        expected_cases = {"product_63": label == "rhythm", "product_64": label == "tracks",
                          "wrong_family_11": False, "short_length_8": False, "wrong_command_70": False}
        if cases != expected_cases:
            raise RuntimeError(f"Product gate differs from static analysis: {label}")
        report["images"][label] = {
            "image_sha256": digest(image),
            "init_gate_address": hex(config["gate"]),
            "init_cases_select_update_state": cases,
            "header_parser_address": hex(config["parser"]),
            "version_digits": list(version_digits),
            "upstream_hex_interpretation": decoded["header_version"],
            "parsed_official_header": dict(zip(("type", "version_field", "image_size", "image_crc"), fields)),
        }
    path = ROOT / "docs/update-receiver-emulation.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
