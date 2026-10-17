"""Publish the SteamRoller website to the repository's gh-pages branch.

  python tools/publish_site.py [--push]

Builds the site (tools/build_site.py) without the local-only files
(serve.py, the .command launcher) and makes it the gh-pages branch's only
commit (a fresh orphan branch every time); with --push it force-pushes it.
GitHub Pages serves gh-pages from its root.
"""
import shutil, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_site
from rhythm_fm_version import VERSION

LOCAL_ONLY = ('serve.py', 'Start SteamRoller Installer.command')


def git(*args, cwd=ROOT, check=True):
    return subprocess.run(['git', *args], cwd=cwd, check=check, capture_output=True, text=True).stdout.strip()


def main():
    push = '--push' in sys.argv
    with tempfile.TemporaryDirectory() as temp:
        site, tree = Path(temp) / 'site', Path(temp) / 'pages'
        build_site.build(site)
        for name in LOCAL_ONLY: (site / name).unlink()
        # gh-pages keeps no history: one fresh commit, force-pushed, so no
        # earlier build (or anything it carried) stays in the branch.
        if subprocess.run(['git', 'rev-parse', '--verify', '--quiet', 'gh-pages'], cwd=ROOT).returncode == 0:
            git('branch', '-D', 'gh-pages')
        git('worktree', 'add', '--orphan', '-b', 'gh-pages', str(tree))
        try:
            for path in tree.iterdir():
                if path.name != '.git': shutil.rmtree(path) if path.is_dir() else path.unlink()
            shutil.copytree(site, tree, dirs_exist_ok=True)
            git('add', '-A', cwd=tree)
            source = git('rev-parse', '--short', 'HEAD')
            git('commit', '-m', f'Website for SteamRoller {VERSION} (from {source})', cwd=tree)
            print('gh-pages:', git('log', '--oneline', '-1', cwd=tree))
            if push: print(git('push', '--force', 'origin', 'gh-pages', cwd=tree) or 'force-pushed gh-pages')
        finally:
            git('worktree', 'remove', '--force', str(tree))


if __name__ == '__main__':
    main()
