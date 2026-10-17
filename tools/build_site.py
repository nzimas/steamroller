"""Assemble the SteamRoller installer as a static site (local or GitHub Pages).

  python tools/build_site.py OUT_DIR

OUT_DIR receives site/* (page, installer, style, local server), the shared
Web MIDI protocol (tools/rhythm_uploader/protocol.mjs), the built firmware as
firmware/<stem>.syx and firmware.json with the hashes, installation marker
and identity build the page checks before installing. Only the modified
firmware goes in: no official Novation file, no download link.
"""
import hashlib, json, shutil, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from rhythm_fm_version import VERSION, STEM

BUILD = ROOT / 'build/rhythm-fm'


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def build(out):
    out = Path(out)
    approved = json.loads((BUILD / 'uploader/approved.json').read_text())
    candidate = next(e for e in approved['firmwares'] if e['role'] == 'candidate')
    syx = BUILD / f'{STEM}.syx'
    if sha(syx) != candidate['sysex_sha256']: raise ValueError('Firmware does not match the validated uploader entry')
    if out.exists(): shutil.rmtree(out)
    (out / 'firmware').mkdir(parents=True)
    for path in (ROOT / 'site').iterdir():
        if path.is_file(): shutil.copy2(path, out / path.name)
    shutil.copy2(ROOT / 'tools/rhythm_uploader/protocol.mjs', out / 'protocol.mjs')
    shutil.copy2(syx, out / 'firmware' / syx.name)
    release = {'name': 'SteamRoller', 'version': VERSION, 'file': f'firmware/{syx.name}', 'size': candidate['size'],
               'sysex_sha256': candidate['sysex_sha256'], 'image_sha256': candidate['image_sha256'],
               'installation_marker': candidate['installation_marker'], 'identity_build': candidate['identity_build']}
    (out / 'firmware.json').write_text(json.dumps(release, indent=2) + '\n')
    (out / '.nojekyll').write_text('')   # GitHub Pages: serve files as they are
    return release


if __name__ == '__main__':
    print(json.dumps(build(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/site'), indent=2))
