/* A virtual CD over ordinary files.
 *
 * MGS's libfs does not open files by name. It reads the disc's ISO9660
 * directory once to learn which SECTOR each file starts at, and from then on
 * everything -- STAGE.DIR's stage table, the movie index, streamed audio --
 * addresses data as absolute sector numbers. There is no CD here and no ISO
 * directory, so that first read found garbage and took the board down.
 *
 * Rather than teach the whole of libfs about filenames, give it the disc it
 * expects: hand out a synthetic sector range per file, and translate every
 * sector read back into a seek on the corresponding file. Everything above
 * this layer then works unmodified, including the trimmed STAGE.DIR that
 * port/pack_stagedir.py builds -- its stage offsets are already sectors
 * relative to the start of the file, which is exactly what the game assumes.
 */

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "libfs/libfs.h"
#include "libfs/cdbios.h"
#include "mgs_snapshot.h"

#define CD_SECTOR 2048

/* Ranges are spaced far enough apart that no real file can ever overlap the
 * next one; the retail disc's biggest file is DEMO.DAT at ~258 MB = 126k
 * sectors, so a million sectors per slot leaves plenty of headroom. */
#define SLOT_SECTORS 1000000u

static const char* const cd_names[FS_MAX_FILEID] = {
    "STAGE.DIR", "RADIO.DAT", "FACE.DAT", "ZMOVIE.STR",
    "VOX.DAT",   "DEMO.DAT",  "BRF.DAT",
};

/* Where the disc data is read from, and why it differs per medium.
 *
 * Flash partition: copied into PSRAM at boot. Reading sectors straight out of
 * it worked, but every read is a flash operation, and a flash operation on this
 * chip switches the instruction cache off on both cores. Any other core
 * executing from flash at that instant takes a cache fault -- which is what
 * killed the boot inside the SPU init, with the sound task running while a CD
 * read was in flight. Copying once, before a single game task exists, means
 * there is never a flash access again afterwards, and a sector read becomes a
 * memcpy. The cost is PSRAM: the partition holds at most 8 MB and the copy has
 * to fit alongside everything else the game allocates.
 *
 * microSD: left on the card and read on demand. The card is a SPI peripheral,
 * not the execute-in-place flash, so a read touches no cache and needs no copy
 * -- which lifts the size limit entirely. That is the only way the full 68.6 MB
 * STAGE.DIR, and with it every stage the game can ask for, fits at all.
 *
 * Whichever medium a file comes from, everything above this layer is unchanged:
 * cd_fetch serves a sector from the copy or from the file. */
static unsigned char* cd_data[FS_MAX_FILEID];
static FILE* cd_files[FS_MAX_FILEID];
static long cd_sizes[FS_MAX_FILEID];
/* where each file-backed slot's handle currently sits, so a sequential run
 * does not re-seek; -1 means unknown */
static long cd_pos[FS_MAX_FILEID];
static int cd_ready;

/* the platform layer knows where the data lives */
extern void Mgs_DataPath(const char* name, char* out, unsigned n);

/* The card shares SPI2 with the panel and the driver cannot arbitrate the two
 * on its own -- see the note in esp32/main/lcd_esp32.c. Every access to a
 * file-backed slot is bracketed by this; a PSRAM-backed one is a memcpy and
 * needs nothing. */
extern void Mgs_SpiBusTake(void);
extern void Mgs_SpiBusGive(void);
static void pf_start(void);    /* read-ahead task, defined with cd_fetch below */
static void pf_shutdown(void);
/* Where the card keeps the disc files. The S3 boards mount at /sd; another
 * platform layer overrides this with its own mount point. */
__attribute__((weak)) const char* Mgs_SdRoot(void) { return "/sd/MGS"; }

/* the task structure the real CD BIOS drives; the callback contract is
 * written in terms of it, so the virtual drive fills in the same fields */
extern CDBIOS_TASK cd_bios_task_800B4E58;

