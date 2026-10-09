/* Save states.
 *
 * A snapshot is the game's memory as the linker lays it out (the PSRAM
 * regions bracketed by the _mgs_* symbols: game, psyz and port statics, which
 * include the main RAM, VRAM, SPU RAM and the game threads' stacks), plus each
 * game thread's saved context. It is taken and restored while the game is
 * paused: every game thread suspended or parked, the rasterizer and the mixer
 * idle.
 *
 * The port's statics hold a few OS resources (task and semaphore handles,
 * file handles, heap pointers) that belong to THIS run, not to the snapshot.
 * Each file registers those addresses here once; the restore copies them out
 * before the memory is replaced and puts them back after. */
#ifndef MGS_SNAPSHOT_H
#define MGS_SNAPSHOT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* keep this range of a game-region static as it is across a restore */
void Mgs_SnapshotPreserve(void* p, size_t n);

/* one game thread's saved state (esp32_threads.c) */
typedef struct {
    int in_use;
    int entry_present; /* 0: the main game task (its stack is the cart's) */
    int forced;        /* stopped mid-code by the tick: suspended, not parked */
    int crit;
    unsigned top;      /* saved stack pointer (FreeRTOS pxTopOfStack) */
    unsigned notify;   /* a wake given but not yet taken */
} MgsThreadSnap;

#define MGS_SNAPSHOT_THREADS 8

void Mgs_ThreadsSnapshotSetup(void);
int Mgs_ThreadsSnapshot(MgsThreadSnap* out, int n);
int Mgs_ThreadsRestore(const MgsThreadSnap* in, int n);
/* where thread i keeps its stack (the fixed pool, or the cart's main stack) */
int Mgs_ThreadStackRegion(int i, void** base, size_t* size);
/* provided by the cart glue: the main game task's static stack */
void Mgs_MainTaskStack(void** base, size_t* size);

void Mgs_CdSnapshotSetup(void);
void Mgs_CdAfterRestore(void);
void Mgs_CdHoldReadAhead(void);
void Mgs_CdReleaseReadAhead(void);
void Mgs_VblankSnapshotSetup(void);
void Mgs_PrintfSnapshotSetup(void);
void Mgs_PrintfAfterRestore(void);
void Psyz_GpuSnapshotSetup(void);
void Psyz_GpuAfterRestore(void);

#ifdef __cplusplus
}
#endif
#endif
