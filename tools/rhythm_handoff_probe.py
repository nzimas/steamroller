"""Offline native DSP bank handshake, block-size propagation and track mixer.

ICC values are supplied manually; no concurrent cores, clock, deadline or audio
hardware is modeled. All firmware routines under test remain unchanged.
"""
import hashlib
import json
import re
import struct
import subprocess

from rhythm_dsp_probe import ROOT, BASE, IMAGES, unpack_stream, data_lines, digest
from rhythm_mix_probe import put, signed

TOOL = ROOT / 'research/analysis/dsptool-io'
OUT = ROOT / 'research/analysis/dsp/handoff'
BUFFER, BUSES = 0x304F0, (0x30430, 0x30470, 0x304B0)


def execute(name, initial, lines):
    path = OUT / f'{name}.txt'
    path.write_text('\n'.join(initial + lines) + '\n')
    result = subprocess.run([str(TOOL), 'run', str(path)], capture_output=True,
                            text=True, check=True)
    runs = [(int(n), int(pc, 16)) for n, pc in re.findall(r'RAN (\d+) pc=([0-9a-f]+)', result.stdout)]
    values = {}
    for m in re.finditer(r'^([XY]|IOY) ([0-9a-f]+):((?: [0-9a-f]{6})+)$', result.stdout, re.M):
        values[f'{m[1]}:{int(m[2],16):x}'] = [int(x, 16) for x in m[3].split()]
    return runs, values


def region(start, stop):
    return [f'PC {start:x}', f'STOP {stop:x}', 'RUN']


