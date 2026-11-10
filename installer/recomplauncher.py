"""RecompLauncher's setup step: python -m installer.recomplauncher --game DIR --dest DIR [--lang CODE]

RecompLauncher (recomplauncher.json, "prepare") hands over one folder, the player's copy of the game: this finds the base
game, the update and prod.keys in it, runs the installer's command line (installer/cli.py) without a window and turns its
JSON events into the lines the launcher reads:

    progress 0.42 Extracting the game: Copying the game data

with the fraction of the whole setup from 0 to 1. Warnings and errors go to stderr in English. The exit code is the
command line's (0 done, 1 failed, 2 bad command line, 3 cancelled); 2 also when the folder lacks a game file or the keys.

The base game and the update are told apart by the title id of their Meta NCA (tools/game_profile.py base_title and
update_title), read with the keys, so the file names do not matter. On a folder that already holds an install only
--dest is needed, as with the command line.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools' / 'vendor' / 'macnativegames'))
from extract_exefs import container_entries, load_keys, nca_inventory  # noqa: E402

from .events import GAME_PARAMS, load_catalog, render  # noqa: E402
from .guilogic import default_game_language, parse_line, windows_ui_language  # noqa: E402
from .layout import GAME  # noqa: E402
from .state import NAME as STATE_NAME  # noqa: E402

GAME_SUFFIXES = ('.nsp', '.nsz', '.xci', '.xcz')
KEY_NAMES = ('prod.keys', 'keys.txt')
# Rough share of the setup's time per step on a first install (the extraction copies about 18 GB, the compile is the
# longest part); steps that do not run are left out and the rest scaled to 1
WEIGHTS = {'check': 0.12, 'fonts': 0.02, 'extract': 0.3, 'lift': 0.13, 'compile': 0.4, 'prepare': 0.2, 'dlss': 0.01,
           'cleanup': 0.02}


def meta_titles(path, keys):
    """The title ids of the Meta NCAs in an NSP, XCI, NSZ or XCZ file; empty when the file cannot be read."""
    try:
        with open(path, 'rb') as f:
            _, entries = container_entries(f, path.stat().st_size)
            return {r['title_id'] for r, _ in nca_inventory(f, entries, keys) if r['content_type'] == 1}
    except Exception:   # not a Switch game file, or not one these keys open: not a candidate
        return set()


def find_inputs(folder):
    """(base, update, keys) in folder; each None when it is not there."""
    keys = next((folder / n for n in KEY_NAMES if (folder / n).is_file()), None)
    if keys is None:
        return None, None, None
    try:
        key_values = load_keys(keys)
    except (OSError, ValueError):
        return None, None, keys
    base = update = None
    for path in sorted(p for p in folder.iterdir() if p.is_file() and p.suffix.lower() in GAME_SUFFIXES):
        titles = meta_titles(path, key_values)
        if titles == {GAME['base_title']} and base is None:
            base = path
        elif titles == {GAME['update_title']} and update is None:
            update = path
    return base, update, keys


class Progress:
    """The fraction of the whole setup from the command line's step and progress events."""

    def __init__(self):
        self.weights = {}
        self.finished = 0.0
        self.step = None

    def start(self, steps):
        total = sum(WEIGHTS.get(s, 0.05) for s in steps) or 1
        self.weights = {s: WEIGHTS.get(s, 0.05) / total for s in steps}

    def step_done(self, step):
        self.finished += self.weights.get(step, 0)
        self.step = None

    def at(self, step, done=None, total=None):
        part = min(1.0, done / total) if done is not None and total else 0.0
        return min(1.0, self.finished + self.weights.get(step, 0) * part)


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python -m installer.recomplauncher', description=__doc__.splitlines()[0])
    ap.add_argument('--game', type=Path, help='folder with the base game, the update and prod.keys')
    ap.add_argument('--dest', type=Path, required=True, help='install folder')
    ap.add_argument('--lang', help='game language (default: en-US, ru on a Russian Windows; a resumed install keeps '
                                   'its own)')
    args = ap.parse_args(argv)

    command = [sys.executable, '-m', 'installer', '--dest', str(args.dest)]
    if args.game:
        base, update, keys = find_inputs(args.game)
        installed = (args.dest / STATE_NAME).is_file()
        missing = [what for what, path in (('prod.keys', keys), ('the base game', base),
                                           (f"the {' / '.join(GAME['supported'])} update", update)) if path is None]
        if missing and not installed:
            print(f"{args.game} lacks {', '.join(missing)} of {GAME['name']}. Put the base game and the update "
                  f'(NSP, XCI, NSZ or XCZ) and prod.keys from your console in one folder and pick that folder.',
                  file=sys.stderr)
            return 2
        if not missing:
            command += ['--base', str(base), '--update', str(update), '--keys', str(keys)]
    if not (args.dest / STATE_NAME).is_file():
        command += ['--lang', args.lang or default_game_language(windows_ui_language())]
    elif args.lang:
        command += ['--lang', args.lang]

    catalog = load_catalog()
    progress = Progress()
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0   # its tools share its hidden console
    proc = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding='utf-8', errors='replace', creationflags=flags)
    try:
        for line in proc.stdout:
            e = parse_line(line)
            if e is None:
                continue
            kind = e['type']
            if kind == 'start':
                progress.start(e.get('steps', []))
            elif kind == 'step':
                name = render(catalog, 'step.' + e['step'], GAME_PARAMS)
                if e['state'] == 'started':
                    print(f"progress {progress.at(e['step']):.3f} {name}", flush=True)
                elif e['state'] in ('done', 'skipped'):
                    progress.step_done(e['step'])
            elif kind == 'progress':
                name = render(catalog, 'step.' + e['step'], GAME_PARAMS)
                phase = render(catalog, f"phase.{e['step']}.{e['phase']}", GAME_PARAMS)
                print(f"progress {progress.at(e['step'], e.get('done'), e.get('total')):.3f} {name}: {phase}",
                      flush=True)
            elif kind in ('warning', 'error'):
                print(f"{kind}: {render(catalog, e['id'], e.get('params'))}", file=sys.stderr, flush=True)
            elif kind == 'end' and e.get('result') == 'ok':
                print('progress 1.000 Ready to play', flush=True)
        return proc.wait()
    except KeyboardInterrupt:
        proc.terminate()
        return 3


if __name__ == '__main__':
    sys.exit(main())
