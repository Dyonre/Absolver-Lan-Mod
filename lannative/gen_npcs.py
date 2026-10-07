"""Generates dist/LanNative/npcs.txt (the F1 menu's NPC list) from the game's own DB/AI folder.
Every .uasset under DB/AI that is an ArchetypeAsset (class name appears in its import table) becomes one entry, sorted into groups:
Open world, MiniBoss, Boss (Cargal & Kilnor / Kuretz / Risryn), PvE Theme 1-3, PvE Bosses, PvE waves & support, Tutorial / test.
Line format (TAB separated): group, label, archetype asset path[, character Blueprint class path]
python gen_npcs.py   (extraction root: set ABSOLVER_CONTENT, default .\\GameExtract\\Absolver\\Content)"""
import os, re, sys
ROOT = os.environ.get('ABSOLVER_CONTENT', r'.\GameExtract\Absolver\Content')
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'dist', 'LanNative', 'npcs.txt')
MAX_GROUP = 96

CLS = '/Game/Blueprints/AI/%s.%s_C'
C_MINIBOSS = CLS % ('BP_AICharacter_MiniBoss', 'BP_AICharacter_MiniBoss')
def boss_cls(name): return CLS % ('Boss/BP_AICharacter_Boss_' + name, 'BP_AICharacter_Boss_' + name)

GROUP_ORDER = ['Open world', 'MiniBoss', 'Boss: Cargal & Kilnor', 'Boss: Kuretz', 'Boss: Risryn', 'PvE Theme 1', 'PvE Theme 2', 'PvE Theme 3',
               'PvE Bosses', 'PvE waves & support', 'Tutorial / test']

def natural(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r'(\d+)', s)]

def is_archetype(p):
    if not p.endswith('.uasset'):
        return False
    n = os.path.basename(p)
    if 'Proba' in n or 'UseCondition' in n:
        return False
    return b'ArchetypeAsset' in open(p, 'rb').read()

rows = []   # (group, sortkey, label, path, class)
def add(group, label, rel, cls=''):
    name = os.path.splitext(os.path.basename(rel))[0]
    path = '/Game/%s/%s.%s' % (os.path.dirname(rel).replace('\\', '/'), name, name)
    rows.append((group, natural(label), label, path, cls))

for base, _, files in os.walk(os.path.join(ROOT, 'DB', 'AI')):
    for f in files:
        p = os.path.join(base, f)
        if not is_archetype(p):
            continue
        rel = os.path.relpath(p, ROOT).replace('\\', '/')
        name = f[:-7]
        label = name.replace('_', ' ')
        parts = rel.split('/')                       # DB/AI/...
        if rel.startswith('DB/AI/BOSSES/'):
            boss = parts[3]
            lab = re.sub(r'^Lvl(\d)_', r'Lv\1 ', name).replace('_', ' ')
            if boss == 'CargalKilnor': add('Boss: Cargal & Kilnor', lab, rel, boss_cls('CargalKilnor'))
            elif boss == 'Kuretz': add('Boss: Kuretz', lab, rel, boss_cls('Kuretz'))
            elif boss == 'Shiraz': add('Boss: Risryn', lab.replace('Shiraz', 'Risryn'), rel, boss_cls('Risryn'))   # the game's folder says Shiraz, the boss is Risryn (Blueprint BP_AICharacter_Boss_Risryn)
            else: add('Boss: ' + boss, lab, rel)
        elif rel.startswith('DB/AI/NPCs/PVE/'):
            sub = parts[4]                           # Theme_1 / AINothing / StatuesArchetypes / Supports / Waves
            m = re.match(r'Theme_(\d)', sub)
            if m and '/Boss/' in rel:
                add('PvE Bosses', label, rel, boss_cls('T' + m.group(1)))
            elif m:
                add('PvE Theme ' + m.group(1), label, rel)
            else:
                add('PvE waves & support', label, rel)
        elif rel.startswith('DB/AI/NPCs/Tutorial/') or rel.startswith(('DB/AI/Tests/', 'DB/AI/E3DEMO/', 'DB/AI/Bots/')) or rel == 'DB/AI/TrainingAI.uasset':
            if name == 'TrainingAI': label = 'Training dummy (invincible, does not fight)'
            add('Tutorial / test', label, rel)
        elif 'MiniBoss' in rel:
            add('MiniBoss', label, rel, C_MINIBOSS)
        elif rel.startswith('DB/AI/NPCs/'):
            add('Open world', label, rel)
        else:
            add('Tutorial / test', label, rel)

known = set(GROUP_ORDER)
for g in sorted({r[0] for r in rows} - known):
    GROUP_ORDER.append(g)
out = ['# npcs-list-version: 2',
       '# NPC types for the F1 menu (Spawn NPC...), made from the game\'s own DB/AI folder by tools/lannative/gen_npcs.py.',
       '# Line format, TAB separated: group, label, archetype asset path, [character Blueprint class path].',
       '# The class column (bosses, minibosses) makes the NPC use that character Blueprint; delete it to use the generic AI character. Add your own lines the same way.']
total = 0
for g in GROUP_ORDER:
    items = sorted([r for r in rows if r[0] == g], key=lambda r: ((r[2] != 'Trickster'), r[1]))
    if not items:
        continue
    if len(items) > MAX_GROUP:
        print('WARNING: group %s has %d entries, the menu keeps %d' % (g, len(items), MAX_GROUP))
    for r in items:
        assert len(r[0]) < 48 and len(r[2]) < 48 and len(r[3]) < 200 and len(r[4]) < 140, r
        out.append('\t'.join([r[0], r[2], r[3]] + ([r[4]] if r[4] else [])))
    total += len(items)
    print('%-26s %3d' % (g, len(items)))
open(OUT, 'w', encoding='ascii', newline='\r\n').write('\n'.join(out) + '\n')
print('total', total, '->', os.path.abspath(OUT))
