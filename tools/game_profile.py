"""The game this checkout's installer and extraction build: title ids, supported versions, expected hashes and the
parts of the TotK pipeline the game uses. tools/build_game.py --game names the same games (its GAMES table holds the
runtime side). This repository builds Marvel Ultimate Alliance 3; TOTK_GAME=totk or cadence selects another profile.
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

PROFILES = {
    'totk': {
        'game': 'totk',                       # build_game.py --game
        'name': 'TotK',
        'file': 'totk',                       # <file>.exe, <file>.ini, <file>-install.json
        'setup': 'TotK-Setup',                # the installer's executable and the release's name
        'base_title': '0100F2C0115B6000',
        'update_title': '0100F2C0115B6800',
        'data': ROOT / 'tools' / 'data',      # expected-v<display version>.json
        'base_version': '1.0.0',
        'supported': ('1.2.1', '1.4.3', '1.1.0'),
        'modules': 'main,subsdk0,sdk',
        'table': 'patches-{tag}.txt',         # recomp/ code patch table per version (v121...), None without one
        'patches': True,                      # cheats and ExeFS patches of mods
        'fonts': True,                        # the Switch shared fonts for Chinese and Korean text
        'prepare': True,                      # RomFS patches and the shader list (MacNativeGames)
        'dlss': True,                         # NVIDIA DLSS for the 3D scene (vk_dlaa.c)
    },
    'mua3': {
        'game': 'mua3',
        'name': 'Marvel Ultimate Alliance 3',
        'file': 'mua3',
        'setup': 'MUA3-Setup',
        'base_title': '010060700AC50000',
        'update_title': '010060700AC50800',
        'data': ROOT / 'tools' / 'data' / 'mua3',
        'base_version': '1.0.0',
        'supported': ('4.0.1',),
        'modules': 'main,subsdk0,subsdk1,sdk',
        'table': 'patches-mua3-{tag}.txt',    # mua3fps60: the 60 FPS frame setup (runtime/cheats.c)
        'patches': False,
        'fonts': False,
        'prepare': False,
        'dlss': False,
    },
    'cadence': {
        'game': 'cadence',
        'name': 'Cadence of Hyrule',
        'file': 'cadence',
        'setup': 'Cadence-Setup',
        'base_title': '01000B900D8B0000',
        'update_title': '01000B900D8B0800',
        'data': ROOT / 'tools' / 'data' / 'cadence',
        'base_version': '1.0.0',
        'supported': ('1.5.0',),
        'modules': 'main,subsdk0,subsdk1,sdk',
        'table': None,
        'patches': False,
        'fonts': False,
        'prepare': False,
        'dlss': False,
    },
}

DEFAULT = 'mua3'
GAME = PROFILES[os.environ.get('TOTK_GAME', DEFAULT)]
