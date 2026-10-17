"""Connect captured ARM write bytes to unmodified DSP receive handlers offline.

SHI word input, GPIO command/data state and shared-memory aliasing are explicit
fixtures. Handlers are entered manually and stop before RTI; no real-time or
physical SPI, interrupt dispatch, dual-core execution or buffer scheduling.
"""
import hashlib
import json
import re
import struct
import subprocess

from rhythm_dsp_probe import ROOT, BASE, IMAGES, unpack_stream, data_lines, digest
from rhythm_transfer_probe import Probe as ArmProbe

TOOL = ROOT / 'research/analysis/dsptool-io'
OUT = ROOT / 'research/analysis/dsp/receiver'
BUFFER = 0x304F0
SENTINEL = 0x112233


def put(space, address, values):
    return f'{space} {address:x} ' + ' '.join(f'{x & 0xFFFFFF:06x}' for x in values)


def receive(core, word, command):
    start, stop = ((0x150, 0x1AD) if core == 0 else (0x6D0, 0x729))
    return [f'HOSTC {0x100 if command else 0:x}', f'SHI {word:06x}',
            f'PC {start:x}', f'STOP {stop:x}', 'RUN']


def execute(name, initial, lines):
    path = OUT / f'{name}.txt'
    path.write_text('\n'.join(initial + lines) + '\n')
    result = subprocess.run([str(TOOL), 'run', str(path)], capture_output=True,
                            text=True, check=True)
    runs = re.findall(r'RAN (\d+) pc=([0-9a-f]+)', result.stdout)
    if not runs or any(int(pc, 16) not in (0x1AD, 0x729, 0x234) for _, pc in runs):
        raise RuntimeError(f'Unexpected handler stop: {result.stdout[:1000]}')
    memory = {}
    for match in re.finditer(r'^([XYP]) ([0-9a-f]+):((?: [0-9a-f]{6})+)$', result.stdout, re.M):
        memory[f'{match[1]}:{int(match[2],16):x}'] = [int(x, 16) for x in match[3].split()]
    return memory, len(runs)


