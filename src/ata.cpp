#include "ata.h"
#include "timer.h"

inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

inline void insw(uint16_t port, void* addr, uint32_t count) {
    asm volatile (
        "cld\n\t"
        "rep insw"
        : "+D"(addr), "+c"(count)
        : "d"(port)
        : "memory"
    );
}

// Static global hardware sector landing pad to completely avoid stack range collisions
static uint8_t ata_io_buffer[512] __attribute__((aligned(4)));

/*
   Gate off the drive's hardware interrupt line (nIEN = 1).

   This driver is a pure polling driver, but by default the drive still raises
   IRQ 14 every time a command completes. Our IDT configures gates 0, 32 and 33
   only, so that stray IRQ 14 vector lands on an empty (non-present) descriptor,
   the CPU faults while faulting, and the machine triple-faults and reboots.
   That is exactly what happened whenever the shell ran 'disktest'.
*/
static void ata_disable_irq() {
    outb(ATA_DEV_CTRL, ATA_CTRL_NIEN);
}

/*
   Dedicated Read Wait.
   Waits until the drive stops being busy AND flags that read data is ready on the bus pins.
*/
static void ata_wait_read() {
    while (inb(ATA_STATUS) & ATA_STATUS_BSY) {
        asm volatile("" : : : "memory");
    }
    while (!(inb(ATA_STATUS) & ATA_STATUS_DRQ)) {
        asm volatile("" : : : "memory");
    }
}

/*
   Dedicated Write Wait.
   We ONLY wait for the hardware to clear the Busy flag (BSY).
   We do not check DRQ because the drive clears it immediately while baking data.
*/
static void ata_wait_write() {
    while (inb(ATA_STATUS) & ATA_STATUS_BSY) {
        asm volatile("" : : : "memory");
    }
}

extern "C" void ata_read_sector(uint32_t lba, uint8_t* buffer) {
    ata_disable_irq();
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_FEATURES, 0x00);
    outb(ATA_SECTOR_CNT, 0x01);
    outb(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ);

    // Call our dedicated read synchronization loop
    ata_wait_read();

    // Stream the raw hardware sector straight into our safe global binary buffer area
    insw(ATA_DATA, ata_io_buffer, 256);

    // Safely replicate the landed bytes back out into the target function container
    for (int i = 0; i < 512; i++) {
        buffer[i] = ata_io_buffer[i];
    }
}

extern "C" void ata_write_sector(uint32_t lba, const uint8_t* buffer) {
    auto outsw = [](uint16_t port, const void* addr, uint32_t count) {
        asm volatile (
            "cld\n\t"
            "rep outsw"
            : "+S"(addr), "+c"(count)
            : "d"(port)
            : "memory"
        );
    };

    // Snapshot the incoming data parameters into our flat global hardware platform segment
    for (int i = 0; i < 512; i++) {
        ata_io_buffer[i] = buffer[i];
    }

    ata_disable_irq();
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECTOR_CNT, 0x01);
    outb(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));

    // Send the write token command
    outb(ATA_COMMAND, ATA_CMD_WRITE);

    /*
       Wait for the drive to enter its data-request phase (BSY clear, DRQ set)
       before streaming words onto the bus. Blasting data while BSY is still
       high works by luck on some emulators but corrupts writes on real drives.
    */
    ata_wait_read();

    // Feed the data words onto the bus pins immediately to unblock the controller cache
    outsw(ATA_DATA, ata_io_buffer, 256);

    // Call our dedicated write synchronization loop to clear out busy cycles safely
    ata_wait_write();
}
