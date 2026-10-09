/* The vertical blank, which the PSX gave games for free.
 *
 * On the console a vblank interrupt fires 60 times a second and the BIOS calls
 * whatever the game registered through VSyncCallback(). MGS's mts scheduler is
 * built on it: mts_wait_vbl() parks a task until the callback has advanced the
 * frame counter, and the whole boot sequence blocks on that. Without a tick
 * here the game reaches mts_init_vsync() and never comes back -- which is
 * exactly the black screen.
 *
 * WHERE the tick runs matters as much as that it runs. On the console the
 * vblank interrupted THE SAME CPU the game was executing on, so the scheduler
 * work inside the callback could never race the task it was preempting. The
 * first version of this file ran the tick on core 1 while the game ran on
 * core 0, and ChangeThFromISR's read of current_thread raced the ChangeTh in
 * flight on the other core -- once in a while it revived the parked boot
 * thread mid-stack, and a SECOND copy of Main() started interleaving with the
 * first. So: every mts task is pinned to core 0 (esp32_threads.c) and this
 * tick is pinned to core 0 at higher priority. Preemption on one core is
 * exactly an interrupt, and the race cannot exist.
 *
 * The LCD scanout stays on core 1: it only reads VRAM, and at 80 MHz SPI it
 * would otherwise eat a third of the game's frame budget.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef MGS_HUNT_ROGUE_WRITER
#include "esp_cpu.h"
#endif
#include <psyz.h>
#include "mgs_snapshot.h"
#include <libgpu.h>

extern void (*g_VsyncCallback)(void);   /* psyz/src/psyz/libetc.c */
extern int Mgs_ChangeInFlight(void);   /* esp32_threads.c */
extern volatile int psyz_critical_depth; /* psyz libapi.c: the running task's "SR" */

/* NTSC field rate. The tick is 1 ms, so 16 ms is the closest whole number;
 * that is 62.5 Hz rather than 59.94, which is near enough for pacing and can
 * be swapped for a hardware timer once frames are actually being drawn. */
#define VBLANK_PERIOD_MS 16

/* mts game tasks run at priority 5 (esp32_threads.c); the tick must preempt
 * them the way the hardware interrupt did */
#define VBLANK_TICK_PRIO 10

unsigned mgs_vblank_count;
volatile unsigned mgs_spu_irq_delivered; /* audio diagnostics */
/* where the tick task is, for the hang detector: a stalled tick is a tick
 * blocked inside one of these */
volatile const char* mgs_tick_phase = "not started";
unsigned mgs_prof_present_us; /* core-1 time in the LCD present, for the audio report */
unsigned mgs_spu_defer_crit, mgs_spu_defer_flight, mgs_spu_defer_printf, mgs_spu_defer_stdio,
    mgs_spu_defer_busy;
extern unsigned mgs_cd_hits, mgs_cd_misses, mgs_cd_sd_us, mgs_cd_pumps; /* virtual_cd.c */
extern volatile int mgs_cd_pump_busy;

/* The SPU interrupt needs a finer clock than the vblank: the sound driver's
 * timing voice fires it at two alternating addresses per loop (~98 Hz) and
 * the handler itself flips the address each time, so a handler delivered
 * 16 ms late misses the other point and the rate halves -- the driver's
 * stream position then runs slow and the voice replays a stale buffer half.
 * The task runs every SPU_TICK_MS and does the vblank work every
 * VBLANK_PERIOD_MS. */
#define SPU_TICK_MS 2

static void spu_irq_deliver(void);

