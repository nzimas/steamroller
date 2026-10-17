"""Build the SteamRoller website (GitHub Pages, or local with serve.py).

  python tools/build_site.py [OUT_DIR]        (default build/site)

Pages come from website/*.html (a first-line comment gives the title,
description and an optional script), wrapped in website/layout.html; the
changelog page is generated from CHANGELOG.md. The installer page gets the
Web MIDI protocol (tools/rhythm_uploader/protocol.mjs) and the validated
firmware, but only scrambled (firmware/<digest>.dat, see scramble()): no
usable firmware file is published. firmware/firmware.json carries the
hashes, installation marker and identity build the installer checks on the
unscrambled bytes before installing.
"""
import hashlib, html, json, re, shutil, struct, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from rhythm_fm_version import VERSION, STEM

WEB = ROOT / 'website'
BUILD = ROOT / 'build/rhythm-fm'
NAV = [('./', 'index.html', 'Home'), ('install.html', 'install.html', 'Install'), ('manual.html', 'manual.html', 'Manual'),
       ('changelog.html', 'changelog.html', 'Changelog'), ('projects.html', 'projects.html', 'Other projects'),
       ('https://github.com/nzimas/steamroller', None, 'GitHub')]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def slug(text): return re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')


def changelog():
    """CHANGELOG.md -> HTML: '#' title, '##' versions, one-word lines as labels, '- ' items, paragraphs."""
    out, items = [], []
    def flush():
        if items: out.append('<ul>\n' + ''.join(f'<li>{i}</li>\n' for i in items) + '</ul>'); items.clear()
    for line in (ROOT / 'CHANGELOG.md').read_text().splitlines():
        text = html.escape(line.strip(), quote=False)
        if not text: flush(); continue
        if line.startswith('## '): flush(); out.append(f'<h2 id="{slug(text[3:]).replace("-", "")}">{text[3:]}</h2>')
        elif line.startswith('# '): flush(); out.append(f'<h1 id="{slug(text[2:])}">{text[2:]}</h1>')
        elif line.startswith('- '): items.append(text[2:])
        else: flush(); out.append(f'<p>{text}</p>')
    flush()
    return ('<!-- title: SteamRoller changelog | description: What changed in each version of SteamRoller. -->\n'
            + '\n'.join(out) + '\n')


def page(layout, source, current, stamp):
    head, _, content = source.partition('\n')
    meta = dict(part.strip().split(': ', 1) for part in head.strip()[4:-3].split(' | '))
    nav = '\n'.join(f'      <a href="{href}"' + (' aria-current="page"' if name == current else '') + f'>{label}</a>'
                    for href, name, label in NAV)
    scripts = f'<script type="module" src="{meta["script"]}?v={stamp}"></script>' if 'script' in meta else ''
    return (layout.replace('{title}', html.escape(meta['title'])).replace('{description}', html.escape(meta['description']))
            .replace('{nav}', nav).replace('{scripts}', scripts).replace('{stamp}', stamp).replace('{content}', content.rstrip()))


SCRAMBLE_SEED = b'SteamRoller installer'


def scramble(data, version):
    """XOR with a xorshift128+ stream seeded from SHA-256(seed + version); the
    installer (website/installer.mjs) runs the same stream to restore it. This
    keeps the published file from being a usable firmware download; it is not
    encryption."""
    k = hashlib.sha256(SCRAMBLE_SEED + version.encode()).digest()
    s0, s1 = struct.unpack('<QQ', k[:16])
    out, mask = bytearray(len(data)), (1 << 64) - 1
    for i in range(0, len(data), 8):
        x, y = s0, s1
        s0 = y; x ^= (x << 23) & mask; s1 = x ^ y ^ (x >> 17) ^ (y >> 26)
        word = struct.pack('<Q', (s1 + y) & mask)
        for j, b in enumerate(data[i:i + 8]): out[i + j] = b ^ word[j]
    return bytes(out)


def firmware():
    approved = json.loads((BUILD / 'uploader/approved.json').read_text())
    candidate = next(e for e in approved['firmwares'] if e['role'] == 'candidate')
    syx = BUILD / f'{STEM}.syx'
    if sha(syx) != candidate['sysex_sha256']: raise ValueError('Firmware does not match the validated uploader entry')
    release = {'name': 'SteamRoller', 'version': VERSION, 'size': candidate['size'],
               'sysex_sha256': candidate['sysex_sha256'], 'image_sha256': candidate['image_sha256'],
               'installation_marker': candidate['installation_marker'], 'identity_build': candidate['identity_build']}
    return syx, release


def build(out):
    out = Path(out)
    syx, release = firmware()
    if out.exists(): shutil.rmtree(out)
    out.mkdir(parents=True)
    for folder in ('css', 'js', 'assets'): shutil.copytree(WEB / folder, out / folder)
    for name in ('installer.mjs', 'serve.py', 'Start SteamRoller Installer.command'): shutil.copy2(WEB / name, out / name)
    shutil.copy2(ROOT / 'tools/rhythm_uploader/protocol.mjs', out / 'protocol.mjs')
    (out / 'firmware').mkdir()
    data = scramble(syx.read_bytes(), VERSION)
    release['data'] = f'firmware/{hashlib.sha256(data).hexdigest()[:16]}.dat'
    (out / release['data']).write_bytes(data)
    (out / 'firmware/firmware.json').write_text(json.dumps(release, indent=2) + '\n')
    stamp = hashlib.sha256(b''.join(p.read_bytes() for p in sorted(out.rglob('*')) if p.is_file())).hexdigest()[:10]
    layout = (WEB / 'layout.html').read_text()
    sources = {name: (WEB / name).read_text() for _, name, _ in NAV if name and name != 'changelog.html'}
    sources['changelog.html'] = changelog()
    for name, source in sources.items(): (out / name).write_text(page(layout, source, name, stamp))
    (out / '.nojekyll').write_text('')   # GitHub Pages: serve the files as they are
    return release


if __name__ == '__main__':
    print(json.dumps(build(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/site'), indent=2))
