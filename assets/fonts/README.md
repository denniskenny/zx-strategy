# Fonts

| file | origin |
|---|---|
| `artic_ship_of_doom.ch8` | *Adventure C - The Ship of Doom v2* (1982, Artic Computing) `#64489` |

Renamed from the original filename because **Make cannot take a
prerequisite containing spaces** -- the rule silently split
`Adventure C - ...` into three targets and failed with
"No rule to make target `assets/fonts/Adventure'".

A `.ch8` is 8 bytes per glyph, one bit per pixel, MSB left, starting at
character 32 -- the same layout the ROM uses at `0x3D00`, which is why
replacing the font is a pointer change. See `tools/mkfont.py`.