void Mgs_CdInit(void) {
    int i;
    if (cd_ready) {
        return;
    }
    cd_ready = 1;
    /* the scanout task is already running by now, so even opening files has to
     * take the bus */
    Mgs_SpiBusTake();
    for (i = 0; i < FS_MAX_FILEID; i++) {
        char path[160];
        char devname[64];
        FILE* f;

        /* the card wins when it carries the file: it is both bigger and
         * cheaper to read from, so a board with a card ignores whatever was
         * packed into flash for the same name */
        snprintf(path, sizeof(path), "%s/%s", Mgs_SdRoot(), cd_names[i]);
        f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            cd_sizes[i] = ftell(f);
            fseek(f, 0, SEEK_SET);
            cd_files[i] = f;
            cd_pos[i] = 0;
            printf("[vcd] %-10s %8ld bytes on microSD, sector %u\n",
                   cd_names[i], cd_sizes[i], (unsigned)(i * SLOT_SECTORS));
            continue;
        }

        snprintf(devname, sizeof(devname), "cdrom:\\MGS\\%s;1", cd_names[i]);
        Mgs_DataPath(devname, path, sizeof(path));
        f = fopen(path, "rb");
        if (!f) {
            cd_sizes[i] = 0;
            printf("[vcd] %-10s ABSENT (not on /sd or %s)\n", cd_names[i], path);
            continue;
        }
        fseek(f, 0, SEEK_END);
        cd_sizes[i] = ftell(f);
        fseek(f, 0, SEEK_SET);

        cd_data[i] = heap_caps_malloc((size_t)cd_sizes[i], MALLOC_CAP_SPIRAM);
        if (cd_data[i]) {
            size_t got = fread(cd_data[i], 1, (size_t)cd_sizes[i], f);
            fclose(f);
            printf("[vcd] %-10s %8ld bytes -> PSRAM, sector %u\n", cd_names[i],
                   (long)got, (unsigned)(i * SLOT_SECTORS));
        } else {
            /* no room: keep it on flash and accept the cache hazard for this
             * one -- still better than the file being missing outright */
            cd_files[i] = f;
            cd_pos[i] = 0;
            printf("[vcd] %-10s %8ld bytes stays on flash, sector %u\n",
                   cd_names[i], cd_sizes[i], (unsigned)(i * SLOT_SECTORS));
        }
    }
    Mgs_SpiBusGive();
}

/* Release everything Mgs_CdInit() opened, so the game can be started again. */
void Mgs_CdDeinit(void) {
    pf_shutdown();
    int i;
    for (i = 0; i < FS_MAX_FILEID; i++) {
        if (cd_files[i]) { fclose(cd_files[i]); cd_files[i] = NULL; }
        if (cd_data[i]) { free(cd_data[i]); cd_data[i] = NULL; }
        cd_sizes[i] = 0;
        cd_pos[i] = 0;
    }
    cd_ready = 0;
}

static void cd_open_all(void) { Mgs_CdInit(); }

/* Not every file on the retail disc is on the board -- ZMOVIE.STR alone is
 * 250 MB of full-motion video. Callers need to be able to ask, because libfs
 * happily parses whatever a missing file returns and acts on the result. */
int Mgs_CdFilePresent(int fileid) {
    cd_open_all();
    if (fileid < 0 || fileid >= FS_MAX_FILEID) {
        return 0;
    }
    return cd_data[fileid] != 0 || cd_files[fileid] != 0;
}

/* Build the position table libfs would otherwise read off the disc. */
int FS_CdMakePositionTable(char* buffer, FS_FILE_INFO* finfo) {
    int i;
    (void)buffer;
    cd_open_all();
    for (i = 0; i < FS_MAX_FILEID; i++) {
        finfo[i].name = cd_names[i];
        finfo[i].pos = (u_int)(i * SLOT_SECTORS);
    }
    /* disc 1; the game uses this to pick its executable name */
    return 0;
}

/* --------------------------------------------------------------------------
 * The CD BIOS itself
 * ------------------------------------------------------------------------ */

static int cd_last_result;

int CDBIOS_Reset(void) {
    cd_open_all();
    pf_start();
    return 0;
}

/* Copy one sector's worth of words out of the virtual disc.
 * Returns 0 when the request falls past the end of the file. */
