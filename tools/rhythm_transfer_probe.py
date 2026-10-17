"""Offline ARM transfer-direction and packing probes; never opens MIDI/USB.

Executes Rhythm 5706 routines with logging SPI/DMA, ready-pin and lock fixtures.
No DSP endpoint, asynchronous DMA, timing or physical memory access is modeled.
"""
import json
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M7, UC_ARM_REG_SP,
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
    UC_ARM_REG_R2, UC_ARM_REG_R3)
from compare_platform import ROOT, BASE, IMAGES, digest

REGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
DEST = 0x20040000
BUFFER = 0x2004B000


class Probe:
    def __init__(self, image):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M7)
        self.cpu.mem_map(0x08000000, 0x80000)
        self.cpu.mem_map(0x20000000, 0x50000)
        self.cpu.mem_write(BASE, image)
        self.cpu.mem_write(0x20000318 + 0xC, struct.pack('<III',
            0x2004A000, 0x2004A100, 0x2004A120))
        self.events = []
        self.received = []
        self.cpu.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, cpu, address, size, context):
        args = [cpu.reg_read(r) for r in REGS]
        result = 0
        if address in (0x08029C8C, 0x08030EB8):
            self.events.append({'kind': 'lock' if address == 0x08029C8C else 'unlock'})
        elif address in (0x0802B68A, 0x0802B684, 0x0802B67E):
            result = BUFFER
        elif address in (0x08046414, 0x08046534, 0x0802B66C):
            self.events.append({'kind': 'wait_fixture', 'function': hex(address)})
        elif address in (0x08031020, 0x08031048, 0x0803107A, 0x08031032):
            self.events.append({'kind': 'select_fixture', 'function': hex(address), 'core': args[0]})
        elif address == 0x08023416:
            self.events.append({'kind': 'ready_fixture'})
        elif address == 0x08041E40:
            # Real lower routine configures RX at r1 and TX at r2.
            self.events.append({'kind': 'duplex', 'tx': bytes(cpu.mem_read(args[2], args[3])).hex()})
            cpu.mem_write(args[1], bytes(args[3]))
        elif address == 0x08041E9C:
            # Real lower routine receives into r1 while repeating byte r3.
            words = [((len(self.received) + i) * 997 + 0x1234) & 0xFFFF
                     for i in range(args[2] // 2)]
            self.received.extend(words)
            cpu.mem_write(args[1], b''.join(struct.pack('>H', x) for x in words))
            self.events.append({'kind': 'receive', 'bytes': args[2], 'fill': args[3] & 255})
        elif address == 0x08041E72:
            self.events.append({'kind': 'transmit', 'tx': bytes(cpu.mem_read(args[1], args[2])).hex()})
        else:
            return
        cpu.reg_write(UC_ARM_REG_R0, result)
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))

    def call(self, function, *args):
        self.cpu.reg_write(UC_ARM_REG_SP, 0x2004F000)
        self.cpu.reg_write(UC_ARM_REG_LR, 0x08000001)
        for r, value in zip(REGS, args):
            self.cpu.reg_write(r, value)
        self.cpu.emu_start(function | 1, 0x08000000, count=200000, timeout=2_000_000)
        if self.cpu.reg_read(UC_ARM_REG_PC) != 0x08000000:
            raise RuntimeError('Function failed to return')
        return self.cpu.reg_read(UC_ARM_REG_R0)


