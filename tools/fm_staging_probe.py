"""Adversarial reference-protocol checks and DSP copy-kernel checks, offline.

Protocol decisions execute in Python. Only the payload copy executes DSP code.
The new DSP kernel and its addresses are research fixtures, not a firmware patch.
"""
import hashlib
import json
import re
import struct
import subprocess
import sys

from rhythm_dsp_probe import ROOT, BASE, IMAGES, unpack_stream, data_lines, digest
from rhythm_mix_probe import put

sys.path.insert(0, str(ROOT / 'prototype'))
from fm_staging import (Bridge, BusySlot, READY, EPOCH, SEQUENCE, TRACK_MASK,
                        FRAME_COUNT, PAYLOAD, SLOT_WORDS, WORD_MASK)

OUT = ROOT / 'research/analysis/dsp/staging'
TOOL = ROOT / 'research/analysis/dsptool-io'
BUFFER = 0x304F0
# All these addresses are fixture choices, not verified free memory.
SLOT_ADDRESSES = (0x34010, 0x34120)
KERNEL, ENTRY = 0x7100, 0x7000
SENTINEL = 0x123456


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def publish(bridge, sequence, samples, epoch=None, mask=None):
    return list(bridge.publication(bridge.epoch if epoch is None else epoch, sequence,
                                  bridge.track_mask if mask is None else mask, samples))


def expected(samples, mask, bank):
    values = [SENTINEL] * 512
    for track in range(8):
        if mask & (1 << track):
            start = 64*track + 32*bank
            values[start:start+32] = samples[32*track:32*track+32]
    return values


