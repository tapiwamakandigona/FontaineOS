#include "fontfs.h"
#include "ata.h"

/*
   ==========================================================================
   FontFS implementation — see include/fontfs.h for the on-disk layout, the
   design rationale, and the documented limits.
   ==========================================================================

   Everything here is freestanding: no libc, no exceptions, no heap. All
   sector-sized scratch space lives in static globals (mirroring ata.cpp's
   ata_io_buffer) so nothing large ever lands on the tiny IRQ-context stack
   the shell runs on. Each operation is a short sequence of single-sector
   ata_read_sector / ata_write_sector calls; those are synchronous polling
   transfers with the drive IRQ gated off, so they are safe to issue directly
   from the keyboard IRQ handler exactly like the existing 'disktest' command.
*/

// -------------------------------------------------------------------------
// Static sector-sized scratch buffers (never on the stack). 4-byte aligned so
// we can safely reinterpret them as arrays of the packed 32-byte entries.
// -------------------------------------------------------------------------
static uint8_t ff_sb_sector[512]    __attribute__((aligned(4)));  // superblock scratch
static uint8_t ff_table_sector[512] __attribute__((aligned(4)));  // file-table scratch
static uint8_t ff_data_sector[512]  __attribute__((aligned(4)));  // per-sector data scratch

// Cached mount state, set by fontfs_mount()/fontfs_format().
static int ff_mounted = 0;

// -------------------------------------------------------------------------
// Minimal freestanding byte/string helpers. Hand-rolled so the -O2 optimizer
// has no reason to emit calls to a libc memset/memcpy/strlen we do not link.
// -------------------------------------------------------------------------
static void ff_memzero(uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = 0;
}