/* Read-ahead.
 *
 * A console drive streams on its own; this one is pumped from the game thread
 * and every sector came straight off the card, 2 KB at a time, with the game
 * stopped for the duration. A cutscene at full speed wants ~150 sectors a
 * second, and the card's latency per call turned that into the game thread
 * spending much of each frame inside fread -- seen as the picture freezing in
 * bursts while the streamed voice ran dry and raised the read-error pause.
 *
 * So a task on the other core reads ahead along the current request, in
 * chunks, into a ring in PSRAM; the pump then copies out of the ring. A miss
 * (a seek: a new stream, a stage load jump) reads directly and re-aims the
 * ring. The in-RAM files (cd_data) never needed this and bypass it. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"

#define PF_SECTORS 192 /* 384 KB: about a second of stream at 2x speed */
#define PF_CHUNK 16    /* sectors per card read */

static unsigned char* pf_buf;
static unsigned pf_base;  /* sector number held in slot pf_head */
static unsigned pf_head;  /* ring slot of pf_base */
static unsigned pf_count; /* valid sectors from pf_base */
static volatile int pf_stop;
static volatile int pf_done; /* the task has finished and parked itself */
static SemaphoreHandle_t pf_lock; /* ring bookkeeping */
static SemaphoreHandle_t sd_lock; /* the FILE streams: one reader at a time */
static TaskHandle_t pf_task;
unsigned mgs_cd_hits, mgs_cd_misses, mgs_cd_sd_us, mgs_cd_pumps; /* tick report */

static int cd_fetch_sd(void* dst, unsigned sector, unsigned words);

/* n whole sectors from the card into dst; returns how many were read (fewer
 * at the file's end, 0 past it or on error) */
static int cd_read_sd_run(unsigned char* dst, unsigned sector, int n) {
    unsigned slot = sector / SLOT_SECTORS;
    long offset = (long)(sector % SLOT_SECTORS) * CD_SECTOR;
    long avail;
    int ok = 1;
    if (slot >= FS_MAX_FILEID || !cd_files[slot] || offset >= cd_sizes[slot]) {
        return 0;
    }
    avail = (cd_sizes[slot] - offset + CD_SECTOR - 1) / CD_SECTOR;
    if (n > avail) {
        n = (int)avail;
    }
    xSemaphoreTake(sd_lock, portMAX_DELAY);
    {
        extern long long esp_timer_get_time(void);
        long long t0 = esp_timer_get_time();
        size_t want = (size_t)n * CD_SECTOR;
        if (offset + (long)want > cd_sizes[slot]) {
            want = (size_t)(cd_sizes[slot] - offset);
            memset(dst + want, 0, (size_t)n * CD_SECTOR - want);
        }
        if (cd_pos[slot] != offset) {
            ok = fseek(cd_files[slot], offset, SEEK_SET) == 0;
            cd_pos[slot] = ok ? offset : -1;
        }
        if (ok) {
            ok = fread(dst, 1, want, cd_files[slot]) == want;
            cd_pos[slot] = ok ? offset + (long)want : -1;
        }
        mgs_cd_sd_us += (unsigned)(esp_timer_get_time() - t0);
    }
    xSemaphoreGive(sd_lock);
    return ok ? n : 0;
}

static void pf_task_main(void* arg) {
    (void)arg;
    while (!pf_stop) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        for (;;) {
            unsigned next, slot;
            int n;
            if (pf_stop) {
                break;
            }
            xSemaphoreTake(pf_lock, portMAX_DELAY);
            if (pf_count + PF_CHUNK > PF_SECTORS) {
                xSemaphoreGive(pf_lock);
                break;
            }
            next = pf_base + pf_count;
            slot = (pf_head + pf_count) % PF_SECTORS;
            xSemaphoreGive(pf_lock);
            n = PF_CHUNK;
            if (slot + n > PF_SECTORS) {
                n = (int)(PF_SECTORS - slot); /* no wrapping inside one read */
            }
            n = cd_read_sd_run(pf_buf + (size_t)slot * CD_SECTOR, next, n);
            if (n <= 0) {
                break; /* end of file, or nothing to follow */
            }
            xSemaphoreTake(pf_lock, portMAX_DELAY);
            if (pf_base + pf_count == next) { /* ring not re-aimed meanwhile */
                pf_count += (unsigned)n;
            }
            xSemaphoreGive(pf_lock);
        }
    }
    /* created WithCaps, so the creator deletes it (vTaskDeleteWithCaps);
     * self-deleting here handed the PSRAM stack and control block to the
     * wrong allocator and corrupted the heap on every relaunch */
    pf_done = 1;
    vTaskSuspend(NULL);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