static void vblank_tick_task(void* arg) {
    TickType_t next = xTaskGetTickCount();
    unsigned sub = 0;
    (void)arg;
    for (;;) {
        mgs_tick_phase = "sleep";
        vTaskDelayUntil(&next, pdMS_TO_TICKS(SPU_TICK_MS));
        mgs_tick_phase = "spu";
        spu_irq_deliver();
        if (++sub < (unsigned)(VBLANK_PERIOD_MS / SPU_TICK_MS)) {
            continue;
        }
        sub = 0;
        mgs_vblank_count++;
        mgs_tick_phase = "cd";
        if (psyz_critical_depth == 0 && !Mgs_ChangeInFlight() && mgs_in_printf == 0) {
            extern void Mgs_CdPumpFromTick(void);
            Mgs_CdPumpFromTick();
        }
        mgs_tick_phase = "pads";
#ifdef MGS_HUNT_ROGUE_WRITER
        /* One known-corrupted pack keeps landing at the same heap address
         * every boot. Arm a store watchpoint on its command word AFTER the
         * stage load settles: the shade pass never touches this obj, so the
         * next writer to trip it IS the rogue -- its PC comes out in the
         * panic backtrace. Core 0, where every mts task runs. */
        /* Trap the load-time writer red-handed. Watch word 3 of the known
         * pack (its xy0): the legitimate initializer only touches len/code
         * and the four rgb words, and this obj's per-frame transform never
         * runs -- the ONLY store to xy0 is the rogue planting its color
         * pointer. Armed early, first trip names the culprit. */
        if (mgs_vblank_count == 60u) {
            esp_cpu_set_watchpoint(0, (void*)0x3c1cd4d4, 4,
                                   ESP_CPU_WATCHPOINT_STORE);
            printf("[hunt] watchpoint armed on xy0 (0x3c1cd4d4)\n");
        }
#endif
        {
            /* sample the buttons first: the BIOS also read pads on vsync,
             * before the game's callback saw the frame */
            extern void Mgs_PadsUpdate(void);
            Mgs_PadsUpdate();
        }
        /* Deliver scheduler wakes ONLY while mts sits in its IDLE task.
         *
         * The idle task is Konami's own design: when every real task waits,
         * mts transfers into task 11 (the SIO console), whose polling spin is
         * the one piece of code that touches no scheduler state -- on the
         * console it ran with interrupts enabled precisely so the vblank
         * could preempt it. Any OTHER task may be mid-way through editing
         * ready masks or wait chains, and preempting those is what produced
         * the intermittent boot wedges: every fix that narrowed a window
         * (change_in_flight, critical sections) shrank the failure rate
         * without reaching zero. Restricting the preemptive switch to the
         * idle spin removes the race by construction, and costs nothing: a
         * frame-paced game passes through idle every frame, so wakes are at
         * most a fraction of a frame late -- exactly a masked interrupt. */
        {
            extern volatile int psyz_critical_depth;
            extern int mts_active_task_800C0DB0;
            static unsigned t_all, t_idle, t_crit, t_fired;
            t_all++;
            if (mts_active_task_800C0DB0 == 11) t_idle++;
            if (psyz_critical_depth == 0) t_crit++;
            /* One gate, evaluated once: delivering an interrupt switches the
             * active task, so a second look at "is it idle" after the first
             * delivery is false by then. The SPU interrupt and the root
             * counters go first: they are raised on the audio core by the
             * sample pull (psyz_spu.c) and their handlers are the sound
             * driver's, which touch the scheduler like the vblank's does. */
            /* Order matters: the vblank first, from idle; then the SPU
             * interrupt. Delivered the other way round, an SPU interrupt
             * that switches the scheduler to the sound task leaves the vblank
             * gate seeing "not idle" on most ticks (the SPU fires faster
             * than the vblank), and the game ran at a fraction of its speed.
             *
             * The SPU interrupt is the sound driver's clock: it counts them to
             * know where the streaming voice is and refills the other half
             * of its buffer in time. Delivered only at idle (a few times a
             * second, in batches) the refill lags and the voice loops a
             * stale half -- speech repeating itself. So it is delivered
             * whenever the game is outside a critical section, not mid-switch
             * and not inside the stdio lock, like the interrupt it emulates:
             * the handler (sd_str.c, then mts_isend -> ChangeThFromISR) is
             * the one the console ran from its own IRQ. */
            /* Every vblank, not only at idle. Delivered only when the game
             * was idle, the vsync count -- which IS game time: mts_wait_vbl,
             * the cutscene clock -- ran at the fraction of real time the
             * game spent idle, while the streamed voice (clocked by the SPU
             * interrupt, real time) ran a second ahead of the pictures and
             * went dry at every heavy scene. It also made each frame pay
             * two idle vblanks on top of its work. The gate is the one the
             * SPU interrupt uses; ChangeThFromISR handles the preemption. */
            if (psyz_critical_depth == 0 && mgs_in_printf == 0 &&
                !Mgs_ChangeInFlight() && !mgs_cd_pump_busy) {
                extern volatile int psyz_pending_rcnt;
                extern void Psyz_RcntAdd(int n);
                int n = __atomic_exchange_n(&psyz_pending_rcnt, 0, __ATOMIC_RELAXED);
                mgs_tick_phase = "rcnt";
                if (n > 0) {
                    Psyz_RcntAdd(n);
                }
                mgs_tick_phase = "vsync";
                if (g_VsyncCallback) {
                    t_fired++;
                    g_VsyncCallback();
                }
            }
#ifdef MGS_BOARD_XIAO
            /* The controls get their own, much faster cadence.
             *
             * The scheduler summary below reports once per 3000 ticks -- 48
             * seconds -- and a stick is pushed and released in a fraction of
             * that. Reporting the controls at that rate showed them at rest
             * every single time and read as a dead axis. Two seconds, and the
             * deflection is the largest seen since the last line rather than
             * the value at the instant of printing. */
            if ((t_all % 120u) == 0u) {
                extern int mgs_stick_dbg[8];
                extern int mgs_adc_loops;
                printf("[stick] raw %4d,%4d center %4d,%4d "
                       "max-dev %4d,%4d ladder %4d pad %04X loops %d\n",
                       mgs_stick_dbg[0], mgs_stick_dbg[1],
                       mgs_stick_dbg[2], mgs_stick_dbg[3],
                       mgs_stick_dbg[4], mgs_stick_dbg[5],
                       mgs_stick_dbg[6], (unsigned)mgs_stick_dbg[7],
                       mgs_adc_loops);
                mgs_stick_dbg[4] = mgs_stick_dbg[5] = mgs_stick_dbg[7] = 0;
            }
#endif
            if ((t_all % 1800u) == 0u) {
                extern void mts_dbg_dump(void);
                mgs_tick_phase = "dump";
                mts_dbg_dump();
            }
            if ((t_all % 3000u) == 0u) {
                mgs_tick_phase = "report";
                /* mgs_where is the last MGS_WHERE breadcrumb any task set. It
                 * rides along on this line because this line is the one that
                 * still gets out when a wedged game has filled the console
                 * buffer with its own printfs -- which is exactly when knowing
                 * where a task stopped is worth anything. */
                {
                    extern const char *mgs_where;
                    printf("[tick] all %u idle %u crit0 %u fired %u active %d"
                           " | spu deferred: crit %u flight %u printf %u stdio %u busy %u"
                           " | cd: %u pumps, hits %u misses %u, card %u ms"
                           " | ultimo: %s\n",
                           t_all, t_idle, t_crit, t_fired,
                           mts_active_task_800C0DB0, mgs_spu_defer_crit,
                           mgs_spu_defer_flight, mgs_spu_defer_printf,
                           mgs_spu_defer_stdio, mgs_spu_defer_busy, mgs_cd_pumps, mgs_cd_hits,
                           mgs_cd_misses, mgs_cd_sd_us / 1000u, mgs_where);
                    mgs_spu_defer_crit = mgs_spu_defer_flight = 0;
                    mgs_spu_defer_printf = mgs_spu_defer_stdio = mgs_spu_defer_busy = 0;
                    mgs_cd_pumps = mgs_cd_hits = mgs_cd_misses = mgs_cd_sd_us = 0;
                }
                {
                    /* How much of each 20 KB mts stack is actually used?
                     *
                     * Eight slots at 20 KB is 160 KB against roughly 160 KB of
                     * free internal SRAM -- the reservation is the single
                     * biggest consumer of the memory that would otherwise hold
                     * the hot texture pages, which is where the frame rate is
                     * stuck (207 cycles per textured pixel, nearly all of it
                     * PSRAM latency). 20 KB was a guess made when a stack
                     * overflow would have been impossible to diagnose; this
                     * says what it can safely become. */
                    extern void Mgs_ReportThreadStacks(void);
                    Mgs_ReportThreadStacks();
                }
            }
        }
    }
}

