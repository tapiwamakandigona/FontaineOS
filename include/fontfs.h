#ifndef FONTFS_H
#define FONTFS_H

#include <stdint.h>

/*
   ==========================================================================
   FontFS — a tiny PERSISTENT on-disk filesystem for FontaineOS.
   ==========================================================================

   FontFS is deliberately minimal: it exists to prove that data written from
   the shell survives a real power cycle of the machine, using nothing but the
   proven polling ATA driver (ata_read_sector / ata_write_sector — one 512-byte
   LBA sector per call). It is NOT a general-purpose filesystem; the layout is
   chosen for clarity and robustness over flexibility.

   ---------------------------- ON-DISK LAYOUT ------------------------------

     LBA 0            SUPERBLOCK      magic + version + geometry counts
     LBA 1            FILE TABLE      one 512-byte sector = 16 dir entries
     LBA 2 .. 257     DATA AREA       16 fixed file regions, 16 sectors each

   Why this shape:

   * ONE superblock sector. fontfs_mount() reads LBA 0 and checks the magic
     number; a matching magic is the whole "is a filesystem present?" test.

   * ONE file-table sector. A directory entry is exactly 32 bytes, so 512/32
     = 16 entries fit in a single sector. Keeping the whole directory in one
     sector means create/delete/rename is a single read-modify-write of LBA 1
     — no multi-sector directory traversal, no torn-write window across
     sectors.

   * FIXED per-file regions instead of a free-block allocator. File slot i
     owns a contiguous run of BLOCKS_PER_FILE sectors starting at
     DATA_START_LBA + i * BLOCKS_PER_FILE. Because a slot's disk region is a
     pure function of its index, there is no allocation bitmap to keep in sync,
     no fragmentation, and overwriting a file just rewrites its own region.
     The cost is the documented limit below.

   ------------------------------- LIMITS -----------------------------------

     * At most FONTFS_MAX_FILES (16) files.
     * Each file is at most FONTFS_BLOCKS_PER_FILE * 512 = 8192 bytes.
     * File names are at most FONTFS_NAME_MAX-1 (19) characters.

   These limits are plenty for the persistence demo and keep every operation a
   handful of single-sector ATA transfers.

   ------------------------- IRQ / ATA CONTEXT ------------------------------

   The shell parser runs inside the keyboard IRQ handler, and these calls
   issue synchronous polling ATA transfers from that context — exactly what
   the existing 'disktest' command already does successfully. The ATA driver
   gates off the drive IRQ line (nIEN), so no stray IRQ 14 can fire mid-op and
   triple-fault the machine. All scratch buffers here are static globals (like
   ata.cpp's ata_io_buffer) so we never place a 512-byte array on the small
   IRQ-context stack.
   ==========================================================================
*/

#define FONTFS_MAGIC            0xF047F5AAu  // "FontFS" tag stored in the superblock
#define FONTFS_VERSION          1u

#define FONTFS_SUPERBLOCK_LBA   0u
#define FONTFS_FILETABLE_LBA    1u
#define FONTFS_DATA_START_LBA   2u

#define FONTFS_MAX_FILES        16u   // 16 * 32-byte entries == exactly one sector
#define FONTFS_BLOCKS_PER_FILE  16u   // 16 * 512 == 8192 bytes max per file
#define FONTFS_NAME_MAX         20u   // includes the terminating NUL
#define FONTFS_MAX_FILE_SIZE    (FONTFS_BLOCKS_PER_FILE * 512u)

/* Return codes for the mutating operations (0 == success). */
#define FONTFS_OK               0
#define FONTFS_ERR_NOTMOUNTED  -1
#define FONTFS_ERR_NOSPACE     -2   // directory full (no free slot)
#define FONTFS_ERR_EXISTS      -3
#define FONTFS_ERR_NOTFOUND    -4
#define FONTFS_ERR_NAME        -5   // empty or too-long name
#define FONTFS_ERR_TOOBIG      -6   // data larger than FONTFS_MAX_FILE_SIZE

/*
   On-disk superblock (LBA 0). Packed so the byte layout is identical across
   builds; only the leading fields are meaningful, the sector's remainder is
   zero padding.
*/
struct fontfs_superblock {
    uint32_t magic;            // FONTFS_MAGIC when a valid FS is present
    uint32_t version;          // FONTFS_VERSION
    uint32_t max_files;        // FONTFS_MAX_FILES
    uint32_t filetable_lba;    // FONTFS_FILETABLE_LBA
    uint32_t data_start_lba;   // FONTFS_DATA_START_LBA
    uint32_t blocks_per_file;  // FONTFS_BLOCKS_PER_FILE
} __attribute__((packed));

/*
   On-disk directory entry. Exactly 32 bytes so 16 fit in the file-table
   sector. 'start_lba' is redundant with the slot index but stored anyway so
   the directory is self-describing on disk.
*/
struct fontfs_entry {
    char     name[FONTFS_NAME_MAX];  // 20 bytes, NUL-terminated
    uint32_t size;                   // file length in bytes
    uint32_t start_lba;              // first data sector for this slot
    uint8_t  in_use;                 // 1 == slot holds a live file
    uint8_t  reserved[3];            // pad to a clean 32-byte stride
} __attribute__((packed));

/* Callback signature for fontfs_list(): invoked once per live file. */
typedef void (*fontfs_list_cb)(const char* name, uint32_t size, void* ctx);

/*
   Reads the superblock and validates the magic number. Returns 1 when a
   valid FontFS is present, 0 otherwise. Must be called once at boot before
   any other operation; the result is cached so later calls are cheap.
*/
int  fontfs_mount();

/* 1 if a valid FS is currently mounted, else 0 (cached mount result). */
int  fontfs_is_mounted();

/* Writes a fresh superblock + empty file table. Leaves the FS mounted. */
void fontfs_format();

/* Create an empty (zero-length) file. FONTFS_ERR_EXISTS if it already exists. */
int  fontfs_create(const char* name);

/* Create-or-overwrite 'name' with 'len' bytes from 'data'. */
int  fontfs_write(const char* name, const uint8_t* data, uint32_t len);

/*
   Copy up to 'maxlen' bytes of 'name' into 'buf'. Returns the number of bytes
   copied (>= 0), or a negative FONTFS_ERR_* code on failure.
*/
int  fontfs_read(const char* name, uint8_t* buf, uint32_t maxlen);

/* Remove 'name' (frees its directory slot). */
int  fontfs_delete(const char* name);

/* Invoke 'cb' once for each live file. Returns the number of files listed. */
int  fontfs_list(fontfs_list_cb cb, void* ctx);

#endif
