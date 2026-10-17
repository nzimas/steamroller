"""Offline per-track DSP accumulation checks, including Tonitor FM samples.

The real P:$06AB..$06C5 instructions execute in the local DSP interpreter;
state and the buffer-bank register are fixtures. No audio transport, full
processing loop, physical clock, or device access is exercised.
"""

import hashlib
import json
import re
import struct
import subprocess
import wave

from rhythm_dsp_probe import ROOT, BASE, IMAGES, unpack_stream, data_lines, digest

OUT = ROOT / "research/analysis/dsp/mix"
TOOL = ROOT / "research/analysis/dsptool-io"
START, STOP = 0x06AB, 0x06C6
BUFFER, BLOCK = 0x304F0, 32
SENTINEL, BIAS = 0x1234, 0x400


def word(value):
    return value & 0xFFFFFF


def signed(value):
    return value - 0x1000000 if value & 0x800000 else value


def put(space, address, values):
    return f"{space} {address:x} " + " ".join(f"{word(value):06x}" for value in values)


def execute(name, lines):
    path = OUT / f"{name}.txt"
    path.write_text("\n".join(INIT + lines) + "\n")
    result = subprocess.run([str(TOOL), "run", str(path)], capture_output=True, text=True, check=True)
    runs = re.findall(r"RAN (\d+) pc=([0-9a-f]+) ins=(\d+) cyc=(\d+)", result.stdout)
    if not runs or any(int(run[1], 16) != STOP for run in runs):
        raise RuntimeError("Accumulation routine did not reach the stop boundary")
    dumps = []
    for match in re.finditer(r"^Y ([0-9a-f]+):((?: [0-9a-f]{6})+)$", result.stdout, re.M):
        dumps.append((int(match[1], 16), [signed(int(value, 16)) for value in match[2].split()]))
    return runs, dumps


def block_lines(track, bank, gain, samples, initial):
    address = BUFFER + 64 * track + BLOCK * bank
    return ["IOY ffffd0 " + str(bank), put("Y", 0x1405, [track]),
            put("X", 0x800, [gain]), put("X", 0x2A02, samples),
            put("Y", address, [initial] * BLOCK),
            "PC 7000", f"STOP {STOP:x}", "RUN"]


