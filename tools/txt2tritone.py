#!/usr/bin/env python3
"""Convert a ZXBeep transcription (.txt) into Beepola Tritone assembly.

The transcription's "PER-BAR ATTACK TABLES" section is the machine-readable
source: one bar per pattern, three tone channels (Lead/Harm/Bass) plus an
optional drum channel, each a list of `row:...` events.  This tool encodes
them into Tritone's byte format and splices the pattern data onto the player
engine copied verbatim from a template .asm (default:
assets/music/tritone_template.asm, a Beepola Tritone export with its song data
stripped).

TRITONE FORMAT (see https://battleofthebits.com/lyceum/View/Tritone/)
---------------------------------------------------------------------
  * 3 tone channels, each with 8 pulse widths (timbre).  The note's high
    nibble is the pulse-width / duty threshold; nibble 8 = 50% "nessy" tone,
    up to nibble F = the thin 8% "speccy coil".  We expose these as pulse
    width 0..7  (nibble = 8 + pw), so every value is valid on all channels
    (the lead channel requires nibble >= 8 to avoid the drum byte range).
  * 1 drum channel, up to 24 instruments in this template (35 in newer
    Tritone).  Instruments 0/1 are unusable (byte 0/1 mean sustain/key-off),
    so a drum stored as byte = instrument+1.  A drum is a leading 2..127 byte
    on its row.
  * Speed is the Fxx-at-pattern-start effect -> the per-pattern tempo word.
  * Volume: "equal" or "quiet-medium-loud L->R" is a compile-time engine
    variant chosen by the template .asm, not by the song data.
  * No arpeggios / slides / portamentos exist in the engine.

Byte encoding:
  note byte1 = ((8+pw)<<4) | pitch-hi-nibble    note byte2 = pitch-lo-byte
  $01 = sustain (ring previous note)            $00 = key-off (silence)
  leading 2..127 byte on a row = drum           $FF = end of pattern

Pitch is the value added to a phase accumulator each audio loop, so it is
directly proportional to frequency.  We use equal temperament anchored at
A4 = 1258 (the reference export's value), which matches the engine's tuning
to within ~4 cents (verified by decoding the .asm back).

TRANSCRIPTION SYNTAX
--------------------
  BAR 3   ... [speed=4]                 ; optional per-bar speed override
    CH1 Lead :  0:G4   8:G4@3(4)  12:A4 ; @N = pulse width 0..7 (default 0)
    CH2 Harm :  0:Eb4  8:Eb4      12:F4
    CH3 Bass :  0:F3   4:C3       8:F3
    CH4 Drum :  0:kick 4:snare    8:12  ; instrument name or 1..24
  (note tokens: row:NOTE[@pw][(duration)];  NOTE is A-G with #/b + octave,
   or OFF for key-off.  Drum tokens: row:instrument.)

Usage:
  tools/txt2tritone.py INPUT.txt -o OUTPUT.asm
      [--template assets/music/tritone_template.asm]
      [--speed N] [--rows 16]
      [--pw 0] [--lead-pw N] [--harm-pw N] [--bass-pw N]
      [--kick INSTR] [--snare INSTR]   # convenience drums (rows 0,8 / 4,12)
"""
import argparse
import math
import re

# --- pitch -> add-value (equal temperament, semitones from A4) ---
REF_A4 = 1258.0
LETTER = {'C': -9, 'D': -7, 'E': -5, 'F': -4, 'G': -2, 'A': 0, 'B': 2}

# drum name aliases -> 1-based instrument number (Shiru 24-drum table;
# indices from the template's DRUM_SETTINGS comments). Override numerically.
DRUM_ALIASES = {
    'tick': 1, 'click': 1,
    'kick': 8, 'bd': 8, 'bass': 8,        # lowest tone
    'tom': 5, 'special': 9,
    'snare': 17, 'sd': 17,                 # mid noise
    'hihat': 15, 'hh': 15, 'hat': 15,      # high noise
    'noise': 19,
}


def note_add(name):
    """'Eb4' / 'A#4' / 'D5' -> 12-bit add value."""
    m = re.fullmatch(r'([A-G])([#b]?)(-?\d)', name)
    if not m:
        raise ValueError(f"unparseable note {name!r}")
    letter, acc, octv = m.group(1), m.group(2), int(m.group(3))
    semi = LETTER[letter] + (1 if acc == '#' else -1 if acc == 'b' else 0)
    semi += (octv - 4) * 12
    add = round(REF_A4 * (2.0 ** (semi / 12.0)))
    if not (1 <= add <= 0x0FFF):
        raise ValueError(f"note {name} -> add {add} out of 12-bit range")
    return add