/* The SPU interrupt, delivered on the game's side (see the tick task) when
 * the game is outside a critical section, not mid-switch and not inside the
 * stdio lock -- like the interrupt it emulates: the handler (sd_str.c, then
 * mts_isend -> ChangeThFromISR) is the one the console ran from its own IRQ.
 * Bounded per delivery, so a long stall does not turn into a burst. */
static void spu_irq_deliver(void) {
    extern volatile int psyz_critical_depth;
    extern volatile int psyz_pending_spu_irq;
    extern void (* volatile _spu_IRQCallback)(void);
    extern volatile unsigned mgs_spu_irq_delivered;
    int k;
    if (psyz_critical_depth != 0 || Mgs_ChangeInFlight() || mgs_in_printf != 0 ||
        mgs_cd_pump_busy) {
        /* why was a pending interrupt not delivered this tick? (pump-busy:
         * the drive callback is rewriting the stream ring; on the console
         * it ran with interrupts masked, so nothing walked the ring
         * half-moved -- here the sound task would, and fetch nothing) */
        if (psyz_pending_spu_irq > 0) {
            if (psyz_critical_depth != 0) mgs_spu_defer_crit++;
            else if (Mgs_ChangeInFlight()) mgs_spu_defer_flight++;
            else mgs_spu_defer_printf++;
        }
        return;
    }
    k = psyz_pending_spu_irq;
    if (k <= 0 || !_spu_IRQCallback) {
        return;
    }
    /* One at a time, and only when the sound task is back waiting for it.
     *
     * The handler advances the stream's playback position and then wakes
     * the sound task with mts_isend -- which DROPS the wake if the task is
     * not already receiving (overrun). Two interrupts delivered back to
     * back (after any deferral above) therefore moved the position twice
     * but ran the task once; the task's pass count is the cutscene clock
     * (FS_StreamGetTick), so the picture fell behind the voice until the
     * voice reached data not yet on the ring, went dry, and the read-error
     * pause fired. On the console the task always finished between two
     * interrupts; make that true here by waiting for it. */
    {
        extern int Mgs_MtsTaskReceivingIntr(int tasknr);
        if (!Mgs_MtsTaskReceivingIntr(1 /* MTSID_SOUND_INT */)) {
            mgs_spu_defer_busy++;
            return;
        }
    }
    __atomic_fetch_sub(&psyz_pending_spu_irq, 1, __ATOMIC_RELAXED);
    mgs_spu_irq_delivered++;
    _spu_IRQCallback();
}

