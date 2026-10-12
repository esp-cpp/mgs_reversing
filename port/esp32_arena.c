/* One PSRAM block for the port's big buffers: the PSX's main RAM image, the
 * codec face buffer, VRAM, the SPU state, the GPU command queues and the
 * game threads' stacks (about 3.8 MB). They used to be static, which held
 * the memory permanently; now they exist from the first launch on.
 *
 * The block is kept across the cart's quits on purpose: a save state is full
 * of absolute pointers into these buffers and loads only while they sit at
 * the same addresses, and MGS launched again from the launcher normally
 * gets the same block back. The launcher releases it before it starts any
 * other cart (Mgs_ArenaRelease), since Jedi Knight needs the room. */
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

/* The block and the three pointers the game reads live in src/mgs.cpp: its
 * objects sit outside the statics that are wiped between launches, while
 * this file's would be wiped with the rest. The per-module pointers (SPU,
 * GPU queues, thread stacks) are inside the wiped statics on purpose and
 * are bound again after each wipe (Mgs_ArenaBind). */
extern unsigned char* g_mgs_arena;
extern size_t g_mgs_arena_size;
extern unsigned char* mgs_main_ram;   /* 0x80000 + 0xC9000 + 0x20000: the console's layout */
extern unsigned char* mgs_face_group; /* the codec's face animation group */
const unsigned mgs_face_group_size = 0x80000;
extern unsigned short* g_RawVram;     /* 1024 x 512 RGB1555 */

#define MGS_MAIN_RAM_BYTES (0x80000 + 0xC9000 + 0x20000)
#define MGS_VRAM_BYTES (1024 * 512 * 2)

size_t Psyz_SpuStateSize(void);
void Psyz_SpuBind(void* p);
size_t Psyz_GpuQueueBytes(void);
void Psyz_GpuBindQueues(void* p);
size_t Mgs_ThreadsStackBytes(void);
void Mgs_ThreadsBindStacks(void* p);

static size_t up64(size_t n) { return (n + 63) & ~(size_t)63; }

static void arena_sizes(size_t sizes[6]) {
    sizes[0] = up64(MGS_MAIN_RAM_BYTES);
    sizes[1] = up64(mgs_face_group_size);
    sizes[2] = up64(MGS_VRAM_BYTES);
    sizes[3] = up64(Psyz_SpuStateSize());
    sizes[4] = up64(Psyz_GpuQueueBytes());
    sizes[5] = up64(Mgs_ThreadsStackBytes());
}

/* allocate (first launch) or reuse the block, and zero it: each launch
 * starts from a blank machine, as the static buffers did */
int Mgs_ArenaAcquire(void) {
    size_t sizes[6], total = 0;
    int i;
    arena_sizes(sizes);
    for (i = 0; i < 6; i++) {
        total += sizes[i];
    }
    if (g_mgs_arena && g_mgs_arena_size != total) {
        heap_caps_free(g_mgs_arena);
        g_mgs_arena = NULL;
    }
    if (!g_mgs_arena) {
        g_mgs_arena = heap_caps_aligned_alloc(64, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_mgs_arena) {
            printf("[arena] no %u bytes of PSRAM (free %u, largest %u)\n", (unsigned)total,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return 0;
        }
        g_mgs_arena_size = total;
        printf("[arena] %u KB at %p\n", (unsigned)(total / 1024), (void*)g_mgs_arena);
    }
    memset(g_mgs_arena, 0, g_mgs_arena_size);
    return 1;
}

/* point every module at its part of the block; after the statics wipe */
void Mgs_ArenaBind(void) {
    size_t sizes[6], off = 0;
    unsigned char* arena = g_mgs_arena;
    arena_sizes(sizes);
    mgs_main_ram = arena ? arena + off : NULL;
    off += sizes[0];
    mgs_face_group = arena ? arena + off : NULL;
    off += sizes[1];
    g_RawVram = arena ? (unsigned short*)(arena + off) : NULL;
    off += sizes[2];
    Psyz_SpuBind(arena ? arena + off : NULL);
    off += sizes[3];
    Psyz_GpuBindQueues(arena ? arena + off : NULL);
    off += sizes[4];
    Mgs_ThreadsBindStacks(arena ? arena + off : NULL);
}

void Mgs_ArenaRelease(void) {
    if (!g_mgs_arena) {
        return;
    }
    heap_caps_free(g_mgs_arena);
    g_mgs_arena = NULL;
    g_mgs_arena_size = 0;
    Mgs_ArenaBind();
}

unsigned char* Mgs_ArenaBase(size_t* size) {
    if (size) {
        *size = g_mgs_arena_size;
    }
    return g_mgs_arena;
}
