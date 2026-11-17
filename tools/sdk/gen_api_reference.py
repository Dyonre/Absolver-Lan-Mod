#!/usr/bin/env python3
"""Generate docs/API-REFERENCE.md from tools/lannative/AbsolverSE.h (re-runnable; the header is the single source of truth).

Usage: python tools/sdk/gen_api_reference.py [--header PATH] [--out PATH]
Parses the `struct AbsApi` body: every `ret (*name)(params);  /* description */` becomes a table row, grouped by topic (TOPICS). The "since" column is the
version header comment ("---- v2.4 (...)") the field sits under; fields before the first versioned block are 2.0.
A comment on the line(s) just above a field is used when the field has no trailing comment.
Also tables AbsHitInfo / AbsCharInfo members and the ABS_HIT_* enum. Exit code 1 if nothing parsed (header format changed).
"""
import argparse, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def balanced(s, i):
    """s[i] == '(' -> index of the matching ')'."""
    d = 0
    for k in range(i, len(s)):
        if s[k] == '(':
            d += 1
        elif s[k] == ')':
            d -= 1
            if d == 0:
                return k
    return -1


def clean(t):
    return re.sub(r'\s+', ' ', t.replace('|', '\\|')).strip()


def parse_struct(text, name):
    m = re.search(r'struct\s+' + name + r'\s*\{', text)
    if not m:
        return None
    j = m.end()
    d = 1
    while d and j < len(text):
        d += text[j] == '{'
        d -= text[j] == '}'
        j += 1
    return text[m.end():j - 1]


def parse_api(body):
    rows, ver = [], '2.0'
    pending = []          # description from standalone comments above a field
    in_hdr = False        # inside a multi-line "---- ..." header comment
    lines = body.split('\n')
    n = 0
    while n < len(lines):
        s = lines[n].strip()
        n += 1
        if in_hdr:
            if '*/' in s:
                in_hdr = False
            continue
        if not s:
            pending = []
            continue
        if s.startswith('/* ----'):
            h = re.match(r'/\*\s*-+\s*(.*)', s).group(1)
            mv = re.match(r'v(\d+\.\d+)', h)
            if mv:
                ver = mv.group(1)
            in_hdr = '*/' not in s
            pending = []
            continue
        if s.startswith('/*'):                       # standalone comment above a field
            buf = s
            while '*/' not in buf and n < len(lines):
                buf += ' ' + lines[n].strip()
                n += 1
            pending.append(re.sub(r'^/\*\s*|\s*\*/$', '', buf))
            continue
        m = re.match(r'^(?P<ret>[\w\s\*]+?)\s*\(\*(?P<name>\w+)\)\s*\(', s)
        if m:
            k = m.end() - 1
            e = balanced(s, k)
            params = s[k + 1:e] if e > 0 else ''
            rest = s[e + 1:] if e > 0 else ''
            c = re.search(r'/\*\s*(.*?)\s*\*/', rest)
            desc = c.group(1) if c else ' '.join(pending)
            rows.append(dict(name=m.group('name'), ret=m.group('ret').strip(), params=params.strip(), desc=desc, since=ver, kind='fn'))
            pending = []
            continue
        m = re.match(r'^(?P<t>[\w\s\*]+?)\s*(?P<n>\w+)\s*;\s*(?:/\*\s*(?P<d>.*?)\s*\*/)?$', s)   # plain data field
        if m:
            rows.append(dict(name=m.group('n'), ret=m.group('t').strip(), params=None, desc=m.group('d') or ' '.join(pending), since='2.0', kind='field'))
            pending = []
    return rows


def parse_plain_struct(text, name):
    body = parse_struct(text, name) or ''
    out, since = [], '2.0'
    for ln in body.split('\n'):
        s = ln.strip()
        mv = re.search(r'appended in API (\d+\.\d+)', s)
        if mv:
            since = mv.group(1)
        m = re.match(r'^(?P<t>[\w\s\*]+?)\s*(?P<n>\w+(?:\[\d+\])?(?:\s*,\s*\w+)*)\s*;\s*(?:/\*\s*(?P<d>.*?)\s*\*/)?$', s)
        if m:
            for nm in [x.strip() for x in m.group('n').split(',')]:
                out.append((m.group('t').strip(), nm, m.group('d') or '', since))
    return out


