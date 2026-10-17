"""Read local Tonitor sources and build an isolated FM reference in this project.

Does not modify Tonitor or connect to either instrument. Cortex-M7 compilation
is a build feasibility check, not a hardware timing or integration result.
"""

import hashlib
import importlib.util
import json
import math
import struct
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path("/Users/nzimas/development/mvave-fm1")
OUT = ROOT / "research/tonitor-reference"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf_sections(path):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("Expected little-endian ELF32")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    offset, stride, count, names_index = header[6], header[11], header[12], header[13]
    sections = [struct.unpack_from("<10I", data, offset + stride * i) for i in range(count)]
    names = sections[names_index]
    strings = data[names[4]:names[4] + names[5]]
    result = {}
    for section in sections:
        if section[2] & 2:  # SHF_ALLOC: includes unwind metadata; excludes relocations and debug info.
            name = strings[section[0]:].split(b"\0", 1)[0].decode()
            result[name] = section[5]
    return result


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    generator = SOURCE / "tools/gen_tables.py"
    spec = importlib.util.spec_from_file_location("tonitor_reference_tables", generator)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    tables = ["#include <stdint.h>", f"#define FS {module.FS}", f"#define CTL {module.CTL}"]
    tables += module.arr("SINE", "int16_t", [int(round(32767 * math.sin(2 * math.pi * i / 1024))) for i in range(1025)])
    tables += module.fm6_tables()
    header = OUT / "fm6_reference_tables.h"
    header.write_text("\n".join(tables) + "\n")
    core = SOURCE / "firmware/src/fm6_core.c"
    adapter = ROOT / "tools/tonitor_fm_reference.c"
    define = f'-DTONITOR_CORE_PATH="{core}"'
    common = ["clang", "-O2", "-std=c99", "-fwrapv", "-I", str(OUT), define]
    binary = OUT / "fm6-reference"
    subprocess.run(common + [str(adapter), "-o", str(binary)], check=True)
    raw = OUT / "all-algorithms-q24le.pcm"
    run = subprocess.run([str(binary), str(raw)], check=True, capture_output=True, text=True)
    facts = json.loads(run.stdout)
    # The reference should not change merely because host optimization changes.
    unoptimized = OUT / "fm6-reference-o0"
    subprocess.run(["-O0" if arg == "-O2" else arg for arg in common] +
                   [str(adapter), "-o", str(unoptimized)], check=True)
    raw_o0 = OUT / "all-algorithms-o0-q24le.pcm"
    subprocess.run([str(unoptimized), str(raw_o0)], check=True, capture_output=True)
    if sha(raw) != sha(raw_o0):
        raise RuntimeError("FM reference differs between host O0 and O2 builds")
    # Compile exactly the same core for the Rhythm's MCU instruction set.
    arm_object = OUT / "fm6-cortex-m7.o"
    arm_assembly = OUT / "fm6-cortex-m7.s"
    target = common + ["--target=arm-none-eabi", "-mcpu=cortex-m7", "-mthumb",
                       "-ffreestanding", "-fno-builtin", "-DREFERENCE_FREESTANDING=1"]
    subprocess.run(target + ["-fstack-usage", "-c", str(adapter), "-o", str(arm_object)], check=True)
    subprocess.run(target + ["-S", str(adapter), "-o", str(arm_assembly)], check=True)
    instructions = arm_assembly.read_text()
    # Toolchain helper calls need explicit integration if any are emitted.
    helpers = sorted(set(__import__("re").findall(r"\b(__aeabi_[A-Za-z0-9_]+)\b", instructions)))
    commit = subprocess.run(["git", "-C", str(SOURCE), "rev-parse", "HEAD"],
                            check=True, capture_output=True, text=True).stdout.strip()
    report = {
        "source_directory": str(SOURCE), "source_commit": commit,
        "source_sha256": {str(p.relative_to(SOURCE)): sha(p) for p in
                          (core, generator, SOURCE / "firmware/src/fm.c", SOURCE / "LICENSING.md")},
        "scope": "Unmodified FM6 core with synthetic patches; excludes Tonitor wrapper/macros/generator sound design and hardware integration",
        "compiler_integer_assumption": "-fwrapv for signed addition/subtraction/multiplication; signed-shift behavior still requires attention in a different implementation",
        "host_reference": facts, "q24_reference_sha256": sha(raw),
        "host_o0_o2_bit_identical": True,
        "generated_fm_tables_sha256": sha(header),
        "cortex_m7_allocated_object_sections": elf_sections(arm_object),
        "cortex_m7_aeabi_helper_symbols": helpers,
        "timing_measured_on_rhythm": False,
    }
    output = ROOT / "docs/tonitor-fm-reference.json"
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(output), "algorithms_rendered": len(facts["algorithms"]),
                      "note_state_bytes": facts["note_state_bytes"], "lfo_state_bytes": facts["lfo_state_bytes"],
                      "reference_voice_bytes": facts["reference_voice_bytes"],
                      "cortex_m7_allocated_sections": report["cortex_m7_allocated_object_sections"],
                      "aeabi_helpers": helpers}, indent=2))


if __name__ == "__main__":
    main()
