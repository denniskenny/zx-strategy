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
        ;; Save the byte currently at 0xC000
        ld  a, (0xC000)
        ld  d, a            ; D = saved original byte

        ;; Select bank 1 (port 0x7FFD, bits 0-2 = bank number)
        ld  bc, 0x7FFD
        ld  a, 0x01
        out (c), a

        ld  a, 0xAA
        ld  (0xC000), a

        ;; Switch to bank 2 and write a different value
        ld  a, 0x02
        out (c), a
        ld  a, 0x55
        ld  (0xC000), a

        ;; Back to bank 1 — on 128K this should still read 0xAA
        ld  a, 0x01
        out (c), a
        ld  a, (0xC000)
        cp  0xAA
        jr  nz, _hw_48k

        ;; 128K: restore both banks, then select bank 0
        ld  a, 0x02
        out (c), a
        ld  a, d
        ld  (0xC000), a
        ld  a, 0x01
        out (c), a
        ld  a, d
        ld  (0xC000), a
        xor a
        out (c), a

        ld  a, 1
        ld  (_is_128k), a
        jr  _hw_done

    _hw_48k:
        ;; Restore original byte at 0xC000
        ld  a, d
        ld  (0xC000), a

        ;; Select bank 0 (harmless on 48K — port is not decoded)
        xor a
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
    __endasm;
}