# --- parsing ---
NOTE_TOKEN = re.compile(
    r'(\d+):(OFF|[A-G][#b]?-?\d)(?:@([0-7]))?(?:\((\d+)\))?')
DRUM_TOKEN = re.compile(r'(\d+):([A-Za-z]+|\d+)')
BAR_RE = re.compile(r'^\s*BAR\s+(\d+)\b')
CH_RE = re.compile(r'^\s*CH\s*([1234])\b')
DRUM_LINE_RE = re.compile(r'^\s*(?:CH\s*4\b|Drums?\b|Perc)', re.I)
SPEED_RE = re.compile(r'speed\s*=\s*(\d+)')


def parse_transcription(text, drum_count):
    """Return (bars, global_speed).  Each bar is a dict:
       {'tone': {chan: {row: (note, pw)}},
        'drum': {row: instrument_number},
        'speed': int|None}."""
    lines = text.splitlines()
    start = next((i for i, l in enumerate(lines) if 'ATTACK TABLE' in l.upper()), 0)
    end = next((i for i, l in enumerate(lines) if 'FULL 16-ROW GRID' in l.upper()),
               len(lines))
    section = lines[start:end]

    bars = []
    cur = None
    for l in section:
        mb = BAR_RE.match(l)
        if mb:
            ms = SPEED_RE.search(l)
            cur = {'tone': {0: {}, 1: {}, 2: {}}, 'drum': {},
                   'speed': int(ms.group(1)) if ms else None}
            bars.append(cur)
            continue
        if cur is None:
            continue
        if DRUM_LINE_RE.match(l):
            for row, val in DRUM_TOKEN.findall(l.split(':', 1)[1] if ':' in l else l):
                r = int(row)
                if val.isdigit():
                    instr = int(val)
                else:
                    key = val.lower()
                    if key not in DRUM_ALIASES:
                        raise SystemExit(f"error: unknown drum '{val}'")
                    instr = DRUM_ALIASES[key]
                if not (1 <= instr <= drum_count):
                    raise SystemExit(
                        f"error: drum instrument {instr} out of range 1..{drum_count}")
                cur['drum'][r] = instr
            continue
        mc = CH_RE.match(l)
        if mc and mc.group(1) in '123':
            chan = int(mc.group(1)) - 1
            body = l.split(':', 1)[1] if ':' in l else l
            for row, note, pw, _dur in NOTE_TOKEN.findall(body):
                cur['tone'][chan][int(row)] = (note, int(pw) if pw else None)
    if not bars:
        raise SystemExit("error: no BAR/CH attack tables found in transcription")

    speed = None
    m = re.search(r'speed"?\s*=\s*(\d+)', text)
    if m:
        speed = int(m.group(1))
    else:
        m = re.search(r'(\d+)\s*frames?/row', text)
        if m:
            speed = int(m.group(1))
    m = re.search(r'\brows\s*=\s*(\d+)', text)
    rows = int(m.group(1)) if m else None
    return bars, speed, rows


# --- encoding ---
def encode_pattern(bar, rows, chan_pw):
    """Return a list of rows, each a list of byte values."""
    out = []
    for r in range(rows):
        rb = []
        if r in bar['drum']:
            rb.append(bar['drum'][r] + 1)      # instrument -> byte (0/1 reserved)
        for ch in (0, 1, 2):
            ev = bar['tone'][ch].get(r)
            if ev is None:
                rb.append(0x01)                # sustain
            elif ev[0] == 'OFF':
                rb.append(0x00)                # key-off
            else:
                note, pw = ev
                pw = chan_pw[ch] if pw is None else pw
                add = note_add(note)
                rb.append(((8 + pw) << 4) | ((add >> 8) & 0x0F))
                rb.append(add & 0xFF)
        out.append(rb)
    return out


def fmt_defb(byte_row):
    return "                DEFB  " + ",".join(f"${b:02X}" for b in byte_row)


def count_drums(template):
    n = 0
    seen = False
    for l in template.splitlines():
        if 'DRUM_SETTINGS:' in l:
            seen = True
            continue
        if seen:
            if 'DEFB' in l:
                n += 1
            elif re.match(r'^[A-Za-z_]\w*:', l):
                break
    return n or 24


def speed_to_tempo(speed):
    return round(speed * 69888 / 153)          # Tritone loop counter