/* The PSX scans out continuously: whatever is in VRAM reaches the screen every
 * field, whether or not the game is drawing. Presenting only when the game
 * calls VSync(0) means a boot that has not reached its frame loop shows
 * nothing at all -- and a black panel then says "the LCD path is broken" when
 * really nothing has been drawn yet. */
/* Present when the picture CHANGES, not on a timer.
 *
 * Each present reads ~120 KB out of PSRAM strided across rows -- twice the
 * data cache -- so a redundant one does not merely waste SPI time, it evicts
 * the texture and CLUT working set the rasterizer is using on the other core.
 * A fixed period cannot win: the game's frame time moves with the scene, from
 * two vblanks in a corridor to four in an open room, so any constant either
 * sends the same buffer twice or holds a finished one back. DG_SwapFrame bumps
 * mgs_frame_seq when it flips, which is exactly the moment there is something
 * new to send.
 *
 * The poll still runs every vblank so a new frame waits at most 16 ms, and the
 * fallback below keeps the PSX's "whatever is in VRAM reaches the screen"
 * behaviour: a boot that has not reached its frame loop -- or a game that has
 * wedged -- still shows what is in the buffer instead of a black panel that
 * wrongly reads as a broken LCD path. */
#define SCANOUT_POLL_MS   VBLANK_PERIOD_MS
#define SCANOUT_IDLE_MS   250

unsigned mgs_frame_seq;

static void scanout_task(void* arg) {
    TickType_t next = xTaskGetTickCount();
    extern unsigned short g_RawVram[];
    void lcd_present(const unsigned short* src);
    extern DISPENV g_dispenv;
    unsigned last_seq = 0;
    int idle_ms = 0;
    (void)arg;
    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(SCANOUT_POLL_MS));
        if (mgs_frame_seq == last_seq) {
            idle_ms += SCANOUT_POLL_MS;
            if (idle_ms < SCANOUT_IDLE_MS) {
                continue;
            }
        }
        last_seq = mgs_frame_seq;
        idle_ms = 0;
        {
            extern long long esp_timer_get_time(void);
            long long t0 = esp_timer_get_time();
            lcd_present(&g_RawVram[(unsigned)g_dispenv.disp.y * 1024u +
                                   (unsigned)g_dispenv.disp.x]);
            mgs_prof_present_us += (unsigned)(esp_timer_get_time() - t0);
        }

