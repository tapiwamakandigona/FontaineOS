#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/*
   Primary ATA Hard Disk I/O Bus Port Definitions.
   These addresses map directly to the physical IDE controller lines on the motherboard.
*/
#define ATA_DATA        0x1F0
#define ATA_FEATURES    0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_COMMAND     0x1F7
#define ATA_STATUS      0x1F7
#define ATA_DEV_CTRL    0x3F6  // Device Control Register (interrupt gating / soft reset)

/* ATA Controller Status Bit Flags */
#define ATA_STATUS_BSY  0x80  // Controller is Busy
#define ATA_STATUS_DRQ  0x08  // Data Request Ready

/* Device Control Register Bit Flags */
#define ATA_CTRL_NIEN   0x02  // "not Interrupt ENabled": suppress drive IRQ line assertions

/* ATA Command Tokens */
#define ATA_CMD_READ    0x20  // Read sectors with retry flags active
#define ATA_CMD_WRITE   0x30  // Write sectors with retry flags active

/*
   Exposing our hard drive block read/write operations
   wrapped cleanly inside C-linkage maps.
*/
extern "C" {
    void ata_read_sector(uint32_t lba, uint8_t* buffer);
    void ata_write_sector(uint32_t lba, const uint8_t* buffer);
}

#endif
