"""Saves the base you stand in (every chest's name, position and contents, plus your backpack) for
the offline chest-choice test: python test/snapshot_base.py [out.json]. Needs the dev build's
bridge (bin-dev) running in the game."""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CALL = os.path.join(ROOT, 'tools', 'mcp', 'call.mjs')


def call(op, args=None):
    out = subprocess.run(['node', CALL, op, json.dumps(args or {})], capture_output=True, text=True,
                         timeout=30, cwd=ROOT).stdout
    return json.loads(out)


def stacks(owner):
    return [{'page': pg['page'], 'slot': s['slot'], 'itemId': s['itemId'], 'count': s['count']}
            for pg in owner.get('pages', []) for s in pg.get('slots', []) if s.get('itemId')]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'test', 'fixtures', 'base.json')
    status = call('status')
    if not status.get('inBase'):
        sys.exit('not in a base: ' + str(status.get('why')))
    chests = []
    items = {}
    for c in call('chests')['chests']:
        owner = call('chest', {'entity': c['entity']})
        chests.append({'entity': c['entity'], 'label': c['label'], 'named': c.get('named', False),
                       'position': c['position'], 'slotsTotal': c['slotsTotal'], 'stacks': stacks(owner)})
    backpack = call('inventory')
    pages = [{'page': pg['page'], 'kind': pg['kind'], 'hotbar': pg.get('hotbar', False)} for pg in backpack['pages']]
    snapshot = {'player': status.get('position'), 'chests': chests, 'pages': pages, 'backpack': stacks(backpack)}
    # The game's own description of every item seen, for items the offline table lacks.
    for st in [s for c in chests for s in c['stacks']] + snapshot['backpack']:
        if st['itemId'] not in items:
            d = call('item', {'itemId': st['itemId']})
            items[st['itemId']] = {'internalName': d.get('internalName'), 'category': d.get('category'),
                                   'maxStack': d.get('maxStack')}
    snapshot['items'] = {str(k): v for k, v in items.items()}
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(snapshot, f, indent=1)
    print(f'{len(chests)} chests, {len(items)} item types -> {out}')


main()