def main():
    image = (ROOT / 'firmware/official' / IMAGES['rhythm'][0]).read_bytes()
    if digest(image) != IMAGES['rhythm'][3]:
        raise ValueError('Unexpected firmware')
    maps, _, _, _ = unpack_stream(image)
    OUT.mkdir(parents=True, exist_ok=True)
    core1 = OUT / 'core1-P.bin'
    core1.write_bytes(b''.join(maps['P'].get(a, 0).to_bytes(3, 'big') for a in range(max(maps['P']) + 1)))
    pointer = struct.unpack_from('<I', image, 0x08031658 - BASE)[0]
    offset = pointer - BASE
    count = int.from_bytes(image[offset:offset+3], 'big')
    core0 = OUT / 'core0-P.bin'
    core0.write_bytes(image[offset+6:offset+6+3*count])
    initial = {
        0: [f'LOADP {core0} 0'],
        1: [f'LOADP {core1} 0'] + data_lines(maps),
    }
    for core in initial:
        initial[core] += ['BRIDGE 30000', 'IOX ffff91 1',
                          'IOX ffffbf 300', 'IOX ffffbe 200', 'MAX 1000']
    reference = (ROOT / 'research/tonitor-reference/all-algorithms-q24le.pcm').read_bytes()
    reference_report = json.loads((ROOT / 'docs/tonitor-fm-reference.json').read_text())
    reference_hash = hashlib.sha256(reference).hexdigest()
    if reference_hash != reference_report['q24_reference_sha256']:
        raise ValueError('Unexpected FM reference')
    source = struct.unpack(f'<{len(reference)//4}i', reference)
    cases = []
    for core in (0, 1):
        state = 0x10 if core == 0 else 6
        for mode in (0, 0x8000):
            for track in range(8):
                for bank in (0, 1):
                    algorithm = track * 4 + bank * 2 + bool(mode)
                    samples = [(x >> 4) & 0xFFFFFF for x in source[algorithm*16384:algorithm*16384+32]]
                    target = BUFFER + track * 64 + bank * 32
                    arm = ArmProbe(image)
                    for i, sample in enumerate(samples):
                        arm.call(0x08031228, core, sample << 8, target + i)
                    tx = [int(e['tx'], 16) for e in arm.events if e['kind'] == 'duplex']
                    assert tx == [0x080000 | target] + samples
                    lines = [put('X', state, [0, mode]), put('Y', BUFFER, [SENTINEL]*512)]
                    lines += receive(core, tx[0], True)
                    for sample in tx[1:]:
                        lines += receive(core, sample, False)
                    for space in 'XYP':
                        lines += [f'DUMP{space} {BUFFER:x} 512']
                    lines += [f'DUMPX {state:x} 2']
                    memory, runs = execute(f'c{core}-m{mode:x}-t{track}-b{bank}', initial[core], lines)
                    want = [SENTINEL]*512
                    start = target - BUFFER
                    want[start:start+32] = samples
                    assert all(memory[f'{space}:{BUFFER:x}'] == want for space in 'XYP')
                    assert memory[f'X:{state:x}'] == [target + 32, mode]
                    assert runs == 33
                    cases.append({'core_handler': core, 'retained_mode': hex(mode),
                                  'track': track, 'bank': bank, 'fm_algorithm': algorithm,
                                  'destination': hex(target), 'words': 32,
                                  'arm_transactions': len(tx), 'receiver_entries': runs,
                                  'all_space_aliases_match': True, 'neighbors_unchanged': True})
    local_cases = []
    for core in (0, 1):
        state = 0x10 if core == 0 else 6
        for mode in (0, 0x8000):
            lines = [put('X', 0x800, [SENTINEL]), put('Y', 0x800, [SENTINEL]),
                     put('X', state, [0, mode ^ 0x8000])]
            # Short address commands set the memory-space mode; extended
            # address commands above retain it. These words are DSP fixtures.
            lines += receive(core, 0x800 | mode, True)
            lines += receive(core, 0xFEDCBA, False)
            lines += ['DUMPX 800 1', 'DUMPY 800 1', f'DUMPX {state:x} 2']
            memory, _ = execute(f'local-c{core}-m{mode:x}', initial[core], lines)
            assert memory['X:800'] == [0xFEDCBA if mode == 0 else SENTINEL]
            assert memory['Y:800'] == [0xFEDCBA if mode else SENTINEL]
            assert memory[f'X:{state:x}'] == [0x801, 0x800 | mode]
            local_cases.append({'core_handler': core, 'command': hex(0x800 | mode),
                                'selected_space': 'Y' if mode else 'X', 'other_space_unchanged': True})
    clear_cases = []
    for bank in (0, 1):
        lines = [put('Y', BUFFER, [SENTINEL]*512), 'X 1 20',
                 f'IOY ffffd0 {bank}', 'PC 223', 'STOP 234', 'RUN',
                 f'DUMPY {BUFFER:x} 512']
        memory, _ = execute(f'clear-bank-{bank}', initial[1], lines)
        want = [0 if (i % 64) // 32 == bank else SENTINEL for i in range(512)]
        assert memory[f'Y:{BUFFER:x}'] == want
        clear_cases.append({'bank': bank, 'cleared_words': 256,
                            'other_bank_unchanged': True, 'block_length_fixture': 32})
    report = {'scope': __doc__.strip(), 'image_sha256': digest(image),
              'active_bank_clear_cases': clear_cases,
              'fm_reference_sha256': reference_hash, 'shared_buffer_cases': cases,
              'local_space_controls': local_cases,
              'shared_alias_fixture': 'BRIDGE 0x30000, exercised only in 0x304f0..0x306ef',
              'manual_source': 'https://www.nxp.com/docs/en/reference-manual/DSP56724RM.pdf#page=75',
              'hardware_writes': False, 'timing_validated': False,
              'actual_buffer_ownership_validated': False}
    output = ROOT / 'docs/rhythm-receiver-probe.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(cases)} shared-buffer streams and {len(local_cases)} local-space controls, {len(clear_cases)} bank clears passed; {output}')


if __name__ == '__main__':
    main()
