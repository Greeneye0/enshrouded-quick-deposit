"""Builds and runs the offline chest-choice test (test/chest_choice.cpp) against a base snapshot,
then checks test/fixtures/expect.tsv and holdout.tsv: python test/run_chest_choice.py [--baseline] [base.json]
--baseline: the same with 1.3.15's rule (the most items of the kind), to compare.

expect.tsv lines: item name as the game shows it (English) <TAB> the chest it must go to.
Exit code 1 when an expectation fails. The full placement is in test/out/placement.tsv."""
import csv
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST = os.path.join(ROOT, 'test')
OUT = os.path.join(TEST, 'out')
VCVARS = r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'


def flatten(snapshot, path):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('P\t%s\t%s\t%s\n' % tuple(snapshot['player']))
        for c in snapshot['chests']:
            f.write('C\t%d\t%s\t%d\t%s\t%s\t%s\t%d\n' % (c['entity'], c['label'], 1 if c['named'] else 0,
                                                        *c['position'], c['slotsTotal']))
            for s in c['stacks']:
                f.write('S\t%d\t%d\t%d\n' % (c['entity'], s['itemId'], s['count']))
        for k, v in snapshot['items'].items():
            f.write('I\t%s\t%s\t%s\t%s\n' % (k, v['internalName'], v['category'], v['maxStack']))


def build(exe, objdir, defines=''):
    os.makedirs(objdir, exist_ok=True)
    bat = os.path.join(OUT, 'build.bat')
    with open(bat, 'w') as f:
        f.write(f'@echo off\ncall "{VCVARS}" >nul || exit /b 1\n'
                f'cl /nologo /std:c++20 /O2 /W4 /utf-8 /EHsc /D_CRT_SECURE_NO_WARNINGS {defines} /Isrc '
                f'/Fo"{objdir}\\\\" /Fe"{exe}" test\\chest_choice.cpp src\\match.cpp\n')
    r = subprocess.run(['cmd', '/c', bat], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stdout + r.stderr)


def main():
    args = [a for a in sys.argv[1:] if a != '--baseline']
    baseline = '--baseline' in sys.argv
    base = args[0] if args else os.path.join(TEST, 'fixtures', 'base.json')
    name = 'baseline' if baseline else 'chest_choice'
    exe = os.path.join(OUT, name + '.exe')
    build(exe, os.path.join(OUT, name), '/DQD_BASELINE' if baseline else '')
    tsv = os.path.join(OUT, 'base.tsv')
    flatten(json.load(open(base, encoding='utf-8')), tsv)
    placement = os.path.join(OUT, ('baseline' if baseline else 'placement') + '.tsv')
    items = os.path.join(ROOT, 'tools', 'mcp', 'data', 'items.txt')
    subprocess.run([exe, tsv, items, placement], check=True)

    rows = list(csv.DictReader(open(placement, encoding='utf-8'), delimiter='\t'))
    by_name = {}
    for row in rows:
        by_name.setdefault(row['item'].lower(), []).append(row)
    failed = 0
    for name in ('expect.tsv', 'holdout.tsv'):
        print(f'== {name}')
        failed += check(os.path.join(TEST, 'fixtures', name), by_name)
    print(f'{failed} failed')
    sys.exit(1 if failed else 0)


def check(expect, by_name):
    failed = 0
    for line in open(expect, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        name, chest = line.split('\t')[:2]
        got = by_name.get(name.lower())
        if not got:
            print(f'?    {name}: not in the item table')
            failed += 1
            continue
        ok = all(g['chest'] == chest for g in got)
        failed += not ok
        print(f'{"ok  " if ok else "FAIL"} {name} -> {", ".join(sorted({g["chest"] + " (" + g["rule"] + ")" for g in got}))}'
              + ('' if ok else f'   expected {chest}'))
    print(f'{failed} failed in {os.path.basename(expect)}')
    return failed


main()
