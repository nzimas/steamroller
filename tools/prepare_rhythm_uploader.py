"""Prepare the local uploader, pinned hashes and official recovery file. No MIDI."""
import hashlib
import json
from pathlib import Path
import shutil

from inspect_firmware import ct_fw
from rhythm_fm_version import STEM, NAME

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/rhythm-fm'
ASSETS=ROOT/'tools/rhythm_uploader'


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    status=json.loads((ROOT/'firmware/rhythm_fm/HARDWARE-STATUS.json').read_text())
    manifest=json.loads((BUILD/'manifest.json').read_text())
    if status['custom_transfer_enabled'] and status.get('allowed_release')!=manifest['name']:
        raise ValueError('Transfer authorization does not match this release')
    validated=json.loads((BUILD/'validation.json').read_text())
    candidate=BUILD/f'{STEM}.syx'
    info=ct_fw.decode(candidate.read_bytes())
    if len(info['image'])%4:raise ValueError('Candidate is not word-aligned for device CRC')
    if sha(candidate)!=manifest['sysex_sha256'] or ct_fw.sha256(info['image'])!=validated['image_sha256']:
        raise ValueError('Candidate hash does not match validation')
    recovery=ROOT/'firmware/official/circuitrhythm-firmware-5706.syx'
    original=ct_fw.decode(recovery.read_bytes())
    if original['product']!=0x63 or ct_fw.sha256(original['image'])!=manifest['base_sha256']:
        raise ValueError('Official recovery image mismatch')
    output=BUILD/'uploader';output.mkdir(parents=True,exist_ok=True)
    for name in ('index.html','style.css','app.mjs','protocol.mjs'):
        shutil.copy2(ASSETS/name,output/name)
    for name in ('serve.py','Start Transfer Tool.command','TRANSFER.md'):
        shutil.copy2(ASSETS/name,BUILD/name)
    (BUILD/'Start Transfer Tool.command').chmod(0o755)
    (BUILD/'recovery').mkdir(exist_ok=True)
    shutil.copy2(recovery,BUILD/'recovery'/recovery.name)
    entries=[]
    for path,decoded,label,role,file in [
        (candidate,info,f'{NAME} — sampler + poundhard generator','candidate',f'../{STEM}.syx'),
        (recovery,original,'Official Rhythm 5706 — recovery','recovery','../recovery/'+recovery.name)]:
        entries.append({'label':label,'role':role,'file':file,'size':len(decoded['image']),
                        'sysex_sha256':sha(path),'image_sha256':ct_fw.sha256(decoded['image'])})
        if role=='candidate':
            entries[-1].update(installation_marker=manifest['installation_marker'],identity_build=manifest['identity_build'])
    if not status['custom_transfer_enabled']:
        entries=[entry for entry in entries if entry['role']=='recovery']
    (output/'approved.json').write_text(json.dumps({'product':0x63,'firmwares':entries,
        'recovery_only':not status['custom_transfer_enabled'],'notice':status['reason']},indent=2)+'\n')
    print(f'Uploader and verified official recovery firmware ready in {BUILD}')


if __name__=='__main__':main()
