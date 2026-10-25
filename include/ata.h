#ifndef ATA_H
#define ATA_H

#include "types.h"

/* ============================================================================
 * 64-Bit Inline Assembly Port I/O Wrappers (Freestanding x86_64)
 * ============================================================================ */
#ifndef PORT_IO_WRAPPERS_DEFINED
#define PORT_IO_WRAPPERS_DEFINED

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %b0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %w1, %w0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %w0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif /* PORT_IO_WRAPPERS_DEFINED */

/* ============================================================================
 * Primary & Secondary IDE / ATA Controller I/O Ports
 * ============================================================================ */
#define ATA_PRIMARY_IO           0x1F0
#define ATA_PRIMARY_CTRL         0x3F6
#define ATA_SECONDARY_IO         0x170
#define ATA_SECONDARY_CTRL       0x376

/* Register Offsets from Channel I/O Base */
#define ATA_REG_DATA             0x00
#define ATA_REG_ERROR            0x01
#define ATA_REG_FEATURES         0x01
#define ATA_REG_SECCOUNT         0x02
#define ATA_REG_LBA_LO           0x03
#define ATA_REG_LBA_MID          0x04
#define ATA_REG_LBA_HI           0x05
#define ATA_REG_DRV_HEAD         0x06
#define ATA_REG_STATUS           0x07
#define ATA_REG_COMMAND          0x07

/* Legacy Primary Macros (kept for compatibility) */
#define ATA_PRIMARY_DATA         0x1F0
#define ATA_PRIMARY_ERROR        0x1F1
#define ATA_PRIMARY_FEATURES     0x1F1
#define ATA_PRIMARY_SECCOUNT     0x1F2
#define ATA_PRIMARY_LBA_LO       0x1F3
#define ATA_PRIMARY_LBA_MID      0x1F4
#define ATA_PRIMARY_LBA_HI       0x1F5
#define ATA_PRIMARY_DRV_HEAD     0x1F6
#define ATA_PRIMARY_STATUS       0x1F7
#define ATA_PRIMARY_COMMAND      0x1F7
#define ATA_PRIMARY_ALT_STATUS   0x3F6
#define ATA_PRIMARY_DEV_CTL      0x3F6

/* ATA Device Control Register Bits (Control Base: 0x3F6 / 0x376) */
#define ATA_CTL_NIEN             0x02  /* Disable interrupts (pure PIO polling) */
#define ATA_CTL_SRST             0x04  /* Software Reset */

/* ATA Status Register Bits (Command Base + 7) */
#define ATA_SR_ERR               0x01  /* Error */
#define ATA_SR_DRQ               0x08  /* Data Request ready */
#define ATA_SR_SRV               0x10  /* Overlapped Mode Service Request */
#define ATA_SR_DF                0x20  /* Drive Fault */
#define ATA_SR_DRDY              0x40  /* Drive Ready */
#define ATA_SR_BSY               0x80  /* Drive Busy */

/* ATA Commands */
#define ATA_CMD_IDENTIFY         0xEC  /* ATA IDENTIFY DEVICE */
#define ATA_CMD_READ_PIO         0x20  /* LBA28 Read Sectors */
#define ATA_CMD_READ_PIO_EXT     0x24  /* LBA48 Read Sectors Ext */
#define ATA_CMD_WRITE_PIO        0x30  /* LBA28 Write Sectors */
#define ATA_CMD_WRITE_PIO_EXT    0x34  /* LBA48 Write Sectors Ext */
#define ATA_CMD_CACHE_FLUSH      0xE7  /* LBA28 Flush Cache */
#define ATA_CMD_CACHE_FLUSH_EXT  0xEA  /* LBA48 Flush Cache Ext */

#define ATA_SECTOR_SIZE          512U
#define ATA_MAX_DRIVES           4

/* Hardware Media Classification (Linux libata / udev conventions) */
enum ata_media_type {
    ATA_MEDIA_UNKNOWN = 0,
    ATA_MEDIA_VHD     = 1,  /* Virtual Disk      -> Tag VHD001..VHD999 */
    ATA_MEDIA_HDD     = 2,  /* Physical HDD      -> Tag HD001..HD999   */
    ATA_MEDIA_SSD     = 3   /* Physical SSD      -> Tag SD001..SD999   */
};

/* Detected Drive Metadata (ATA IDENTIFY) */
struct ata_drive_info {
    uint8_t  present;
    uint8_t  slot;                  /* 0..3 (Primary Master/Slave, Secondary Master/Slave) */
    uint16_t io_base;               /* 0x1F0 or 0x170 */
    uint16_t ctrl_base;             /* 0x3F6 or 0x376 */
    uint8_t  is_slave;              /* 0 = Master, 1 = Slave */
    uint8_t  lba48_supported;
    uint8_t  media_type;            /* enum ata_media_type */
    uint16_t rotation_rate;         /* ATA IDENTIFY Word 217 (0x0001 = SSD, >=0x0401 = RPM) */
    char     dev_name[12];          /* Linux device node, e.g. "/dev/sda" */
    char     tag[8];                /* Persistent tag: VHD001..999, HD001..999, SD001..999 */
    char     model[41];
    char     serial[21];
    uint64_t total_sectors;
    uint64_t capacity_bytes;
    uint64_t capacity_gib;
    uint64_t capacity_gib_hundredths;
};

/* Public Storage Driver API */
int storage_init(void);
int ata_is_present(void);
int ata_get_drive_count(void);
int ata_get_active_slot(void);
int ata_select_drive(int slot);

const struct ata_drive_info *ata_get_drive_info(void);
const struct ata_drive_info *ata_get_drive_by_slot(int slot);

const char *ata_media_type_name(uint8_t media_type);
const char *ata_media_tag_prefix(uint8_t media_type);
void ata_format_tag(uint8_t media_type, int index_1_to_999, char out_tag[8]);
void ata_set_drive_tag(int slot, const char *tag);

/* Sector I/O on Active Drive */
int ata_read_sectors(uint64_t lba, uint32_t count, void *buffer);
int ata_write_sectors(uint64_t lba, uint32_t count, const void *buffer);
int ata_flush_cache_ext(void);

/* Sector I/O on Explicit Slot (0..3) */
int ata_read_sectors_slot(int slot, uint64_t lba, uint32_t count, void *buffer);
int ata_write_sectors_slot(int slot, uint64_t lba, uint32_t count, const void *buffer);
int ata_flush_cache_slot(int slot);

#endif /* ATA_H */