# Topic of each function: the first matching rule wins; a function that matches no rule is listed under "Other", so nothing is dropped.
TOPICS = [
    ('Logging, settings, mod data', r'^(log|config_get|kv_get|kv_set|sidecar_get|sidecar_set)$'),
    ('Hooks and call interception', r'^(hook_\w+|find_symbol|on_call|replace_native|trace\w*|frame_peek_int)$'),
    ('Timers and threads', r'(timer|^run_on_game_thread$|^is_game_thread$)'),
    ('UI: menus, text on screen, overlay', r'^(create_widget|add_to_viewport|remove_from_parent|add_menu_item|show_\w+|hud_score|overlay_\w+|on_menu|reload_content|declare_key|input_owner)$'),
    ('Events', r'^on_\w+$'),
    ('Reflection, memory, calling functions', r'^(find_\w+|load_object|object_\w+|class_name|is_a|\w*propert\w*|prop_ptr\w*|get_bool|set_bool|call_function\w*|for_each_\w+|dump_\w+|fname_\w+|make_text|text_to_string|set_text|char_info)$'),
    ('Spawning, objects, assets, tables', r'^(spawn_actor|construct_object|duplicate_object|create_asset|add_to_root|create_texture|texture_from_file|get_world|get_local_player_controller|table_\w+|row_set_text|register_content_kind|inventory_\w+)$'),
    ('Combat, characters, NPCs', r'^(test_\w+|power_use|combo_\w+|set_combat_style|anim_style\w*|real_style|npc_\w+|faction_\w+|area_remove|damage_mod|poise_\w+)$'),
    ('Modes, maps, progression', r'^(travel|console_cmd|solo_duel|match_info|set_mode_prop|mapscript_\w+|xp_add|gm_points_add|rank_info|trial_reward|bot_trial_\w+)$'),
]


def topic_of(name):
    for t, rx in TOPICS:
        if re.search(rx, name):
            return t
    return 'Other'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--header', default=str(ROOT / 'tools/lannative/AbsolverSE.h'))
    ap.add_argument('--out', default=str(ROOT / 'docs/API-REFERENCE.md'))
    a = ap.parse_args()
    text = Path(a.header).read_text(encoding='utf-8', errors='replace')
    ver = re.search(r'#define\s+ABS_API_VERSION\s+(\d+)', text)
    body = parse_struct(text, 'AbsApi')
    rows = parse_api(body) if body else []
    if not rows:
        print('ERROR: no AbsApi members parsed', file=sys.stderr)
        return 1
    fns = [r for r in rows if r['kind'] == 'fn']
    o = ['# AbsolverSE API reference\n',
         'Generated from `tools/lannative/AbsolverSE.h` (`python tools/sdk/gen_api_reference.py`). This page may lag the header: the header is the source of truth.  \n'
         f'`ABS_API_VERSION` = {ver.group(1) if ver else "?"}; {len(fns)} functions. Every function takes `self` (the `const AbsApi *` you were given) as its first argument. '
         'Fields are only ever appended: check `api->size >= offsetof(AbsApi, <field>) + sizeof(void *)` before using a field newer than the version you built against.  \n'
         'How a mod is loaded: `docs/MODS.md`. Notes for larger plugins: `docs/ADVANCED.md`.\n',
         '## Header fields\n\n| Name | Type | Description |\n|---|---|---|']
    for r in rows:
        if r['kind'] == 'field':
            o.append(f"| `{r['name']}` | `{clean(r['ret'])}` | {clean(r['desc'])} |")
    o.append('')
    o.append('## Functions\n\nGrouped by topic. Since = the API minor version that added the function (check `api->size` before using anything newer than 2.0).')
    for t in [t for t, _ in TOPICS] + ['Other']:
        part = [r for r in fns if topic_of(r['name']) == t]
        if part:
            o.append(f'\n### {t}\n\n| Name | Signature | Description | Since |\n|---|---|---|---|')
            o += [f"| `{r['name']}` | `{clean(r['ret'] + ' ' + r['name'] + '(' + r['params'] + ')')}` | {clean(r['desc'])} | {r['since']} |" for r in part]
    o.append('\n## Structs\n')
    o.append('`AbsHitPre` (`on_hit_pre`, 2.19), `AbsDefenseInfo` with the `ABS_DEF_*` types (`on_defense`, 2.20), `AbsOverlayInfo` with the `ABS_OV_*` modes (`overlay_*`, 2.21) '
             'and `AbsPropInfo` (`for_each_property`, 2.22) are documented in the header.\n')
    for sn in ('AbsHitInfo', 'AbsCharInfo'):
        mem = parse_plain_struct(text, sn)
        if mem:
            o.append(f'### `{sn}`\n\n| Member | Type | Description | Since |\n|---|---|---|---|')
            for t, nm, d, sv in mem:
                o.append(f'| `{nm}` | `{clean(t)}` | {clean(d)} | {sv} |')
            o.append('')
    em = re.search(r'enum\s*\{([^}]*ABS_HIT[^}]*)\}', text)
    if em:
        o.append('### `ABS_HIT_*` (on_hit outcome)\n\n| Name | Value |\n|---|---|')
        for nm, v in re.findall(r'(ABS_HIT\w*)\s*=\s*(\d+)', em.group(1)):
            o.append(f'| `{nm}` | {v} |')
        o.append('')
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text('\n'.join(o), encoding='utf-8', newline='\n')
    print(f'wrote {a.out}: {len(fns)} functions, {len(rows) - len(fns)} header fields')
    return 0


if __name__ == '__main__':
    sys.exit(main())
