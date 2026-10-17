"""Independent reference for the FM tracks' native distortion/filter chain.

Runs the stock core-1 routines P:$0648 (distortion), P:$056E (high-pass) and
P:$0590 (low-pass/resonance) on a 32-sample block in X:$2A02, in the same
order as the head of the native voice stage P:$0511, from a test stub at
P:$0F40. Used to check the FM consumer's output bit for bit; it shares no
code with the consumer. No device access.
"""
import re
import subprocess

from build_rhythm_fm import T, ROOT
from rhythm_dsp_probe import data_lines
from rhythm_mix_probe import put

TOOL = ROOT/'research/analysis/dsptool-io'
STUB = 0x0F40
PARAMS = 0x307C8
# Knob parameters used by the chain, by native parameter index.
DISTORTION, HIGHPASS, LOWPASS, RESONANCE = 0xA, 0xB, 0xC, 0xD
NEUTRAL = {HIGHPASS: 0, LOWPASS: 127, RESONANCE: 0, DISTORTION: 0}


def param_block(values):
    block = [0]*32
    for index, value in values.items(): block[index] = value
    return block


def stub(track):
    code = []
    for i in range(8): code += T(f'move #>$ffff,m{i}')
    code += T(f'move #>${0x40+16*track:x},r6') + T(f'move #>${PARAMS+32*track:x},r7')
    code += T('move x:(r6+$6),n7') + T('clr a') + T('move a,x:(r6+$6)') + T('lua (r6+$e),r4')
    code += T('move y:(r7+$a),a') + T('move #>$2a02,r1') + T('tst a')
    after = STUB + len(code) + 2 + 2          # jeq (2 words) + jsr (2 words)
    code += [0x0AF0AA, after, 0x0BF080, 0x648]
    code += T('move y:(r7+$b),n5') + T('lua (r6+$6),r4') + T('move #>$2a4e,r5') + [0x0BF080, 0x56E]
    code += T('move y:(r7+$c),n5') + T('move y:(r7+$d),b') + T('move #>$2a4e,r5') + [0x0BF080, 0x590]
    return code


def signed(word): return word-0x1000000 if word & 0x800000 else word


def fixture_lines(maps, program, params, distortion_table=None):
    """Common initial state: stock memory, per-track knob blocks, optional
    distortion table fixture (filled at runtime on the instrument)."""
    lines = ['CORE1', f'LOADP {program} 0'] + data_lines(maps) + ['BRIDGE 30000', 'MAX 400000', 'X 1 20', 'X 2 10']
    for track, values in params.items(): lines.append(put('Y', PARAMS+32*track, param_block(values)))
    if distortion_table: lines.append(put('X', 0x30200, distortion_table))
    return lines


def run(maps, program, params, blocks, name, distortion_table=None, folder=None):
    """blocks: list of {track: 32 samples}. Executes in order, carrying each
    track's filter state; the first processed block of a track snaps its
    coefficients (as the consumer does after FIFO initialization). Returns a
    list of {track: 32 processed samples}."""
    lines = fixture_lines(maps, program, params, distortion_table)
    snapped = set()
    order = []
    for block in blocks:
        for track, samples in sorted(block.items()):
            if track not in snapped: lines.append(put('X', 0x46+16*track, [1])); snapped.add(track)
            code = stub(track)
            lines += [put('P', STUB, code), put('X', 0x2A02, samples), f'PC {STUB:x}',
                      f'STOP {STUB+len(code):x}', 'RUN', 'DUMPX 2a02 32']
            order.append(track)
    path = (folder or ROOT/'build/rhythm-fm/validation')/f'fx-reference-{name}.txt'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines)+'\n')
    output = subprocess.run([str(TOOL), 'run', str(path)], capture_output=True, text=True, check=True).stdout
    rows = [[int(x, 16) for x in row.split()] for row in re.findall(r'^X 002a02:((?: [0-9a-f]{6})+)$', output, re.M)]
    if len(rows) != len(order): raise RuntimeError('FX reference did not complete every block')
    result, index = [], 0
    for block in blocks:
        processed = {}
        for track in sorted(block):
            processed[track] = rows[index]; index += 1
        result.append(processed)
    return result
