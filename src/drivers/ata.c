#include "ata.h"
#include "vga.h"
#include "string.h"

/* Live telemetry exported for the dismasmOS 'pr' hardware monitor */
extern uint32_t last_io_lba;
extern uint8_t  last_ata_status;

static struct ata_drive_info g_drives[ATA_MAX_DRIVES];
static struct ata_drive_info g_empty_drive;
static int g_active_slot = -1;
static int g_drive_count = 0;

/* ----------------------------------------------------------------------------
 * 64-Bit Integer & Case-Insensitive String Helpers (Freestanding, no libc)
 * ---------------------------------------------------------------------------- */
static void ata_u64_to_str(uint64_t val, char *buf) {
    char tmp[24];
    int i = 0;

    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    while (val > 0) {
        tmp[i++] = (char)('0' + (val % 10ULL));
        val /= 10ULL;
    }

    for (int j = 0; j < i; j++) {
        buf[j] = tmp[i - 1 - j];
    }
    buf[i] = '\0';
}

static char ata_toupper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static int ata_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle || needle[0] == '\0') {
        return 0;
    }
    for (size_t i = 0; haystack[i] != '\0'; i++) {
        size_t j = 0;
        while (needle[j] != '\0' && haystack[i + j] != '\0' &&
               ata_toupper(haystack[i + j]) == ata_toupper(needle[j])) {
            j++;
        }
        if (needle[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

const char *ata_media_type_name(uint8_t media_type) {
    switch (media_type) {
        case ATA_MEDIA_VHD: return "Virtual Disk (VHD)";
        case ATA_MEDIA_SSD: return "Solid State Drive (SSD)";
        case ATA_MEDIA_HDD: return "Hard Disk Drive (HDD)";
        default:            return "Unknown Media";
    }
}

const char *ata_media_tag_prefix(uint8_t media_type) {
    switch (media_type) {
        case ATA_MEDIA_VHD: return "VHD";
        case ATA_MEDIA_SSD: return "SD";
        case ATA_MEDIA_HDD: return "HD";
        default:            return "HD";
    }
}

void ata_format_tag(uint8_t media_type, int index_1_to_999, char out_tag[8]) {
    const char *pfx = ata_media_tag_prefix(media_type);
    if (index_1_to_999 < 1) {
        index_1_to_999 = 1;
    }
    if (index_1_to_999 > 999) {
        index_1_to_999 = 999;
    }

    size_t p = 0;
    while (pfx[p] != '\0' && p < 4) {
        out_tag[p] = pfx[p];
        p++;
    }
    out_tag[p++] = (char)('0' + ((index_1_to_999 / 100) % 10));
    out_tag[p++] = (char)('0' + ((index_1_to_999 / 10) % 10));
    out_tag[p++] = (char)('0' + (index_1_to_999 % 10));
    out_tag[p]   = '\0';
}

void ata_set_drive_tag(int slot, const char *tag) {
    if (slot < 0 || slot >= ATA_MAX_DRIVES || !tag) {
        return;
    }
    strncpy(g_drives[slot].tag, tag, sizeof(g_drives[slot].tag) - 1);
    g_drives[slot].tag[sizeof(g_drives[slot].tag) - 1] = '\0';
}

/* Classify drive media using Linux libata / udev rules:
 * 1. Virtual hypervisor signatures in model/serial -> ATA_MEDIA_VHD (VHD001..999)
 * 2. ATA8-ACS Word 217 == 0x0001 (non-rotating), Word 169 Bit 0 (TRIM), or SSD model -> ATA_MEDIA_SSD (SD001..999)
 * 3. Rotational RPM (Word 217 >= 0x0401) or physical magnetic disk -> ATA_MEDIA_HDD (HD001..999) */
static uint8_t ata_classify_media(const uint16_t *identify, const char *model, const char *serial) {
    static const char * const virt_vendors[] = {
        "QEMU", "VBOX", "VIRTUALBOX", "VMWARE", "VIRTUAL",
        "VHD", "VIRTIO", "XEN", "BOCHS", "PARALLELS", "KVM", "MSFT"
    };

    for (size_t i = 0; i < sizeof(virt_vendors) / sizeof(virt_vendors[0]); i++) {
        if (ata_strcasestr(model, virt_vendors[i]) || ata_strcasestr(serial, virt_vendors[i])) {
            return ATA_MEDIA_VHD;
        }
    }

    uint16_t rot_rate = identify[217];
    uint16_t dsm_trim = identify[169];

    if (rot_rate == 0x0001U) {
        return ATA_MEDIA_SSD;
    }
    if (rot_rate >= 0x0401U && rot_rate <= 0xFFFEU) {
        return ATA_MEDIA_HDD;
    }
    if ((dsm_trim & 0x0001U) != 0) {
        return ATA_MEDIA_SSD;
    }
    if (ata_strcasestr(model, "SSD") || ata_strcasestr(model, "NVME") ||
        ata_strcasestr(model, "SOLID") || ata_strcasestr(model, "FLASH")) {
        return ATA_MEDIA_SSD;
    }

    return ATA_MEDIA_HDD;
}

/* 400ns ATA delay by reading the channel's Alternate Status register 4 times */
static inline void ata_delay_400ns(uint16_t ctrl_base) {
    (void)inb(ctrl_base);
    (void)inb(ctrl_base);
    (void)inb(ctrl_base);
    (void)inb(ctrl_base);
}

/* Poll until BSY (Bit 7) clears */
static int ata_poll_not_busy(uint16_t io_base, uint32_t timeout) {
    while (timeout--) {
        uint8_t st = inb(io_base + ATA_REG_STATUS);
        last_ata_status = st;
        if (st == 0xFF) {
            return -1; /* Floating bus */
        }
        if (!(st & ATA_SR_BSY)) {
            return 0;
        }
    }
    return -1;
}

/* Poll until BSY=0 and DRQ=1 (with no ERR or DF) */
static int ata_poll_drq(uint16_t io_base, uint32_t timeout) {
    while (timeout--) {
        uint8_t st = inb(io_base + ATA_REG_STATUS);
        last_ata_status = st;
        if (st == 0xFF) {
            return -1;
        }
        if (st & ATA_SR_BSY) {
            continue;
        }
        if (st & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
        if (st & ATA_SR_DRQ) {
            return 0;
        }
    }
    return -1;
}

/* Extract and trim big-endian word string from ATA IDENTIFY block */
static void ata_extract_string(const uint16_t *identify, int start_word, int word_count, char *out) {
    int pos = 0;
    for (int i = 0; i < word_count; i++) {
        uint16_t w = identify[start_word + i];
        out[pos++] = (char)((w >> 8) & 0xFF);
        out[pos++] = (char)(w & 0xFF);
    }
    out[pos] = '\0';

    while (pos > 0 && (out[pos - 1] == ' ' || out[pos - 1] == '\0')) {
        out[--pos] = '\0';
    }

    int lead = 0;
    while (out[lead] == ' ') {
        lead++;
    }
    if (lead > 0) {
        int i = 0;
        while (out[lead + i] != '\0') {
            out[i] = out[lead + i];
            i++;
        }
        out[i] = '\0';
    }
}

/* Probe a single ATA channel/drive slot (0..3) via ATA IDENTIFY (0xEC) */
static int ata_probe_single_slot(int slot, uint16_t io_base, uint16_t ctrl_base, uint8_t is_slave) {
    uint16_t identify[256];
    struct ata_drive_info *drv = &g_drives[slot];
    memset(drv, 0, sizeof(*drv));

    drv->slot      = (uint8_t)slot;
    drv->io_base   = io_base;
    drv->ctrl_base = ctrl_base;
    drv->is_slave  = is_slave;

    /* Select Master (0xA0) or Slave (0xB0) and disable IRQ (pure PIO polling) */
    outb(io_base + ATA_REG_DRV_HEAD, (uint8_t)(0xA0U | ((is_slave & 1U) << 4)));
    ata_delay_400ns(ctrl_base);
    outb(ctrl_base, ATA_CTL_NIEN);

    uint8_t bus_status = inb(io_base + ATA_REG_STATUS);
    last_ata_status = bus_status;
    if (bus_status == 0xFF || bus_status == 0x7F) {
        return -1;
    }

    /* Zero LBA and sector count registers per ATA IDENTIFY specification */
    outb(io_base + ATA_REG_SECCOUNT, 0x00);
    outb(io_base + ATA_REG_LBA_LO,   0x00);
    outb(io_base + ATA_REG_LBA_MID,  0x00);
    outb(io_base + ATA_REG_LBA_HI,   0x00);

    /* Send ATA IDENTIFY (0xEC) */
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns(ctrl_base);

    uint8_t status = inb(io_base + ATA_REG_STATUS);
    last_ata_status = status;
    if (status == 0x00 || status == 0xFF) {
        return -1; /* No device attached on this slot */
    }

    if (ata_poll_not_busy(io_base, 1000000) != 0) {
        return -1;
    }

    uint8_t lba_mid = inb(io_base + ATA_REG_LBA_MID);
    uint8_t lba_hi  = inb(io_base + ATA_REG_LBA_HI);
    status = inb(io_base + ATA_REG_STATUS);
    last_ata_status = status;

    if (lba_mid != 0x00 || lba_hi != 0x00 || (status & (ATA_SR_ERR | ATA_SR_DF))) {
        /* E.g. ATAPI optical CD-ROM (0x14 / 0xEB) rather than writable ATA disk */
        return -1;
    }

    if (ata_poll_drq(io_base, 1000000) != 0) {
        return -1;
    }

    for (int i = 0; i < 256; i++) {
        identify[i] = inw(io_base + ATA_REG_DATA);
    }

    ata_extract_string(identify, 27, 20, drv->model);
    ata_extract_string(identify, 10, 10, drv->serial);
    if (drv->model[0] == '\0') {
        strcpy(drv->model, "ATA Generic Disk");
    }
    if (drv->serial[0] == '\0') {
        strcpy(drv->serial, "N/A");
    }

    uint64_t sectors48 = ((uint64_t)identify[100])
                       | ((uint64_t)identify[101] << 16)
                       | ((uint64_t)identify[102] << 32)
                       | ((uint64_t)identify[103] << 48);

    if ((identify[83] & (1U << 10)) && sectors48 > 0) {
        drv->lba48_supported = 1;
        drv->total_sectors   = sectors48;
    } else {
        uint64_t sectors28 = ((uint64_t)identify[61] << 16) | (uint64_t)identify[60];
        drv->lba48_supported = (sectors48 > 0) ? 1 : 0;
        drv->total_sectors   = (sectors48 > 0) ? sectors48 : sectors28;
    }

    if (drv->total_sectors == 0) {
        return -1;
    }

    drv->present        = 1;
    drv->rotation_rate  = identify[217];
    drv->media_type     = ata_classify_media(identify, drv->model, drv->serial);
    drv->capacity_bytes = drv->total_sectors * (uint64_t)ATA_SECTOR_SIZE;
    drv->capacity_gib   = drv->capacity_bytes >> 30;
    drv->capacity_gib_hundredths =
        ((drv->capacity_bytes & ((1ULL << 30) - 1ULL)) * 100ULL) >> 30;

    return 0;
}

static void ata_print_no_disk_warning(void) {
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_puts("[WARN] storage: No writable ATA disk detected. Running in RAMFS mode.\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ============================================================================
 * 1. storage_init(): Multi-Channel ATA Bus Scan & Hardware Media Tagging
 * ============================================================================ */
int storage_init(void) {
    static const struct {
        uint16_t io;
        uint16_t ctrl;
        uint8_t  slave;
    } bus_map[ATA_MAX_DRIVES] = {
        { ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   0 }, /* Slot 0: Primary Master   */
        { ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   1 }, /* Slot 1: Primary Slave    */
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0 }, /* Slot 2: Secondary Master */
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 1 }  /* Slot 3: Secondary Slave  */
    };

    memset(g_drives, 0, sizeof(g_drives));
    memset(&g_empty_drive, 0, sizeof(g_empty_drive));
    g_active_slot = -1;
    g_drive_count = 0;

    int vhd_seq = 0;
    int hd_seq  = 0;
    int sd_seq  = 0;

    for (int s = 0; s < ATA_MAX_DRIVES; s++) {
        if (ata_probe_single_slot(s, bus_map[s].io, bus_map[s].ctrl, bus_map[s].slave) == 0) {
            struct ata_drive_info *drv = &g_drives[s];

            /* Assign Linux-style /dev/sdX block node in discovery order */
            strcpy(drv->dev_name, "/dev/sd");
            drv->dev_name[7] = (char)('a' + g_drive_count);
            drv->dev_name[8] = '\0';

            /* Assign provisional hardware media tag (001-999); ntfs_automount()
             * reconciles any persisted on-disk LABEL tags across boots. */
            int idx = 1;
            if (drv->media_type == ATA_MEDIA_VHD) {
                idx = ++vhd_seq;
            } else if (drv->media_type == ATA_MEDIA_SSD) {
                idx = ++sd_seq;
            } else {
                idx = ++hd_seq;
            }
            ata_format_tag(drv->media_type, idx, drv->tag);

            if (g_active_slot < 0) {
                g_active_slot = s;
            }
            g_drive_count++;

            char num_buf[24];
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_puts("[  ");
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            vga_puts("OK");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_puts("  ] ");
            vga_puts(drv->dev_name);
            vga_puts(" [");
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            vga_puts(drv->tag);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_puts("]: ");
            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            vga_puts(drv->model);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_puts(" | ");

            ata_u64_to_str(drv->capacity_gib, num_buf);
            vga_puts(num_buf);
            vga_putchar('.');
            if (drv->capacity_gib_hundredths < 10) {
                vga_putchar('0');
            }
            ata_u64_to_str(drv->capacity_gib_hundredths, num_buf);
            vga_puts(num_buf);
            vga_puts(" GiB (");
            ata_u64_to_str(drv->total_sectors, num_buf);
            vga_puts(num_buf);
            vga_puts(" sectors)\n");
        }
    }

    if (g_drive_count == 0) {
        ata_print_no_disk_warning();
        return -1;
    }

    return 0;
}

int ata_is_present(void) {
    if (g_active_slot < 0 || g_active_slot >= ATA_MAX_DRIVES) {
        return 0;
    }
    return (int)g_drives[g_active_slot].present;
}

int ata_get_drive_count(void) {
    return g_drive_count;
}

int ata_get_active_slot(void) {
    return g_active_slot;
}

int ata_select_drive(int slot) {
    if (slot < 0 || slot >= ATA_MAX_DRIVES || !g_drives[slot].present) {
        return -1;
    }
    g_active_slot = slot;
    return 0;
}

const struct ata_drive_info *ata_get_drive_info(void) {
    if (g_active_slot < 0 || g_active_slot >= ATA_MAX_DRIVES || !g_drives[g_active_slot].present) {
        return &g_empty_drive;
    }
    return &g_drives[g_active_slot];
}

const struct ata_drive_info *ata_get_drive_by_slot(int slot) {
    if (slot < 0 || slot >= ATA_MAX_DRIVES) {
        return &g_empty_drive;
    }
    return &g_drives[slot];
}

/* ============================================================================
 * 2. LBA48 / LBA28 PIO Sector Read (Slot-Aware)
 * ============================================================================ */
int ata_read_sectors_slot(int slot, uint64_t lba, uint32_t count, void *buffer) {
    if (slot < 0 || slot >= ATA_MAX_DRIVES || !g_drives[slot].present || buffer == NULL) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    const struct ata_drive_info *drv = &g_drives[slot];
    if (lba >= drv->total_sectors || (uint64_t)count > (drv->total_sectors - lba)) {
        return -1;
    }

    uint16_t io   = drv->io_base;
    uint16_t ctrl = drv->ctrl_base;
    uint8_t  slv  = (uint8_t)((drv->is_slave & 1U) << 4);
    uint8_t *dst  = (uint8_t *)buffer;

    while (count > 0) {
        uint32_t chunk = (count > 256U) ? 256U : count;

        if (ata_poll_not_busy(io, 1000000) != 0) {
            return -1;
        }

        last_io_lba = (uint32_t)(lba & 0xFFFFFFFFULL);

        if (drv->lba48_supported) {
            outb(io + ATA_REG_DRV_HEAD, (uint8_t)(0x40U | slv));
            ata_delay_400ns(ctrl);
            if (ata_poll_not_busy(io, 1000000) != 0) {
                return -1;
            }

            /* High bytes first */
            outb(io + ATA_REG_SECCOUNT, (uint8_t)((chunk >> 8) & 0xFF));
            outb(io + ATA_REG_LBA_LO,   (uint8_t)((lba >> 24) & 0xFF));
            outb(io + ATA_REG_LBA_MID,  (uint8_t)((lba >> 32) & 0xFF));
            outb(io + ATA_REG_LBA_HI,   (uint8_t)((lba >> 40) & 0xFF));

            /* Low bytes second */
            outb(io + ATA_REG_SECCOUNT, (uint8_t)(chunk & 0xFF));
            outb(io + ATA_REG_LBA_LO,   (uint8_t)(lba & 0xFF));
            outb(io + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
            outb(io + ATA_REG_LBA_HI,   (uint8_t)((lba >> 16) & 0xFF));

            outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO_EXT);
        } else {
            uint32_t chunk28 = (chunk > 255U) ? 255U : chunk;
            chunk = chunk28;
            outb(io + ATA_REG_DRV_HEAD, (uint8_t)(0xE0U | slv | ((lba >> 24) & 0x0FU)));
            ata_delay_400ns(ctrl);
            if (ata_poll_not_busy(io, 1000000) != 0) {
                return -1;
            }
            outb(io + ATA_REG_SECCOUNT, (uint8_t)(chunk & 0xFF));
            outb(io + ATA_REG_LBA_LO,   (uint8_t)(lba & 0xFF));
            outb(io + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
            outb(io + ATA_REG_LBA_HI,   (uint8_t)((lba >> 16) & 0xFF));
            outb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
        }

        for (uint32_t s = 0; s < chunk; s++) {
            ata_delay_400ns(ctrl);
            if (ata_poll_drq(io, 1000000) != 0) {
                return -1;
            }

            uint16_t *wptr = (uint16_t *)(dst + (size_t)s * ATA_SECTOR_SIZE);
            for (int w = 0; w < 256; w++) {
                wptr[w] = inw(io + ATA_REG_DATA);
            }
        }

        dst   += (size_t)chunk * ATA_SECTOR_SIZE;
        lba   += chunk;
        count -= chunk;
    }

    return 0;
}

/* ============================================================================
 * 3. ATA FLUSH CACHE EXT (0xEA): Flush Hardware Write Buffer to Non-Volatile Media
 * ============================================================================ */
int ata_flush_cache_slot(int slot) {
    if (slot < 0 || slot >= ATA_MAX_DRIVES || !g_drives[slot].present) {
        return -1;
    }

    const struct ata_drive_info *drv = &g_drives[slot];
    uint16_t io   = drv->io_base;
    uint16_t ctrl = drv->ctrl_base;
    uint8_t  slv  = (uint8_t)((drv->is_slave & 1U) << 4);

    if (ata_poll_not_busy(io, 2000000) != 0) {
        return -1;
    }

    if (drv->lba48_supported) {
        outb(io + ATA_REG_DRV_HEAD, (uint8_t)(0x40U | slv));
        ata_delay_400ns(ctrl);
        outb(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH_EXT); /* 0xEA */
    } else {
        outb(io + ATA_REG_DRV_HEAD, (uint8_t)(0xE0U | slv));
        ata_delay_400ns(ctrl);
        outb(io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);     /* 0xE7 */
    }

    ata_delay_400ns(ctrl);
    if (ata_poll_not_busy(io, 10000000) != 0) {
        return -1;
    }

    uint8_t st = inb(io + ATA_REG_STATUS);
    last_ata_status = st;
    if (st & (ATA_SR_ERR | ATA_SR_DF)) {
        return -1;
    }

    return 0;
}

/* ============================================================================
 * 4. LBA48 / LBA28 PIO Sector Write + Hardware Cache Flush (Slot-Aware)
 * ============================================================================ */
int ata_write_sectors_slot(int slot, uint64_t lba, uint32_t count, const void *buffer) {
    if (slot < 0 || slot >= ATA_MAX_DRIVES || !g_drives[slot].present || buffer == NULL) {
        return -1;
    }
    if (count == 0) {
        return 0;
    }

    const struct ata_drive_info *drv = &g_drives[slot];
    if (lba >= drv->total_sectors || (uint64_t)count > (drv->total_sectors - lba)) {
        return -1;
    }

    uint16_t io   = drv->io_base;
    uint16_t ctrl = drv->ctrl_base;
    uint8_t  slv  = (uint8_t)((drv->is_slave & 1U) << 4);
    const uint8_t *src = (const uint8_t *)buffer;

    while (count > 0) {
        uint32_t chunk = (count > 256U) ? 256U : count;

        if (ata_poll_not_busy(io, 1000000) != 0) {
            return -1;
        }

        last_io_lba = (uint32_t)(lba & 0xFFFFFFFFULL);

        if (drv->lba48_supported) {
            outb(io + ATA_REG_DRV_HEAD, (uint8_t)(0x40U | slv));
            ata_delay_400ns(ctrl);
            if (ata_poll_not_busy(io, 1000000) != 0) {
                return -1;
            }

            /* High bytes first */
            outb(io + ATA_REG_SECCOUNT, (uint8_t)((chunk >> 8) & 0xFF));
            outb(io + ATA_REG_LBA_LO,   (uint8_t)((lba >> 24) & 0xFF));
            outb(io + ATA_REG_LBA_MID,  (uint8_t)((lba >> 32) & 0xFF));
            outb(io + ATA_REG_LBA_HI,   (uint8_t)((lba >> 40) & 0xFF));

            /* Low bytes second */
            outb(io + ATA_REG_SECCOUNT, (uint8_t)(chunk & 0xFF));
            outb(io + ATA_REG_LBA_LO,   (uint8_t)(lba & 0xFF));
            outb(io + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
            outb(io + ATA_REG_LBA_HI,   (uint8_t)((lba >> 16) & 0xFF));

            outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO_EXT);
        } else {
            uint32_t chunk28 = (chunk > 255U) ? 255U : chunk;
            chunk = chunk28;
            outb(io + ATA_REG_DRV_HEAD, (uint8_t)(0xE0U | slv | ((lba >> 24) & 0x0FU)));
            ata_delay_400ns(ctrl);
            if (ata_poll_not_busy(io, 1000000) != 0) {
                return -1;
            }
            outb(io + ATA_REG_SECCOUNT, (uint8_t)(chunk & 0xFF));
            outb(io + ATA_REG_LBA_LO,   (uint8_t)(lba & 0xFF));
            outb(io + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
            outb(io + ATA_REG_LBA_HI,   (uint8_t)((lba >> 16) & 0xFF));
            outb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
        }

        for (uint32_t s = 0; s < chunk; s++) {
            ata_delay_400ns(ctrl);
            if (ata_poll_drq(io, 1000000) != 0) {
                return -1;
            }

            const uint16_t *wptr = (const uint16_t *)(src + (size_t)s * ATA_SECTOR_SIZE);
            for (int w = 0; w < 256; w++) {
                outw(io + ATA_REG_DATA, wptr[w]);
            }

            if (ata_poll_not_busy(io, 2000000) != 0) {
                return -1;
            }
        }

        src   += (size_t)chunk * ATA_SECTOR_SIZE;
        lba   += chunk;
        count -= chunk;
    }

    if (ata_flush_cache_slot(slot) != 0) {
        return -1;
    }

    return 0;
}

int ata_read_sectors(uint64_t lba, uint32_t count, void *buffer) {
    return ata_read_sectors_slot(g_active_slot, lba, count, buffer);
}

int ata_write_sectors(uint64_t lba, uint32_t count, const void *buffer) {
    return ata_write_sectors_slot(g_active_slot, lba, count, buffer);
}

int ata_flush_cache_ext(void) {
    return ata_flush_cache_slot(g_active_slot);
}