def main():
    global INIT
    image = (ROOT / "firmware/official" / IMAGES["rhythm"][0]).read_bytes()
    if digest(image) != IMAGES["rhythm"][3]:
        raise ValueError("Unexpected Rhythm image")
    reference_path = ROOT / "research/tonitor-reference/all-algorithms-q24le.pcm"
    reference_report = json.loads((ROOT / "docs/tonitor-fm-reference.json").read_text())
    raw = reference_path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != reference_report["q24_reference_sha256"]:
        raise ValueError("FM reference hash mismatch; rebuild and review its report")
    source = struct.unpack(f"<{len(raw)//4}i", raw)
    maps, _, _, _ = unpack_stream(image)
    OUT.mkdir(parents=True, exist_ok=True)
    program = OUT / "core1-P.bin"
    program.write_bytes(b"".join(maps["P"].get(a, 0).to_bytes(3, "big") for a in range(max(maps["P"]) + 1)))
    INIT = [f"LOADP {program} 0"] + data_lines(maps) + [
        # A research-only entry stub sets the current gain pointer, then
        # jumps into the unchanged accumulation instructions.
        "P 7000 60f400 000800 0c06ab", "X 1 20", "MAX 10000"]
    controls = [-0x400000, -0x80000, -16, 0, 16, 0x80000, 0x400000, 0] * 4
    controlled = []
    maximum_error = 0
    for track in range(8):
        for bank in range(2):
            for gain in (0, 0x400000, 0x7FFFFF):
                address = BUFFER + 64 * track + BLOCK * bank
                lines = [put("Y", BUFFER, [SENTINEL] * 512)]
                lines += block_lines(track, bank, gain, controls, BIAS)
                lines += [f"DUMPY {BUFFER:x} 512"]
                runs, dumps = execute(f"control-t{track}-b{bank}-g{gain:x}", lines)
                if len(runs) != 1 or len(dumps) != 1 or len(dumps[0][1]) != 512:
                    raise RuntimeError("Unexpected controlled probe output")
                values = dumps[0][1]
                offset = address - BUFFER
                if any(value != SENTINEL for i, value in enumerate(values) if not offset <= i < offset + BLOCK):
                    raise RuntimeError("Neighboring track/bank was modified")
                errors = [abs(values[offset + i] - (BIAS + ((sample * gain + (1 << 22)) >> 23)))
                          for i, sample in enumerate(controls)]
                error = max(errors)
                if error > 1:
                    raise RuntimeError(f"Unexpected fixed-point gain error: {error}")
                maximum_error = max(maximum_error, error)
                controlled.append({"track": track, "bank": bank, "gain_q23": gain,
                                   "max_lsb_error": error, "neighbors_unchanged": True,
                                   "interpreter_instructions": int(runs[0][2])})

    # Exercise all previously rendered FM algorithms. These are diagnostic
    # patches, with a release halfway through each 16,384-sample segment.
    fm_lines = []
    for index in range(0, len(source), BLOCK):
        algorithm = index // 16384
        track = algorithm % 8
        bank = (index // BLOCK) % 2
        samples = [value >> 4 for value in source[index:index + BLOCK]]
        if any(not -0x800000 <= value <= 0x7FFFFF for value in samples):
            raise RuntimeError("FM conversion exceeds signed 24-bit headroom")
        fm_lines += block_lines(track, bank, 0x400000, samples, 0)
        fm_lines += [f"DUMPY {BUFFER + 64 * track + BLOCK * bank:x} {BLOCK}"]
    runs, dumps = execute("tonitor-all-algorithms", fm_lines)
    if len(runs) != len(source) // BLOCK or len(dumps) != len(runs):
        raise RuntimeError("FM block count mismatch")
    output = []
    fm_error = 0
    for index, (address, values) in enumerate(dumps):
        algorithm = (index * BLOCK) // 16384
        expected_address = BUFFER + 64 * (algorithm % 8) + BLOCK * (index % 2)
        if address != expected_address or len(values) != BLOCK:
            raise RuntimeError("FM output address/count mismatch")
        samples = [value >> 4 for value in source[index * BLOCK:(index + 1) * BLOCK]]
        error = max(abs(value - ((sample + 1) >> 1)) for value, sample in zip(values, samples))
        if error > 1:
            raise RuntimeError("FM gain differs from reference by more than one DSP LSB")
        fm_error = max(fm_error, error)
        output.extend(values)
    pcm24 = b"".join(word(value).to_bytes(3, "little") for value in output)
    audio = OUT / "tonitor-fm-through-rhythm-accumulator.wav"
    with wave.open(str(audio), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(3)
        wav.setframerate(reference_report["host_reference"]["sample_rate"])
        wav.writeframes(pcm24)
    report = {
        "scope": __doc__.strip(), "image_sha256": digest(image),
        "reference_q24_sha256": reference_report["q24_reference_sha256"],
        "controlled_cases": controlled, "controlled_max_lsb_error": maximum_error,
        "fm_algorithms": 32, "fm_blocks": len(runs), "fm_samples": len(output),
        "fm_max_lsb_error": fm_error, "fm_input_conversion": "signed Q24 >> 4, yielding Q23 with 18 dB pre-attenuation",
        "fm_stage_gain_q23": 0x400000, "fm_output_pcm24_sha256": hashlib.sha256(pcm24).hexdigest(),
        "fm_output_peak_q23": max(abs(value) for value in output),
        "diagnostic_wav": str(audio), "sample_rate_for_file_only": 44100,
        "physical_audio_clock_validated": False, "whole_dsp_pipeline_validated": False,
        "hardware_writes": False,
    }
    path = ROOT / "docs/rhythm-mix-probe.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(path), "controlled_cases": len(controlled),
                      "fm_blocks": len(runs), "fm_samples": len(output),
                      "max_fm_lsb_error": fm_error, "diagnostic_wav": str(audio)}, indent=2))


if __name__ == "__main__":
    main()
