"""Run the offline transfer tests; never request MIDI access."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

from rhythm_fm_version import STEM
ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/rhythm-fm'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    assets = ROOT / 'tools/rhythm_uploader'
    for name in ('index.html', 'app.mjs', 'protocol.mjs', 'style.css'):
        if sha(assets / name) != sha(BUILD / 'uploader' / name):
            raise ValueError(f'Stale prepared uploader: {name}')
    for name in ('serve.py', 'Start Transfer Tool.command', 'TRANSFER.md'):
        if sha(assets / name) != sha(BUILD / name):
            raise ValueError(f'Stale prepared tool: {name}')
    approved = json.loads((BUILD / 'uploader/approved.json').read_text())['firmwares']
    for entry in approved:
        path = (BUILD / 'uploader' / entry['file']).resolve()
        if sha(path) != entry['sysex_sha256']:
            raise ValueError('Prepared firmware differs from approved hash')
    encoder = json.loads((BUILD / 'request-encoder-validation.json').read_text())
    manifest = json.loads((BUILD / 'manifest.json').read_text())
    if encoder['firmware_sha256'] != manifest['base_sha256'] or len(encoder['cases']) != 9:
        raise ValueError('Missing matching ARM request encoder evidence')
    result = subprocess.run(
        ['node', '--test', '--test-reporter=tap', str(assets / 'protocol.test.mjs')],
        cwd=ROOT, capture_output=True, text=True)
    transcript = result.stdout + result.stderr
    (BUILD / 'uploader-tests.tap').write_text(transcript)
    print(transcript, end='')
    def count(label):
        match = re.search(r'^# ' + label + r' (\d+)$', transcript, re.M)
        return int(match.group(1)) if match else -1
    tests, passed, failed = count('tests'), count('pass'), count('fail')
    if result.returncode or tests < 26 or passed != tests or failed != 0:
        raise RuntimeError('Uploader tests failed; see uploader-tests.tap')
    report = {
        'image_sha256': sha(BUILD / f'{STEM}.bin'),
        'sysex_sha256': sha(BUILD / f'{STEM}.syx'),
        'tests': tests, 'passed': passed, 'failed': failed,
        'arm_request_encoder_cases': len(encoder['cases']),
        'hardware_access': False,
        'scope': 'Full candidate/recovery streams and failure handling against simulated devices; not a physical bootloader test.',
        'files': {str(p.relative_to(BUILD)): sha(p) for p in
                  sorted(list((BUILD / 'uploader').glob('*')) +
                         [BUILD / name for name in ('serve.py', 'Start Transfer Tool.command', 'TRANSFER.md',
                                                   'recovery/circuitrhythm-firmware-5706.syx')]) if p.is_file()},
    }
    (BUILD / 'uploader-validation.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
