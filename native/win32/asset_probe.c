/* In-process search for the game's live asset records (see asset_probe.h).
 *
 * Why a scan instead of a call: the pack's name -> (offset, size) index is built
 * at start-up in a context object whose address is not known statically, and the
 * resolver takes that context as its first argument. Scanning for the measured
 * 0x58-byte record layout needs no addresses at all, only the layout, and it is
 * the one thing the previous sessions were missing: a single live
 * (offset, size) pair makes the pack readable, and after that the name only has
 * to agree with `zlib.crc32(name) & 0x3ff`.
 *
 * The scan is deliberately conservative. A slot is accepted only when
 *
 *   - offset >= 0x18 and length >= 0x18 and offset + length <= the pack size,
 *     because every payload carries a 0x18-byte header;
 *   - the type tag is one of the five values observed in the loader's dispatch;
 *   - the neighbouring 0x58-byte slot parses as well, i.e. the hit sits inside a
 *     record array rather than in a lone structure that happens to look similar.
 */
#include "hypr_win32.h"
#include "asset_probe.h"

#include <stdio.h>
#include <string.h>

#define HYPRLODR_ASSET_STRIDE 0x58u
#define HYPRLODR_ASSET_HEADER 0x18u
/* Size of this build's asset/asset, verified on disk; a record never points past
 * the end of the pack, so this bounds false positives. */
#define HYPRLODR_ASSET_PACK_BYTES 306794369ull
#define HYPRLODR_ASSET_NAME_MAX 96u
/* Record arrays are small; a runaway scan would only slow the game down. */
#define HYPRLODR_ASSET_MAX_RECORDS 400u
/* Raw dumps of the first few accepted records: they settle where the name field
 * actually starts when the std::string probe does not match. */
#define HYPRLODR_ASSET_DUMP_MAX 24u
#define HYPRLODR_ASSET_SCAN_BYTES (1536ull * 1024ull * 1024ull)
#define HYPRLODR_ASSET_ATTEMPTS 40u
#define HYPRLODR_ASSET_FIRST_DELAY_MS 4000u
#define HYPRLODR_ASSET_ATTEMPT_MS 1000u

static char g_log_path[1024] = "";
static char g_status[32] = "idle";
static unsigned g_records = 0;

static void set_status(const char *text) {
    snprintf(g_status, sizeof g_status, "%s", text);
}

static unsigned load_u32(const unsigned char *p) {
    unsigned value;
    memcpy(&value, p, sizeof value);
    return value;
}

static unsigned long long load_u64(const unsigned char *p) {
    unsigned long long value;
    memcpy(&value, p, sizeof value);
    return value;
}

/* Extract the record's std::string name when it is a plausible asset name.
 * Returns 0 when the field does not look like one; the record itself may still
 * be valid, because unnamed or empty entries exist. */
static int read_name(const unsigned char *record, char *out, size_t capacity) {
    unsigned long long length = load_u64(record + 0x30);
    unsigned long long room = load_u64(record + 0x38);
    const unsigned char *chars;
    size_t index;

    if (out && capacity) out[0] = '\0';
    if (length == 0 || length > HYPRLODR_ASSET_NAME_MAX) return 0;
    if (!out || capacity <= (size_t)length) return 0;

    if (room <= 15u) {
        /* Small-string optimisation: the characters live inside the object. */
        chars = record + 0x20;
    } else {
        chars = (const unsigned char *)(size_t)load_u64(record + 0x20);
        if (!chars) return 0;
    }
    for (index = 0; index < (size_t)length; ++index) {
        unsigned char c = chars[index];
        if (c < 0x21 || c > 0x7eu) return 0;
    }
    memcpy(out, chars, (size_t)length);
    out[length] = '\0';
    return 1;
}

static int record_parses(const unsigned char *record) {
    unsigned offset = load_u32(record + 0x00);
    unsigned length = load_u32(record + 0x04);
    unsigned tag = load_u32(record + 0x40);

    if (offset < HYPRLODR_ASSET_HEADER || length < HYPRLODR_ASSET_HEADER) return 0;
    if ((unsigned long long)offset + (unsigned long long)length > HYPRLODR_ASSET_PACK_BYTES)
        return 0;
    switch (tag) {
        case 1u:
        case 2u:
        case 4u:
        case 0x20u:
        case 0x100u:
            return 1;
        default:
            return 0;
    }
}

static void emit(FILE *log, const unsigned char *record) {
    char name[HYPRLODR_ASSET_NAME_MAX + 2];
    char escaped[2 * (HYPRLODR_ASSET_NAME_MAX + 2)];
    size_t out = 0;
    size_t index;

    read_name(record, name, sizeof name);
    for (index = 0; name[index] && out + 2 < sizeof escaped; ++index) {
        if (name[index] == '"' || name[index] == '\\') escaped[out++] = '\\';
        escaped[out++] = name[index];
    }
    escaped[out] = '\0';

    fprintf(log,
            "{\"event\":\"asset_record\",\"addr\":\"0x%llx\",\"offset\":%u,\"size\":%u,"
            "\"tag\":%u,\"name\":\"%s\"}\n",
            (unsigned long long)(size_t)record, load_u32(record + 0x00),
            load_u32(record + 0x04), load_u32(record + 0x40), escaped);
    ++g_records;
}

