#include "ata.h"
#include "timer.h" // Pulls in our working outb and inb shared port definitions

/*
   Low-level hardware bus communications.
   Reads a single 8-bit byte from an input port.
*/
inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
   Fast Word Streaming Assembler Line.
   Pulls 256 words (512 bytes) from the hard disk data bus buffer
   and streams them straight into your memory pointer layout.
*/
/*
   Fast Word Streaming Assembler Line.
   Fixed: Enforces explicit register operand tracking constraints
   so the stack pointer address is forcefully loaded into EDI.
*/
inline void insw(uint16_t port, void* addr, uint32_t count) {
    asm volatile (
        "cld\n\t"
        "rep insw"
        : "+D"(addr), "+c"(count)
        : "d"(port)
        : "memory"
    );
}


/*
   Waits until the motherboard hard disk controller clears its Busy bit (BSY)
   and raises its Data Request Ready flag (DRQ).
*/
static void ata_wait_ready() {
    // Poll the status port endlessly until the controller is unblocked
    while (inb(ATA_STATUS) & ATA_STATUS_BSY);
    while (!(inb(ATA_STATUS) & ATA_STATUS_DRQ));
}

/*
   Reads one 512-byte sector using 28-bit Logical Block Addressing (LBA).
*/
extern "C" void ata_read_sector(uint32_t lba, uint8_t* buffer) {
    // 1. Send the standard LBA bus registers and drive target bits to the disk controller
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F)); // Select Master Drive + Top 4 bits of LBA
    outb(ATA_FEATURES, 0x00);                         // Clear error parameters
    outb(ATA_SECTOR_CNT, 0x01);                       // We want to request exactly 1 sector (512 bytes)
    outb(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));        // Bits 0-7
    outb(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF)); // Bits 8-15
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));// Bits 16-23

    // 2. Issue the active sector read token code to the command port
    outb(ATA_COMMAND, ATA_CMD_READ);

    // 3. Wait safely until the physical controller chips prepare the sector stream
    ata_wait_ready();

    /*
       4. Stream the data! 1 sector is 512 bytes, which equals exactly 256 words.
       We stream the data directly out from the primary disk port into your buffer tracking map.
    */
    insw(ATA_DATA, buffer, 256);
}

/*
   Writes a single 512-byte block from your memory buffer directly onto the disk sector plates.
*/
extern "C" void ata_write_sector(uint32_t lba, const uint8_t* buffer) {
    // Fast Word Streaming Assembler Line out to port
    auto outsw = [](uint16_t port, const void* addr, uint32_t count) {
        asm volatile ("cld; rep outsw" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
    };

    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECTOR_CNT, 0x01);
    outb(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE);

    ata_wait_ready();

    // Stream 256 words (512 bytes) outwards onto the hardware bus line traces
    outsw(ATA_DATA, buffer, 256);
}
