/* Telemetry that never waits for a listener.
 *
 * The Waveshare board talks over a UART: characters go down the wire whether
 * anyone is attached or not, and a write never blocks for long. The handheld
 * talks over the chip's native USB, and that is a different contract -- when no
 * host is draining the endpoint the buffer fills and the write BLOCKS.
 *
 * This game prints enormously: the decompiled sources narrate their own
 * progress, and the port adds per-frame telemetry on top. On USB that turns
 * into a cooperative scheduler stopped dead inside a printf, with every mts
 * task frozen behind it -- the board booted, mounted the card, found all 96
 * stages, started the sound task and then went silent forever. Nothing
 * crashed, and from the outside it looked like a hang in the sound code.
 *
 * So printing is best-effort: format into a small buffer and hand it to the
 * USB driver with a zero timeout. With a terminal attached everything appears
 * as before; with nobody listening the bytes are dropped and the game runs at
 * full speed, which is what a console should do anyway.
 *
 * Included into every game translation unit through -include, so the game's own
 * printf calls are covered without editing 700 files.
 */
#ifndef MGS_PRINTF_H
#define MGS_PRINTF_H

#if defined(MGS_BOARD_XIAO) || defined(ESP_PLATFORM)

/* no <stdio.h> here: libsio/dummy.c defines its own getchar() and must not
 * see stdio's macro; the prototype needs nothing from it */
int Mgs_Printf(const char* fmt, ...);
/* the va_list form, for the game's own printf shims (mts_printf, fprintf)
 * which must not reach stdio behind the wrapper's back */
int Mgs_Vprintf(const char* fmt, __builtin_va_list ap);

/* The game calls printf; route it here. Undefined first because some headers
 * in this tree have their own ideas about the name.
 *
 * On every ESP build the wrapper also counts how many callers are inside it
 * (mgs_in_printf): stdio takes a lock, and the vblank tick must not deliver
 * an interrupt -- which can stop the running thread where it stands -- while
 * a thread holds that lock, or the next printf anywhere (the tick's own
 * periodic report, the sound task's) waits for a thread that will never run
 * again. */
#undef printf
#define printf Mgs_Printf
extern volatile int mgs_in_printf;

#endif /* MGS_BOARD_XIAO || ESP_PLATFORM */

/* A breadcrumb that survives a saturated console.
 *
 * When the game is wedged, its own printfs are exactly what cannot get out --
 * the buffer is full of them. This writes a pointer to a string literal into
 * one global, costing a single store, and the vblank tick prints it with its
 * own once-per-3000-ticks line. That line does get through, so the last place
 * a stuck task reached is always visible. */
#ifdef __psyz
extern const char *mgs_where;
#define MGS_WHERE(s) (mgs_where = (s))
#else
#define MGS_WHERE(s) ((void)0)
#endif

#endif /* MGS_PRINTF_H */