def main():
    image = (ROOT / 'firmware/official' / IMAGES['rhythm'][0]).read_bytes()
    if digest(image) != IMAGES['rhythm'][3]:
        raise ValueError('Unexpected firmware')
    OUT.mkdir(parents=True, exist_ok=True)
    maps, _, _, _ = unpack_stream(image)
    core1 = OUT / 'core1-P.bin'
    core1.write_bytes(b''.join(maps['P'].get(a, 0).to_bytes(3, 'big') for a in range(max(maps['P'])+1)))
    ptr = struct.unpack_from('<I', image, 0x08031658 - BASE)[0] - BASE
    count = int.from_bytes(image[ptr:ptr+3], 'big')
    core0 = OUT / 'core0-P.bin'
    core0.write_bytes(image[ptr+6:ptr+6+count*3])
    init = {0: [f'LOADP {core0} 0'], 1: [f'LOADP {core1} 0'] + data_lines(maps)}
    for core in init:
        init[core] += ['BRIDGE 30000', 'MAX 10000',
                      # Test stub: linear address modes, then enter core-0 mixer.
                      'P 7000 05f420 00ffff 0461a0 0462a0 0463a0 0464a0 0465a0 0466a0 0467a0 0c0367']
    publish, gates, acknowledgments = [], [], []
    for previous in (0, 1):
        request = previous ^ 1
        for length in (16, 32):
            lines = [f'X 3a {length:x}', f'IOY ffffd0 {previous}']
            lines += region(0x202, 0x205) + region(0x20D, 0x20E)
            lines += ['DUMPX 307b4 1', 'READIOY ffffd0']
            runs, values = execute(f'publish-{previous}-{length}', init[0], lines)
            assert [pc for _, pc in runs] == [0x205, 0x20E]
            assert values['X:307b4'] == [length] and values['IOY:ffffd0'] == [request]
            # Supply published shared word and both ICC poll values to core 1.
            lines = [f'Y 307b4 {length:x}', f'IOY ffffd0 {previous}', f'IOY ffffd1 {request}']
            lines += region(0x112, 0x119) + ['DUMPX 1 2']
            runs, values = execute(f'accept-{previous}-{length}', init[1], lines)
            assert runs[-1][1] == 0x119 and values['X:1'] == [length, length//2]
            publish.append({'previous_bank': previous, 'new_request': request,
                            'core0_length_fixture': length, 'shared_length': length,
                            'core1_full_length': length, 'core1_half_length': length//2,
                            'core0_mix_bank': request, 'core1_render_bank': previous})
        # Equal poll values send core 1 back to the idle helper without changing
        # its block-length fields; unequal values above pass the work gate.
        lines = [f'IOY ffffd0 {previous}', f'IOY ffffd1 {previous}', 'X 1 1234 5678']
        lines += region(0x112, 0x111) + ['DUMPX 1 2']
        runs, values = execute(f'idle-{previous}', init[1], lines)
        assert runs[-1][1] == 0x111 and values['X:1'] == [0x1234, 0x5678]
        gates.append({'equal_poll_value': previous, 'idle_branch_taken': True})
        # End-of-pass acknowledgment copies the peer request into own poll.
        lines = [f'IOY ffffd0 {previous}', f'IOY ffffd1 {request}']
        lines += region(0x127, 0x129) + ['READIOY ffffd0']
        runs, values = execute(f'ack-{previous}', init[1], lines)
        assert runs[-1][1] == 0x129 and values['IOY:ffffd0'] == [request]
        acknowledgments.append({'previous': previous, 'request': request, 'ack': request})
    waits = []
    for request in (0, 1):
        for ack in (0, 1):
            lines = ['MAX 40', f'IOY ffffd0 {request}', f'IOY ffffd1 {ack}']
            lines += region(0x1A11, 0x1A17)
            runs, _ = execute(f'wait-{request}-{ack}', init[0], lines)
            passed = runs[-1][1] == 0x1A17
            assert passed == (request == ack)
            if not passed:
                assert runs[-1][0] == 40 and 0x1A11 <= runs[-1][1] <= 0x1A16
            waits.append({'request': request, 'ack': ack, 'passes_gate': passed})

    raw = (ROOT / 'research/tonitor-reference/all-algorithms-q24le.pcm').read_bytes()
    reference_hash = hashlib.sha256(raw).hexdigest()
    report = json.loads((ROOT / 'docs/tonitor-fm-reference.json').read_text())
    if reference_hash != report['q24_reference_sha256']:
        raise ValueError('Unexpected FM reference')
    source = struct.unpack(f'<{len(raw)//4}i', raw)
    profiles = [(0, 0, 0, 0, 0, 0),
                (0x400000, 0x200000, 0x100000, 0, 0, 0x200000),
                (0x200000, 0x400000, 0, 0x200000, 0x100000, 0)]
    mixes = []
    max_error = 0
    for track in range(8):
        for bank in (0, 1):
            for profile, gains in enumerate(profiles):
                algorithm = 4*track + 2*bank + (profile % 2)
                samples = [x >> 4 for x in source[algorithm*16384:algorithm*16384+32]]
                values = [0] * 512
                values[64*track+32*bank:64*track+32*bank+32] = samples
                # Deliberately different opposite bank, to detect wrong selection.
                other = 64*track + 32*(bank ^ 1)
                values[other:other+32] = [0x155555] * 32
                lines = [put('Y', BUFFER, values), 'X 3a 20', f'IOY ffffd0 {bank}',
                         put('X', BUSES[0]-1, [0x112233] * 193)]
                for bus in range(3):
                    coefficients = [0] * 512
                    pair = gains[2*bus:2*bus+2]
                    coefficients[64*track:64*track+64] = list(pair) * 32
                    lines += [put('X', 0xDE + 0x200*bus, coefficients)]
                lines += region(0x7000, 0x3AF)
                lines += [f'DUMPX {BUSES[0]-1:x} 193', f'DUMPY {BUFFER:x} 512']
                runs, memory = execute(f'mix-t{track}-b{bank}-p{profile}', init[0], lines)
                assert runs[-1][1] == 0x3AF
                actual = [signed(x) for x in memory[f'X:{BUSES[0]-1:x}']]
                assert actual[0] == 0x112233
                errors = []
                for bus in range(3):
                    expected = [(sample * gain) >> 23 for sample in samples
                                for gain in gains[2*(0, 2, 1)[bus]:2*(0, 2, 1)[bus]+2]]
                    result = actual[1+64*bus:1+64*(bus+1)]
                    errors.extend(abs(a-b) for a, b in zip(result, expected))
                error = max(errors)
                assert error <= 1, (track, bank, profile, error)
                assert memory[f'Y:{BUFFER:x}'] == [x & 0xFFFFFF for x in values]
                max_error = max(max_error, error)
                mixes.append({'track': track, 'bank': bank, 'gain_profile': profile,
                              'fm_algorithm': algorithm, 'gains_q23': gains,
                              'max_lsb_error': error, 'source_unchanged': True,
                              'preceding_output_guard_unchanged': True})
    summed = []
    for bank in (0, 1):
        gains = profiles[1]
        source_banks = [0] * 512
        track_samples = []
        for track in range(8):
            samples = [16 * (track + 1) * (i - 16) for i in range(32)]
            track_samples.append(samples)
            start = 64*track + 32*bank
            source_banks[start:start+32] = samples
            other = 64*track + 32*(bank ^ 1)
            source_banks[other:other+32] = [0x155555]*32
        lines = [put('Y', BUFFER, source_banks), 'X 3a 20', f'IOY ffffd0 {bank}']
        for bus in range(3):
            lines += [put('X', 0xDE + 0x200*bus, list(gains[2*bus:2*bus+2])*256)]
        lines += region(0x7000, 0x3AF) + [f'DUMPX {BUSES[0]:x} 192']
        runs, memory = execute(f'mix-all-tracks-b{bank}', init[0], lines)
        assert runs[-1][1] == 0x3AF
        actual = [signed(x) for x in memory[f'X:{BUSES[0]:x}']]
        expected = []
        for bus in range(3):
            coefficient_bus = (0, 2, 1)[bus]
            expected += [sum((samples[i]*gain) >> 23 for samples in track_samples)
                         for i in range(32)
                         for gain in gains[2*coefficient_bus:2*coefficient_bus+2]]
        assert actual == expected
        summed.append({'bank': bank, 'active_tracks': 8, 'output_matches_exactly': True})
    output = ROOT / 'docs/rhythm-handoff-probe.json'
    result = {'scope': __doc__.strip(), 'firmware_sha256': digest(image),
              'reference_sha256': reference_hash, 'publish_and_accept_cases': publish,
              'idle_cases': gates, 'ack_cases': acknowledgments, 'wait_cases': waits,
              'core0_mixer_cases': mixes, 'all_tracks_sum_cases': summed, 'max_mixer_lsb_error': max_error,
              'hardware_timing_validated': False, 'block_configuration_origin_validated': False,
              'icc_register_values_are_fixtures': True, 'hardware_writes': False}
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'4 publish/accept pairs, 2 idle, 2 ack, 4 wait and {len(mixes)} isolated mixer and {len(summed)} all-track sum cases passed; {output}')


if __name__ == '__main__':
    main()