def main():
    filename, _, _, expected = IMAGES['rhythm']
    image = (ROOT / 'firmware/official' / filename).read_bytes()
    if digest(image) != expected:
        raise ValueError('Unexpected firmware')
    reads = []
    for core in (0, 1):
        for offset, count in [(0, 0), (0, 1), (0, 32), (0, 511), (0, 512),
                              (0, 513), (0, 1025), (7, 32), (255, 1), (0, 65536)]:
            p = Probe(image)
            sentinel = b'\xa5' * (2 * (count & 0xFFFF) + 16)
            p.cpu.mem_write(DEST - 8, sentinel)
            address = 0x123400 | offset
            returned = p.call(0x08031430, core, address, DEST, count)
            actual = bytes(p.cpu.mem_read(DEST, 2 * (count & 0xFFFF)))
            want = b''.join(struct.pack('<H', x) for x in p.received[offset:])
            assert actual == want and returned == count
            assert p.cpu.mem_read(DEST - 8, 8) == b'\xa5' * 8
            assert p.cpu.mem_read(DEST + len(actual), 8) == b'\xa5' * 8
            duplex = [e['tx'] for e in p.events if e['kind'] == 'duplex']
            assert duplex == ['101234', '200100', '40004000'], duplex
            assert [e['tx'] for e in p.events if e['kind'] == 'transmit'] == ['2000']
            chunks = [e['bytes'] for e in p.events if e['kind'] == 'receive']
            want_chunks = ([offset * 2] if offset else [])
            remaining = count & 0xFFFF
            while remaining:
                chunk = min(512, remaining)
                want_chunks.append(chunk * 2)
                remaining -= chunk
            assert chunks == want_chunks
            assert sum(e['kind'] == 'lock' for e in p.events) == 1
            assert sum(e['kind'] == 'unlock' for e in p.events) == 1
            reads.append({'core': core, 'address': hex(address), 'count_argument': count,
                          'returned': returned, 'received_bytes_per_call': chunks,
                          'copied_samples': len(actual) // 2, 'events': p.events})
    writes = []
    for core in (0, 1):
        for address, word in [(0x304F0, 0x123456), (0x30510, 0x800000),
                              (0x307B5, 0xFFFFFF), (0x30410, 0)]:
            p = Probe(image)
            p.call(0x08031228, core, word << 8, address)
            tx = [e['tx'] for e in p.events if e['kind'] == 'duplex']
            want = [bytes([8 + (address >> 16), (address >> 8) & 255, address & 255]).hex(),
                    word.to_bytes(3, 'big').hex()]
            assert tx == want, (tx, want)
            assert struct.unpack('<I', p.cpu.mem_read(0x20003684, 4))[0] == address + 1
            writes.append({'core': core, 'address': hex(address), 'word': hex(word),
                           'tx': tx, 'events': p.events})
    # Consecutive calls reuse the cached next-address state; data still uses one
    # three-byte transaction per word. This fixture tests no competing caller.
    p = Probe(image)
    for i in range(32):
        p.call(0x08031228, 0, (0x123400 + i) << 8, 0x304F0 + i)
    tx = [e['tx'] for e in p.events if e['kind'] == 'duplex']
    assert tx == ['0b04f0'] + [(0x123400 + i).to_bytes(3, 'big').hex() for i in range(32)]
    bursts = []
    for core in (0, 1):
        for length in (0, 3, 96, 510):
            p = Probe(image)
            payload = bytes((i * 37 + 11) & 255 for i in range(length))
            p.cpu.mem_write(DEST, payload or b'\x00')
            p.call(0x080315E6, core, DEST, length)
            tx = [e['tx'] for e in p.events if e['kind'] == 'transmit']
            assert tx == [payload.hex()]
            assert not any(e['kind'] in ('lock', 'unlock') for e in p.events)
            bursts.append({'core': core, 'byte_count': length, 'verbatim': True,
                           'events': p.events})
    report = {'firmware_sha256': expected, 'read_cases': reads, 'write_cases': writes,
              'raw_transmit_cases': bursts,
              'sequential_write': {'words': 32, 'transactions': len(tx),
                                   'wire_bytes': sum(len(x) // 2 for x in tx)},
              'limitations': ['Hardware dependencies stubbed; no timing or physical device access.',
                             'No DSP endpoint semantics or permission to use candidate destination verified.',
                             'Count 65536 is diagnostic only; low 16 bits become zero while return stays 65536.']}
    output = ROOT / 'docs/rhythm-transfer-probe.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(reads)} read cases, {len(writes)} single writes, {len(bursts)} raw bursts, 32-word cached sequence passed; {output}')


if __name__ == '__main__':
    main()