static void ff_memcpy(uint8_t* dst, const uint8_t* src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

static uint32_t ff_strlen(const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* Compare two NUL-terminated names for exact equality. */
static bool ff_nameeq(const char* a, const char* b) {
    uint32_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return false;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

/* Copy a name into a fixed FONTFS_NAME_MAX field, always NUL-terminating. */
static void ff_name_store(char* dst, const char* src) {
    uint32_t i = 0;
    while (src[i] != '\0' && i < FONTFS_NAME_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Validate a caller-supplied name: non-empty and fits (with NUL) in the field. */
static bool ff_name_ok(const char* name) {
    if (name == nullptr) return false;
    uint32_t len = ff_strlen(name);
    return len > 0 && len < FONTFS_NAME_MAX;
}

// -------------------------------------------------------------------------
// The file-table sector reinterpreted as an array of directory entries.
// ff_table_sector is 4-byte aligned and each entry is 32 bytes, so every
// entry is naturally aligned.
// -------------------------------------------------------------------------
static fontfs_entry* ff_table() {
    return reinterpret_cast<fontfs_entry*>(ff_table_sector);
}

/* Data region base LBA for directory slot 'index'. */
static uint32_t ff_slot_lba(uint32_t index) {
    return FONTFS_DATA_START_LBA + index * FONTFS_BLOCKS_PER_FILE;
}

/* Pull the file table off disk (LBA 1) into ff_table_sector. */
static void ff_load_table() {
    ata_read_sector(FONTFS_FILETABLE_LBA, ff_table_sector);
}

/* Flush ff_table_sector back to disk (LBA 1). */
static void ff_store_table() {
    ata_write_sector(FONTFS_FILETABLE_LBA, ff_table_sector);
}

/*
   Find the slot index of a live file by name, or -1 if absent.
   Assumes ff_load_table() has already populated ff_table_sector.
*/
static int ff_find(const char* name) {
    fontfs_entry* t = ff_table();
    for (uint32_t i = 0; i < FONTFS_MAX_FILES; i++) {
        if (t[i].in_use && ff_nameeq(t[i].name, name)) return (int)i;
    }
    return -1;
}

/* Find a free directory slot, or -1 if the directory is full. */
static int ff_find_free() {
    fontfs_entry* t = ff_table();
    for (uint32_t i = 0; i < FONTFS_MAX_FILES; i++) {
        if (!t[i].in_use) return (int)i;
    }
    return -1;
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

int fontfs_is_mounted() {
    return ff_mounted;
}

int fontfs_mount() {
    ata_read_sector(FONTFS_SUPERBLOCK_LBA, ff_sb_sector);
    fontfs_superblock* sb = reinterpret_cast<fontfs_superblock*>(ff_sb_sector);
    ff_mounted = (sb->magic == FONTFS_MAGIC) ? 1 : 0;
    return ff_mounted;
}

void fontfs_format() {
    // 1. Write a fresh superblock describing the fixed geometry.
    ff_memzero(ff_sb_sector, 512);
    fontfs_superblock* sb = reinterpret_cast<fontfs_superblock*>(ff_sb_sector);
    sb->magic           = FONTFS_MAGIC;
    sb->version         = FONTFS_VERSION;
    sb->max_files       = FONTFS_MAX_FILES;
    sb->filetable_lba   = FONTFS_FILETABLE_LBA;
    sb->data_start_lba  = FONTFS_DATA_START_LBA;
    sb->blocks_per_file = FONTFS_BLOCKS_PER_FILE;
    ata_write_sector(FONTFS_SUPERBLOCK_LBA, ff_sb_sector);

    // 2. Write an empty file table: every slot zeroed (in_use == 0). We still
    //    record each slot's fixed start_lba so the directory is self-describing
    //    even before any file is created.
    ff_memzero(ff_table_sector, 512);
    fontfs_entry* t = ff_table();
    for (uint32_t i = 0; i < FONTFS_MAX_FILES; i++) {
        t[i].start_lba = ff_slot_lba(i);
    }
    ff_store_table();

    ff_mounted = 1;
}

int fontfs_create(const char* name) {
    if (!ff_mounted) return FONTFS_ERR_NOTMOUNTED;
    if (!ff_name_ok(name)) return FONTFS_ERR_NAME;

    ff_load_table();
    if (ff_find(name) >= 0) return FONTFS_ERR_EXISTS;

    int slot = ff_find_free();
    if (slot < 0) return FONTFS_ERR_NOSPACE;

    fontfs_entry* t = ff_table();
    ff_name_store(t[slot].name, name);
    t[slot].size      = 0;
    t[slot].start_lba = ff_slot_lba((uint32_t)slot);
    t[slot].in_use    = 1;
    ff_store_table();
    return FONTFS_OK;
}

int fontfs_write(const char* name, const uint8_t* data, uint32_t len) {
    if (!ff_mounted) return FONTFS_ERR_NOTMOUNTED;
    if (!ff_name_ok(name)) return FONTFS_ERR_NAME;
    if (len > FONTFS_MAX_FILE_SIZE) return FONTFS_ERR_TOOBIG;

    ff_load_table();

    // Overwrite in place if it exists, otherwise claim a fresh slot.
    int slot = ff_find(name);
    if (slot < 0) {
        slot = ff_find_free();
        if (slot < 0) return FONTFS_ERR_NOSPACE;
        fontfs_entry* t = ff_table();
        ff_name_store(t[slot].name, name);
        t[slot].start_lba = ff_slot_lba((uint32_t)slot);
        t[slot].in_use    = 1;
    }

    fontfs_entry* t = ff_table();
    uint32_t base = t[slot].start_lba;

    // Write the payload one 512-byte sector at a time. The final partial
    // sector is zero-padded so no stale bytes from a previous, longer file in
    // this slot bleed into a later read.
    uint32_t written = 0;
    uint32_t block   = 0;
    while (written < len) {
        ff_memzero(ff_data_sector, 512);
        uint32_t chunk = len - written;
        if (chunk > 512) chunk = 512;
        ff_memcpy(ff_data_sector, data + written, chunk);
        ata_write_sector(base + block, ff_data_sector);
        written += chunk;
        block++;
    }

    t[slot].size = len;
    ff_store_table();
    return FONTFS_OK;
}

int fontfs_read(const char* name, uint8_t* buf, uint32_t maxlen) {
    if (!ff_mounted) return FONTFS_ERR_NOTMOUNTED;
    if (!ff_name_ok(name)) return FONTFS_ERR_NAME;

    ff_load_table();
    int slot = ff_find(name);
    if (slot < 0) return FONTFS_ERR_NOTFOUND;

    fontfs_entry* t = ff_table();
    uint32_t size = t[slot].size;
    uint32_t want = (size < maxlen) ? size : maxlen;
    uint32_t base = t[slot].start_lba;

    uint32_t done  = 0;
    uint32_t block = 0;
    while (done < want) {
        ata_read_sector(base + block, ff_data_sector);
        uint32_t chunk = want - done;
        if (chunk > 512) chunk = 512;
        ff_memcpy(buf + done, ff_data_sector, chunk);
        done += chunk;
        block++;
    }
    return (int)want;
}

int fontfs_delete(const char* name) {
    if (!ff_mounted) return FONTFS_ERR_NOTMOUNTED;
    if (!ff_name_ok(name)) return FONTFS_ERR_NAME;

    ff_load_table();
    int slot = ff_find(name);
    if (slot < 0) return FONTFS_ERR_NOTFOUND;

    // Freeing a slot is just clearing its entry; the data region is left as-is
    // and will be overwritten (and zero-padded) the next time the slot is used.
    fontfs_entry* t = ff_table();
    ff_memzero(reinterpret_cast<uint8_t*>(&t[slot]), sizeof(fontfs_entry));
    t[slot].start_lba = ff_slot_lba((uint32_t)slot);  // keep it self-describing
    ff_store_table();
    return FONTFS_OK;
}

int fontfs_list(fontfs_list_cb cb, void* ctx) {
    if (!ff_mounted) return FONTFS_ERR_NOTMOUNTED;

    ff_load_table();
    fontfs_entry* t = ff_table();
    int count = 0;
    for (uint32_t i = 0; i < FONTFS_MAX_FILES; i++) {
        if (t[i].in_use) {
            if (cb) cb(t[i].name, t[i].size, ctx);
            count++;
        }
    }
    return count;
}