def main():
    checks = []
    samples = [((i+1)*7919) & WORD_MASK for i in range(256)]
    # Interrupt the producer at every word boundary. Before commit, the consumer
    # must see no block; after a missed deadline, its late commit must be stale.
    write_count = 4 + 256 + 1
    for cut in range(write_count + 1):
        bridge = Bridge(track_mask=0x81)
        steps = bridge.publication(bridge.epoch, 0, bridge.track_mask, samples)
        for _ in range(cut):
            next(steps)
        bank = cut & 1
        output = [SENTINEL] * 512
        accepted = bridge.consume(output, bank)
        committed = cut == write_count
        require(accepted == (0 if committed else None), f'Partial publication accepted at {cut}')
        require(output == expected(samples if committed else [0]*256, 0x81, bank), 'Partial audio leaked')
        list(steps)
        output = [SENTINEL] * 512
        require(bridge.consume(output, bank ^ 1) is None, 'Late block replayed')
        require(output == expected([0]*256, 0x81, bank ^ 1), 'Late payload leaked')
        require(bridge.slots[0].words[READY] == 0, 'Late block not released')
    checks.append({'case': 'all_publication_word_boundaries', 'boundaries': write_count+1})

    bridge = Bridge(track_mask=3)
    publish(bridge, 1, samples)  # Deliberate look-ahead in the second slot.
    output = [SENTINEL]*512
    require(bridge.consume(output, 0) is None and bridge.slots[1].words[READY] == 1, 'Future block lost')
    want = expected([0]*256, 3, 0)
    require(output == want, 'Look-ahead copied before its deadline')
    require(bridge.consume(output, 1) == 1, 'Future block not consumed at deadline')
    for track in (0, 1):
        want[64*track+32:64*track+64] = samples[32*track:32*track+32]
    require(output == want, 'Look-ahead block copied incorrectly')
    checks.append({'case': 'one_block_lookahead_retained'})

    bridge = Bridge(track_mask=255)
    publish(bridge, 0, samples)
    attempts = []
    def try_overwrite(slot, track, index):
        require(slot is not None and slot.words[READY] == 1, 'Released during copy')
        try:
            bridge.publication(bridge.epoch, 0, 255, [0]*256)
        except BusySlot:
            attempts.append((track, index))
        else:
            raise RuntimeError('Producer overwrote in-flight payload')
    output = [SENTINEL]*512
    require(bridge.consume(output, 0, during_copy=try_overwrite) == 0, 'Owned block missing')
    require(len(attempts) == 256 and output == expected(samples, 255, 0), 'Ownership failure')
    publish(bridge, 2, samples)
    checks.append({'case': 'producer_reuse_blocked_through_copy', 'attempts': len(attempts)})

    for field, bad in [(EPOCH, 2), (SEQUENCE, 2), (SEQUENCE, 1),
                       (TRACK_MASK, 3), (TRACK_MASK, 0x101), (FRAME_COUNT, 31), (READY, 2)]:
        bridge = Bridge()
        publish(bridge, 0, samples)
        bridge.slots[0].words[field] = bad
        output = [SENTINEL]*512
        require(bridge.consume(output, 0) is None and bridge.dropped == 1, 'Malformed metadata accepted')
        require(output == expected([0]*256, 1, 0), 'Malformed payload copied')
        checks.append({'case': 'invalid_metadata', 'field': field, 'value': bad})

    bridge = Bridge(track_mask=1)
    steps = bridge.publication(bridge.epoch, 0, 1, samples)
    for _ in range(90):
        next(steps)
    bridge.configure(0x80)
    list(steps)  # An old-generation transfer completes after the engine change.
    output = [SENTINEL]*512
    require(bridge.consume(output, 0) is None and bridge.dropped == 1, 'Old configuration leaked')
    require(output == expected([0]*256, 0x80, 0), 'Engine mask not respected')
    publish(bridge, 1, samples)
    require(bridge.consume(output, 1) == 1, 'New generation failed')
    checks.append({'case': 'configuration_change_during_publication'})

    bridge = Bridge(sequence=WORD_MASK)
    publish(bridge, WORD_MASK, samples)
    publish(bridge, 0, samples)
    require(bridge.consume([SENTINEL]*512, 1) == WORD_MASK, 'Last sequence failed')
    require(bridge.consume([SENTINEL]*512, 0) == 0 and bridge.sequence == 1, 'Wrap failed')
    checks.append({'case': '24_bit_sequence_wrap_with_lookahead'})
    bridge = Bridge(epoch=WORD_MASK)
    try:
        bridge.configure(2)
    except RuntimeError:
        checks.append({'case': 'epoch_reuse_requires_quiescent_reset'})
    else:
        raise RuntimeError('Epoch silently reused')

    bridge = Bridge(track_mask=0)
    output = [SENTINEL]*512
    require(bridge.consume(output, 0) is None and output == [SENTINEL]*512 and bridge.underruns == 0,
            'All-sampler configuration changed output')
    checks.append({'case': 'all_sampler_passthrough'})
    bridge = Bridge()
    publish(bridge, 0, samples)
    output = [SENTINEL]*512
    require(bridge.consume(output, 0, block_length=16) is None and bridge.sequence == 0 and
            bridge.slots[0].words[READY] == 1 and output == [SENTINEL]*512 and
            bridge.status == 'unsupported_block_length', 'Unsupported length touched state')
    checks.append({'case': 'unsupported_length_returns_without_copy'})
    bridge = Bridge()
    steps = bridge.publication(bridge.epoch, 0, 1, samples)
    try:
        bridge.publication(bridge.epoch, 0, 1, samples)
    except BusySlot:
        pass
    else:
        raise RuntimeError('Reservation reused before publication started')
    checks.append({'case': 'producer_reservation_prevents_second_writer'})

    image = (ROOT / 'firmware/official' / IMAGES['rhythm'][0]).read_bytes()
    require(digest(image) == IMAGES['rhythm'][3], 'Unexpected firmware')
    maps, _, _, _ = unpack_stream(image)
    OUT.mkdir(parents=True, exist_ok=True)
    program = OUT / 'core1-P.bin'
    program.write_bytes(b''.join(maps['P'].get(a, 0).to_bytes(3, 'big') for a in range(max(maps['P'])+1)))
    # A new research kernel: 32 explicit word-copy pairs; no backward branch,
    # polling or caller state preservation. Stop just before its return.
    kernel_source = ROOT / 'prototype/fm_copy_block.asm'
    assembly = [line.strip() for line in kernel_source.read_text().splitlines()
                if line.strip() and not line.lstrip().startswith(';')]
    assembled = subprocess.run([str(TOOL), 'asm'], input='\n'.join(assembly)+'\n',
                               capture_output=True, text=True, check=True).stdout.splitlines()
    require(len(assembled) == len(assembly), 'Assembler line mismatch')
    words = []
    for line in assembled:
        parts = line.split()
        require(parts[0] == '1', f'Unexpected copy instruction: {line}')
        words.append(int(parts[1], 16))
    (OUT / 'copy-kernel.asm').write_text('\n'.join(assembly)+'\n')
    raw = (ROOT / 'research/tonitor-reference/all-algorithms-q24le.pcm').read_bytes()
    reference_hash = hashlib.sha256(raw).hexdigest()
    reference_report = json.loads((ROOT / 'docs/tonitor-fm-reference.json').read_text())
    require(reference_hash == reference_report['q24_reference_sha256'], 'Unexpected FM reference')
    fm = struct.unpack(f'<{len(raw)//4}i', raw)
    kernel_cases = []
    for bank in (0, 1):
        for mask in (1, 0x80, 0x55, 0xAA, 0xFF, 0):
            bridge = Bridge(track_mask=mask, sequence=bank)
            payload = [((fm[(track*4+bank)*16384+i] >> 4) & WORD_MASK)
                       for track in range(8) for i in range(32)]
            publish(bridge, bank, payload)
            before = list(bridge.slots[bank].words)
            output = [SENTINEL]*512
            require(bridge.consume(output, bank) == bank, 'Reference failed to consume')
            lines = [f'LOADP {program} 0'] + data_lines(maps) + ['BRIDGE 30000', 'MAX 2000']
            lines += [put('P', KERNEL, words), put('Y', BUFFER, [SENTINEL]*512),
                      put('Y', SLOT_ADDRESSES[bank], before)]
            for track in range(8):
                if not mask & (1 << track):
                    continue
                src = SLOT_ADDRESSES[bank] + PAYLOAD + 32*track
                dst = BUFFER + 64*track + 32*bank
                # Configure only the address modes used by this isolated kernel.
                entry_words = [0x05F421, 0xFFFF, 0x05F425, 0xFFFF,
                               0x61F400, src, 0x65F400, dst, 0x0AF080, KERNEL]
                lines += [put('P', ENTRY, entry_words), f'PC {ENTRY:x}',
                          f'STOP {KERNEL+64:x}', 'RUN']
            lines += [f'DUMPY {BUFFER:x} 512', f'DUMPY {SLOT_ADDRESSES[bank]:x} {SLOT_WORDS}']
            path = OUT / f'copy-b{bank}-m{mask:x}.txt'
            path.write_text('\n'.join(lines)+'\n')
            result = subprocess.run([str(TOOL), 'run', str(path)], capture_output=True, text=True, check=True)
            runs = re.findall(r'RAN (\d+) pc=([0-9a-f]+)', result.stdout)
            require(len(runs) == mask.bit_count() and all(int(pc,16) == KERNEL+64 for _,pc in runs),
                    'DSP copy did not stop at return')
            dumps = {}
            for m in re.finditer(r'^Y ([0-9a-f]+):((?: [0-9a-f]{6})+)$', result.stdout, re.M):
                dumps[int(m[1],16)] = [int(x,16) for x in m[2].split()]
            require(dumps[BUFFER] == output, 'DSP copy disagrees with reference protocol output')
            require(dumps[SLOT_ADDRESSES[bank]] == before, 'Copy mutated source slot')
            kernel_cases.append({'bank': bank, 'track_mask': hex(mask), 'copy_calls': len(runs),
                                 'matches_reference': True, 'source_unchanged': True})
    result = {'scope': __doc__.strip(), 'protocol_checks': checks,
              'publication_boundaries_tested': write_count+1, 'dsp_copy_cases': kernel_cases,
              'firmware_sha256': digest(image), 'fm_reference_sha256': reference_hash,
              'slot_words': SLOT_WORDS, 'two_slot_bytes_packed': 2*SLOT_WORDS*3,
              'kernel_program_words': len(words),
              'kernel_source_sha256': hashlib.sha256(kernel_source.read_bytes()).hexdigest(), 'kernel_clobbers': ['r1', 'r5', 'y0'],
              'fixture_slot_addresses': [hex(x) for x in SLOT_ADDRESSES],
              'dsp_protocol_state_machine_implemented': False,
              'addresses_verified_free': False, 'hardware_writes': False}
    report = ROOT / 'docs/fm-staging-probe.json'
    report.write_text(json.dumps(result, indent=2)+'\n')
    print(f'{write_count+1} publication boundaries, {len(checks)} protocol checks and {len(kernel_cases)} DSP copy cases passed; {report}')


if __name__ == '__main__':
    main()