def build_asm(template, bars, global_speed, rows, chan_pw):
    tlines = template.splitlines()
    split = next(i for i, l in enumerate(tlines) if 'Song layout' in l)
    header = "\n".join(tlines[:split]).rstrip("\n")

    # IDENTICAL BARS SHARE A PATTERN.
    #
    # The song layout is a list of pointers, so a bar that encodes to the
    # same bytes as an earlier one costs two bytes in the list instead of
    # a whole pattern.  Repeats are the norm in marches and dance tunes --
    # The British Grenadiers repeats "d2 G2 BAGF" verbatim -- and at 8
    # rows a pattern is ~34 bytes, which is the difference between a tune
    # fitting under the 0xC000 ceiling and not.
    encoded = []
    for bar in bars:
        speed = bar['speed'] if bar['speed'] is not None else global_speed
        pat = encode_pattern(bar, rows, chan_pw)
        encoded.append((speed, speed_to_tempo(speed),
                        tuple(tuple(r) for r in pat)))

    uniq, order, seq = {}, [], []
    for e in encoded:
        if e not in uniq:
            uniq[e] = len(order)
            order.append(e)
        seq.append(uniq[e])

    out = [header, ""]
    out.append("; *** Song layout (generated by tools/txt2tritone.py) ***")
    out.append(f"; {len(seq)} bars, {len(order)} distinct patterns")
    out.append(f"LOOPSTART:            DEFW      PAT{seq[0]}")
    for i in seq[1:]:
        out.append(f"                      DEFW      PAT{i}")
    out.append("                      DEFW      $0000")
    out.append("                      DEFW      LOOPSTART")
    out.append("")
    out.append("; *** Patterns ***")

    for i, (speed, tempo, pat) in enumerate(order):
        out.append(f"PAT{i}:")
        out.append(f"                DEFW  {tempo}     ; Pattern tempo (speed {speed})")
        out.append("                ;    Drum,Chan.1 ,Chan.2 ,Chan.3")
        for byte_row in pat:
            out.append(fmt_defb(list(byte_row)))
        out.append("                DEFB  $FF  ; End of Pattern")
        out.append("")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description="ZXBeep .txt -> Tritone .asm")
    ap.add_argument("input", help="transcription .txt")
    ap.add_argument("-o", "--output", required=True, help="output .asm")
    ap.add_argument("--template", default="assets/music/tritone_template.asm",
                    help="template .asm supplying the player engine + drums + "
                         "volume mode")
    ap.add_argument("--speed", type=int, default=None,
                    help="frames per row (overrides value parsed from txt)")
    ap.add_argument("--rows", type=int, default=None,
                    help="rows per bar/pattern (overrides 'rows =' in txt; "
                         "default 16)")
    ap.add_argument("--pw", type=int, default=0,
                    help="default pulse width 0..7 (0 = 50%%, 7 = thin coil)")
    ap.add_argument("--lead-pw", type=int, default=None, dest="lead_pw")
    ap.add_argument("--harm-pw", type=int, default=None, dest="harm_pw")
    ap.add_argument("--bass-pw", type=int, default=None, dest="bass_pw")
    ap.add_argument("--kick", type=int, default=None,
                    help="drum instrument for kick on rows 0 and 8 (if no "
                         "CH4/Drum line present)")
    ap.add_argument("--snare", type=int, default=None,
                    help="drum instrument for snare on rows 4 and 12")
    args = ap.parse_args()

    for v in (args.pw, args.lead_pw, args.harm_pw, args.bass_pw):
        if v is not None and not (0 <= v <= 7):
            ap.error("pulse width must be 0..7")

    template = open(args.template).read()
    drum_count = count_drums(template)
    text = open(args.input).read()
    bars, parsed_speed, parsed_rows = parse_transcription(text, drum_count)
    rows = args.rows if args.rows is not None else (parsed_rows or 16)

    # CLI march drums as a fallback when the transcription has no drum line.
    if not any(b['drum'] for b in bars) and (args.kick or args.snare):
        for b in bars:
            if args.kick:
                b['drum'][0] = args.kick
                b['drum'][8] = args.kick
            if args.snare:
                b['drum'][4] = args.snare
                b['drum'][12] = args.snare

    speed = args.speed if args.speed is not None else (parsed_speed or 5)
    chan_pw = [
        args.lead_pw if args.lead_pw is not None else args.pw,
        args.harm_pw if args.harm_pw is not None else args.pw,
        args.bass_pw if args.bass_pw is not None else args.pw,
    ]

    asm = build_asm(template, bars, speed, rows, chan_pw)
    open(args.output, "w").write(asm)

    nnotes = sum(len(c) for b in bars for c in b['tone'].values())
    ndrums = sum(len(b['drum']) for b in bars)
    print(f"parsed {len(bars)} bars ({rows} rows each), "
          f"{nnotes} tone attacks, {ndrums} drum hits")
    print(f"tempo: speed {speed} frames/row -> counter {speed_to_tempo(speed)}"
          + (" (per-bar overrides applied)"
             if any(b['speed'] for b in bars) else ""))
    print(f"pulse widths (lead,harm,bass) = {tuple(chan_pw)}; "
          f"template drums = {drum_count}")
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