#ifdef MGS_VRAM_MAP
        /* Did the game DRAW, and is anything IN the window we scan out?
         * Decides between "renders but wrong origin" and "never draws"
         * without touching the game. Off by default now that it does: the
         * sweep itself reads PSRAM strided, and it buries the game's own
         * console output. */
        {
            static unsigned beat;
            if (++beat >= 300u) {
                extern unsigned sotn_push_count, sotn_exeque_count,
                    sotn_pkt_drops;
                unsigned nz = 0, y, x;
                const unsigned short* base =
                    &g_RawVram[(unsigned)g_dispenv.disp.y * 1024u +
                               (unsigned)g_dispenv.disp.x];
                for (y = 0; y < 240; y += 8) {
                    for (x = 0; x < 320; x += 4) {
                        if (base[y * 1024u + x]) {
                            nz++;
                        }
                    }
                }
                {
                    /* Where in VRAM is the picture actually landing? An 8x4
                     * coarse map of the whole 1024x512 framebuffer memory:
                     * each cell is the count of non-black samples. If the
                     * game draws somewhere other than the two 320-wide
                     * buffers we scan out, this shows it immediately. */
                    unsigned cy, cx, yy, xx, cell;
                    printf("[vram] disp %d,%d exeq %u map:\n",
                           g_dispenv.disp.x, g_dispenv.disp.y,
                           sotn_exeque_count);
                    for (cy = 0; cy < 4; cy++) {
                        printf("[vram] ");
                        for (cx = 0; cx < 8; cx++) {
                            cell = 0;
                            for (yy = cy * 128; yy < (cy + 1) * 128; yy += 8) {
                                for (xx = cx * 128; xx < (cx + 1) * 128;
                                     xx += 4) {
                                    if (g_RawVram[yy * 1024u + xx]) {
                                        cell++;
                                    }
                                }
                            }
                            printf("%5u", cell);
                        }
                        printf("\n");
                    }
                    (void)nz; (void)sotn_push_count; (void)sotn_pkt_drops;
                }
                beat = 0;
            }
        }
#endif /* MGS_VRAM_MAP */
    }
}

static int started;
static TaskHandle_t tick_task, lcd_task;

/* is the caller the vblank tick? (its prints must not take stdio's lock) */
int Mgs_IsTickTask(void) {
    return tick_task != NULL && xTaskGetCurrentTaskHandle() == tick_task;
}

void Mgs_StartVblank(void) {
    if (started) {
        return;
    }
    started = 1;
    {
        /* the rasterizer on core 1, so a frame costs the longer of game
         * logic and drawing instead of their sum */
        extern void Psyz_GpuWorkerStart(void);
        Psyz_GpuWorkerStart();
    }
    if (xTaskCreatePinnedToCore(vblank_tick_task, "mgs_vbl", 4096, NULL,
                                VBLANK_TICK_PRIO, &tick_task, 0) != pdPASS) {
        printf("[vblank] could not start the tick task\n");
        started = 0;
        return;
    }
    if (xTaskCreatePinnedToCore(scanout_task, "mgs_lcd", 4096, NULL, 6, &lcd_task,
                                1) != pdPASS) {
        printf("[vblank] could not start the scanout task\n");
    }
    printf("[vblank] %d ms tick on core 0 (prio %d), scanout on core 1 "
           "(present on frame change)\n",
           VBLANK_PERIOD_MS, VBLANK_TICK_PRIO);
}

/* A platform with a pause menu freezes the game by holding the vblank tick
 * (nothing wakes an mts task without it) and the scanout; stop deletes both
 * so the game can be started over. */
void Mgs_PauseVblank(void) {
    if (tick_task) vTaskSuspend(tick_task);
    if (lcd_task) vTaskSuspend(lcd_task);
}

void Mgs_ResumeVblank(void) {
    if (lcd_task) vTaskResume(lcd_task);
    if (tick_task) vTaskResume(tick_task);
}

void Mgs_StopVblank(void) {
    extern void Psyz_GpuWorkerStop(void);
    if (tick_task) { vTaskDelete(tick_task); tick_task = NULL; }
    Psyz_GpuWorkerStop(); /* finishes its batch first; needs no tick */
    if (lcd_task) { vTaskDelete(lcd_task); lcd_task = NULL; }
    started = 0;
}

/* --- save states --------------------------------------------------------- */
void Mgs_VblankSnapshotSetup(void) {
    Mgs_SnapshotPreserve(&started, sizeof started);
    Mgs_SnapshotPreserve(&tick_task, sizeof tick_task);
    Mgs_SnapshotPreserve(&lcd_task, sizeof lcd_task);
}
