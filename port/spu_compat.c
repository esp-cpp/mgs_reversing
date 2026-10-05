/* libspu entry points the PSY-Q decompilation only has as assembly, written
 * against psyz's SPU register model. The IRQ pair is what MGS's sound driver
 * runs on (sd_main.c: a silent sample loops at the IRQ address and every pass
 * ticks the driver); the reverb and LFO calls are accepted and ignored, psyz's
 * SPU has neither.
 */
#include <stddef.h>
#include <libspu.h>
#include "../../decomp/src/libspu/libspu_private.h" /* psyz's layout, as its own libspu.c does */

extern void Psyz_SpuWrite(unsigned int reg_offset, unsigned short value);
extern unsigned short Psyz_SpuRead(unsigned int reg_offset);

long SpuSetIRQ(long on_off) {
    unsigned short cnt = Psyz_SpuRead(offsetof(SPU_RXX, spucnt));
    long prev = (cnt & SPU_CTRL_MASK_IRQ9_ENABLE) ? SPU_ON : SPU_OFF;
    if (on_off == SPU_ON) {
        cnt |= SPU_CTRL_MASK_IRQ9_ENABLE;
    } else if (on_off == SPU_OFF) {
        cnt &= ~SPU_CTRL_MASK_IRQ9_ENABLE;
    } else {
        return prev; /* SPU_BIT: query */
    }
    Psyz_SpuWrite(offsetof(SPU_RXX, spucnt), cnt);
    return prev;
}

unsigned long SpuSetIRQAddr(unsigned long addr) {
    /* the register holds the address in 8-byte units */
    Psyz_SpuWrite(offsetof(SPU_RXX, irq_addr), (unsigned short)(addr >> 3));
    return addr & ~7ul;
}

long SpuGetKeyStatus(unsigned long voice_bit) {
    /* one voice: on while its envelope is still producing output */
    int v;
    for (v = 0; v < 24; v++) {
        if (voice_bit & (1ul << v)) {
            return _spu_RXX->rxx.voice[v].volumex ? SPU_ON : SPU_OFF;
        }
    }
    return SPU_OFF;
}

/* reverb: not emulated */
long SpuSetReverbDepth(SpuReverbAttr* attr) { (void)attr; return 0; }
unsigned long SpuSetReverbVoice(long on_off, unsigned long voice_bit) {
    (void)on_off;
    return voice_bit;
}
long SpuReserveReverbWorkArea(long on_off) { (void)on_off; return SPU_ON; }
unsigned long SpuSetPitchLFOVoice(long on_off, unsigned long voice_bit) {
    (void)on_off;
    return voice_bit;
}