static void dump_raw(FILE *log, const unsigned char *record) {
    size_t index;

    fprintf(log, "{\"event\":\"asset_record_raw\",\"addr\":\"0x%llx\",\"bytes\":\"",
            (unsigned long long)(size_t)record);
    for (index = 0; index < HYPRLODR_ASSET_STRIDE; ++index) {
        fprintf(log, "%02x", record[index]);
    }
    fprintf(log, "\"}\n");
}

/* Returns the number of records reported from this region.
 *
 * A slot is only believed when three consecutive 0x58-byte slots parse: the first
 * pass showed that two is common enough in unrelated memory to produce hundreds
 * of coincidences, and a real record table is long. */
static unsigned scan_region(const unsigned char *base, size_t span, FILE *log) {
    static unsigned dumps = 0;
    const size_t run = 3u * HYPRLODR_ASSET_STRIDE;
    unsigned reported = 0;
    size_t index = 0;

    if (span < run) return 0;
    while (index + run <= span) {
        const unsigned char *record = base + index;
        size_t cursor;

        if (!(record_parses(record) && record_parses(record + HYPRLODR_ASSET_STRIDE) &&
              record_parses(record + 2u * HYPRLODR_ASSET_STRIDE))) {
            index += 8u;
            continue;
        }
        /* Start of a run: report every member, including one trailing slot that
         * still parses, so the array is described as far as it goes. */
        if (dumps < HYPRLODR_ASSET_DUMP_MAX) {
            dump_raw(log, record);
            ++dumps;
        }
        /* Only named members are reported: an unnamed record proves the table is
         * real but cannot be tied to a CRC-32 seed, and the first arrays are full
         * of unnamed slices. */
        cursor = index;
        while (cursor + HYPRLODR_ASSET_STRIDE <= span &&
               g_records < HYPRLODR_ASSET_MAX_RECORDS) {
            const unsigned char *slot = base + cursor;
            if (!record_parses(slot)) break;
            /* Report every member: names are not stored in these records (a
             * 722 MB scan found none), so slices are identified by content. */
            emit(log, slot);
            ++reported;
            cursor += HYPRLODR_ASSET_STRIDE;
        }
        if (g_records >= HYPRLODR_ASSET_MAX_RECORDS) return reported;
        index = cursor;
    }
    return reported;
}

static int region_readable(const MEMORY_BASIC_INFORMATION64A *info) {
    const DWORD wanted = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (info->State != MEM_COMMIT) return 0;
    if (info->Type != MEM_PRIVATE && info->Type != MEM_MAPPED) return 0;
    if (info->Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    return (info->Protect & wanted) != 0;
}

static unsigned scan_space(FILE *log, unsigned *regions_out, unsigned long long *bytes_out) {
    const unsigned long long limit = 0x00007ffffffeffffull;
    unsigned long long address = 0x10000ull;
    unsigned long long scanned = 0;
    unsigned regions = 0;

    while (address < limit && g_records < HYPRLODR_ASSET_MAX_RECORDS &&
           scanned < HYPRLODR_ASSET_SCAN_BYTES) {
        MEMORY_BASIC_INFORMATION64A info;
        unsigned long long base;
        unsigned long long span;

        if (VirtualQuery((const void *)(size_t)address, &info, sizeof info) != sizeof info) break;
        base = (unsigned long long)(size_t)info.BaseAddress;
        span = info.RegionSize;
        if (span == 0) {
            address = base + 0x1000ull;
            continue;
        }
        if (region_readable(&info)) {
            ++regions;
            scanned += span;
            (void)scan_region((const unsigned char *)(size_t)base, (size_t)span, log);
        }
        address = base + span;
    }
    *regions_out = regions;
    *bytes_out = scanned;
    return g_records;
}

static DWORD WINAPI probe_thread(LPVOID unused) {
    FILE *log;
    unsigned attempt;
    unsigned records = 0;

    (void)unused;
    log = fopen(g_log_path, "wb");
    if (!log) {
        set_status("log_open_failed");
        return 0;
    }
    set_status("scanning");
    for (attempt = 1; attempt <= HYPRLODR_ASSET_ATTEMPTS; ++attempt) {
        unsigned regions = 0;
        unsigned long long bytes = 0;

        Sleep(attempt == 1 ? HYPRLODR_ASSET_FIRST_DELAY_MS : HYPRLODR_ASSET_ATTEMPT_MS);
        g_records = 0;
        records = scan_space(log, &regions, &bytes);
        fprintf(log, "{\"event\":\"asset_scan\",\"attempt\":%u,\"regions\":%u,\"bytes\":%llu,"
                     "\"records\":%u}\n", attempt, regions, bytes, records);
        if (records) break;
    }
    fclose(log);
    set_status(records ? "found" : "not_found");
    return 0;
}

int hyprlodr_asset_probe_start(const char *game_dir, const char *log_path) {
    HANDLE thread;

    (void)game_dir;
    if (!log_path || !log_path[0]) return -1;
    snprintf(g_log_path, sizeof g_log_path, "%s", log_path);
    thread = CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
    if (!thread) {
        set_status("no_memory");
        return -1;
    }
    CloseHandle(thread);
    return 0;
}

int hyprlodr_asset_probe_records(void) {
    return (int)g_records;
}

const char *hyprlodr_asset_probe_status(void) {
    return g_status;
}
