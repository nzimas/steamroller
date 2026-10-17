"""Offline capture of Rhythm 5706 ARM sampler command preparation.

Emulates official ARM functions. Replaces selected dependencies with logging
stubs; does not emulate DSP consumption, sound, the sequencer, or live hardware.
"""

import json
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M7, UC_ARM_REG_SP,
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)

from compare_platform import ROOT, BASE, IMAGES, digest

NOTE_ON = 0x08040874
NOTE_OFF = 0x0804083C
FORCE_STOP = 0x08040814
PREPARE = 0x0803081C
WRITE_PARAMETER = 0x08031200
QUEUE_PAIR = 0x080406A8
WRITE_MEMORY = 0x08031228


class Probe:
    def __init__(self, image, execute_queue=False):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
        self.cpu.mem_map(0x08000000, 0x80000)
        self.cpu.mem_map(0x20000000, 0x50000)
        self.cpu.mem_write(BASE, image)
        # Resolve literals referenced by the inspected functions instead of
        # assigning guessed runtime object addresses.
        self.notes = struct.unpack_from("<I", image, 0x080408D0 - BASE)[0]
        self.settings = struct.unpack_from("<I", image, 0x080408D4 - BASE)[0]
        for address in (self.notes, self.settings):
            if not 0x20000000 <= address < 0x2004FF00:
                raise ValueError("Unexpected state pointer")
        self.cpu.mem_write(self.notes, bytes([0xFF]) * 16)
        self.events = []
        self.execute_queue = execute_queue
        self.cpu.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, cpu, address, size, context):
        args = [cpu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)]
        if address == PREPARE:
            self.events.append({"stub": "sample_prepare", "track_argument": args[0]})
        elif address == WRITE_PARAMETER:
            self.events.append({"stub": "dsp_parameter_write", "arguments": args})
        elif address == QUEUE_PAIR:
            if self.execute_queue:
                return
            pair = struct.unpack("<II", cpu.mem_read(args[0], 8))
            self.events.append({"stub": "queue_pair", "words": [hex(x) for x in pair]})
        elif address == WRITE_MEMORY:
            self.events.append({"stub": "dsp_memory_write", "arguments": args})
        else:
            return
        cpu.reg_write(UC_ARM_REG_R0, 0)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    def call(self, function, track, note=0, velocity=0):
        self.events = []
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2004F000)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x08000001)
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), (track, note, velocity)):
            self.cpu.reg_write(reg, value)
        self.cpu.emu_start(function | 1, 0x08000000, count=10000, timeout=1_000_000)
        if self.cpu.reg_read(UC_ARM_REG_PC) != 0x08000000:
            raise RuntimeError("Probe did not return")
        return list(self.events)


def pairs(events):
    return [event["words"] for event in events if event["stub"] == "queue_pair"]


def main():
    filename, _, _, expected = IMAGES["rhythm"]
    image = (ROOT / "firmware/official" / filename).read_bytes()
    if digest(image) != expected:
        raise ValueError("Unexpected Rhythm image")
    cases = []
    for track in range(8):
        probe = Probe(image)
        events = probe.call(NOTE_ON, track, 60, 100)
        want = [[hex(0x900000 | (track << 16) | (100 << 8) | track), "0x0"]]
        if pairs(events) != want:
            raise RuntimeError(f"Unexpected note-on pair for track {track}")
        cases.append({"case": "note_on", "track": track, "note": 60, "velocity": 100, "events": events})
    probe = Probe(image)
    first = probe.call(NOTE_ON, 2, 60, 100)
    retrigger = probe.call(NOTE_ON, 2, 72, 80)
    if pairs(retrigger) != [["0x820002", "0x3"], ["0x925002", "0xc0000"]]:
        raise RuntimeError("Unexpected retrigger pairs")
    cases.append({"case": "retrigger_same_track", "track": 2, "note": 72, "events": retrigger})
    # Only the boolean at settings + track*16 + 1 is assumed here; its UI
    # meaning remains a hypothesis until parameter descriptors are mapped.
    probe.cpu.mem_write(probe.settings + 2 * 16 + 1, b"\x01")
    wrong = probe.call(NOTE_OFF, 2, 60)
    correct = probe.call(NOTE_OFF, 2, 72)
    if pairs(wrong) or pairs(correct) != [["0x820002", "0x2"]]:
        raise RuntimeError("Unexpected conditional release behavior")
    cases.extend([{"case": "release_old_note", "events": wrong},
                  {"case": "release_current_note_flag_1", "events": correct}])
    probe = Probe(image)
    probe.call(NOTE_ON, 2, 60, 100)
    ignored = probe.call(NOTE_OFF, 2, 60)
    stopped = probe.call(FORCE_STOP, 2)
    if pairs(ignored) or pairs(stopped) != [["0x820002", "0x3"]]:
        raise RuntimeError("Unexpected flag-0 release or forced-stop behavior")
    cases.extend([{"case": "release_current_note_flag_0", "events": ignored},
                  {"case": "force_stop", "events": stopped}])
    # Execute the queue producer itself, stopping only at its DSP-write API.
    ring_probe = Probe(image, execute_queue=True)
    memory_writes = ring_probe.call(NOTE_ON, 0, 60, 100)
    actual = [event["arguments"] for event in memory_writes if event["stub"] == "dsp_memory_write"]
    if actual != [[0, 0x90640000, 0x30410], [0, 0, 0x30411], [0, 0x100, 0x307B5]]:
        raise RuntimeError("Unexpected command-ring memory writes")
    wrap_probe = Probe(image, execute_queue=True)
    wrap_probe.cpu.mem_write(0x2004E000, struct.pack("<II", 0x900000, 0))
    wrap_events = []
    for _ in range(16):
        wrap_events = wrap_probe.call(QUEUE_PAIR, 0x2004E000)
    if wrap_events[-1]["arguments"] != [0, 0, 0x307B5]:
        raise RuntimeError("Command ring did not wrap after 16 pairs")
    report = {"scope": __doc__.strip(), "image_sha256": digest(image),
              "state_addresses": {"notes": hex(probe.notes), "settings": hex(probe.settings)},
              "functions": {"note_on": hex(NOTE_ON), "note_off": hex(NOTE_OFF),
                            "force_stop": hex(FORCE_STOP), "queue_pair": hex(QUEUE_PAIR)},
              "cases": cases,
              "ring_probe": {"note_on_events": memory_writes,
                             "wrap_after_16_pairs_verified": True,
                             "last_wrap_events": wrap_events}}
    path = ROOT / "docs/rhythm-note-probe.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"report": str(path), "cases_verified": len(cases),
                      "state_addresses": report["state_addresses"]}, indent=2))


if __name__ == "__main__":
    main()
