#!/usr/bin/env python3
"""Generate docs/sdk/API-REFERENCE.md from tools/lannative/AbsolverSE.h (re-runnable; the header is the single source of truth).

Usage: python tools/sdk/gen_api_reference.py [--header PATH] [--out PATH]
Parses the `struct AbsApi` body: every `ret (*name)(params);  /* description */` becomes a table row. The "since" column is the
version header comment ("---- v2.4 (...)") the field sits under; fields before the first versioned block are 2.0.
A comment on the line(s) just above a field is used when the field has no trailing comment.
Also tables AbsHitInfo / AbsCharInfo members and the ABS_HIT_* enum. Exit code 1 if nothing parsed (header format changed).
"""
import argparse, re, sys, datetime
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--header', default=str(ROOT / 'tools/lannative/AbsolverSE.h'))
    ap.add_argument('--out', default=str(ROOT / 'docs/sdk/API-REFERENCE.md'))
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
         'GENERATED from `tools/lannative/AbsolverSE.h` by `python tools/sdk/gen_api_reference.py` - do not edit by hand, re-run it after the header changes.  \n'
         f'`ABS_API_VERSION` = {ver.group(1) if ver else "?"}; {len(fns)} functions. Every function takes `self` (the `const AbsApi *` you were given) as its first argument. '
         'Fields are only ever appended: check `api->size >= offsetof(AbsApi, <field>) + sizeof(void *)` before using a field newer than the version you built against.  \n'
         'Task-oriented guide: `docs/sdk/README.md`. Behaviour details, verification status and traps: `docs/PLUGIN-API.md`.\n',
         '## Header fields\n\n| Name | Type | Description |\n|---|---|---|']
    for r in rows:
        if r['kind'] == 'field':
            o.append(f"| `{r['name']}` | `{clean(r['ret'])}` | {clean(r['desc'])} |")
    o.append('')
    o.append('## Functions\n\nSince = the header block the field was added in (2.0 = the original v2 set, 2.1 = script extender additions).')
    last = None
    for r in fns:
        if r['since'] != last:
            last = r['since']
            o.append(f'\n### Since {last}\n\n| Name | Signature | Description |\n|---|---|---|')
        o.append(f"| `{r['name']}` | `{clean(r['ret'] + ' ' + r['name'] + '(' + r['params'] + ')')}` | {clean(r['desc'])} |")
    o.append('\n## Structs\n')
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
    o.append(f'_Generated {datetime.date.today().isoformat()} from {len(rows)} members._\n')
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text('\n'.join(o), encoding='utf-8', newline='\n')
    print(f'wrote {a.out}: {len(fns)} functions, {len(rows) - len(fns)} header fields')
    return 0


if __name__ == '__main__':
    sys.exit(main())
