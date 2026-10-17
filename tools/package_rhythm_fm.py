"""Package the validated local firmware candidate and its source; no installation."""
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

from rhythm_fm_version import VERSION, NAME, STEM, IDENTITY_BUILD, MEMORY_FAILURE_BUILD

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/rhythm-fm'
DIST=ROOT/'dist'/STEM


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    status=json.loads((ROOT/'firmware/rhythm_fm/HARDWARE-STATUS.json').read_text())
    if not status['custom_transfer_enabled']:
        raise RuntimeError('Custom firmware release withdrawn: '+status['reason'])
    manifest=json.loads((BUILD/'manifest.json').read_text())
    if status.get('allowed_release')!=manifest['name']:
        raise RuntimeError('Hardware-test authorization does not match release')
    validation=json.loads((BUILD/'validation.json').read_text())
    if not all(validation.get(k) for k in ('physical_dsp_layout_checked','dsp_runtime_bounds_enforced','actual_arm_boot_trampoline')):
        raise ValueError('Physical memory and boot trampoline checks are required')
    if sha(BUILD/f'{STEM}.bin')!=validation['image_sha256'] or manifest['image_sha256']!=validation['image_sha256']:
        raise ValueError('Candidate does not match validation')
    if sha(BUILD/f'{STEM}.syx')!=manifest['sysex_sha256']:
        raise ValueError('SysEx hash mismatch')
    uploader=json.loads((BUILD/'uploader-validation.json').read_text())
    if uploader['image_sha256']!=manifest['image_sha256'] or uploader['sysex_sha256']!=manifest['sysex_sha256'] or uploader['tests']<28 or uploader['passed']!=uploader['tests'] or uploader['failed']!=0:
        raise ValueError('Uploader validation does not match candidate')
    crc=json.loads((BUILD/'update-crc-validation.json').read_text())
    if crc['image_sha256']!=manifest['image_sha256'] or not any(c['case']=='candidate' and c['matches'] and c['image_bytes']%4==0 for c in crc['cases']):
        raise ValueError('Reference bootloader CRC validation missing')
    delayed=json.loads((BUILD/'delayed-transfer-validation.json').read_text())
    if delayed['image_sha256']!=manifest['image_sha256'] or len(delayed['cases'])!=10:
        raise ValueError('Interleaved ARM/DSP delayed-transfer validation missing')
    bulk=json.loads((BUILD/'bulk-transfer-validation.json').read_text())
    if bulk['image_sha256']!=manifest['image_sha256'] or len(bulk['cases'])!=11:
        raise ValueError('Compiled ARM burst / DSP receiver validation missing')
    fifo=json.loads((BUILD/'fifo-jitter-validation.json').read_text())
    if fifo['image_sha256']!=manifest['image_sha256'] or len(fifo['cases'])!=12:
        raise ValueError('Actual ARM/DSP FIFO jitter validation missing')
    for name,digest in uploader['files'].items():
        if sha(BUILD/name)!=digest:raise ValueError(f'Stale uploader validation: {name}')
    DIST.mkdir(parents=True,exist_ok=True)
    if DIST.parent.joinpath(DIST.name+'.zip').exists() and VERSION in ('0.1.4','0.1.5','0.1.6','0.1.7','0.1.8','0.1.9','0.1.10','0.1.11','0.1.12','0.1.13','0.1.14','0.1.15','0.1.16','0.1.17','0.1.18','0.1.19','0.1.20','0.2.0','0.2.1','0.2.2','0.2.3','0.2.4','0.2.5','0.2.6','0.2.7','0.2.8','0.2.9','0.2.10','0.2.11','0.2.12','0.3.0'):
        raise RuntimeError('Refusing to overwrite an immutable reference archive')
    for name in (f'{STEM}.bin',f'{STEM}.syx','manifest.json','validation.json','generator-preview.wav','runtime-report-validation.json',
                 'serve.py','Start Transfer Tool.command','TRANSFER.md','request-encoder-validation.json',
                 'uploader-validation.json','uploader-tests.tap','update-crc-validation.json','delayed-transfer-validation.json','bulk-transfer-validation.json','fifo-jitter-validation.json'):
        shutil.copy2(BUILD/name,DIST/name)
    for directory in ('uploader','recovery'):
        shutil.copytree(BUILD/directory,DIST/directory,dirs_exist_ok=True)
    shutil.copy2(ROOT/'firmware/rhythm_fm/README.md',DIST/'FIRMWARE-NOTES.md')
    (DIST/'README.md').write_text(f'''# {NAME} — firmware and transfer tool

The stock Circuit Rhythm sampler on all eight tracks, plus:

- **Sound generator (Sample Rec view):** pad 22 (third row, second from right)
  turns generate mode on (green): track buttons 1–8 generate new sounds with
  Vascular's poundhard FM drum engine (1 kick, 2 snare, 3 hi-hat/cymbal,
  4 metal, 5 noise, 6 glitch/click, 7 impact, 8 gnarly); Shift + track plays
  one again. Pad 23 (rightmost) turns commit mode on (red): hold a track button
  and press a slot pad to store its sound as a real sample. Pad 21 (third
  from the right) makes kits: tap it to fill this page with 16 sounds that
  extend the generated sounds already in the pack (up to 128); hold it
  1.5 s to clear the page.
- **Parameter locks:** hold a step and turn a knob (Record engaged); the
  value plays on that step only.
**Not yet verified on a physical Rhythm.** After transfer and restart,
normal-mode identity must report build **{IDENTITY_BUILD}**. Build
{MEMORY_FAILURE_BUILD} reports FM memory initialization failure. See TRANSFER.md.

1. Read **TRANSFER.md** before writing firmware.
2. Open **Start Transfer Tool.command**, or run `python3 serve.py` in this folder.
3. Load the FM candidate in the local Chrome/Edge page. Play a busy pattern,
   check the normal-mode device again and Save log: it measures CPU load
   and whether the Rhythm's timer ticks back up (tempo).

The tool uses the existing Python 3 and Chrome/Edge; it installs no packages.
The original Rhythm 5706 recovery SysEx is included in `recovery/`.
See FIRMWARE-NOTES.md for implementation, hardware limitations and licenses.
Reports, source and SHA256SUMS are included. No firmware is sent by extraction,
the launcher, the build scripts or the offline validation tools.
''')
    for path in (ROOT/'firmware/rhythm_fm').rglob('*'):
        if path.is_file():
            target=DIST/'source'/path.relative_to(ROOT)
            target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
    scripts=['rhythm_fm_version.py','rhythm_fm_fx_reference.py','gen_poundhard_tables.py','build_rhythm_fm.py','validate_rhythm_fm.py','package_rhythm_fm.py','rhythm_fm_host.c',
             'compare_platform.py','rhythm_dsp_probe.py','rhythm_mix_probe.py','rhythm_note_probe.py',
             'inspect_firmware.py','build_dsp_analysis.py','requirements.txt',
             'prepare_rhythm_uploader.py','probe_rhythm_update_requests.py','validate_rhythm_uploader.py','probe_update_crc.py',
             'rhythm_dsp_layout.py','investigate_fm_boot.py','probe_fm_delayed_transfer.py','probe_fm_bulk_transfer.py','probe_fm_fifo_jitter.py','rhythm_transfer_probe.py','rhythm_receiver_probe.py']
    for name in scripts:
        target=DIST/'source/tools'/name;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(ROOT/'tools'/name,target)
    for path in (ROOT/'tools/rhythm_uploader').glob('*'):
        if path.is_file():
            target=DIST/'source/tools/rhythm_uploader'/path.name
            target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
    shutil.copy2(ROOT/'Makefile',DIST/'source/Makefile')
    (DIST/'source/BUILD-INPUTS.md').write_text('''# External build inputs

Build scripts use the project's existing `research/CIRCUITS-HACKS-RC` checkout
(commit 3d6d25211a0fee1282df9303be07f27891dce815), its Python container/Thumb/DSP
helpers, and the locally built `research/analysis/dsptool-io` interpreter wrapper.
The repository is https://github.com/Ghillermo/CIRCUITS-HACKS-RC.
The interpreter source is https://github.com/dsp56300/dsp56300,
commit 44353be2f0156cac970c6ab4f467ea1998b40605.
These research dependencies already exist in the development workspace; they
are not installed or fetched by this bundle. See the workspace research reports
for the interpreter build and wrapper source.

The proprietary stock Rhythm 5706 BIN build input remains separate; its matching
official SysEx is included as the recovery file. The builder requires pinned
hashes and refuses other application versions. The Tonitor
FM6 source and generated tables used by the extension are included in this bundle.
The complete development workspace is /Users/nzimas/development/circuit-rhythm.

A standalone browser uploader, Python standard-library localhost server and
official recovery SysEx are included at the bundle root. No automatic firmware
installation occurs. Rebuilding/testing firmware needs the existing development
dependencies; transferring the packaged files does not.

The offline update-CRC regression also uses the existing reference Tracks
bootloader at research/CircuitTracksReverseEngineering/Firmware-Analysis/
diffCompare/bootloader.bin (SHA-256 recorded in update-crc-validation.json).
This proprietary reference input is not bundled; it is not a Rhythm bootloader.
The historical timing and FIFO comparisons also use immutable workspace archives
dist/rhythm-fm-0.1.4.zip, dist/rhythm-fm-0.1.5.zip and
dist/rhythm-fm-0.1.6.zip (the 0.1.7–0.3.0 archives are likewise left untouched).
The patch generator (firmware/rhythm_fm/fm_generator.c) is adapted from the
local Tonitor checkout at commit 209a1d3ed9a827dd32568312c455184c309c87a0. Their reference images are SHA-256 pinned by the probes.
These old archives are not copied into the new transfer bundle.
''')
    files=sorted(p for p in DIST.rglob('*') if p.is_file() and p.name!='SHA256SUMS')
    (DIST/'SHA256SUMS').write_text(''.join(f'{sha(p)}  {p.relative_to(DIST)}\n' for p in files))
    archive=DIST.parent/(DIST.name+'.zip')
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for path in sorted(DIST.rglob('*')):
            if path.is_file(): z.write(path,Path(DIST.name)/path.relative_to(DIST))
    print(json.dumps({'bundle':str(archive),'sha256':sha(archive),'files':len(files)+1,
                      'hardware_tested':False},indent=2))


if __name__=='__main__':main()
