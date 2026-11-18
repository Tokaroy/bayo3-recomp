#!/usr/bin/env python3
"""tools/b3_report.py: what a bug report zip holds, and what it never holds."""
import os
import sys
import tempfile
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import b3_report  # noqa: E402

failures = 0


def check(ok, what):
    global failures
    if not ok:
        print('FAIL:', what)
        failures += 1


with tempfile.TemporaryDirectory(prefix='b3-report-') as temp:
    root = Path(temp)
    logs = root / 'private' / 'win' / 'logs'
    logs.mkdir(parents=True)
    (logs / 'game.log').write_text('nvn/vk: GPU 0: Test GPU (discrete, Vulkan 1.3, driver 1)\nhello\n', encoding='utf-8')
    (logs / 'game.log.prev').write_text('previous run\n', encoding='utf-8')
    for k in range(5):  # only the newest three setup logs
        f = logs / f'setup-2026111{k}-000000.log'
        f.write_text(f'setup {k}\n', encoding='utf-8')
        os.utime(f, (1_000_000 + k, 1_000_000 + k))
    (logs / 'crash-1-x.txt').write_text('CRASH\n', encoding='utf-8')
    (root / 'private' / 'b3.conf').write_text('language=system\n', encoding='utf-8')
    # Game material that must never be packed.
    game = root / 'private' / 'win' / 'run' / 'b3'
    (game / 'extract' / 'romfs').mkdir(parents=True)
    (game / 'extract' / 'romfs' / 'secret.bin').write_bytes(b'game data')
    (game / 'user' / 'save').mkdir(parents=True)
    (game / 'user' / 'save' / 'GameData.dat').write_bytes(b'save')
    big = logs / 'game.log-prog1'
    out = b3_report.make_report(root, host='win', now=time.mktime((2026, 11, 19, 12, 0, 0, 0, 0, -1)))
    check(out.name == 'b3-report-20261119-120000.zip', f'name {out.name}')
    check(out.parent == root / 'private' / 'reports', 'in private/reports')
    with zipfile.ZipFile(out) as z:
        names = set(z.namelist())
        check({'system.txt', 'b3.conf', 'logs/game.log', 'logs/game.log.prev', 'crash/crash-1-x.txt'} <= names,
              f'expected files, got {sorted(names)}')
        setups = sorted(n for n in names if n.startswith('logs/setup-'))
        check(setups == ['logs/setup-20261112-000000.log', 'logs/setup-20261113-000000.log',
                         'logs/setup-20261114-000000.log'], f'newest three setup logs, got {setups}')
        check(not any('romfs' in n or 'save' in n or n.endswith('.bin') or n.endswith('.dat') for n in names),
              'no game data or saves')
        system = z.read('system.txt').decode()
        check('Test GPU' in system, 'GPU line in the summary')
        check('logs/game.log (' in system, 'file list in the summary')
    # A large log keeps its end.
    (logs / 'game.log').write_bytes(b'a' * (b3_report.MAX_FILE + 10) + b'THE END\n')
    out2 = b3_report.make_report(root, host='win', now=time.mktime((2026, 11, 19, 12, 0, 1, 0, 0, -1)))
    with zipfile.ZipFile(out2) as z:
        data = z.read('logs/game.log')
        check(data.endswith(b'THE END\n') and data.startswith(b'[... the first') and len(data) < b3_report.MAX_FILE + 100,
              'a large log keeps its end')

if failures:
    sys.exit(1)
print('b3_report: ok')
