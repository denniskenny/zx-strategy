/*
 * hw_detect.c — 128K + Kempston detection
 *
 * 128K test: write to bank 1 at 0xC000, switch to bank 2, write a
 * different value, switch back to bank 1 and check the original value
 * survived.  On 48K the port 0x7FFD writes are ignored so 0xC000 always
 * sees the last write → detection fails.  On 128K the banks are
 * separate → detection succeeds.
 *
 * Must run before paging is locked.
 */

#include "../include/hw.h"

uint8_t is_128k = 0;
uint8_t has_kempston = 0;

void hw_detect(void)
{
    __asm
        ;; Interrupts OFF for the whole probe.  Every OUT below changes
        ;; the memory map, and on a +2A/+3 it changes the ROM with it
        ;; (see the bit-4 note); an IM 1 interrupt landing in the middle
        ;; vectors to 0x0038 in whatever ROM happens to be paged, which
        ;; on a +3 is +3DOS rather than BASIC.
        di

        ;; Save the byte currently at 0xC000
        ld  a, (0xC000)
        ld  d, a            ; D = saved original byte

        ;; Select bank 1.  Bits 0-2 are the bank; BIT 4 IS THE ROM, and
        ;; it is set in every value written here on purpose.
        ;;
        ;; On a 128K bit 4 chooses ROM 0 (128 editor) or ROM 1 (48K
        ;; BASIC).  On a +2A/+3 the ROM number is two bits — 0x1FFD bit
        ;; 2 above 0x7FFD bit 4 — and a 48K-format tap loads from 48
        ;; BASIC, which is ROM 3.  Writing bit 4 clear would drop that
        ;; to ROM 2: +3DOS.  Every interrupt from then on runs +3DOS
        ;; code at 0x0038, and print_at() reads its character set from
        ;; 0x3D00 in a ROM that has not got one.
        ;;
        ;; That is the suspected cause of both the garbled text and the
        ;; "Nonsense in BASIC" crash on a +3.  Leaving bit 4 alone costs
        ;; nothing and the probe only cares about bits 0-2.
        ld  bc, 0x7FFD
        ld  a, 0x11
        out (c), a

        ld  a, 0xAA
        ld  (0xC000), a

        ;; Switch to bank 2 and write a different value
        ld  a, 0x12
        out (c), a
        ld  a, 0x55
        ld  (0xC000), a

        ;; Back to bank 1 — on 128K this should still read 0xAA
        ld  a, 0x11
        out (c), a
        ld  a, (0xC000)
        cp  0xAA
        jr  nz, _hw_48k

        ;; 128K: restore both banks, then select bank 0 — still with
        ;; bit 4 set, so the ROM is exactly as the loader left it.
        ld  a, 0x12
        out (c), a
        ld  a, d
        ld  (0xC000), a
        ld  a, 0x11
        out (c), a
        ld  a, d
        ld  (0xC000), a
        ld  a, 0x10
        out (c), a

        ld  a, 1
        ld  (_is_128k), a
        jr  _hw_done

    _hw_48k:
        ;; Restore original byte at 0xC000
        ld  a, d
        ld  (0xC000), a

        ;; Select bank 0 (harmless on 48K — port is not decoded).
        ;; Bit 4 set, as above.
        ld  a, 0x10
        out (c), a

        xor a
        ld  (_is_128k), a

    _hw_done:

        ;; --- Kempston joystick detection ---
        ;; An idle Kempston reads 0 on every sample.  With no interface
        ;; the port is unattached, so the floating bus answers with
        ;; whatever the ULA last fetched — usually noise, but it hits 0
        ;; often enough that "any zero read" is a false positive (which
        ;; then feeds random directions into scan_input()).  Require
        ;; EVERY sample to be zero instead.
        ;; Bits 5-7 are undefined on many interfaces, so mask to 0x1F.
        ld  b, 16           ; sample 16 times
        ld  c, 0x1F
    _kemp_loop:
        in  a, (c)
        and 0x1F
        jr  nz, _kemp_none  ; any non-zero → floating bus, no Kempston
        djnz _kemp_loop
        ld  a, 1            ; all samples idle → Kempston present
        ld  (_has_kempston), a
        jr  _kemp_done
    _kemp_none:
        xor a
        ld  (_has_kempston), a
    _kemp_done:

        ;; Interrupts back on.  The memory map and the ROM are exactly
        ;; as the loader left them, bar the bank at 0xC000.
        ei
    __endasm;
}
