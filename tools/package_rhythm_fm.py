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
    if DIST.parent.joinpath(DIST.name+'.zip').exists() and VERSION in ('0.1.4','0.1.5','0.1.6','0.1.7','0.1.8','0.1.9','0.1.10','0.1.11','0.1.12','0.1.13','0.1.14','0.1.15','0.1.16','0.1.17','0.1.18','0.1.19','0.1.20','0.2.0','0.2.1','0.2.2','0.2.3','0.2.4','0.2.5','0.2.6','0.2.7','0.2.8','0.2.9','0.2.10','0.2.11','0.2.12','0.3.0','0.3.1'):
        raise RuntimeError('Refusing to overwrite an immutable reference archive')
    # The public bundle is the installer site only (tools/build_site.py): the
    # page, the modified firmware and its checks. No official Novation file,
    # no reports or sources (they stay in build/ and the repository). The same
    # folder runs locally (serve.py / the .command launcher) or on GitHub Pages.
    import build_site
    build_site.build(DIST)
    (DIST/'README.md').write_text(f'''# {NAME}

Custom firmware for the Novation Circuit Rhythm, installed from the browser.

- **On this computer:** open `Start SteamRoller Installer.command` (or run
  `python3 serve.py` here), then follow the page in Chrome or Edge.
- **On the web:** publish this folder as it is (e.g. GitHub Pages). Web MIDI
  needs https or localhost.

After installing, the Rhythm reports build {IDENTITY_BUILD}.
''')
    files=sorted(p for p in DIST.rglob('*') if p.is_file())   # the page checks the firmware against firmware.json
    archive=DIST.parent/(DIST.name+'.zip')
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for path in sorted(DIST.rglob('*')):
            if path.is_file(): z.write(path,Path(DIST.name)/path.relative_to(DIST))
    print(json.dumps({'bundle':str(archive),'sha256':sha(archive),'files':len(files),
                      'hardware_tested':False},indent=2))


if __name__=='__main__':main()
