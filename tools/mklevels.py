#!/usr/bin/env python3
"""mklevels.py — docs/levels.json -> text/levels.txt + include/levels.h

`docs/levels.json` is the level book: one entry per level giving its
mission title, dateline, turn limit, both rosters and the briefing text.
This reads it so nobody retypes it.

    {
      "default_turns": 20,
      "levels": [
        { "mission":  "'Our Blood is of Iron'",
          "dateline": "Switzerland",
          "turns":    20,
          "player":   "Base, 2xShadows",
          "enemy":    "Base",
          "briefing": "Marcus Kassels, son of an aristocratic..." },
        ...
      ]
    }

JSON, NOT the markdown it started as.  The markdown was parsed with
regular expressions and they were wrong twice in one sitting: `\s*` after
a key matched the NEWLINE as well, so an empty "Mission:" swallowed the
line beneath it and two levels took the dateline as their title.  A
format with a parser in the standard library cannot fail that way, and
`json.load` reports the line and column when the book is malformed.

WHAT COMES OUT, AND WHY THEY ARE DIFFERENT SHAPES

  text/levels.txt   the MISSION titles, in tools/mktext.py's format, so
                    they join the resident string pool.  Resident because
                    the mission replaces "THE FIELD" in the ST_PLAY header
                    and is therefore drawn while the board is being
                    composed -- it cannot live in the compose buffer.

  include/levels.h  the turn limits, and ONE ZX0 block holding every
                    dateline and briefing with an offset table.

ONE BLOCK, NOT TEN.  Per-level blobs measured 1 207 bytes against 1 326
raw -- worse than not compressing -- because each is too short for ZX0 to
find anything and every stream pays its own overhead.  The repetition
worth having is BETWEEN briefings: RED SHADOWS, ACTION FORCE, RED JACKAL.
Together they go to 611.

The block is unpacked into MEM_VBUF, which is 4 096 bytes and holds
nothing during ST_BRIEF: entering ST_PLAY calls render_play(), which
rebuilds every cell of it.  So the briefings cost 611 bytes of program
region and no RAM at all, on a 48K as much as a 128K.

The roster lines are read and checked but not yet used: populate_map()
still takes its armies from config/game_config.h.  Wiring those up is a
separate job and the parser is ready for it.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

DEFAULT_TURNS = 20          # what a level gets if it does not say
WIDTH = 31                  # a briefing wraps to this; the screen is 32
NO_MISSION = 'THE FIELD'    # header text for a level with none written yet
HEADER_COLS = 31            # the mission replaces "THE FIELD" in the header,
                            # which starts at column 1 of a 32-column screen

# The ROM font has 96 glyphs from space; anything else cannot be printed.
# These are the characters the level book actually uses that fall outside
# it, mapped to what a Spectrum can show.  A character NOT in here fails
# the build rather than being silently dropped -- a missing glyph on a
# real machine is a question mark nobody can explain.
FOLD = {
    '\u00a1': '!', '\u00bf': '?',
    '\u2018': "'", '\u2019': "'", '\u201c': '"', '\u201d': '"',
    '\u2013': '-', '\u2014': '-', '\u2026': '...',
    '\u00e1': 'A', '\u00e9': 'E', '\u00ed': 'I', '\u00f3': 'O',
    '\u00fa': 'U', '\u00f1': 'N', '\u00fc': 'U',
    '\u00c1': 'A', '\u00c9': 'E', '\u00cd': 'I', '\u00d3': 'O',
    '\u00da': 'U', '\u00d1': 'N',
}
folded = []                 # what got changed, for the report


def printable(s, where):
    """Fold what can be folded; fail on what cannot."""
    out = []
    for ch in s:
        if 32 <= ord(ch) < 127:
            out.append(ch)
        elif ch in FOLD:
            folded.append((where, ch, FOLD[ch]))
            out.append(FOLD[ch])
        else:
            sys.exit('mklevels: %s contains %r (U+%04X), which the ROM font '
                     'cannot print and mklevels does not know how to fold'
                     % (where, ch, ord(ch)))
    return ''.join(out)


KNOWN = ('mission', 'dateline', 'turns', 'player', 'enemy', 'briefing')

# The unit types a roster may name, in UNIT_* id order.  Must match
# config/game_config.h -- a name here that the game does not have would
# put units on the board that cannot be drawn.
UNIT_NAMES = ('infantry', 'tank', 'cannon', 'base', 'cruiser')


def parse(path):
    """[{n, mission, dateline, turns, player, enemy, briefing}] in order."""
    try:
        doc = json.load(open(path))
    except json.JSONDecodeError as e:
        sys.exit('mklevels: %s is not valid JSON: %s' % (path, e))

    if isinstance(doc, list):           # a bare array is allowed
        doc = {'levels': doc}
    if 'levels' not in doc:
        sys.exit('mklevels: %s has no "levels" array' % path)
    default_turns = doc.get('default_turns', DEFAULT_TURNS)

    out = []
    for i, lv in enumerate(doc['levels'], 1):
        if not isinstance(lv, dict):
            sys.exit('mklevels: level %d is not an object' % i)
        # A typo in a key would otherwise be silently ignored and the
        # field would quietly take its default, which is how a briefing
        # goes missing without anyone being told.
        for k in lv:
            if k not in KNOWN and not k.startswith('_'):
                sys.exit('mklevels: level %d has an unknown field %r '
                         '(known: %s)' % (i, k, ', '.join(KNOWN)))

        def field(key, default=None):
            val = str(lv.get(key, '')).strip()
            val = re.sub(r'\s+', ' ', val)
            if val:
                return printable(val, 'level %d %s' % (i, key))
            if default is None:
                sys.exit('mklevels: level %d has no "%s"' % (i, key))
            return default

        # THE ROSTERS.  Counts per unit type, replacing the formula in
        # config/game_config.h -- see docs/DESIGN.md section Army
        # composition.  Validated here and not yet emitted: populate_map()
        # still uses the formula, and this phase is the data moving first.
        rosters = {}
        for side in ('player', 'enemy'):
            r = lv.get(side, {})
            if isinstance(r, str):
                sys.exit('mklevels: level %d "%s" is still prose (%r). '
                         'Rosters are counts now: {"infantry": 3, ...}'
                         % (i, side, r))
            if not isinstance(r, dict):
                sys.exit('mklevels: level %d "%s" must be an object' % (i, side))
            for k, v in r.items():
                if k not in UNIT_NAMES:
                    sys.exit('mklevels: level %d "%s" names an unknown unit '
                             '%r (known: %s)'
                             % (i, side, k, ', '.join(UNIT_NAMES)))
                if not isinstance(v, int) or v < 0:
                    sys.exit('mklevels: level %d "%s" %s is %r, wanted a '
                             'count' % (i, side, k, v))
            if r.get('base', 0) != 1:
                sys.exit('mklevels: level %d "%s" has %d bases -- the base '
                         'is the win condition, so each side needs exactly '
                         'one' % (i, side, r.get('base', 0)))
            rosters[side] = {n: r.get(n, 0) for n in UNIT_NAMES}

        turns = lv.get('turns', default_turns)
        if not isinstance(turns, int) or turns < 1:
            sys.exit('mklevels: level %d has a bad "turns": %r' % (i, turns))

        out.append({
            'n': i,
            'mission': field('mission', NO_MISSION),
            'dateline': field('dateline', ''),
            'turns': turns,
            'player': rosters['player'],
            'enemy': rosters['enemy'],
            'briefing': field('briefing'),
        })
    if not out:
        sys.exit('mklevels: %s defines no levels' % path)
    return out


def wrap(s, width=WIDTH):
    """Word wrap, the way the game will have to do it."""
    words, lines, cur = s.split(), [], ''
    for w in words:
        if len(w) > width:
            sys.exit('mklevels: %r is longer than the %d-column screen'
                     % (w, width))
        if not cur:
            cur = w
        elif len(cur) + 1 + len(w) <= width:
            cur += ' ' + w
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input', help='docs/levels.json')
    ap.add_argument('--text', required=True, help='output .txt for mktext')
    ap.add_argument('--header', required=True, help='output .h')
    ap.add_argument('--source', required=True,
                    help='output .c for the ZX0 block and the mission table')
    ap.add_argument('--zx0', default=os.path.expanduser('~/z88dk/bin/z88dk-zx0'))
    ap.add_argument('--dzx0', default=os.path.expanduser('~/z88dk/bin/z88dk-dzx0'))
    ap.add_argument('--rows', type=int, default=14,
                    help='rows the brief screen has for the briefing')
    a = ap.parse_args()

    lv = parse(a.input)

    # A briefing that will not fit the screen is a writing bug, and the
    # build is the only place it gets caught before somebody sees it
    # truncated on a real machine.
    for x in lv:
        rows = len(wrap(x['briefing']))
        if rows > a.rows:
            sys.exit('mklevels: level %d\'s briefing wraps to %d rows, the '
                     'screen has %d' % (x['n'], rows, a.rows))

    # A mission title too long for the header is a WRITING bug.  It is
    # truncated rather than fatal because the level book is still being
    # written, but it is reported every build so it cannot be missed.
    for x in lv:
        if len(x['mission']) > HEADER_COLS:
            print('mklevels: level %d mission is %d chars, the header holds '
                  '%d -- TRUNCATED to %r'
                  % (x['n'], len(x['mission']), HEADER_COLS,
                     x['mission'][:HEADER_COLS]))
            x['mission'] = x['mission'][:HEADER_COLS]

    # --- the missions, for the resident pool -------------------------
    txt = ['# GENERATED by tools/mklevels.py from %s -- do not edit.'
           % os.path.basename(a.input),
           '#',
           '# The mission titles, which replace "THE FIELD" in the ST_PLAY',
           '# header.  Resident rather than in the briefing block because',
           '# the header is drawn while the board is composed, and the',
           '# block unpacks into the compose buffer.',
           '']
    for x in lv:
        txt.append('%-30s = %s' % ('MISSION_%d' % x['n'], x['mission']))
    txt.append('')
    open(a.text, 'w').write('\n'.join(txt))

    # --- dateline + briefing, one ZX0 block --------------------------
    # PRE-WRAPPED, one NUL-terminated LINE at a time, and the briefing
    # ends with an empty line.
    #
    # The runtime used to hold the wrap: it measured each word, tracked a
    # column and printed character by character.  That was ~250 bytes of
    # code and it would not fit the contended window, so it paid for
    # uncontended space instead -- the expensive kind.  Wrapping here
    # instead turns render_brief() into "print_at each line until an
    # empty one", which is a handful of instructions.
    #
    # The data cost is one byte per line, ~90 across the ten levels, and
    # ZX0 barely notices them.  Trading 90 bytes of compressible data for
    # ~170 bytes of code in the scarcest region is the whole point.
    pool, offs = bytearray(), []
    for x in lv:
        offs.append(len(pool))
        pool += x['dateline'].encode('latin-1') + b'\x00'
        for line in wrap(x['briefing']):
            pool += line.encode('latin-1') + b'\x00'
        pool += b'\x00'                  # empty line: end of the briefing

    with tempfile.TemporaryDirectory() as t:
        raw, z, rt = (os.path.join(t, n) for n in ('a.bin', 'a.zx0', 'a.rt'))
        open(raw, 'wb').write(bytes(pool))
        subprocess.run([a.zx0, '-f', raw, z], check=True, capture_output=True)
        blob = open(z, 'rb').read()
        # ROUND TRIP: a compressor that quietly alters data has cost this
        # project two shipped bugs.  See .claude/skills/test-design.
        subprocess.run([a.dzx0, z, rt], check=True, capture_output=True)
        if open(rt, 'rb').read() != bytes(pool):
            sys.exit('mklevels: the briefing block fails a ZX0 round trip')

    h = ['/* GENERATED by tools/mklevels.py from %s -- do not edit. */'
         % os.path.basename(a.input),
         '',
         '#ifndef _LEVELS_H_',
         '#define _LEVELS_H_',
         '',
         '#include <stdint.h>',
         '',
         '#define LEVEL_BOOK_COUNT %d' % len(lv),
         '',
         '/* Turns allowed, per level.  ST_PLAY shows what is LEFT of this',
         '   and the level score is whatever was not spent. */',
         'static const uint8_t level_turns[LEVEL_BOOK_COUNT] = {',
         '    ' + ','.join(str(x['turns']) for x in lv) + '',
         '};',
         '',
         '/* Dateline then briefing, NUL-terminated, per level.',
         '   %d bytes raw, ZX0 to %d.  Unpacked into MEM_VBUF by ST_BRIEF:'
         % (len(pool), len(blob)),
         '   the compose buffer is idle there and render_play() rebuilds it. */',
         '#define LEVEL_BRIEF_RAW %d' % len(pool),
         '',
         'static const uint16_t level_brief_off[LEVEL_BOOK_COUNT] = {',
         '    ' + ','.join(str(o) for o in offs),
         '};',
         '',
         'extern const uint8_t level_brief_zx0[%d];' % len(blob),
         '',
         '/* The mission titles, by level.  Pointers into the resident',
         '   string pool: the header is drawn while the board is composed,',
         '   so these cannot live in the briefing block. */',
         'extern const char *const level_mission[LEVEL_BOOK_COUNT];',
         '',
         '#endif /* _LEVELS_H_ */',
         '']
    open(a.header, 'w').write('\n'.join(h))

    # The .c is named explicitly.  It used to be DERIVED from the header
    # path -- dirname(dirname(header)) + /src/levels.c -- which quietly
    # resolved to /src/levels.c when the header was given as /tmp/a.h.
    # A generator that writes outside the tree when its arguments change
    # shape is a generator waiting to do damage.
    src = a.source
    c = ['/* GENERATED by tools/mklevels.py -- do not edit. */',
         '',
         '#include <stdint.h>',
         '',
         '#include "../include/levels.h"',
         '#include "../include/strings.h"',
         '',
         'const char *const level_mission[LEVEL_BOOK_COUNT] = {']
    for x in lv:
        c.append('    TXT_MISSION_%d,' % x['n'])
    c += ['};',
          '',
          'const uint8_t level_brief_zx0[%d] = {' % len(blob)]
    for i in range(0, len(blob), 16):
        c.append('    ' + ','.join('%d' % b for b in blob[i:i + 16]) + ',')
    c += ['};', '']
    open(src, 'w').write('\n'.join(c))

    nodate = [x['n'] for x in lv if not x['dateline']]
    if nodate:
        print('mklevels: levels %s have no Dateline: -- the brief will omit it'
              % ','.join(str(n) for n in nodate))
    blank = [x['n'] for x in lv if x['mission'] == NO_MISSION]
    if blank:
        print('mklevels: levels %s have no Mission: -- using %r'
              % (','.join(str(n) for n in blank), NO_MISSION))
    if folded:
        seen = sorted(set((c, r) for _, c, r in folded))
        print('mklevels: folded %s'
              % ', '.join('%r->%r' % (c, r) for c, r in seen))

    longest = max(len(x['briefing']) for x in lv)
    biggest = max(sum(x[side].values()) for x in lv for side in ('player','enemy'))
    print('mklevels: armies %d..%d a side, largest %d'
          % (min(sum(x['player'].values()) for x in lv),
             max(sum(x['player'].values()) for x in lv), biggest))
    print('mklevels: %d levels, turns %s;  briefings %d chars, block %d -> %d '
          'ZX0, round trip ok'
          % (len(lv), '/'.join(str(x['turns']) for x in lv),
             sum(len(x['briefing']) for x in lv), len(pool), len(blob)))
    print('mklevels: longest briefing %d chars, %d rows'
          % (longest, max(len(wrap(x['briefing'])) for x in lv)))


if __name__ == '__main__':
    sys.exit(main())
