"""The vendored MacNativeGames extractors (tools/vendor/macnativegames, kept byte-identical) accept only TotK's title
ids in extract_exefs.program_nca. program_nca here does the same checks with the ids of tools/game_profile.py, and
run() starts a vendored script with it in place: the script is imported as a module (its directory on sys.path, as
the scripts import each other by bare name), every loaded vendored module's program_nca is replaced, then its main()
runs with the given arguments. tools/extract.py starts each vendored script through run().
"""
import importlib
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
VENDOR = HERE / 'vendor' / 'macnativegames'
for p in (str(VENDOR), str(HERE)):
    if p not in sys.path:
        sys.path.insert(0, p)

from game_profile import GAME  # noqa: E402
import extract_exefs  # noqa: E402

require = extract_exefs.require


def program_nca(records, update=False):
    """(record, header) of the one Program NCA of the profile's base game or update package. An update's Program NCA
    keeps the base title id; its Meta NCA carries the update id (extract_exefs.program_nca, MacNativeGames)."""
    base, upd, name = GAME['base_title'], GAME['update_title'], GAME['name']
    programs = [(r, h) for r, h in records if r['content_type'] == 0]
    if update:
        require(len(programs) == 1, 'Expected one update Program NCA; combined packages are unsupported')
        record, hdr = programs[0]
        require(record['title_id'] == base, f'Update belongs to another game ({record["title_id"]})')
        require(any(hdr[0x400 + i * 512 + 4] == 4 for i in range(4)), 'Expected a BKTR update, not a base game')
        require(all(r['title_id'] in (base, upd) for r, _ in records), 'Mixed-title update package')
        return record, hdr
    metadata = [r for r, _ in records if r['content_type'] == 1]
    require(len(metadata) <= 1 and all(r['title_id'] == base for r in metadata),
            f'Expected {name} base game metadata; updates, DLC and combined packages are unsupported')
    require(len(programs) == 1, 'Expected one base Program NCA; updates, DLC and combined packages are unsupported')
    record, hdr = programs[0]
    require(record['title_id'] == base, f'Expected the {name} base game {base}, got {record["title_id"]}')
    return record, hdr


def patch_loaded():
    """Replace program_nca in every vendored module loaded so far (they import it by name)."""
    for mod in list(sys.modules.values()):
        f = getattr(mod, '__file__', None)
        if f and Path(f).resolve().parent == VENDOR and hasattr(mod, 'program_nca'):
            mod.program_nca = program_nca


def run(script, args):
    """Run a vendored script's main() with args, program_nca replaced."""
    mod = importlib.import_module(Path(script).stem)
    patch_loaded()
    sys.argv = [str(script), *map(str, args)]
    mod.main()


patch_loaded()

if __name__ == '__main__':
    run(sys.argv[1], sys.argv[2:])
