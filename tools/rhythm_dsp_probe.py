"""Reconstruct Rhythm core-1 P/X/Y segments and probe its command consumer.

Uses the locally built interpreter-only dsptool. No device connection.
"""

import json
import re
import struct
import subprocess
from pathlib import Path

from compare_platform import ROOT, BASE, IMAGES, digest

OUT = ROOT / "research/analysis/dsp"
TOOL = ROOT / "research/analysis/dsptool"
CONSUMER = 0x04CD
STUB = 0x7000


def unpack_stream(image):
    address = struct.unpack_from("<I", image, 0x08031660 - BASE)[0]
    offset = address - BASE
    count = int.from_bytes(image[offset:offset + 3], "big")
    raw = image[offset + 6:offset + 6 + 3 * count]
    words = [int.from_bytes(raw[i:i + 3], "big") for i in range(0, len(raw), 3)]
    cursor, segments = 0, []
    maps = {space: {} for space in "PXY"}
    while cursor < len(words):
        kind = words[cursor]
        cursor += 1
        if kind > 2:
            terminator = kind
            break
        size, destination = words[cursor:cursor + 2]
        cursor += 2
        if size < 1 or cursor + size > len(words):
            raise ValueError("Invalid segment length")
        space = "PXY"[kind]
        for i, word in enumerate(words[cursor:cursor + size]):
            if destination + i in maps[space]:
                raise ValueError("Overlapping segment")
            maps[space][destination + i] = word
        segments.append({"space": space, "address": destination, "words": size})
        cursor += size
    else:
        raise ValueError("No end marker")
    if terminator != 0x100:
        raise ValueError("Unexpected end marker")
    return maps, segments, count, words[cursor:]


def data_lines(maps):
    lines = []
    for space in "XY":
        addresses = sorted(maps[space])
        for address in addresses:
            lines.append(f"{space} {address:x} {maps[space][address]:06x}")
    return lines


def run_case(name, initial, dumps, calls=1):
    lines = list(INIT)
    lines += initial
    lines += [f"P {STUB:x} 0bf080 {CONSUMER:06x} {0x0C0000 | (STUB + 2):06x}", "MAX 10000"]
    for _ in range(calls):
        lines += [f"PC {STUB:x}", f"STOP {STUB + 2:x}", "RUN"]
    lines += dumps
    path = OUT / f"probe-{name}.txt"
    path.write_text("\n".join(lines) + "\n")
    result = subprocess.run([str(TOOL), "run", str(path)], capture_output=True, text=True, check=True)
    runs = re.findall(r"RAN (\d+) pc=([0-9a-f]+) ins=(\d+) cyc=(\d+)", result.stdout)
    if len(runs) != calls or any(int(run[1], 16) != STUB + 2 for run in runs):
        raise RuntimeError(f"Consumer did not return: {result.stdout}")
    values = {}
    for match in re.finditer(r"^([XY]) ([0-9a-f]+):((?: [0-9a-f]{6})+)$", result.stdout, re.M):
        values[f"{match[1]}:{int(match[2], 16):x}"] = [int(word, 16) for word in match[3].split()]
    return {"runs": [{"instructions_including_call_stub": int(run[2]),
                      "interpreter_cycles_including_call_stub": int(run[3])} for run in runs],
            "memory": values}


def main():
    global INIT
    filename, _, _, expected = IMAGES["rhythm"]
    image = (ROOT / "firmware/official" / filename).read_bytes()
    if digest(image) != expected:
        raise ValueError("Unexpected firmware image")
    if not TOOL.exists():
        raise SystemExit("Build research/analysis/dsptool first; see DSP-CONSUMER-FINDINGS.md")
    maps, segments, count, trailing = unpack_stream(image)
    OUT.mkdir(parents=True, exist_ok=True)
    program = b"".join(maps["P"].get(a, 0).to_bytes(3, "big") for a in range(max(maps["P"]) + 1))
    path = OUT / "rhythm-core1-P.bin"
    path.write_bytes(program)
    INIT = [f"LOADP {path} 0"] + data_lines(maps)
    cases = {}
    cases["empty"] = run_case("empty", ["Y 307b5 0 0"], ["DUMPY 307b5 2"])
    if cases["empty"]["memory"]["Y:307b5"] != [0, 0]:
        raise RuntimeError("Empty ring modified cursors")
    for track in range(8):
        command = 0x900000 | track << 16 | 100 << 8 | track
        mailbox = 0x307B7 + 2 * track
        state = 0x4A + 16 * track
        result = run_case(f"trigger-{track}",
            ["Y 307b5 1 0", f"Y 30410 {command:06x} 0c0000"],
            ["DUMPY 307b5 2", f"DUMPY {mailbox:x} 2", f"DUMPX {state:x} 1"])
        mem = result["memory"]
        if mem["Y:307b5"] != [1, 1] or mem[f"Y:{mailbox:x}"] != [command, 0x0C0000] or mem[f"X:{state:x}"] != [4]:
            raise RuntimeError(f"Unexpected trigger state: {track}")
        cases[f"trigger_{track}"] = result
    result = run_case("release", ["Y 307b5 1 0", "Y 30410 820002 000003"],
                      ["DUMPY 307b5 2", "DUMPX 6a 1"])
    if result["memory"]["X:6a"] != [2]:
        raise RuntimeError("Slot-2 release state mismatch")
    cases["slot_2_release"] = result
    result = run_case("parameter", ["Y 307b5 1 0", "Y 30410 b20102 000003"],
                      ["DUMPY 307b5 2", "DUMPY 30809 1"])
    if result["memory"]["Y:30809"] != [3]:
        raise RuntimeError("Slot-2 parameter destination mismatch")
    cases["slot_2_parameter_1"] = result
    result = run_case("two-events-one-call", ["Y 307b5 2 0", "Y 30410 906400 0 916401 0"],
                      ["DUMPY 307b5 2", "DUMPY 307b7 4"])
    if result["memory"]["Y:307b5"] != [2, 1] or result["memory"]["Y:307b7"] != [0x906400, 0, 0, 0]:
        raise RuntimeError("Unexpected single-call event consumption")
    cases["two_events_one_call"] = result
    result = run_case("two-events-two-calls", ["Y 307b5 2 0", "Y 30410 906400 0 916401 0"],
                      ["DUMPY 307b5 2", "DUMPY 307b7 4"], calls=2)
    if result["memory"]["Y:307b5"] != [2, 2] or result["memory"]["Y:307b7"] != [0x906400, 0, 0x916401, 0]:
        raise RuntimeError("Unexpected two-call event consumption")
    cases["two_events_two_calls"] = result
    report = {
        "scope": "Isolated core-1 consumer in local DSP interpreter; synthetic ring inputs, no audio or device execution",
        "image_sha256": digest(image), "download_payload_words": count,
        "segments": segments, "populated_words": {k: len(v) for k, v in maps.items()},
        "last_populated_address": {k: hex(max(v)) for k, v in maps.items()},
        "end_marker": "0x100", "trailing_words": [hex(w) for w in trailing],
        "consumer_program_address": hex(CONSUMER), "cases": cases,
    }
    output = ROOT / "docs/rhythm-dsp-consumer-probe.json"
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(output), "populated_words": report["populated_words"],
                      "cases_verified": len(cases)}, indent=2))


if __name__ == "__main__":
    main()
