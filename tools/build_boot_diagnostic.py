"""Create an explicitly labelled boot isolation image. No device access.

This is not an FM release. D1 restores all five ARM hooks; D2 enables only
FM initialization. The exact failed 0.1.3 image is the immutable reference.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile

from compare_platform import ROOT, BASE
from inspect_firmware import ct_fw
from rhythm_dsp_probe import unpack_stream
from rhythm_dsp_layout import validate_segments
from probe_update_crc import captured_crc_input

FAILED_SHA = '58b80030461eaa2c3650ce117dcb70e728ba051948fa2eede5a8ce0c18c896d8'
STOCK_SHA = 'd5b9ff5dadf2e284f80d69ecb7087486cf0df315a697bed8272af9de0e90df15'
HOOKS = ((0x08025688, 6), (0x08040778, 4), (0x0804083c, 6),
         (0x08040814, 6), (0x08029f70, 4))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arm-init', action='store_true',
                        help='D2: enable only the original FM boot initialization hook')
    parser.add_argument('--stock-heap', action='store_true',
                        help='D3: D2 with only its heap-limit store replaced by NOP')
    args = parser.parse_args()
    if args.stock_heap:
        args.arm_init = True
    NAME = 'rhythm-boot-diagnostic-arm-init' if args.arm_init else 'rhythm-boot-diagnostic-dsp-only'
    if args.stock_heap:
        NAME = 'rhythm-boot-diagnostic-stock-heap'
    OUT = ROOT / 'dist' / NAME
    MARKER, IDENTITY = (b'RFD002', 15710) if args.arm_init else (b'RFD001', 15709)
    stage = 'D2' if args.arm_init else 'D1'
    description = 'ARM INIT ONLY' if args.arm_init else 'DSP ONLY'
    if args.stock_heap:
        MARKER, IDENTITY, stage, description = b'RFD003', 15711, 'D3', 'ARM INIT / STOCK HEAP'
    label = f'Boot diagnostic {stage} — {description}, NO FM'
    restored_hooks = HOOKS[1:] if args.arm_init else HOOKS
    stock = (ROOT / 'firmware/official/circuitrhythm-firmware-5706.bin').read_bytes()
    with zipfile.ZipFile(ROOT / 'dist/withdrawn/rhythm-fm-0.1.3.zip') as archive:
        failed = archive.read('rhythm-fm-0.1.3/rhythm-fm-0.1.3.bin')
    if sha(stock) != STOCK_SHA or sha(failed) != FAILED_SHA:
        raise ValueError('Pinned source image mismatch')
    # Start with stock, retaining ONLY the relocated DSP stream from the failed
    # image. No ARM FM code, hooks, state reservation or producer is included.
    pointer = struct.unpack_from('<I', failed, 0x08031660 - BASE)[0]
    start = pointer - BASE
    count = int.from_bytes(failed[start:start + 3], 'big')
    blob = failed[start:start + 6 + 3 * count]
    image = bytearray(stock)
    image.extend(b'\xff' * (start - len(image)))
    image.extend(blob)
    image[0x08031660 - BASE:0x08031664 - BASE] = struct.pack('<I', pointer)
    if args.arm_init:
        # Keep the exact failed ARM extension bytes, activating ONLY fm_boot.
        # Native note/release/stop and the service tail remain byte-for-byte stock.
        image = bytearray(failed)
        for address, size in HOOKS[1:]:
            offset = address - BASE
            image[offset:offset + size] = stock[offset:offset + size]
        if args.stock_heap:
            offset = 0x0806b024 - BASE
            if image[offset:offset + 2] != bytes.fromhex('0460'):
                raise ValueError('Expected heap-limit store instruction missing')
            image[offset:offset + 2] = bytes.fromhex('00bf')
    image[0x1e8:0x1ee] = MARKER
    identity_offset = 0x080270d8 - BASE + 14
    image[identity_offset:identity_offset + 2] = bytes((IDENTITY >> 7, IDENTITY & 127))
    image.extend(b'\xff' * (-len(image) % 4))
    image = bytes(image)
    allowed = set(range(0x1e8, 0x1ee)) | set(range(identity_offset, identity_offset + 2))
    allowed |= set(range(0x08031660 - BASE, 0x08031664 - BASE))
    if args.arm_init:
        allowed |= set(range(HOOKS[0][0] - BASE, HOOKS[0][0] - BASE + HOOKS[0][1]))
    changed = [i for i, (a, b) in enumerate(zip(stock, image)) if a != b]
    if any(i not in allowed for i in changed):
        raise ValueError('Unexpected stock application mutation')
    for address, size in restored_hooks:
        offset = address - BASE
        if image[offset:offset + size] != stock[offset:offset + size]:
            raise ValueError('ARM hook still active')
    maps, segments, _, _ = unpack_stream(image)
    failed_maps, failed_segments, _, _ = unpack_stream(failed)
    if maps != failed_maps or segments != failed_segments:
        raise ValueError('Diagnostic DSP differs from failed image')
    physical = validate_segments(image, segments)
    boot_check = None
    if args.arm_init:
        from investigate_fm_boot import boot_continuation
        boot_check = boot_continuation(image)
        if boot_check['continuation'] != boot_continuation(stock)['continuation'] or boot_check['fm_magic'] != '0x464d31':
            raise ValueError('D2 did not initialize FM state and reach the native boot continuation')
        expected_limit = '0x2004f760' if args.stock_heap else '0x2004e760'
        if boot_check['heap_limit'] != expected_limit:
            raise ValueError('Diagnostic heap limit differs from intended test')
    if args.stock_heap:
        previous = ROOT / 'dist/rhythm-boot-diagnostic-arm-init/rhythm-boot-diagnostic-arm-init.bin'
        reference = previous.read_bytes()
        permitted = set(range(0x1e8, 0x1ee)) | set(range(identity_offset, identity_offset + 2))
        permitted |= set(range(0x0806b024 - BASE, 0x0806b026 - BASE))
        if len(image) != len(reference) or any(a != b and i not in permitted for i, (a, b) in enumerate(zip(image, reference))):
            raise ValueError('D3 differs from D2 beyond marker, identity and single heap store')
    official = ROOT / 'firmware/official/circuitrhythm-firmware-5706.syx'
    recovery = ct_fw.decode(official.read_bytes())
    sysex = ct_fw.encode(image, proto=recovery['proto'], product=0x63)
    if ct_fw.decode(sysex)['image'] != image:
        raise ValueError('SysEx round trip failed')
    fed, crc = captured_crc_input(image)
    if fed != image or crc != ct_fw.crc32_mpeg2(image):
        raise ValueError('Reference bootloader CRC mismatch')
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / (NAME + '.bin')).write_bytes(image)
    (OUT / (NAME + '.syx')).write_bytes(sysex)
    assets = ROOT / 'tools/rhythm_uploader'
    uploader = OUT / 'uploader'
    uploader.mkdir(exist_ok=True)
    for name in ('index.html', 'style.css', 'app.mjs', 'protocol.mjs'):
        shutil.copy2(assets / name, uploader / name)
    page = (uploader / 'index.html').read_text()
    page = page.replace('RHYTHM FM 0.1.3', f'BOOT DIAGNOSTIC {stage} — {description}')
    page = page.replace('Tracks 1–2: six-operator FM. Tracks 3–8: sample player.',
                        'Boot isolation test. All eight tracks intentionally remain sample players. No FM producer runs.')
    page = page.replace('Offline validation passes. Runtime timing and memory use have not been verified on the unit.',
                        f'This diagnostic isolates {description.lower()}. It is not a corrected FM release.')
    (uploader / 'index.html').write_text(page)
    app = (uploader / 'app.mjs').read_text().replace('Custom FM image identified; test audio separately.',
        f'{stage} diagnostic identified; all tracks remain samplers.').replace('Custom FM installation',
        'Diagnostic installation').replace('packaged FM candidate', 'packaged boot diagnostic')
    (uploader / 'app.mjs').write_text(app)
    for name in ('serve.py', 'Start Transfer Tool.command'):
        shutil.copy2(assets / name, OUT / name)
    (OUT / 'Start Transfer Tool.command').chmod(0o755)
    (OUT / 'recovery').mkdir(exist_ok=True)
    shutil.copy2(official, OUT / 'recovery' / official.name)
    entries = []
    for data, raw, label, role, file in (
        (sysex, image, label, 'candidate', '../' + NAME + '.syx'),
        (official.read_bytes(), recovery['image'], 'Official Rhythm 5706 — recovery', 'recovery', '../recovery/' + official.name)):
        entries.append(dict(label=label, role=role, file=file, size=len(raw),
                            sysex_sha256=sha(data), image_sha256=sha(raw)))
    entries[0].update(installation_marker=MARKER.decode(), identity_build=IDENTITY)
    (uploader / 'approved.json').write_text(json.dumps(dict(product=0x63, firmwares=entries,
        recovery_only=False, notice='Diagnostic image only. No FM audio is expected.'), indent=2) + '\n')
    instructions = '''# Boot diagnostic D1 — DSP only, no FM

This is an isolation test, not a corrected FM release. All eight tracks remain
native sample players. It restores all original ARM hooks and omits the ARM FM
extension and its RAM reservation. Only the modified DSP download, installation
marker and application identity differ from stock execution.

1. Open Start Transfer Tool.command in THIS folder. It opens Terminal and the
   local Chrome/Edge page using existing software. Keep Terminal open.
2. Select “Boot diagnostic D1 — DSP only, NO FM”, then Load packaged file.
3. With the Rhythm in bootloader mode, allow MIDI and check the device.
4. Check the acknowledgment and transfer the diagnostic. Keep USB and power
   connected through receipt, verification and restart.
5. Observe whether it reaches the normal playable layout or restarts/freezes.
   If it boots, check application identity: build 15709 identifies D1. Save the log.
   Samples on tracks 1–2 are EXPECTED in this diagnostic.

If D1 boots, the ARM extension/producer or its interaction becomes the next
isolation target. If it fails, the DSP modification or its download remains
suspect. Neither result alone identifies an exact faulting instruction.

To restore stock, enter bootloader mode, select Official Rhythm 5706 — recovery,
load it and transfer. This official recovery worked on this unit previously.
No firmware is sent by the launcher or offline tests. Nothing is installed on
the laptop. This diagnostic has not been tested on hardware.
'''
    if args.arm_init:
        instructions = f'''# Boot diagnostic D2 — ARM initialization only, no FM

D1 completed startup on the unit. D2 keeps the same DSP program and enables
only the FM boot initialization hook from 0.1.3. This reserves 4096 bytes from
the native heap and initializes the two voice states. All note/release/stop
hooks and the FM service hook remain disabled. All eight tracks are samplers;
no FM audio or FM bus traffic is expected.

1. Open Start Transfer Tool.command in THIS folder. Keep Terminal open.
2. Select “{label}”, then Load packaged file.
3. Put the Rhythm in bootloader mode, allow MIDI and check the device.
4. Check the acknowledgment and transfer. Keep USB and power connected through
   receipt, verification and restart.
5. Report whether startup completes, restarts or freezes. If it boots, select
   normal-mode MIDI ports and check identity: D2 reports build {IDENTITY}.
   Save the session log. All eight tracks remaining samplers is expected.

If D2 boots, the disabled ARM runtime hooks/producer remain the isolation target.
If D2 fails, investigate the FM initialization/RAM reservation and its effect
on native startup. Neither result alone identifies an exact faulting instruction.

Restore stock through the included Official Rhythm 5706 — recovery selection
in bootloader mode if needed. No device access occurs during build, validation
or launcher startup. Nothing is installed on the laptop. Hardware boot of D2
has not been verified. This is a diagnostic, not a corrected FM release.
'''
    if args.stock_heap:
        instructions = f'''# Boot diagnostic D3 — ARM initialization with stock heap

D1 completed startup; D2 failed. D3 differs from D2 in exactly one executable
instruction: the write lowering the native heap limit is replaced by NOP.
It retains the same FM state initialization, boot trampoline and DSP program.
The installation marker and application identity also change to identify D3.

No persistent FM state is reserved. Native allocations may reuse the temporary
state area after initialization. All FM runtime hooks remain disabled, so that
area is never used by FM after startup. All eight tracks remain samplers.
This image tests the heap-limit write; it is not an FM release or a RAM fix
that can be used with the FM producer enabled.

1. Open Start Transfer Tool.command in THIS folder. Keep Terminal open.
2. Select “{label}”, then Load packaged file.
3. Put the Rhythm into bootloader mode (Sample Rec + Samples + Note at power-on),
   allow MIDI, check the device and transfer after checking the acknowledgment.
4. Keep USB and power connected through receipt, verification and restart.
5. Report whether startup completes. If it boots, check normal-mode identity:
   D3 reports build {IDENTITY}. Save the session log.

If D3 boots while D2 fails, the changed heap-limit store is the causal difference
in this pair. Heap exhaustion is still an inference unless its allocation fault
is captured. If D3 also fails, the initialization/boot integration remains suspect.

Official Rhythm 5706 recovery is included and selectable in bootloader mode.
No transfer occurs automatically, and nothing is installed on the laptop.
D3 has not been tested on hardware. All eight tracks should play samples.
'''
    (OUT / 'TRANSFER.md').write_text(instructions)
    (OUT / 'README.md').write_text(instructions)
    # Run the existing complete sender/failure tests against THIS diagnostic,
    # using its own explicit allowlist, rather than re-enabling failed 0.1.3.
    tests = (assets / 'protocol.test.mjs').read_text()
    tests = tests.replace("from './protocol.mjs'", "from '" + (uploader / 'protocol.mjs').as_uri() + "'")
    tests = tests.replace("new URL('../../',import.meta.url)", "new URL('" + ROOT.as_uri() + "/')")
    tests = tests.replace('build/rhythm-fm/uploader/approved.json', 'dist/' + NAME + '/uploader/approved.json')
    tests = tests.replace('build/rhythm-fm/rhythm-fm-0.1.3.syx', 'dist/' + NAME + '/' + NAME + '.syx')
    tests = tests.replace('432112', str(len(image))).replace("'RF0103'", repr(MARKER.decode())).replace('15708', str(IDENTITY))
    test_path = OUT / 'offline-transfer.test.mjs'
    test_path.write_text(tests)
    result = subprocess.run(['node', '--test', '--test-reporter=tap', str(test_path)], capture_output=True, text=True,
                            env=dict(os.environ,RHYTHM_TEST_BUNDLE='dist/'+NAME))
    (OUT / 'offline-transfer.tap').write_text(result.stdout + result.stderr)
    if result.returncode or '# tests 26' not in result.stdout or '# pass 26' not in result.stdout or '# fail 0' not in result.stdout:
        raise RuntimeError('Diagnostic sender tests failed; see offline-transfer.tap')
    report = dict(name=NAME, purpose=description.lower() + ' boot isolation; no FM audio expected',
        image_sha256=sha(image), image_bytes=len(image), sysex_sha256=sha(sysex),
        stock_sha256=STOCK_SHA, failed_reference_sha256=FAILED_SHA,
        original_arm_hooks_restored=[hex(a) for a, _ in restored_hooks],
        stock_flash_changed_addresses=[hex(BASE + i) for i in changed],
        dsp_identical_to_failed_reference=True, dsp_physical_layout=physical,
        arm_extension_present=args.arm_init, arm_state_reserved=args.arm_init and not args.stock_heap,
        arm_service_hook_enabled=False, arm_startup_hook_enabled=args.arm_init,
        heap_limit_store_disabled=args.stock_heap,
        only_executable_difference_from_d2='NOP at 0x0806B024' if args.stock_heap else None,
        actual_arm_boot_continuation=boot_check,
        reference_crc=hex(crc), reference_crc_matches=True,
        installation_marker=MARKER.decode(), identity_build=IDENTITY,
        sender_tests_passed=26, hardware_access=False, hardware_boot_verified=False)
    (OUT / 'diagnostic-validation.json').write_text(json.dumps(report, indent=2) + '\n')
    source = OUT / 'source'
    source.mkdir(exist_ok=True)
    shutil.copy2(Path(__file__), source / Path(__file__).name)
    shutil.copy2(ROOT / 'tools/build_rhythm_fm.py', source / 'build_rhythm_fm.py')
    shutil.copy2(ROOT / 'firmware/rhythm_fm/LICENSE', source / 'LICENSE')
    paths = sorted(p for p in OUT.rglob('*') if p.is_file() and p.name != 'SHA256SUMS')
    (OUT / 'SHA256SUMS').write_text(''.join(sha(p.read_bytes()) + '  ' + str(p.relative_to(OUT)) + '\n' for p in paths))
    with zipfile.ZipFile(ROOT / 'dist' / (NAME + '.zip'), 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(OUT.rglob('*')):
            if path.is_file():
                archive.write(path, str(Path(NAME) / path.relative_to(OUT)))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