static void pf_start(void) {
    if (pf_task) {
        return;
    }
    if (!pf_buf) {
        pf_buf = heap_caps_malloc((size_t)PF_SECTORS * CD_SECTOR, MALLOC_CAP_SPIRAM);
    }
    if (!pf_lock) pf_lock = xSemaphoreCreateMutex();
    if (!sd_lock) sd_lock = xSemaphoreCreateMutex();
    if (!pf_buf || !pf_lock || !sd_lock) {
        printf("[vcd] no read-ahead (allocation failed)\n");
        return;
    }
    pf_base = pf_head = pf_count = 0;
    pf_stop = 0;
    pf_done = 0;
    if (xTaskCreatePinnedToCoreWithCaps(pf_task_main, "vcd_prefetch", 4096, NULL, 5,
                                        &pf_task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        pf_task = NULL;
        printf("[vcd] no read-ahead (task failed)\n");
        return;
    }
    printf("[vcd] read-ahead on core 1: %d sectors, %d per read\n", PF_SECTORS, PF_CHUNK);
}

static void pf_shutdown(void) {
    if (pf_task) {
        pf_stop = 1;
        xTaskNotifyGive(pf_task);
        while (!pf_done) {
            vTaskDelay(1);
        }
        vTaskDeleteWithCaps(pf_task);
        pf_task = NULL;
    }
    if (pf_buf) {
        heap_caps_free(pf_buf);
        pf_buf = NULL;
    }
    if (pf_lock) { vSemaphoreDelete(pf_lock); pf_lock = NULL; }
    if (sd_lock) { vSemaphoreDelete(sd_lock); sd_lock = NULL; }
}

static int cd_fetch(void* dst, unsigned sector, unsigned words) {
    unsigned slot = sector / SLOT_SECTORS;
    if (pf_task && slot < FS_MAX_FILEID && !cd_data[slot]) {
        int hit = 0;
        xSemaphoreTake(pf_lock, portMAX_DELAY);
        if (pf_count && sector >= pf_base && sector < pf_base + pf_count) {
            unsigned adv = sector - pf_base + 1;
            unsigned at = (pf_head + (sector - pf_base)) % PF_SECTORS;
            memcpy(dst, pf_buf + (size_t)at * CD_SECTOR, (size_t)words * 4u);
            pf_head = (pf_head + adv) % PF_SECTORS;
            pf_base = sector + 1;
            pf_count -= adv;
            hit = 1;
        } else {
            /* re-aim the ring just past this sector; the task refills */
            pf_base = sector + 1;
            pf_head = 0;
            pf_count = 0;
        }
        xSemaphoreGive(pf_lock);
        xTaskNotifyGive(pf_task);
        if (hit) {
            mgs_cd_hits++;
            return 1;
        }
        mgs_cd_misses++;
    }
    return cd_fetch_sd(dst, sector, words);
}

/* Copy one sector's worth of words out of the virtual disc, straight from the
 * copy or the card. Returns 0 when the request falls past the end of the file. */
static int cd_fetch_sd(void* dst, unsigned sector, unsigned words) {
    unsigned slot = sector / SLOT_SECTORS;
    long offset = (long)(sector % SLOT_SECTORS) * CD_SECTOR;
    unsigned bytes = words * 4u;

    if (slot >= FS_MAX_FILEID || offset >= cd_sizes[slot]) {
        return 0;
    }
    if (offset + (long)bytes > cd_sizes[slot]) {
        unsigned have = (unsigned)(cd_sizes[slot] - offset);
        memset((char*)dst + have, 0, bytes - have);
        bytes = have;
    }
    if (cd_data[slot]) {
        memcpy(dst, cd_data[slot] + offset, bytes);
    } else if (cd_files[slot]) {
        int ok;
        extern long long esp_timer_get_time(void);
        long long t0 = esp_timer_get_time();
        if (sd_lock) xSemaphoreTake(sd_lock, portMAX_DELAY);
        Mgs_SpiBusTake();
        /* Seek only when the read is not already where we left off.
         *
         * A stage load is one long sequential run of ~550 sectors, and FATFS
         * resolves a byte offset by walking the file's cluster chain -- on a
         * 68 MB STAGE.DIR that is a long walk, repeated for every sector, and
         * it dominated the load: 1.1 MB took 11.5 s, about 95 KB/s, when the
         * bus itself can do more than ten times that. Tracking the position
         * and skipping the redundant seek costs one long per file. */
        ok = 1;
        if (cd_pos[slot] != offset) {
            ok = fseek(cd_files[slot], offset, SEEK_SET) == 0;
            cd_pos[slot] = ok ? offset : -1;
        }
        if (ok) {
            ok = fread(dst, 1, bytes, cd_files[slot]) == bytes;
            cd_pos[slot] = ok ? offset + (long)bytes : -1;
        }
        Mgs_SpiBusGive();
        if (sd_lock) xSemaphoreGive(sd_lock);
        mgs_cd_sd_us += (unsigned)(esp_timer_get_time() - t0);
        if (!ok) {
            return 0;
        }
    } else {
        return 0;
    }
    return 1;
}

/* The real CD BIOS is asynchronous, and that is LOAD-BEARING, not an
 * implementation detail. The stage loader is a pair of co-routines: the drive
 * delivers sectors into a shared window while the FS daemon parses and copies
 * them out between deliveries, and when a file finishes, its callback points
 * the SAME window at the next file. Delivering the whole 275-sector stage in
 * one synchronous burst let file 2 overwrite file 1 before the parser had seen
 * a byte of it -- the "ntag size 0" garbage was our own second file on top of
 * the first.
 *
 * So: ReadRequest only records the request, and CDBIOS_ReadSync -- the exact
 * point where libfs polls "has more data arrived?" -- delivers it. One burst
 * per poll, stopped early whenever the callback returns 2, because that is the
 * "I redirected the buffer" signal: the parser must be given the CPU before
 * anything lands on the redirected window. Callbacks also rewrite
 * task->size/remaining mid-flight (that is how the config sector extends the
 * read to the whole stage block), so both are re-read every iteration. Sizes
 * are in 32-bit words, 512 to a sector, the way the task structure counts. */
void CDBIOS_ReadRequest(void* buffer, unsigned int sector, unsigned int size,
                        void* callback) {
    CDBIOS_TASK* task = &cd_bios_task_800B4E58;

    task->buffer = buffer;
    if (size == 0) {
        size = 0x7fff0000; /* "read until the callback says stop" */
    }
    task->remaining = (int)((size + 3) >> 2);
    task->size = (int)((size + 3) >> 2);
    task->sector = (int)sector;
    task->callback = (cdbios_task_pfn)callback;
    task->sectors_delivered = 0;
    task->ticks = 0;
    task->state = CDBIOS_STATE_READ;
    cd_last_result = 0;
}

/* deliver sectors for the outstanding request; returns 1 while incomplete */
volatile int mgs_cd_pump_busy; /* a pump (and so the stream callback) is in progress */
#define cd_pump_busy mgs_cd_pump_busy

static int cd_pump_n(int max_burst) {
    CDBIOS_TASK* task = &cd_bios_task_800B4E58;
    int burst;

    static int pump_log = 40;
    if (task->state != CDBIOS_STATE_READ) {
        return 0;
    }
    mgs_cd_pumps++;
    cd_pump_busy = 1;

    /* A sector out of PSRAM is a memcpy, so the whole request can be delivered
     * in one poll and the early-stops below do the real pacing. A sector off
     * the card is a real SPI transfer, and this runs on the loader actor, so a
     * big burst stalls the frame loop.
     *
     * That stall was worth avoiding when the concern was a hitch mid-game, but
     * a stage load is not mid-game: the screen is black and nothing is being
     * drawn until it finishes. Pacing it at 16 sectors a frame just made the
     * black screen last longer -- the heliport took 12 seconds, which reads as
     * a hang rather than a load. Take bigger bites; the only thing being kept
     * responsive during a load is a picture nobody can see. */
    {
        unsigned slot = (unsigned)task->sector / SLOT_SECTORS;
        int from_file = slot < FS_MAX_FILEID && cd_data[slot] == 0;
        burst = from_file ? 128 : 512;
        if (max_burst > 0 && burst > max_burst) {
            burst = max_burst;
        }
    }
    if (pump_log > 0) {
        pump_log--;
        printf("[vcd] pump sector %d remaining %d buf %p\n", task->sector,
               task->remaining, task->buffer);
    }
    while (task->remaining > 0 && burst-- > 0) {
        unsigned words = task->remaining <= 512 ? (unsigned)task->remaining : 512u;

        if (task->buffer && !cd_fetch(task->buffer, (unsigned)task->sector, words)) {
            task->state = CDBIOS_STATE_IDLE; /* off the end of the file */
            return 0;
        }
        task->remaining -= (int)words;
        task->buffer_size = (int)words;

        if (task->callback) {
            int status = task->callback(task);
            if (status == 0) {
                task->state = CDBIOS_STATE_IDLE; /* callback ended the read */
                return 0;
            }
            if (status == 2) {
                /* buffer redirected: let the parser run before more arrives */
                task->sector++;
                task->sectors_delivered++;
                return 1;
            }
        }
        if (task->buffer) {
            task->buffer = (int*)task->buffer + words;
        }
        task->sector++;
        task->sectors_delivered++;
    }
    if (task->remaining <= 0) {
        task->state = CDBIOS_STATE_IDLE;
        return 0;
    }
    return 1;
}

static int cd_pump(void) {
    int r = cd_pump_n(0);
    cd_pump_busy = 0;
    return r;
}

/* From the vblank tick, each vblank: keep the stream ring fed the way the
 * console's drive interrupt did, independent of how often the game's own
 * stream actor gets to run. Streams only, never re-entrant, small bites. */
void Mgs_CdPumpFromTick(void) {
    extern int FS_StreamOwnsCdTask(void);
    if (cd_pump_busy || !FS_StreamOwnsCdTask()) {
        return;
    }
    cd_pump_n(16);
    cd_pump_busy = 0;
}

int CDBIOS_ReadSync(void) {
    return cd_pump();
}

/* FS_StreamStop: whatever request is outstanding must deliver nothing more
 * (the disc version only flags its own task, which does not exist here). */
void CDBIOS_ForceStop(void) {
    cd_bios_task_800B4E58.state = CDBIOS_STATE_IDLE;
}

/* Does the card hold the file a stream at this sector would read from?
 * strctrl.c declines a stream whose data is absent (the console could not
 * have that happen, so nothing downstream copes with it). */
int Mgs_CdSectorPresent(unsigned sector) {
    unsigned slot = sector / SLOT_SECTORS;
    return slot < FS_MAX_FILEID && Mgs_CdFilePresent((int)slot);
}

/* --- save states --------------------------------------------------------- */
void Mgs_CdSnapshotSetup(void) {
    Mgs_SnapshotPreserve(cd_files, sizeof cd_files);
    Mgs_SnapshotPreserve(cd_data, sizeof cd_data);
    Mgs_SnapshotPreserve(cd_sizes, sizeof cd_sizes);
    Mgs_SnapshotPreserve(cd_pos, sizeof cd_pos);
    Mgs_SnapshotPreserve(&cd_ready, sizeof cd_ready);
    Mgs_SnapshotPreserve(&pf_buf, sizeof pf_buf);
    Mgs_SnapshotPreserve(&pf_task, sizeof pf_task);
    Mgs_SnapshotPreserve(&pf_lock, sizeof pf_lock);
    Mgs_SnapshotPreserve(&sd_lock, sizeof sd_lock);
    Mgs_SnapshotPreserve((void*)&pf_stop, sizeof pf_stop);
    Mgs_SnapshotPreserve((void*)&pf_done, sizeof pf_done);
}

/* the ring holds this run's read-ahead; the file positions are this run's */
void Mgs_CdAfterRestore(void) {
    int i;
    if (pf_lock) {
        xSemaphoreTake(pf_lock, portMAX_DELAY);
    }
    pf_base = pf_head = pf_count = 0;
    if (pf_lock) {
        xSemaphoreGive(pf_lock);
    }
    for (i = 0; i < FS_MAX_FILEID; i++) {
        cd_pos[i] = -1;
    }
    mgs_cd_pump_busy = 0;
}
