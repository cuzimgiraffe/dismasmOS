#include "ntfs.h"
#include "ata.h"
#include "vga.h"
#include "string.h"
#include "heap.h"

/* Compile-time verification of packed on-disk NTFS 3.1 structure sizes */
_Static_assert(sizeof(struct ntfs_vbr) == 512, "NTFS VBR must be strictly 512 bytes");
_Static_assert(sizeof(struct ntfs_mft_record_header) == 48, "MFT Record Header must be 48 bytes");
_Static_assert(sizeof(struct ntfs_attr_resident) == 24, "Resident Attribute Header must be 24 bytes");
_Static_assert(sizeof(struct ntfs_attr_nonresident) == 64, "Non-Resident Attribute Header must be 64 bytes");

#define ALIGN8(x) (((x) + 7ULL) & ~7ULL)

/* Fixed metadata timestamp (2026-10-25 UTC in 100ns ticks since 1601-01-01) */
#define NTFS_DEFAULT_FILETIME 0x01DC43E080000000ULL

/* Static state of the active mounted NTFS volume */
static struct ntfs_volume_info g_vol;
static struct ntfs_extent      g_mft_extents[NTFS_MAX_EXTENTS];
static int                     g_mft_extent_count = 0;

/* Static BSS scratch buffers (preserves the 32 KiB kernel stack) */
static uint8_t g_sector_buf[ATA_SECTOR_SIZE];
static uint8_t g_zero_sector[ATA_SECTOR_SIZE];

/* MBR Partition Entry (fallback for partitioned disk images) */
struct mbr_part_entry {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed));

/* ============================================================================
 * Freestanding Memory, Hex UUID, & UTF-16LE Helpers
 * ============================================================================ */

static int ntfs_memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (int)p1[i] - (int)p2[i];
        }
    }
    return 0;
}

static void ntfs_memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) {
        return;
    }
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
}

static void ntfs_u64_to_str(uint64_t val, char *buf) {
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

void ntfs_format_uuid_hex(uint64_t serial, char out_hex17[17]) {
    static const char hex_digits[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        uint8_t nib = (uint8_t)((serial >> ((15 - i) * 4)) & 0x0FULL);
        out_hex17[i] = hex_digits[nib];
    }
    out_hex17[16] = '\0';
}

static uint16_t ntfs_u16_toupper(uint16_t c) {
    if (c >= 'a' && c <= 'z') {
        return (uint16_t)(c - ('a' - 'A'));
    }
    return c;
}

static inline uint16_t ntfs_read_u16le(const void *ptr, size_t index) {
    const uint8_t *p = (const uint8_t *)ptr + index * 2U;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void ntfs_utf16_to_ascii(const void *u16_raw, size_t u16_len, char *out, size_t max_out) {
    if (max_out == 0) {
        return;
    }
    size_t limit = (u16_len < (max_out - 1)) ? u16_len : (max_out - 1);
    for (size_t i = 0; i < limit; i++) {
        uint16_t ch = ntfs_read_u16le(u16_raw, i);
        out[i] = (ch < 0x80) ? (char)ch : '?';
    }
    out[limit] = '\0';
}

static size_t ntfs_ascii_to_utf16(const char *ascii, uint16_t *u16, size_t max_chars) {
    size_t i = 0;
    while (ascii[i] != '\0' && i < max_chars) {
        u16[i] = (uint16_t)(uint8_t)ascii[i];
        i++;
    }
    return i;
}

static int ntfs_name_equals_utf16(const char *ascii, const void *u16_raw, size_t u16_len) {
    size_t alen = strlen(ascii);
    if (alen != u16_len) {
        return 0;
    }
    for (size_t i = 0; i < alen; i++) {
        uint16_t ca = ntfs_u16_toupper((uint16_t)(uint8_t)ascii[i]);
        uint16_t cb = ntfs_u16_toupper(ntfs_read_u16le(u16_raw, i));
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

/* Case-insensitive UTF-16LE comparator for $I30 B-Tree sort order */
static int ntfs_cmp_utf16_ci(const void *a_raw, size_t alen, const void *b_raw, size_t blen) {
    size_t min_len = (alen < blen) ? alen : blen;
    for (size_t i = 0; i < min_len; i++) {
        uint16_t ca = ntfs_u16_toupper(ntfs_read_u16le(a_raw, i));
        uint16_t cb = ntfs_u16_toupper(ntfs_read_u16le(b_raw, i));
        if (ca != cb) {
            return (ca < cb) ? -1 : 1;
        }
    }
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

/* Strip optional path prefixes such as "/" or "/ntfs/" */
static const char *ntfs_normalize_filename(const char *path) {
    if (!path) {
        return "";
    }
    if (strncmp(path, "/ntfs/", 6) == 0) {
        path += 6;
    }
    while (*path == '/') {
        path++;
    }
    return path;
}

static int ntfs_attr_name_is_i30(const struct ntfs_attr_header *attr) {
    if (attr->name_length != 4) {
        return 0;
    }
    const uint16_t *n = (const uint16_t *)((const uint8_t *)attr + attr->name_offset);
    return (n[0] == '$' && n[1] == 'I' && n[2] == '3' && n[3] == '0');
}

/* ============================================================================
 * Multi-Sector Update Sequence Array (USA) Fixup Logic (Read & Write)
 * ============================================================================ */

static int ntfs_usa_unfixup(uint8_t *buf, uint32_t size, uint16_t usa_offset, uint16_t usa_count) {
    uint32_t sectors = size / ATA_SECTOR_SIZE;
    if (sectors == 0 || usa_count != (uint16_t)(sectors + 1U)) {
        return -1;
    }
    if ((uint32_t)usa_offset + (uint32_t)usa_count * 2U > size) {
        return -1;
    }

    uint16_t *usa = (uint16_t *)(buf + usa_offset);
    uint16_t usn = usa[0];

    for (uint32_t s = 1; s <= sectors; s++) {
        uint16_t *sec_tail = (uint16_t *)(buf + s * ATA_SECTOR_SIZE - 2U);
        if (*sec_tail != usn) {
            return -1; /* Torn write or corrupted record */
        }
        *sec_tail = usa[s];
    }
    return 0;
}

static int ntfs_usa_apply_fixup(uint8_t *buf, uint32_t size, uint16_t usa_offset, uint16_t usa_count) {
    uint32_t sectors = size / ATA_SECTOR_SIZE;
    if (sectors == 0 || usa_count != (uint16_t)(sectors + 1U)) {
        return -1;
    }
    if ((uint32_t)usa_offset + (uint32_t)usa_count * 2U > size) {
        return -1;
    }

    uint16_t *usa = (uint16_t *)(buf + usa_offset);
    uint16_t usn = (uint16_t)(usa[0] + 1U);
    if (usn == 0x0000U || usn == 0xFFFFU) {
        usn = 0x0001U;
    }
    usa[0] = usn;

    for (uint32_t s = 1; s <= sectors; s++) {
        uint16_t *sec_tail = (uint16_t *)(buf + s * ATA_SECTOR_SIZE - 2U);
        usa[s] = *sec_tail;
        *sec_tail = usn;
    }
    return 0;
}

/* ============================================================================
 * Runlist Decoding & Encoding (Non-Resident Extents)
 * ============================================================================ */

int ntfs_decode_runlist(const uint8_t *runlist, size_t max_bytes,
                        struct ntfs_extent *extents, int max_extents) {
    if (!runlist || !extents || max_extents <= 0) {
        return -1;
    }

    size_t pos = 0;
    int count = 0;
    int64_t current_lcn = 0;

    while (pos < max_bytes) {
        uint8_t hdr = runlist[pos++];
        if (hdr == 0x00) {
            break;
        }

        uint8_t len_bytes = hdr & 0x0F;
        uint8_t off_bytes = (hdr >> 4) & 0x0F;

        if (len_bytes == 0 || len_bytes > 8 || off_bytes > 8) {
            return -1;
        }
        if (pos + len_bytes + off_bytes > max_bytes) {
            return -1;
        }
        if (off_bytes == 0) {
            return -1; /* Sparse run not physically backed */
        }
        if (count >= max_extents) {
            return -1;
        }

        uint64_t run_len = 0;
        for (uint8_t i = 0; i < len_bytes; i++) {
            run_len |= ((uint64_t)runlist[pos++]) << (i * 8);
        }

        uint64_t raw_off = 0;
        for (uint8_t i = 0; i < off_bytes; i++) {
            raw_off |= ((uint64_t)runlist[pos++]) << (i * 8);
        }
        if (off_bytes < 8 && (runlist[pos - 1] & 0x80)) {
            raw_off |= (~0ULL) << (off_bytes * 8);
        }

        int64_t lcn_delta = (int64_t)raw_off;
        current_lcn += lcn_delta;
        if (current_lcn < 0) {
            return -1;
        }

        extents[count].lcn = (uint64_t)current_lcn;
        extents[count].cluster_count = run_len;
        count++;
    }

    return count;
}

static int ntfs_encode_u64(uint64_t val, uint8_t out[8]) {
    int len = 0;
    do {
        out[len++] = (uint8_t)(val & 0xFF);
        val >>= 8;
    } while (val > 0 && len < 8);
    return len;
}

static int ntfs_encode_s64(int64_t val, uint8_t out[8]) {
    int len = 0;
    int more = 1;
    while (more && len < 8) {
        uint8_t byte = (uint8_t)(val & 0xFF);
        out[len++] = byte;
        val >>= 8;
        if ((val == 0 && (byte & 0x80) == 0) ||
            (val == -1 && (byte & 0x80) != 0)) {
            more = 0;
        }
    }
    return len;
}

int ntfs_encode_runlist(const struct ntfs_extent *extents, int extent_count,
                        uint8_t *out_buf, size_t max_bytes) {
    if (!out_buf || max_bytes == 0 || extent_count < 0) {
        return -1;
    }

    size_t pos = 0;
    int64_t prev_lcn = 0;

    for (int i = 0; i < extent_count; i++) {
        if (extents[i].cluster_count == 0) {
            continue;
        }
        uint8_t len_enc[8];
        uint8_t off_enc[8];

        int len_b = ntfs_encode_u64(extents[i].cluster_count, len_enc);
        int64_t delta = (int64_t)extents[i].lcn - prev_lcn;
        int off_b = ntfs_encode_s64(delta, off_enc);
        prev_lcn = (int64_t)extents[i].lcn;

        if (pos + 1 + (size_t)len_b + (size_t)off_b + 1 > max_bytes) {
            return -1;
        }

        out_buf[pos++] = (uint8_t)(((off_b & 0x0F) << 4) | (len_b & 0x0F));
        for (int k = 0; k < len_b; k++) {
            out_buf[pos++] = len_enc[k];
        }
        for (int k = 0; k < off_b; k++) {
            out_buf[pos++] = off_enc[k];
        }
    }

    if (pos >= max_bytes) {
        return -1;
    }
    out_buf[pos++] = 0x00;
    return (int)pos;
}

static inline uint64_t ntfs_lcn_to_lba(uint64_t lcn) {
    return g_vol.part_start_lba + lcn * (uint64_t)g_vol.sectors_per_cluster;
}

static int ntfs_vcn_to_lcn(const struct ntfs_extent *extents, int ext_count,
                           uint64_t target_vcn, uint64_t *out_lcn) {
    uint64_t cur_vcn = 0;
    for (int i = 0; i < ext_count; i++) {
        if (target_vcn >= cur_vcn && target_vcn < cur_vcn + extents[i].cluster_count) {
            *out_lcn = extents[i].lcn + (target_vcn - cur_vcn);
            return 0;
        }
        cur_vcn += extents[i].cluster_count;
    }
    return -1;
}

/* ============================================================================
 * Attribute Search Within a 1024-Byte MFT Record
 * ============================================================================ */

static struct ntfs_attr_header *ntfs_find_attr(uint8_t *mft_rec, uint32_t type, int require_unnamed, int require_i30) {
    struct ntfs_mft_record_header *hdr = (struct ntfs_mft_record_header *)mft_rec;
    uint32_t off = hdr->attrs_offset;
    uint32_t limit = hdr->bytes_in_use;
    if (limit > g_vol.mft_record_size) {
        limit = g_vol.mft_record_size;
    }

    while (off + sizeof(struct ntfs_attr_header) <= limit) {
        struct ntfs_attr_header *attr = (struct ntfs_attr_header *)(mft_rec + off);
        if (attr->type == NTFS_ATTR_END) {
            break;
        }
        if (attr->length == 0 || off + attr->length > limit) {
            break;
        }
        if (attr->type == type) {
            if (require_unnamed && attr->name_length != 0) {
                off += attr->length;
                continue;
            }
            if (require_i30 && !ntfs_attr_name_is_i30(attr)) {
                off += attr->length;
                continue;
            }
            return attr;
        }
        off += attr->length;
    }
    return NULL;
}

/* ============================================================================
 * MFT Record Read & Write (with USA Fixups & $MFTMirr Synchronization)
 * ============================================================================ */

static int ntfs_mft_record_lba(uint64_t record_num, uint64_t *out_lba) {
    uint64_t byte_off = record_num * (uint64_t)g_vol.mft_record_size;

    if (g_mft_extent_count == 0) {
        /* Bootstrap load of Record 0 ($MFT itself) directly via mft_start_lba */
        *out_lba = g_vol.mft_start_lba + (byte_off / ATA_SECTOR_SIZE);
        return 0;
    }

    uint64_t vcn = byte_off / (uint64_t)g_vol.bytes_per_cluster;
    uint64_t in_cluster_off = byte_off % (uint64_t)g_vol.bytes_per_cluster;
    uint64_t lcn = 0;

    if (ntfs_vcn_to_lcn(g_mft_extents, g_mft_extent_count, vcn, &lcn) != 0) {
        return -1;
    }

    *out_lba = ntfs_lcn_to_lba(lcn) + (in_cluster_off / ATA_SECTOR_SIZE);
    return 0;
}

int ntfs_read_mft_record(uint64_t record_num, void *buf_out) {
    if (!buf_out || g_vol.mft_record_size == 0 || g_vol.mft_record_size > NTFS_MAX_MFT_RECORD_SIZE) {
        return -1;
    }

    uint64_t lba = 0;
    if (ntfs_mft_record_lba(record_num, &lba) != 0) {
        return -1;
    }

    uint32_t secs = g_vol.mft_record_size / ATA_SECTOR_SIZE;
    if (ata_read_sectors(lba, secs, buf_out) != 0) {
        return -1;
    }

    uint8_t *raw = (uint8_t *)buf_out;
    struct ntfs_mft_record_header *hdr = (struct ntfs_mft_record_header *)raw;
    if (ntfs_memcmp(hdr->magic, NTFS_MFT_MAGIC, 4) != 0) {
        return -1;
    }

    if (ntfs_usa_unfixup(raw, g_vol.mft_record_size, hdr->usa_offset, hdr->usa_count) != 0) {
        return -1;
    }

    return 0;
}

int ntfs_write_mft_record(uint64_t record_num, const void *buf_in) {
    if (!g_vol.mounted || !buf_in || g_vol.mft_record_size > NTFS_MAX_MFT_RECORD_SIZE) {
        return -1;
    }

    uint8_t disk_rec[NTFS_MAX_MFT_RECORD_SIZE];
    memcpy(disk_rec, buf_in, g_vol.mft_record_size);

    struct ntfs_mft_record_header *hdr = (struct ntfs_mft_record_header *)disk_rec;
    if (ntfs_memcmp(hdr->magic, NTFS_MFT_MAGIC, 4) != 0) {
        return -1;
    }

    if (ntfs_usa_apply_fixup(disk_rec, g_vol.mft_record_size, hdr->usa_offset, hdr->usa_count) != 0) {
        return -1;
    }

    uint64_t lba = 0;
    if (ntfs_mft_record_lba(record_num, &lba) != 0) {
        return -1;
    }

    uint32_t secs = g_vol.mft_record_size / ATA_SECTOR_SIZE;
    if (ata_write_sectors(lba, secs, disk_rec) != 0) {
        return -1;
    }

    /* Mirror system records 0..3 into $MFTMirr for NTFS consistency */
    if (record_num < 4ULL && g_vol.mft_mirr_lcn > 0) {
        uint64_t mirr_lba = ntfs_lcn_to_lba(g_vol.mft_mirr_lcn) + record_num * (uint64_t)secs;
        (void)ata_write_sectors(mirr_lba, secs, disk_rec);
    }

    return 0;
}

static int ntfs_reload_mft_runs(void) {
    uint8_t rec0[NTFS_MAX_MFT_RECORD_SIZE];
    g_mft_extent_count = 0;

    if (ntfs_read_mft_record(NTFS_MFT_REC_MFT, rec0) != 0) {
        return -1;
    }

    struct ntfs_attr_header *data_attr = ntfs_find_attr(rec0, NTFS_ATTR_DATA, 1, 0);
    if (!data_attr || !data_attr->non_resident) {
        return -1;
    }

    struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)data_attr;
    const uint8_t *rl = (const uint8_t *)data_attr + nr->runlist_offset;
    size_t max_rl = data_attr->length - nr->runlist_offset;

    int ext_cnt = ntfs_decode_runlist(rl, max_rl, g_mft_extents, NTFS_MAX_EXTENTS);
    if (ext_cnt <= 0) {
        return -1;
    }

    g_mft_extent_count = ext_cnt;
    g_vol.mft_total_records = nr->real_size / (uint64_t)g_vol.mft_record_size;
    return 0;
}

/* ============================================================================
 * $Volume Label Read & Write (MFT Record 3, $VOLUME_NAME Attribute 0x60)
 * ============================================================================ */

int ntfs_get_volume_label(char *out, size_t max_len) {
    if (!out || max_len == 0) {
        return -1;
    }
    out[0] = '\0';

    uint8_t vol_rec[NTFS_MAX_MFT_RECORD_SIZE];
    if (ntfs_read_mft_record(NTFS_MFT_REC_VOLUME, vol_rec) != 0) {
        return -1;
    }

    struct ntfs_attr_header *vn_attr = ntfs_find_attr(vol_rec, NTFS_ATTR_VOLUME_NAME, 1, 0);
    if (!vn_attr || vn_attr->non_resident) {
        return 0;
    }

    struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)vn_attr;
    size_t u16_chars = res->value_length / 2U;
    ntfs_utf16_to_ascii((const uint8_t *)vn_attr + res->value_offset, u16_chars, out, max_len);
    return 0;
}

int ntfs_set_volume_label(const char *label) {
    if (!g_vol.mounted || !label) {
        return -1;
    }

    uint16_t u16_buf[32];
    size_t u16_len = ntfs_ascii_to_utf16(label, u16_buf, 31);
    uint32_t val_bytes = (uint32_t)(u16_len * 2U);
    uint32_t new_attr_len = (uint32_t)ALIGN8(24ULL + val_bytes);

    uint8_t vol_rec[NTFS_MAX_MFT_RECORD_SIZE];
    if (ntfs_read_mft_record(NTFS_MFT_REC_VOLUME, vol_rec) != 0) {
        return -1;
    }

    struct ntfs_mft_record_header *mhdr = (struct ntfs_mft_record_header *)vol_rec;
    struct ntfs_attr_header *vn_attr = ntfs_find_attr(vol_rec, NTFS_ATTR_VOLUME_NAME, 1, 0);

    if (vn_attr && !vn_attr->non_resident) {
        uint32_t off = (uint32_t)((uint8_t *)vn_attr - vol_rec);
        uint32_t old_len = vn_attr->length;
        if (mhdr->bytes_in_use - old_len + new_attr_len > mhdr->bytes_allocated) {
            return -1;
        }
        uint32_t tail = mhdr->bytes_in_use - (off + old_len);
        ntfs_memmove(vol_rec + off + new_attr_len, vol_rec + off + old_len, tail);

        struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)(vol_rec + off);
        memset(vol_rec + off + 24U, 0, new_attr_len - 24U);
        if (val_bytes > 0) {
            memcpy(vol_rec + off + 24U, u16_buf, val_bytes);
        }
        res->common.length = new_attr_len;
        res->value_length  = val_bytes;
        res->value_offset  = 24U;
        mhdr->bytes_in_use = mhdr->bytes_in_use - old_len + new_attr_len;
    } else {
        /* Insert 0x60 ($VOLUME_NAME) in ascending order before first attr > 0x60 */
        if (mhdr->bytes_in_use + new_attr_len > mhdr->bytes_allocated) {
            return -1;
        }
        uint32_t ins_off = mhdr->attrs_offset;
        while (ins_off + sizeof(struct ntfs_attr_header) <= mhdr->bytes_in_use) {
            struct ntfs_attr_header *a = (struct ntfs_attr_header *)(vol_rec + ins_off);
            if (a->type == NTFS_ATTR_END || a->type > NTFS_ATTR_VOLUME_NAME || a->length == 0) {
                break;
            }
            ins_off += a->length;
        }
        uint32_t tail = mhdr->bytes_in_use - ins_off;
        ntfs_memmove(vol_rec + ins_off + new_attr_len, vol_rec + ins_off, tail);
        memset(vol_rec + ins_off, 0, new_attr_len);

        struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)(vol_rec + ins_off);
        res->common.type         = NTFS_ATTR_VOLUME_NAME;
        res->common.length       = new_attr_len;
        res->common.non_resident = 0;
        res->common.attr_id      = mhdr->next_attr_id++;
        res->value_length        = val_bytes;
        res->value_offset        = 24U;
        if (val_bytes > 0) {
            memcpy(vol_rec + ins_off + 24U, u16_buf, val_bytes);
        }
        mhdr->bytes_in_use += new_attr_len;
    }

    if (ntfs_write_mft_record(NTFS_MFT_REC_VOLUME, vol_rec) != 0) {
        return -1;
    }

    strncpy(g_vol.volume_label, label, sizeof(g_vol.volume_label) - 1);
    g_vol.volume_label[sizeof(g_vol.volume_label) - 1] = '\0';
    return 0;
}

/* ============================================================================
 * 1. NTFS Initialization & VBR Parsing: ntfs_init_slot() / ntfs_init()
 * ============================================================================ */

int ntfs_init_slot(int slot, int quiet) {
    memset(&g_vol, 0, sizeof(g_vol));
    memset(g_zero_sector, 0, sizeof(g_zero_sector));
    g_mft_extent_count = 0;

    if (ata_select_drive(slot) != 0) {
        return -1;
    }

    if (ata_read_sectors(0, 1, g_sector_buf) != 0) {
        if (!quiet) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_puts("[FAIL] NTFS: Failed to read Sector 0.\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
        return -1;
    }

    uint64_t part_lba = 0;
    struct ntfs_vbr *vbr = (struct ntfs_vbr *)g_sector_buf;

    if (ntfs_memcmp(vbr->oem_id, NTFS_OEM_ID, 8) != 0) {
        /* Check if an MBR partition table with partition type 0x07 (NTFS) exists */
        uint16_t mbr_sig = *(uint16_t *)(g_sector_buf + 0x1FE);
        if (mbr_sig == 0xAA55) {
            struct mbr_part_entry *parts = (struct mbr_part_entry *)(g_sector_buf + 0x1BE);
            for (int i = 0; i < 4; i++) {
                if (parts[i].type == 0x07 && parts[i].lba_start > 0) {
                    part_lba = (uint64_t)parts[i].lba_start;
                    break;
                }
            }
        }
        if (part_lba == 0 || ata_read_sectors(part_lba, 1, g_sector_buf) != 0) {
            if (!quiet) {
                vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                vga_puts("[WARN] NTFS: No valid NTFS volume found on disk.\n");
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            }
            return -1;
        }
        vbr = (struct ntfs_vbr *)g_sector_buf;
    }

    if (ntfs_memcmp(vbr->oem_id, NTFS_OEM_ID, 8) != 0 ||
        vbr->boot_signature != NTFS_BOOT_SIGNATURE ||
        vbr->bytes_per_sector != ATA_SECTOR_SIZE) {
        if (!quiet) {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            vga_puts("[WARN] NTFS: Invalid Volume Boot Record (VBR) or sector size != 512B.\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
        return -1;
    }

    uint32_t spc = 0;
    if (vbr->sectors_per_cluster <= 0x80U) {
        spc = vbr->sectors_per_cluster;
    } else {
        uint32_t shift = 256U - (uint32_t)vbr->sectors_per_cluster;
        if (shift < 31U) {
            spc = (1U << shift) / vbr->bytes_per_sector;
        }
    }
    if (spc == 0) {
        return -1;
    }

    uint32_t bpc = (uint32_t)vbr->bytes_per_sector * spc;

    uint32_t mft_rec_sz = 0;
    if (vbr->clusters_per_mft_record < 0) {
        int shift = -vbr->clusters_per_mft_record;
        if (shift >= 9 && shift <= 12) {
            mft_rec_sz = 1U << shift;
        }
    } else if (vbr->clusters_per_mft_record > 0) {
        mft_rec_sz = (uint32_t)vbr->clusters_per_mft_record * bpc;
    }
    if (mft_rec_sz == 0 || mft_rec_sz > NTFS_MAX_MFT_RECORD_SIZE) {
        return -1;
    }

    uint32_t idx_blk_sz = 4096U;
    if (vbr->clusters_per_index_block < 0) {
        int shift = -vbr->clusters_per_index_block;
        if (shift >= 9 && shift <= 16) {
            idx_blk_sz = 1U << shift;
        }
    } else if (vbr->clusters_per_index_block > 0) {
        idx_blk_sz = (uint32_t)vbr->clusters_per_index_block * bpc;
    }

    g_vol.slot                = slot;
    g_vol.part_start_lba      = part_lba;
    g_vol.bytes_per_sector    = vbr->bytes_per_sector;
    g_vol.sectors_per_cluster = spc;
    g_vol.bytes_per_cluster   = bpc;
    g_vol.mft_record_size     = mft_rec_sz;
    g_vol.index_block_size    = idx_blk_sz;
    g_vol.total_sectors       = vbr->total_sectors;
    g_vol.total_clusters      = vbr->total_sectors / (uint64_t)spc;
    g_vol.mft_lcn             = vbr->mft_lcn;
    g_vol.mft_mirr_lcn        = vbr->mft_mirr_lcn;
    g_vol.mft_start_lba       = part_lba + vbr->mft_lcn * (uint64_t)spc;
    g_vol.volume_serial       = vbr->volume_serial_number;
    ntfs_format_uuid_hex(g_vol.volume_serial, g_vol.uuid_str);

    if (ntfs_reload_mft_runs() != 0) {
        if (!quiet) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_puts("[FAIL] NTFS: Failed to load Master File Table ($MFT)!\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
        return -1;
    }

    g_vol.mounted = 1;
    (void)ntfs_get_volume_label(g_vol.volume_label, sizeof(g_vol.volume_label));

    if (!quiet) {
        char nbuf[24];
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("[  ");
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_puts("OK");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("  ] NTFS mounted: LABEL=\"");
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_puts(g_vol.volume_label[0] ? g_vol.volume_label : "UNLABELED");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("\" UUID=");
        vga_puts(g_vol.uuid_str);
        vga_puts(" (");
        ntfs_u64_to_str(g_vol.bytes_per_cluster, nbuf);
        vga_puts(nbuf);
        vga_puts(" B/cluster)\n");
    }

    return 0;
}

int ntfs_init(void) {
    int slot = ata_get_active_slot();
    if (slot < 0) {
        return -1;
    }
    return ntfs_init_slot(slot, 0);
}

int ntfs_is_mounted(void) {
    return (int)g_vol.mounted;
}

const struct ntfs_volume_info *ntfs_get_volume_info(void) {
    return &g_vol;
}

/* ============================================================================
 * Cluster Allocation & Deallocation via $Bitmap (MFT Record 6)
 * ============================================================================ */

static int ntfs_zero_clusters(uint64_t start_lcn, uint64_t cluster_count) {
    uint64_t start_lba = ntfs_lcn_to_lba(start_lcn);
    uint64_t total_secs = cluster_count * (uint64_t)g_vol.sectors_per_cluster;
    for (uint64_t s = 0; s < total_secs; s++) {
        if (ata_write_sectors(start_lba + s, 1, g_zero_sector) != 0) {
            return -1;
        }
    }
    return 0;
}

static int ntfs_bitmap_alloc_clusters(uint64_t needed, uint64_t preferred_lcn, uint64_t *out_lcn) {
    if (needed == 0 || !out_lcn) {
        return -1;
    }

    uint8_t bm_rec[NTFS_MAX_MFT_RECORD_SIZE];
    if (ntfs_read_mft_record(NTFS_MFT_REC_BITMAP, bm_rec) != 0) {
        return -1;
    }

    struct ntfs_attr_header *data_attr = ntfs_find_attr(bm_rec, NTFS_ATTR_DATA, 1, 0);
    if (!data_attr) {
        return -1;
    }

    /* Case A: Resident $Bitmap $DATA (tiny volumes) */
    if (!data_attr->non_resident) {
        struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)data_attr;
        uint8_t *bm = (uint8_t *)data_attr + res->value_offset;
        uint64_t max_bits = (uint64_t)res->value_length * 8ULL;
        if (max_bits > g_vol.total_clusters) {
            max_bits = g_vol.total_clusters;
        }

        uint64_t run_start = 0;
        uint64_t run_len = 0;
        for (uint64_t bit = 2; bit < max_bits; bit++) {
            int used = (bm[bit >> 3] >> (bit & 7)) & 1;
            if (!used) {
                if (run_len == 0) run_start = bit;
                run_len++;
                if (run_len == needed) {
                    for (uint64_t b = run_start; b < run_start + needed; b++) {
                        bm[b >> 3] |= (uint8_t)(1U << (b & 7));
                    }
                    if (ntfs_write_mft_record(NTFS_MFT_REC_BITMAP, bm_rec) != 0) {
                        return -1;
                    }
                    if (ntfs_zero_clusters(run_start, needed) != 0) {
                        return -1;
                    }
                    *out_lcn = run_start;
                    return 0;
                }
            } else {
                run_len = 0;
            }
        }
        return -1;
    }

    /* Case B: Standard Non-Resident $Bitmap $DATA */
    struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)data_attr;
    struct ntfs_extent bm_exts[NTFS_MAX_EXTENTS];
    int bm_ext_cnt = ntfs_decode_runlist((const uint8_t *)data_attr + nr->runlist_offset,
                                         data_attr->length - nr->runlist_offset,
                                         bm_exts, NTFS_MAX_EXTENTS);
    if (bm_ext_cnt <= 0) {
        return -1;
    }

    uint64_t total_bm_bytes = nr->real_size;
    size_t alloc_sz = (size_t)ALIGN8(total_bm_bytes + ATA_SECTOR_SIZE);
    if (alloc_sz > 262144U) {
        alloc_sz = 262144U; /* Covers > 2.1M clusters (8 GiB @ 4 KiB) per pass */
    }

    uint8_t *bm_buf = (uint8_t *)malloc(alloc_sz);
    if (!bm_buf) {
        return -1;
    }

    uint32_t secs_to_load = (uint32_t)((total_bm_bytes + ATA_SECTOR_SIZE - 1ULL) / ATA_SECTOR_SIZE);
    if ((size_t)secs_to_load * ATA_SECTOR_SIZE > alloc_sz) {
        secs_to_load = (uint32_t)(alloc_sz / ATA_SECTOR_SIZE);
    }

    for (uint32_t s = 0; s < secs_to_load; s++) {
        uint64_t byte_pos = (uint64_t)s * ATA_SECTOR_SIZE;
        uint64_t vcn = byte_pos / g_vol.bytes_per_cluster;
        uint64_t off_in_cl = byte_pos % g_vol.bytes_per_cluster;
        uint64_t lcn = 0;
        if (ntfs_vcn_to_lcn(bm_exts, bm_ext_cnt, vcn, &lcn) != 0) {
            free(bm_buf);
            return -1;
        }
        uint64_t sec_lba = ntfs_lcn_to_lba(lcn) + (off_in_cl / ATA_SECTOR_SIZE);
        if (ata_read_sectors(sec_lba, 1, bm_buf + (size_t)s * ATA_SECTOR_SIZE) != 0) {
            free(bm_buf);
            return -1;
        }
    }

    uint64_t max_clusters = (uint64_t)secs_to_load * ATA_SECTOR_SIZE * 8ULL;
    if (max_clusters > g_vol.total_clusters) {
        max_clusters = g_vol.total_clusters;
    }

    uint64_t chosen = 0;
    int found = 0;

    /* 1. Try contiguous allocation immediately after preferred_lcn */
    if (preferred_lcn >= 2 && preferred_lcn + needed <= max_clusters) {
        int ok = 1;
        for (uint64_t b = preferred_lcn; b < preferred_lcn + needed; b++) {
            if ((bm_buf[b >> 3] >> (b & 7)) & 1) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            chosen = preferred_lcn;
            found = 1;
        }
    }

    /* 2. Search for any contiguous free cluster range */
    if (!found) {
        uint64_t start_search = (g_vol.mft_lcn + 16ULL < max_clusters) ? (g_vol.mft_lcn + 16ULL) : 2ULL;
        for (int pass = 0; pass < 2 && !found; pass++) {
            uint64_t from = (pass == 0) ? start_search : 2ULL;
            uint64_t to   = (pass == 0) ? max_clusters : start_search;
            uint64_t run_s = 0;
            uint64_t run_l = 0;
            for (uint64_t b = from; b < to; b++) {
                if (!((bm_buf[b >> 3] >> (b & 7)) & 1)) {
                    if (run_l == 0) run_s = b;
                    run_l++;
                    if (run_l == needed) {
                        chosen = run_s;
                        found = 1;
                        break;
                    }
                } else {
                    run_l = 0;
                }
            }
        }
    }

    if (!found) {
        free(bm_buf);
        return -1;
    }

    for (uint64_t b = chosen; b < chosen + needed; b++) {
        bm_buf[b >> 3] |= (uint8_t)(1U << (b & 7));
    }

    uint32_t first_sec = (uint32_t)((chosen >> 3) / ATA_SECTOR_SIZE);
    uint32_t last_sec  = (uint32_t)(((chosen + needed - 1ULL) >> 3) / ATA_SECTOR_SIZE);
    for (uint32_t s = first_sec; s <= last_sec; s++) {
        uint64_t byte_pos = (uint64_t)s * ATA_SECTOR_SIZE;
        uint64_t vcn = byte_pos / g_vol.bytes_per_cluster;
        uint64_t off_in_cl = byte_pos % g_vol.bytes_per_cluster;
        uint64_t lcn = 0;
        if (ntfs_vcn_to_lcn(bm_exts, bm_ext_cnt, vcn, &lcn) != 0) {
            free(bm_buf);
            return -1;
        }
        uint64_t sec_lba = ntfs_lcn_to_lba(lcn) + (off_in_cl / ATA_SECTOR_SIZE);
        if (ata_write_sectors(sec_lba, 1, bm_buf + (size_t)s * ATA_SECTOR_SIZE) != 0) {
            free(bm_buf);
            return -1;
        }
    }

    free(bm_buf);

    if (ntfs_zero_clusters(chosen, needed) != 0) {
        return -1;
    }

    *out_lcn = chosen;
    return 0;
}

static int ntfs_bitmap_free_clusters(uint64_t start_lcn, uint64_t count) {
    if (count == 0) {
        return 0;
    }

    uint8_t bm_rec[NTFS_MAX_MFT_RECORD_SIZE];
    if (ntfs_read_mft_record(NTFS_MFT_REC_BITMAP, bm_rec) != 0) {
        return -1;
    }

    struct ntfs_attr_header *data_attr = ntfs_find_attr(bm_rec, NTFS_ATTR_DATA, 1, 0);
    if (!data_attr) {
        return -1;
    }

    if (!data_attr->non_resident) {
        struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)data_attr;
        uint8_t *bm = (uint8_t *)data_attr + res->value_offset;
        for (uint64_t b = start_lcn; b < start_lcn + count; b++) {
            bm[b >> 3] &= (uint8_t)~(1U << (b & 7));
        }
        return ntfs_write_mft_record(NTFS_MFT_REC_BITMAP, bm_rec);
    }

    struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)data_attr;
    struct ntfs_extent bm_exts[NTFS_MAX_EXTENTS];
    int bm_ext_cnt = ntfs_decode_runlist((const uint8_t *)data_attr + nr->runlist_offset,
                                         data_attr->length - nr->runlist_offset,
                                         bm_exts, NTFS_MAX_EXTENTS);
    if (bm_ext_cnt <= 0) {
        return -1;
    }

    uint32_t first_sec = (uint32_t)((start_lcn >> 3) / ATA_SECTOR_SIZE);
    uint32_t last_sec  = (uint32_t)(((start_lcn + count - 1ULL) >> 3) / ATA_SECTOR_SIZE);

    for (uint32_t s = first_sec; s <= last_sec; s++) {
        uint64_t byte_pos = (uint64_t)s * ATA_SECTOR_SIZE;
        uint64_t vcn = byte_pos / g_vol.bytes_per_cluster;
        uint64_t off_in_cl = byte_pos % g_vol.bytes_per_cluster;
        uint64_t lcn = 0;
        if (ntfs_vcn_to_lcn(bm_exts, bm_ext_cnt, vcn, &lcn) != 0) {
            return -1;
        }
        uint64_t sec_lba = ntfs_lcn_to_lba(lcn) + (off_in_cl / ATA_SECTOR_SIZE);
        if (ata_read_sectors(sec_lba, 1, g_sector_buf) != 0) {
            return -1;
        }

        uint64_t sec_bit_start = (uint64_t)s * ATA_SECTOR_SIZE * 8ULL;
        uint64_t sec_bit_end   = sec_bit_start + ATA_SECTOR_SIZE * 8ULL;
        uint64_t b_from = (start_lcn > sec_bit_start) ? start_lcn : sec_bit_start;
        uint64_t b_to   = (start_lcn + count < sec_bit_end) ? (start_lcn + count) : sec_bit_end;

        for (uint64_t b = b_from; b < b_to; b++) {
            uint64_t rel = b - sec_bit_start;
            g_sector_buf[rel >> 3] &= (uint8_t)~(1U << (rel & 7));
        }

        if (ata_write_sectors(sec_lba, 1, g_sector_buf) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ============================================================================
 * Physical Runlist Cluster Data Write
 * ============================================================================ */

static int ntfs_write_data_to_extents(const struct ntfs_extent *extents, int ext_count,
                                      const void *data, size_t size) {
    const uint8_t *src = (const uint8_t *)data;
    size_t remaining = size;
    uint64_t total_clusters = 0;
    for (int i = 0; i < ext_count; i++) {
        total_clusters += extents[i].cluster_count;
    }

    if ((uint64_t)size > total_clusters * (uint64_t)g_vol.bytes_per_cluster) {
        return -1;
    }

    for (int i = 0; i < ext_count && remaining > 0; i++) {
        uint64_t base_lba = ntfs_lcn_to_lba(extents[i].lcn);
        uint64_t total_ext_secs = extents[i].cluster_count * (uint64_t)g_vol.sectors_per_cluster;

        uint64_t full_secs = remaining / ATA_SECTOR_SIZE;
        if (full_secs > total_ext_secs) {
            full_secs = total_ext_secs;
        }

        if (full_secs > 0) {
            if (ata_write_sectors(base_lba, (uint32_t)full_secs, src) != 0) {
                return -1;
            }
            src       += (size_t)full_secs * ATA_SECTOR_SIZE;
            remaining -= (size_t)full_secs * ATA_SECTOR_SIZE;
        }

        if (remaining > 0 && full_secs < total_ext_secs) {
            memset(g_sector_buf, 0, ATA_SECTOR_SIZE);
            memcpy(g_sector_buf, src, remaining);
            if (ata_write_sectors(base_lba + full_secs, 1, g_sector_buf) != 0) {
                return -1;
            }

            uint64_t next_sec = full_secs + 1ULL;
            uint64_t cl_rem = next_sec % (uint64_t)g_vol.sectors_per_cluster;
            if (cl_rem != 0) {
                uint64_t pad_secs = (uint64_t)g_vol.sectors_per_cluster - cl_rem;
                for (uint64_t p = 0; p < pad_secs && (next_sec + p) < total_ext_secs; p++) {
                    if (ata_write_sectors(base_lba + next_sec + p, 1, g_zero_sector) != 0) {
                        return -1;
                    }
                }
            }
            remaining = 0;
        } else if (remaining == 0 && full_secs > 0 && full_secs < total_ext_secs) {
            uint64_t cl_rem = full_secs % (uint64_t)g_vol.sectors_per_cluster;
            if (cl_rem != 0) {
                uint64_t pad_secs = (uint64_t)g_vol.sectors_per_cluster - cl_rem;
                for (uint64_t p = 0; p < pad_secs && (full_secs + p) < total_ext_secs; p++) {
                    if (ata_write_sectors(base_lba + full_secs + p, 1, g_zero_sector) != 0) {
                        return -1;
                    }
                }
            }
        }
    }

    return 0;
}

/* ============================================================================
 * Root Directory ($I30 in MFT Record 5): Size Sync & Sorted Entry Insertion
 * ============================================================================ */

static void ntfs_update_entries_size_in_node(struct ntfs_index_header *ihdr,
                                             uint64_t target_mft_rec,
                                             uint64_t real_sz,
                                             uint64_t alloc_sz,
                                             int *modified) {
    uint32_t off = ihdr->entries_offset;
    while (off + sizeof(struct ntfs_index_entry_header) <= ihdr->index_length) {
        struct ntfs_index_entry_header *ie =
            (struct ntfs_index_entry_header *)((uint8_t *)ihdr + off);
        if (ie->index_flags & NTFS_INDEX_ENTRY_END) {
            break;
        }
        if (ie->entry_length == 0) {
            break;
        }
        if ((ie->mft_reference & 0x0000FFFFFFFFFFFFULL) == target_mft_rec &&
            ie->key_length >= sizeof(struct ntfs_attr_filename)) {
            struct ntfs_attr_filename *fn =
                (struct ntfs_attr_filename *)((uint8_t *)ie + sizeof(struct ntfs_index_entry_header));
            fn->real_size = real_sz;
            fn->allocated_size = alloc_sz;
            *modified = 1;
        }
        off += ie->entry_length;
    }
}

static void ntfs_dir_sync_file_size(uint64_t target_mft_rec, uint64_t real_sz, uint64_t alloc_sz) {
    uint8_t root_rec[NTFS_MAX_MFT_RECORD_SIZE];
    if (ntfs_read_mft_record(NTFS_MFT_REC_ROOT, root_rec) != 0) {
        return;
    }

    struct ntfs_attr_header *ir_attr = ntfs_find_attr(root_rec, NTFS_ATTR_INDEX_ROOT, 0, 1);
    if (ir_attr && !ir_attr->non_resident) {
        struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)ir_attr;
        struct ntfs_index_root *ir = (struct ntfs_index_root *)((uint8_t *)ir_attr + res->value_offset);
        int mod = 0;
        ntfs_update_entries_size_in_node(&ir->index_hdr, target_mft_rec, real_sz, alloc_sz, &mod);
        if (mod) {
            (void)ntfs_write_mft_record(NTFS_MFT_REC_ROOT, root_rec);
            return;
        }
    }

    struct ntfs_attr_header *ia_attr = ntfs_find_attr(root_rec, NTFS_ATTR_INDEX_ALLOCATION, 0, 1);
    if (ia_attr && ia_attr->non_resident) {
        struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)ia_attr;
        struct ntfs_extent exts[NTFS_MAX_EXTENTS];
        int ext_cnt = ntfs_decode_runlist((const uint8_t *)ia_attr + nr->runlist_offset,
                                          ia_attr->length - nr->runlist_offset,
                                          exts, NTFS_MAX_EXTENTS);
        if (ext_cnt <= 0) {
            return;
        }

        uint8_t *indx_buf = (uint8_t *)malloc(g_vol.index_block_size);
        if (!indx_buf) {
            return;
        }

        uint64_t num_blocks = nr->real_size / g_vol.index_block_size;
        uint32_t cl_per_blk = g_vol.index_block_size / g_vol.bytes_per_cluster;
        if (cl_per_blk == 0) cl_per_blk = 1;

        for (uint64_t b = 0; b < num_blocks; b++) {
            uint64_t lcn = 0;
            if (ntfs_vcn_to_lcn(exts, ext_cnt, b * cl_per_blk, &lcn) != 0) {
                continue;
            }
            uint64_t blk_lba = ntfs_lcn_to_lba(lcn);
            uint32_t blk_secs = g_vol.index_block_size / ATA_SECTOR_SIZE;
            if (ata_read_sectors(blk_lba, blk_secs, indx_buf) != 0) {
                continue;
            }
            struct ntfs_indx_block_header *ibh = (struct ntfs_indx_block_header *)indx_buf;
            if (ntfs_memcmp(ibh->magic, NTFS_INDX_MAGIC, 4) != 0) {
                continue;
            }
            if (ntfs_usa_unfixup(indx_buf, g_vol.index_block_size, ibh->usa_offset, ibh->usa_count) != 0) {
                continue;
            }

            int mod = 0;
            ntfs_update_entries_size_in_node(&ibh->index_hdr, target_mft_rec, real_sz, alloc_sz, &mod);
            if (mod) {
                if (ntfs_usa_apply_fixup(indx_buf, g_vol.index_block_size, ibh->usa_offset, ibh->usa_count) == 0) {
                    (void)ata_write_sectors(blk_lba, blk_secs, indx_buf);
                }
                break;
            }
        }
        free(indx_buf);
    }
}

static int ntfs_insert_entry_into_index_hdr(struct ntfs_index_header *ihdr,
                                            const uint8_t *new_entry,
                                            uint16_t new_entry_len,
                                            const uint16_t *new_name,
                                            size_t new_name_len) {
    uint32_t off = ihdr->entries_offset;
    uint32_t insert_off = ihdr->index_length;

    while (off + sizeof(struct ntfs_index_entry_header) <= ihdr->index_length) {
        struct ntfs_index_entry_header *ie =
            (struct ntfs_index_entry_header *)((uint8_t *)ihdr + off);
        if (ie->index_flags & NTFS_INDEX_ENTRY_END) {
            insert_off = off;
            break;
        }
        if (ie->entry_length == 0) {
            return -1;
        }
        const struct ntfs_attr_filename *fn =
            (const struct ntfs_attr_filename *)((const uint8_t *)ie + sizeof(struct ntfs_index_entry_header));
        const uint8_t *fn_name_bytes = (const uint8_t *)fn + sizeof(struct ntfs_attr_filename);
        if (ntfs_cmp_utf16_ci(new_name, new_name_len, fn_name_bytes, fn->name_length) < 0) {
            insert_off = off;
            break;
        }
        off += ie->entry_length;
    }

    uint8_t *base = (uint8_t *)ihdr;
    size_t tail_bytes = ihdr->index_length - insert_off;
    ntfs_memmove(base + insert_off + new_entry_len, base + insert_off, tail_bytes);
    memcpy(base + insert_off, new_entry, new_entry_len);
    ihdr->index_length += new_entry_len;
    return 0;
}

static int ntfs_dir_add_root_entry(uint64_t child_mft_ref,
                                   const uint8_t *fn_payload,
                                   uint16_t fn_payload_len,
                                   const uint16_t *u16_name,
                                   size_t u16_len) {
    uint8_t root_rec[NTFS_MAX_MFT_RECORD_SIZE];
    if (ntfs_read_mft_record(NTFS_MFT_REC_ROOT, root_rec) != 0) {
        return -1;
    }

    struct ntfs_mft_record_header *mhdr = (struct ntfs_mft_record_header *)root_rec;
    struct ntfs_attr_header *ir_attr = ntfs_find_attr(root_rec, NTFS_ATTR_INDEX_ROOT, 0, 1);
    if (!ir_attr || ir_attr->non_resident) {
        return -1;
    }

    uint16_t entry_len = (uint16_t)ALIGN8(sizeof(struct ntfs_index_entry_header) + fn_payload_len);
    uint8_t entry_buf[256];
    if (entry_len > sizeof(entry_buf)) {
        return -1;
    }
    memset(entry_buf, 0, entry_len);

    struct ntfs_index_entry_header *neh = (struct ntfs_index_entry_header *)entry_buf;
    neh->mft_reference = child_mft_ref;
    neh->entry_length  = entry_len;
    neh->key_length    = fn_payload_len;
    neh->index_flags   = 0;
    neh->reserved      = 0;
    memcpy(entry_buf + sizeof(struct ntfs_index_entry_header), fn_payload, fn_payload_len);

    struct ntfs_attr_resident *ir_res = (struct ntfs_attr_resident *)ir_attr;
    struct ntfs_index_root *ir = (struct ntfs_index_root *)((uint8_t *)ir_attr + ir_res->value_offset);

    /* Case 1: Small Directory ($INDEX_ROOT directly in MFT Record 5) */
    if ((ir->index_hdr.flags & NTFS_INDEX_FLAG_LARGE) == 0) {
        if (mhdr->bytes_in_use + entry_len <= mhdr->bytes_allocated) {
            uint32_t attr_end_off = (uint32_t)((uint8_t *)ir_attr - root_rec) + ir_attr->length;
            size_t after_ir_bytes = mhdr->bytes_in_use - attr_end_off;

            ntfs_memmove(root_rec + attr_end_off + entry_len,
                         root_rec + attr_end_off,
                         after_ir_bytes);

            if (ntfs_insert_entry_into_index_hdr(&ir->index_hdr, entry_buf, entry_len, u16_name, u16_len) != 0) {
                return -1;
            }
            ir->index_hdr.allocated_size += entry_len;
            ir_res->value_length         += entry_len;
            ir_attr->length              += entry_len;
            mhdr->bytes_in_use           += entry_len;

            return ntfs_write_mft_record(NTFS_MFT_REC_ROOT, root_rec);
        }

        /* Small Index full -> Promote to Large Index (4096-byte INDX block) */
        uint32_t cl_per_blk = g_vol.index_block_size / g_vol.bytes_per_cluster;
        if (cl_per_blk == 0) cl_per_blk = 1;

        uint64_t idx_lcn = 0;
        if (ntfs_bitmap_alloc_clusters(cl_per_blk, 0, &idx_lcn) != 0) {
            return -1;
        }

        uint8_t *indx_buf = (uint8_t *)malloc(g_vol.index_block_size);
        if (!indx_buf) {
            return -1;
        }
        memset(indx_buf, 0, g_vol.index_block_size);

        uint16_t usa_cnt = (uint16_t)(g_vol.index_block_size / ATA_SECTOR_SIZE + 1U);
        uint32_t entries_rel = (uint32_t)ALIGN8(0x28U + (uint32_t)usa_cnt * 2U) - 0x18U;

        struct ntfs_indx_block_header *ibh = (struct ntfs_indx_block_header *)indx_buf;
        memcpy(ibh->magic, NTFS_INDX_MAGIC, 4);
        ibh->usa_offset                 = 0x28U;
        ibh->usa_count                  = usa_cnt;
        ibh->lsn                        = 0;
        ibh->index_block_vcn            = 0;
        ibh->index_hdr.entries_offset   = entries_rel;
        ibh->index_hdr.allocated_size   = g_vol.index_block_size - 0x18U;
        ibh->index_hdr.flags            = 0;

        uint32_t old_entries_bytes = ir->index_hdr.index_length - ir->index_hdr.entries_offset;
        memcpy((uint8_t *)&ibh->index_hdr + entries_rel,
               (uint8_t *)&ir->index_hdr + ir->index_hdr.entries_offset,
               old_entries_bytes);
        ibh->index_hdr.index_length = entries_rel + old_entries_bytes;

        if (ntfs_insert_entry_into_index_hdr(&ibh->index_hdr, entry_buf, entry_len, u16_name, u16_len) != 0) {
            free(indx_buf);
            return -1;
        }

        if (ntfs_usa_apply_fixup(indx_buf, g_vol.index_block_size, ibh->usa_offset, ibh->usa_count) != 0) {
            free(indx_buf);
            return -1;
        }

        if (ata_write_sectors(ntfs_lcn_to_lba(idx_lcn), g_vol.index_block_size / ATA_SECTOR_SIZE, indx_buf) != 0) {
            free(indx_buf);
            return -1;
        }
        free(indx_buf);

        uint32_t ir_off = (uint32_t)((uint8_t *)ir_attr - root_rec);
        uint32_t new_ir_val_len = sizeof(struct ntfs_index_root) + 24U;
        uint32_t new_ir_len     = ir_res->value_offset + new_ir_val_len;

        struct ntfs_extent ia_ext = { idx_lcn, cl_per_blk };
        uint8_t rl_buf[16];
        int rl_len = ntfs_encode_runlist(&ia_ext, 1, rl_buf, sizeof(rl_buf));
        if (rl_len <= 0) {
            return -1;
        }

        uint32_t ia_len = (uint32_t)ALIGN8(72U + (uint32_t)rl_len);
        uint32_t bm_len = 40U;

        if (ir_off + new_ir_len + ia_len + bm_len + 8U > mhdr->bytes_allocated) {
            return -1;
        }

        ir->index_hdr.entries_offset = 16U;
        ir->index_hdr.index_length   = 16U + 24U;
        ir->index_hdr.allocated_size = 16U + 24U;
        ir->index_hdr.flags          = NTFS_INDEX_FLAG_LARGE;

        uint8_t *root_ie_ptr = (uint8_t *)&ir->index_hdr + 16U;
        memset(root_ie_ptr, 0, 24U);
        struct ntfs_index_entry_header *root_ie = (struct ntfs_index_entry_header *)root_ie_ptr;
        root_ie->entry_length = 24U;
        root_ie->index_flags  = NTFS_INDEX_ENTRY_END | NTFS_INDEX_ENTRY_NODE;
        *(uint64_t *)(root_ie_ptr + 16U) = 0ULL;

        ir_res->value_length = new_ir_val_len;
        ir_attr->length      = new_ir_len;

        uint8_t *ia_ptr = root_rec + ir_off + new_ir_len;
        memset(ia_ptr, 0, ia_len);
        struct ntfs_attr_nonresident *ia_nr = (struct ntfs_attr_nonresident *)ia_ptr;
        ia_nr->common.type           = NTFS_ATTR_INDEX_ALLOCATION;
        ia_nr->common.length         = ia_len;
        ia_nr->common.non_resident   = 1;
        ia_nr->common.name_length    = 4;
        ia_nr->common.name_offset    = 64U;
        ia_nr->common.attr_id        = mhdr->next_attr_id++;
        ia_nr->start_vcn             = 0;
        ia_nr->last_vcn              = cl_per_blk - 1ULL;
        ia_nr->runlist_offset        = 72U;
        ia_nr->allocated_size        = g_vol.index_block_size;
        ia_nr->real_size             = g_vol.index_block_size;
        ia_nr->initialized_size      = g_vol.index_block_size;
        uint16_t *ia_name = (uint16_t *)(ia_ptr + 64U);
        ia_name[0] = '$'; ia_name[1] = 'I'; ia_name[2] = '3'; ia_name[3] = '0';
        memcpy(ia_ptr + 72U, rl_buf, (size_t)rl_len);

        uint8_t *bm_ptr = ia_ptr + ia_len;
        memset(bm_ptr, 0, bm_len);
        struct ntfs_attr_resident *bm_res = (struct ntfs_attr_resident *)bm_ptr;
        bm_res->common.type          = NTFS_ATTR_BITMAP;
        bm_res->common.length        = bm_len;
        bm_res->common.non_resident  = 0;
        bm_res->common.name_length   = 4;
        bm_res->common.name_offset   = 24U;
        bm_res->common.attr_id       = mhdr->next_attr_id++;
        bm_res->value_length         = 8U;
        bm_res->value_offset         = 32U;
        uint16_t *bm_name = (uint16_t *)(bm_ptr + 24U);
        bm_name[0] = '$'; bm_name[1] = 'I'; bm_name[2] = '3'; bm_name[3] = '0';
        bm_ptr[32] = 0x01;

        uint8_t *end_ptr = bm_ptr + bm_len;
        *(uint32_t *)end_ptr = NTFS_ATTR_END;
        *(uint32_t *)(end_ptr + 4) = 0;
        mhdr->bytes_in_use = (uint32_t)(end_ptr - root_rec) + 8U;

        return ntfs_write_mft_record(NTFS_MFT_REC_ROOT, root_rec);
    }

    /* Case 2: Large Directory ($INDEX_ALLOCATION already present) */
    struct ntfs_attr_header *ia_attr = ntfs_find_attr(root_rec, NTFS_ATTR_INDEX_ALLOCATION, 0, 1);
    if (!ia_attr || !ia_attr->non_resident) {
        return -1;
    }

    struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)ia_attr;
    struct ntfs_extent exts[NTFS_MAX_EXTENTS];
    int ext_cnt = ntfs_decode_runlist((const uint8_t *)ia_attr + nr->runlist_offset,
                                      ia_attr->length - nr->runlist_offset,
                                      exts, NTFS_MAX_EXTENTS);
    if (ext_cnt <= 0) {
        return -1;
    }

    uint8_t *indx_buf = (uint8_t *)malloc(g_vol.index_block_size);
    if (!indx_buf) {
        return -1;
    }

    uint64_t num_blocks = nr->real_size / g_vol.index_block_size;
    uint32_t cl_per_blk = g_vol.index_block_size / g_vol.bytes_per_cluster;
    if (cl_per_blk == 0) cl_per_blk = 1;

    int inserted = 0;
    for (uint64_t b = 0; b < num_blocks; b++) {
        uint64_t lcn = 0;
        if (ntfs_vcn_to_lcn(exts, ext_cnt, b * cl_per_blk, &lcn) != 0) {
            continue;
        }
        uint64_t blk_lba = ntfs_lcn_to_lba(lcn);
        uint32_t blk_secs = g_vol.index_block_size / ATA_SECTOR_SIZE;
        if (ata_read_sectors(blk_lba, blk_secs, indx_buf) != 0) {
            continue;
        }
        struct ntfs_indx_block_header *ibh = (struct ntfs_indx_block_header *)indx_buf;
        if (ntfs_memcmp(ibh->magic, NTFS_INDX_MAGIC, 4) != 0) {
            continue;
        }
        if (ntfs_usa_unfixup(indx_buf, g_vol.index_block_size, ibh->usa_offset, ibh->usa_count) != 0) {
            continue;
        }
        if ((ibh->index_hdr.flags & NTFS_INDEX_FLAG_LARGE) != 0) {
            continue;
        }
        if (ibh->index_hdr.index_length + entry_len > ibh->index_hdr.allocated_size) {
            continue;
        }

        if (ntfs_insert_entry_into_index_hdr(&ibh->index_hdr, entry_buf, entry_len, u16_name, u16_len) == 0) {
            if (ntfs_usa_apply_fixup(indx_buf, g_vol.index_block_size, ibh->usa_offset, ibh->usa_count) == 0) {
                if (ata_write_sectors(blk_lba, blk_secs, indx_buf) == 0) {
                    inserted = 1;
                }
            }
            break;
        }
    }

    free(indx_buf);
    return inserted ? 0 : -1;
}

/* ============================================================================
 * 2. File Lookup & Read (Resident & Non-Resident Runlists)
 * ============================================================================ */

static int ntfs_inspect_record(uint64_t rec_num, uint8_t *rec_buf,
                               const char *match_name, struct ntfs_file_info *out_info) {
    struct ntfs_mft_record_header *hdr = (struct ntfs_mft_record_header *)rec_buf;
    if (!(hdr->flags & NTFS_MFT_FLAG_IN_USE)) {
        return -1;
    }

    uint32_t off = hdr->attrs_offset;
    uint32_t limit = hdr->bytes_in_use;
    if (limit > g_vol.mft_record_size) limit = g_vol.mft_record_size;

    const struct ntfs_attr_filename *best_fn = NULL;
    int name_matched = 0;
    uint64_t data_real_sz = 0;
    uint64_t data_alloc_sz = 0;
    uint8_t  data_resident = 1;
    int      has_data = 0;

    while (off + sizeof(struct ntfs_attr_header) <= limit) {
        struct ntfs_attr_header *attr = (struct ntfs_attr_header *)(rec_buf + off);
        if (attr->type == NTFS_ATTR_END || attr->length == 0 || off + attr->length > limit) {
            break;
        }

        if (attr->type == NTFS_ATTR_FILE_NAME && !attr->non_resident) {
            struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)attr;
            const struct ntfs_attr_filename *fn =
                (const struct ntfs_attr_filename *)(rec_buf + off + res->value_offset);
            uint64_t parent_rec = fn->parent_directory & 0x0000FFFFFFFFFFFFULL;

            if (parent_rec == NTFS_MFT_REC_ROOT) {
                if (!best_fn || best_fn->name_namespace == NTFS_NAMESPACE_DOS) {
                    best_fn = fn;
                }
                const uint8_t *fn_name_bytes = (const uint8_t *)fn + sizeof(struct ntfs_attr_filename);
                if (match_name && ntfs_name_equals_utf16(match_name, fn_name_bytes, fn->name_length)) {
                    name_matched = 1;
                    best_fn = fn;
                }
            }
        } else if (attr->type == NTFS_ATTR_DATA && attr->name_length == 0) {
            has_data = 1;
            if (!attr->non_resident) {
                struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)attr;
                data_real_sz  = res->value_length;
                data_alloc_sz = ALIGN8(res->value_length);
                data_resident = 1;
            } else {
                struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)attr;
                data_real_sz  = nr->real_size;
                data_alloc_sz = nr->allocated_size;
                data_resident = 0;
            }
        }

        off += attr->length;
    }

    if (!best_fn) {
        return -1;
    }
    if (match_name && !name_matched) {
        return -1;
    }

    if (out_info) {
        out_info->mft_record     = rec_num;
        const uint8_t *best_name_bytes = (const uint8_t *)best_fn + sizeof(struct ntfs_attr_filename);
        ntfs_utf16_to_ascii(best_name_bytes, best_fn->name_length, out_info->name, sizeof(out_info->name));
        out_info->is_directory   = (hdr->flags & NTFS_MFT_FLAG_IS_DIRECTORY) ? 1 : 0;
        out_info->real_size      = has_data ? data_real_sz : best_fn->real_size;
        out_info->allocated_size = has_data ? data_alloc_sz : best_fn->allocated_size;
        out_info->is_resident    = data_resident;
    }

    return 0;
}

int ntfs_find_file(const char *filename, struct ntfs_file_info *out_info) {
    if (!g_vol.mounted || !filename) {
        return -1;
    }

    const char *clean = ntfs_normalize_filename(filename);
    if (clean[0] == '\0') {
        return -1;
    }

    uint8_t rec_buf[NTFS_MAX_MFT_RECORD_SIZE];
    for (uint64_t r = 0; r < g_vol.mft_total_records; r++) {
        if (ntfs_read_mft_record(r, rec_buf) != 0) {
            continue;
        }
        if (ntfs_inspect_record(r, rec_buf, clean, out_info) == 0) {
            return 0;
        }
    }

    return -1;
}

int ntfs_list_root(struct ntfs_file_info *entries, int max_entries) {
    if (!g_vol.mounted || !entries || max_entries <= 0) {
        return -1;
    }

    uint8_t rec_buf[NTFS_MAX_MFT_RECORD_SIZE];
    int count = 0;

    for (uint64_t r = 0; r < g_vol.mft_total_records && count < max_entries; r++) {
        if (r < 16ULL) {
            continue; /* Hide NTFS system metadata records ($MFT, $Volume, etc.) */
        }
        if (ntfs_read_mft_record(r, rec_buf) != 0) {
            continue;
        }
        struct ntfs_file_info info;
        if (ntfs_inspect_record(r, rec_buf, NULL, &info) == 0) {
            if (info.name[0] == '$') {
                continue;
            }
            entries[count++] = info;
        }
    }

    return count;
}

int64_t ntfs_read_file(const char *filename, void *buffer, size_t max_size) {
    if (!g_vol.mounted || !filename || (!buffer && max_size > 0)) {
        return -1;
    }

    struct ntfs_file_info finfo;
    if (ntfs_find_file(filename, &finfo) != 0 || finfo.is_directory) {
        return -1;
    }

    uint8_t rec_buf[NTFS_MAX_MFT_RECORD_SIZE];
    if (ntfs_read_mft_record(finfo.mft_record, rec_buf) != 0) {
        return -1;
    }

    struct ntfs_attr_header *data_attr = ntfs_find_attr(rec_buf, NTFS_ATTR_DATA, 1, 0);
    if (!data_attr) {
        return -1;
    }

    /* Case 1: Resident $DATA attribute */
    if (!data_attr->non_resident) {
        struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)data_attr;
        size_t to_copy = res->value_length;
        if (to_copy > max_size) {
            to_copy = max_size;
        }
        if (to_copy > 0) {
            memcpy(buffer, (const uint8_t *)data_attr + res->value_offset, to_copy);
        }
        return (int64_t)to_copy;
    }

    /* Case 2: Non-Resident $DATA attribute */
    struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)data_attr;
    if (data_attr->flags & (NTFS_ATTR_FLAG_COMPRESSED | NTFS_ATTR_FLAG_ENCRYPTED)) {
        return -1;
    }

    struct ntfs_extent extents[NTFS_MAX_EXTENTS];
    int ext_cnt = ntfs_decode_runlist((const uint8_t *)data_attr + nr->runlist_offset,
                                      data_attr->length - nr->runlist_offset,
                                      extents, NTFS_MAX_EXTENTS);
    if (ext_cnt <= 0) {
        return (nr->real_size == 0) ? 0 : -1;
    }

    size_t target_bytes = (nr->real_size < (uint64_t)max_size) ? (size_t)nr->real_size : max_size;
    size_t init_limit   = (nr->initialized_size < (uint64_t)target_bytes)
                        ? (size_t)nr->initialized_size
                        : target_bytes;

    uint8_t *dst = (uint8_t *)buffer;
    size_t copied = 0;

    for (int i = 0; i < ext_cnt && copied < init_limit; i++) {
        uint64_t base_lba = ntfs_lcn_to_lba(extents[i].lcn);
        uint64_t ext_secs = extents[i].cluster_count * (uint64_t)g_vol.sectors_per_cluster;
        size_t   rem      = init_limit - copied;

        uint64_t full_secs = rem / ATA_SECTOR_SIZE;
        if (full_secs > ext_secs) {
            full_secs = ext_secs;
        }

        if (full_secs > 0) {
            if (ata_read_sectors(base_lba, (uint32_t)full_secs, dst + copied) != 0) {
                return -1;
            }
            copied += (size_t)full_secs * ATA_SECTOR_SIZE;
            rem    -= (size_t)full_secs * ATA_SECTOR_SIZE;
        }

        if (rem > 0 && full_secs < ext_secs) {
            if (ata_read_sectors(base_lba + full_secs, 1, g_sector_buf) != 0) {
                return -1;
            }
            memcpy(dst + copied, g_sector_buf, rem);
            copied += rem;
        }
    }

    if (copied < target_bytes) {
        memset(dst + copied, 0, target_bytes - copied);
    }

    return (int64_t)target_bytes;
}

/* ============================================================================
 * 3. Persistent File Write / Overwrite / Resize & Creation
 * ============================================================================ */

static void ntfs_update_mft_filename_sizes(uint8_t *rec_buf, uint64_t real_sz, uint64_t alloc_sz) {
    struct ntfs_mft_record_header *hdr = (struct ntfs_mft_record_header *)rec_buf;
    uint32_t off = hdr->attrs_offset;
    while (off + sizeof(struct ntfs_attr_header) <= hdr->bytes_in_use) {
        struct ntfs_attr_header *attr = (struct ntfs_attr_header *)(rec_buf + off);
        if (attr->type == NTFS_ATTR_END || attr->length == 0) {
            break;
        }
        if (attr->type == NTFS_ATTR_FILE_NAME && !attr->non_resident) {
            struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)attr;
            struct ntfs_attr_filename *fn =
                (struct ntfs_attr_filename *)(rec_buf + off + res->value_offset);
            fn->real_size      = real_sz;
            fn->allocated_size = alloc_sz;
        }
        off += attr->length;
    }
}

int ntfs_write_file(const char *filename, const void *data, size_t size) {
    if (!g_vol.mounted || !filename || (!data && size > 0)) {
        return -1;
    }

    struct ntfs_file_info finfo;
    if (ntfs_find_file(filename, &finfo) != 0 || finfo.is_directory) {
        return -1;
    }

    uint8_t rec_buf[NTFS_MAX_MFT_RECORD_SIZE];
    if (ntfs_read_mft_record(finfo.mft_record, rec_buf) != 0) {
        return -1;
    }

    struct ntfs_mft_record_header *mhdr = (struct ntfs_mft_record_header *)rec_buf;
    struct ntfs_attr_header *data_attr  = ntfs_find_attr(rec_buf, NTFS_ATTR_DATA, 1, 0);
    if (!data_attr) {
        return -1;
    }
    if (data_attr->flags & (NTFS_ATTR_FLAG_COMPRESSED | NTFS_ATTR_FLAG_ENCRYPTED | NTFS_ATTR_FLAG_SPARSE)) {
        return -1;
    }

    uint32_t attr_off     = (uint32_t)((uint8_t *)data_attr - rec_buf);
    uint32_t old_attr_len = data_attr->length;
    uint32_t tail_bytes   = mhdr->bytes_in_use - (attr_off + old_attr_len);

    /* Path A: $DATA is currently RESIDENT */
    if (!data_attr->non_resident) {
        struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)data_attr;
        uint16_t val_off = res->value_offset;
        uint32_t new_res_len = (uint32_t)ALIGN8((uint64_t)val_off + (uint64_t)size);

        if (mhdr->bytes_in_use - old_attr_len + new_res_len <= mhdr->bytes_allocated) {
            ntfs_memmove(rec_buf + attr_off + new_res_len,
                         rec_buf + attr_off + old_attr_len,
                         tail_bytes);

            memset(rec_buf + attr_off + val_off, 0, new_res_len - val_off);
            if (size > 0) {
                memcpy(rec_buf + attr_off + val_off, data, size);
            }

            res->value_length       = (uint32_t)size;
            res->common.length      = new_res_len;
            mhdr->bytes_in_use      = mhdr->bytes_in_use - old_attr_len + new_res_len;

            ntfs_update_mft_filename_sizes(rec_buf, size, ALIGN8(size));
            if (ntfs_write_mft_record(finfo.mft_record, rec_buf) != 0) {
                return -1;
            }
            ntfs_dir_sync_file_size(finfo.mft_record, size, ALIGN8(size));
            return 0;
        }

        /* Promote Resident $DATA -> Non-Resident Runlist */
        uint64_t needed_clusters = ((uint64_t)size + g_vol.bytes_per_cluster - 1ULL) / g_vol.bytes_per_cluster;
        if (needed_clusters == 0) needed_clusters = 1;

        uint64_t new_lcn = 0;
        if (ntfs_bitmap_alloc_clusters(needed_clusters, 0, &new_lcn) != 0) {
            return -1;
        }

        struct ntfs_extent ext = { new_lcn, needed_clusters };
        if (ntfs_write_data_to_extents(&ext, 1, data, size) != 0) {
            return -1;
        }

        uint8_t rl_buf[32];
        int rl_len = ntfs_encode_runlist(&ext, 1, rl_buf, sizeof(rl_buf));
        if (rl_len <= 0) {
            return -1;
        }

        uint16_t saved_id   = data_attr->attr_id;
        uint32_t new_nr_len = (uint32_t)ALIGN8(64U + (uint32_t)rl_len);
        if (mhdr->bytes_in_use - old_attr_len + new_nr_len > mhdr->bytes_allocated) {
            return -1;
        }

        ntfs_memmove(rec_buf + attr_off + new_nr_len,
                     rec_buf + attr_off + old_attr_len,
                     tail_bytes);

        memset(rec_buf + attr_off, 0, new_nr_len);
        struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)(rec_buf + attr_off);
        nr->common.type          = NTFS_ATTR_DATA;
        nr->common.length        = new_nr_len;
        nr->common.non_resident  = 1;
        nr->common.name_length   = 0;
        nr->common.name_offset   = 64U;
        nr->common.flags         = 0;
        nr->common.attr_id       = saved_id;
        nr->start_vcn            = 0;
        nr->last_vcn             = needed_clusters - 1ULL;
        nr->runlist_offset       = 64U;
        nr->allocated_size       = needed_clusters * (uint64_t)g_vol.bytes_per_cluster;
        nr->real_size            = size;
        nr->initialized_size     = size;
        memcpy(rec_buf + attr_off + 64U, rl_buf, (size_t)rl_len);

        mhdr->bytes_in_use = mhdr->bytes_in_use - old_attr_len + new_nr_len;
        ntfs_update_mft_filename_sizes(rec_buf, size, nr->allocated_size);

        if (ntfs_write_mft_record(finfo.mft_record, rec_buf) != 0) {
            return -1;
        }
        ntfs_dir_sync_file_size(finfo.mft_record, size, nr->allocated_size);
        return 0;
    }

    /* Path B: $DATA is NON-RESIDENT */
    struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)data_attr;
    struct ntfs_extent extents[NTFS_MAX_EXTENTS];
    int ext_cnt = ntfs_decode_runlist((const uint8_t *)data_attr + nr->runlist_offset,
                                      data_attr->length - nr->runlist_offset,
                                      extents, NTFS_MAX_EXTENTS);
    if (ext_cnt <= 0) {
        return -1;
    }

    uint64_t cur_clusters = 0;
    for (int i = 0; i < ext_cnt; i++) {
        cur_clusters += extents[i].cluster_count;
    }

    if (size == 0) {
        for (int i = 0; i < ext_cnt; i++) {
            (void)ntfs_bitmap_free_clusters(extents[i].lcn, extents[i].cluster_count);
        }
        uint16_t saved_id = data_attr->attr_id;
        uint32_t res_len  = 24U;
        ntfs_memmove(rec_buf + attr_off + res_len,
                     rec_buf + attr_off + old_attr_len,
                     tail_bytes);
        memset(rec_buf + attr_off, 0, res_len);
        struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)(rec_buf + attr_off);
        res->common.type         = NTFS_ATTR_DATA;
        res->common.length       = res_len;
        res->common.non_resident = 0;
        res->common.attr_id      = saved_id;
        res->value_length        = 0;
        res->value_offset        = 24U;

        mhdr->bytes_in_use = mhdr->bytes_in_use - old_attr_len + res_len;
        ntfs_update_mft_filename_sizes(rec_buf, 0, 0);
        if (ntfs_write_mft_record(finfo.mft_record, rec_buf) != 0) {
            return -1;
        }
        ntfs_dir_sync_file_size(finfo.mft_record, 0, 0);
        return 0;
    }

    uint64_t needed_clusters = ((uint64_t)size + g_vol.bytes_per_cluster - 1ULL) / g_vol.bytes_per_cluster;

    if (needed_clusters > cur_clusters) {
        uint64_t extra = needed_clusters - cur_clusters;
        uint64_t pref_lcn = extents[ext_cnt - 1].lcn + extents[ext_cnt - 1].cluster_count;
        uint64_t alloc_lcn = 0;

        if (ntfs_bitmap_alloc_clusters(extra, pref_lcn, &alloc_lcn) != 0) {
            return -1;
        }

        if (alloc_lcn == pref_lcn) {
            extents[ext_cnt - 1].cluster_count += extra;
        } else {
            if (ext_cnt >= NTFS_MAX_EXTENTS) {
                return -1;
            }
            extents[ext_cnt].lcn           = alloc_lcn;
            extents[ext_cnt].cluster_count = extra;
            ext_cnt++;
        }
        cur_clusters = needed_clusters;
    } else if (needed_clusters < cur_clusters) {
        uint64_t to_free = cur_clusters - needed_clusters;
        while (to_free > 0 && ext_cnt > 0) {
            struct ntfs_extent *last = &extents[ext_cnt - 1];
            if (last->cluster_count <= to_free) {
                (void)ntfs_bitmap_free_clusters(last->lcn, last->cluster_count);
                to_free -= last->cluster_count;
                ext_cnt--;
            } else {
                uint64_t keep = last->cluster_count - to_free;
                (void)ntfs_bitmap_free_clusters(last->lcn + keep, to_free);
                last->cluster_count = keep;
                to_free = 0;
            }
        }
        cur_clusters = needed_clusters;
    }

    if (ntfs_write_data_to_extents(extents, ext_cnt, data, size) != 0) {
        return -1;
    }

    uint8_t rl_buf[128];
    int rl_len = ntfs_encode_runlist(extents, ext_cnt, rl_buf, sizeof(rl_buf));
    if (rl_len <= 0) {
        return -1;
    }

    uint16_t rl_off     = nr->runlist_offset;
    uint32_t new_nr_len = (uint32_t)ALIGN8((uint64_t)rl_off + (uint64_t)rl_len);
    if (mhdr->bytes_in_use - old_attr_len + new_nr_len > mhdr->bytes_allocated) {
        return -1;
    }

    ntfs_memmove(rec_buf + attr_off + new_nr_len,
                 rec_buf + attr_off + old_attr_len,
                 tail_bytes);

    nr = (struct ntfs_attr_nonresident *)(rec_buf + attr_off);
    memset(rec_buf + attr_off + rl_off, 0, new_nr_len - rl_off);
    memcpy(rec_buf + attr_off + rl_off, rl_buf, (size_t)rl_len);

    nr->common.length    = new_nr_len;
    nr->start_vcn        = 0;
    nr->last_vcn         = cur_clusters - 1ULL;
    nr->allocated_size   = cur_clusters * (uint64_t)g_vol.bytes_per_cluster;
    nr->real_size        = size;
    nr->initialized_size = size;

    mhdr->bytes_in_use = mhdr->bytes_in_use - old_attr_len + new_nr_len;
    ntfs_update_mft_filename_sizes(rec_buf, size, nr->allocated_size);

    if (ntfs_write_mft_record(finfo.mft_record, rec_buf) != 0) {
        return -1;
    }

    ntfs_dir_sync_file_size(finfo.mft_record, size, nr->allocated_size);
    return 0;
}

static int ntfs_alloc_mft_slot(uint64_t *out_rec_num) {
    uint8_t mft0[NTFS_MAX_MFT_RECORD_SIZE];
    if (ntfs_read_mft_record(NTFS_MFT_REC_MFT, mft0) != 0) {
        return -1;
    }

    struct ntfs_attr_header *bm_attr = ntfs_find_attr(mft0, NTFS_ATTR_BITMAP, 1, 0);
    if (!bm_attr) {
        return -1;
    }

    if (!bm_attr->non_resident) {
        struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)bm_attr;
        uint8_t *bm = (uint8_t *)bm_attr + res->value_offset;
        uint64_t max_bits = (uint64_t)res->value_length * 8ULL;

        for (uint64_t r = NTFS_MFT_REC_FIRST_USER; r < g_vol.mft_total_records && r < max_bits; r++) {
            if (!((bm[r >> 3] >> (r & 7)) & 1)) {
                bm[r >> 3] |= (uint8_t)(1U << (r & 7));
                if (ntfs_write_mft_record(NTFS_MFT_REC_MFT, mft0) != 0) {
                    return -1;
                }
                *out_rec_num = r;
                return 0;
            }
        }

        /* Grow $MFT by 1 cluster (4 MFT records) if preallocated slots are full */
        struct ntfs_attr_header *d_attr = ntfs_find_attr(mft0, NTFS_ATTR_DATA, 1, 0);
        if (!d_attr || !d_attr->non_resident) {
            return -1;
        }

        struct ntfs_mft_record_header *mhdr = (struct ntfs_mft_record_header *)mft0;

        if (g_vol.mft_total_records >= max_bits) {
            if (mhdr->bytes_in_use + 8U > mhdr->bytes_allocated) {
                return -1;
            }
            uint32_t bm_end_off = (uint32_t)((uint8_t *)bm_attr - mft0) + bm_attr->length;
            ntfs_memmove(mft0 + bm_end_off + 8U, mft0 + bm_end_off, mhdr->bytes_in_use - bm_end_off);
            memset(mft0 + bm_end_off, 0, 8U);
            res->value_length += 8U;
            bm_attr->length   += 8U;
            mhdr->bytes_in_use += 8U;
            max_bits += 64ULL;
            d_attr = ntfs_find_attr(mft0, NTFS_ATTR_DATA, 1, 0);
        }

        uint64_t new_mft_lcn = 0;
        uint64_t pref = g_mft_extents[g_mft_extent_count - 1].lcn +
                        g_mft_extents[g_mft_extent_count - 1].cluster_count;
        if (ntfs_bitmap_alloc_clusters(1, pref, &new_mft_lcn) != 0) {
            return -1;
        }

        if (new_mft_lcn == pref) {
            g_mft_extents[g_mft_extent_count - 1].cluster_count += 1;
        } else {
            if (g_mft_extent_count >= NTFS_MAX_EXTENTS) {
                return -1;
            }
            g_mft_extents[g_mft_extent_count].lcn           = new_mft_lcn;
            g_mft_extents[g_mft_extent_count].cluster_count = 1;
            g_mft_extent_count++;
        }

        struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)d_attr;
        uint8_t rl_buf[128];
        int rl_len = ntfs_encode_runlist(g_mft_extents, g_mft_extent_count, rl_buf, sizeof(rl_buf));
        if (rl_len <= 0) {
            return -1;
        }

        uint32_t d_off      = (uint32_t)((uint8_t *)d_attr - mft0);
        uint32_t old_d_len  = d_attr->length;
        uint32_t new_d_len  = (uint32_t)ALIGN8((uint64_t)nr->runlist_offset + (uint64_t)rl_len);
        if (mhdr->bytes_in_use - old_d_len + new_d_len > mhdr->bytes_allocated) {
            return -1;
        }

        ntfs_memmove(mft0 + d_off + new_d_len,
                     mft0 + d_off + old_d_len,
                     mhdr->bytes_in_use - (d_off + old_d_len));
        memset(mft0 + d_off + nr->runlist_offset, 0, new_d_len - nr->runlist_offset);
        memcpy(mft0 + d_off + nr->runlist_offset, rl_buf, (size_t)rl_len);

        uint64_t new_rec_idx   = g_vol.mft_total_records;
        uint64_t added_records = g_vol.bytes_per_cluster / g_vol.mft_record_size;
        nr->common.length      = new_d_len;
        nr->last_vcn          += 1ULL;
        nr->allocated_size    += g_vol.bytes_per_cluster;
        nr->real_size         += g_vol.bytes_per_cluster;
        nr->initialized_size  += g_vol.bytes_per_cluster;
        mhdr->bytes_in_use     = mhdr->bytes_in_use - old_d_len + new_d_len;
        g_vol.mft_total_records += added_records;

        bm_attr = ntfs_find_attr(mft0, NTFS_ATTR_BITMAP, 1, 0);
        res = (struct ntfs_attr_resident *)bm_attr;
        bm  = (uint8_t *)bm_attr + res->value_offset;
        bm[new_rec_idx >> 3] |= (uint8_t)(1U << (new_rec_idx & 7));

        if (ntfs_write_mft_record(NTFS_MFT_REC_MFT, mft0) != 0) {
            return -1;
        }
        *out_rec_num = new_rec_idx;
        return 0;
    }

    struct ntfs_attr_nonresident *bm_nr = (struct ntfs_attr_nonresident *)bm_attr;
    struct ntfs_extent bm_exts[NTFS_MAX_EXTENTS];
    int ext_cnt = ntfs_decode_runlist((const uint8_t *)bm_attr + bm_nr->runlist_offset,
                                      bm_attr->length - bm_nr->runlist_offset,
                                      bm_exts, NTFS_MAX_EXTENTS);
    if (ext_cnt <= 0) {
        return -1;
    }

    uint32_t bm_secs = (uint32_t)((bm_nr->real_size + ATA_SECTOR_SIZE - 1ULL) / ATA_SECTOR_SIZE);
    for (uint32_t s = 0; s < bm_secs; s++) {
        uint64_t byte_pos = (uint64_t)s * ATA_SECTOR_SIZE;
        uint64_t vcn = byte_pos / g_vol.bytes_per_cluster;
        uint64_t off_in_cl = byte_pos % g_vol.bytes_per_cluster;
        uint64_t lcn = 0;
        if (ntfs_vcn_to_lcn(bm_exts, ext_cnt, vcn, &lcn) != 0) {
            return -1;
        }
        uint64_t sec_lba = ntfs_lcn_to_lba(lcn) + (off_in_cl / ATA_SECTOR_SIZE);
        if (ata_read_sectors(sec_lba, 1, g_sector_buf) != 0) {
            return -1;
        }

        uint64_t base_rec = (uint64_t)s * ATA_SECTOR_SIZE * 8ULL;
        for (uint64_t i = 0; i < ATA_SECTOR_SIZE * 8ULL; i++) {
            uint64_t r = base_rec + i;
            if (r < NTFS_MFT_REC_FIRST_USER) continue;
            if (r >= g_vol.mft_total_records) return -1;
            if (!((g_sector_buf[i >> 3] >> (i & 7)) & 1)) {
                g_sector_buf[i >> 3] |= (uint8_t)(1U << (i & 7));
                if (ata_write_sectors(sec_lba, 1, g_sector_buf) != 0) {
                    return -1;
                }
                *out_rec_num = r;
                return 0;
            }
        }
    }

    return -1;
}

int ntfs_create_file(const char *filename) {
    if (!g_vol.mounted || !filename) {
        return -1;
    }

    const char *clean = ntfs_normalize_filename(filename);
    size_t nlen = strlen(clean);
    if (nlen == 0 || nlen >= NTFS_MAX_FILENAME) {
        return -1;
    }
    if (strchr(clean, '/') != NULL || strchr(clean, '\\') != NULL) {
        return -1;
    }
    if (ntfs_find_file(clean, NULL) == 0) {
        return -1;
    }

    uint8_t root_rec[NTFS_MAX_MFT_RECORD_SIZE];
    if (ntfs_read_mft_record(NTFS_MFT_REC_ROOT, root_rec) != 0) {
        return -1;
    }
    uint16_t root_seq = ((struct ntfs_mft_record_header *)root_rec)->sequence_number;
    uint64_t root_ref = NTFS_MFT_REC_ROOT | ((uint64_t)root_seq << 48);

    uint64_t rec_num = 0;
    if (ntfs_alloc_mft_slot(&rec_num) != 0) {
        return -1;
    }

    uint8_t new_rec[NTFS_MAX_MFT_RECORD_SIZE];
    uint16_t seq_num = 1;
    uint64_t rec_lba = 0;
    if (ntfs_mft_record_lba(rec_num, &rec_lba) == 0 &&
        ata_read_sectors(rec_lba, g_vol.mft_record_size / ATA_SECTOR_SIZE, new_rec) == 0) {
        struct ntfs_mft_record_header *old_hdr = (struct ntfs_mft_record_header *)new_rec;
        if (ntfs_memcmp(old_hdr->magic, NTFS_MFT_MAGIC, 4) == 0 && old_hdr->sequence_number > 0) {
            seq_num = old_hdr->sequence_number;
        }
    }

    memset(new_rec, 0, g_vol.mft_record_size);
    uint16_t usa_cnt   = (uint16_t)(g_vol.mft_record_size / ATA_SECTOR_SIZE + 1U);
    uint16_t attrs_off = (uint16_t)ALIGN8(sizeof(struct ntfs_mft_record_header) + (uint32_t)usa_cnt * 2U);

    struct ntfs_mft_record_header *hdr = (struct ntfs_mft_record_header *)new_rec;
    memcpy(hdr->magic, NTFS_MFT_MAGIC, 4);
    hdr->usa_offset        = sizeof(struct ntfs_mft_record_header);
    hdr->usa_count         = usa_cnt;
    hdr->lsn               = 0;
    hdr->sequence_number   = seq_num;
    hdr->hard_link_count   = 1;
    hdr->attrs_offset      = attrs_off;
    hdr->flags             = NTFS_MFT_FLAG_IN_USE;
    hdr->bytes_allocated   = g_vol.mft_record_size;
    hdr->base_mft_record   = 0;
    hdr->next_attr_id      = 3;
    hdr->mft_record_number = (uint32_t)rec_num;

    uint32_t cur = attrs_off;

    /* Attribute 0: $STANDARD_INFORMATION (0x10) */
    struct ntfs_attr_resident *si_res = (struct ntfs_attr_resident *)(new_rec + cur);
    si_res->common.type         = NTFS_ATTR_STANDARD_INFORMATION;
    si_res->common.length       = 24U + sizeof(struct ntfs_attr_stdinfo);
    si_res->common.non_resident = 0;
    si_res->common.attr_id      = 0;
    si_res->value_length        = sizeof(struct ntfs_attr_stdinfo);
    si_res->value_offset        = 24U;

    struct ntfs_attr_stdinfo *si = (struct ntfs_attr_stdinfo *)(new_rec + cur + 24U);
    si->creation_time    = NTFS_DEFAULT_FILETIME;
    si->altered_time     = NTFS_DEFAULT_FILETIME;
    si->mft_changed_time = NTFS_DEFAULT_FILETIME;
    si->read_time        = NTFS_DEFAULT_FILETIME;
    si->dos_permissions  = (clean[0] == '.') ? (NTFS_FILE_ATTR_HIDDEN | NTFS_FILE_ATTR_SYSTEM)
                                             : NTFS_FILE_ATTR_ARCHIVE;
    cur += si_res->common.length;

    /* Attribute 1: $FILE_NAME (0x30) */
    uint16_t u16_name[NTFS_MAX_FILENAME];
    size_t u16_len = ntfs_ascii_to_utf16(clean, u16_name, NTFS_MAX_FILENAME);
    uint16_t fn_val_len  = (uint16_t)(sizeof(struct ntfs_attr_filename) + u16_len * 2U);
    uint32_t fn_attr_len = (uint32_t)ALIGN8(24U + (uint32_t)fn_val_len);

    struct ntfs_attr_resident *fn_res = (struct ntfs_attr_resident *)(new_rec + cur);
    fn_res->common.type         = NTFS_ATTR_FILE_NAME;
    fn_res->common.length       = fn_attr_len;
    fn_res->common.non_resident = 0;
    fn_res->common.attr_id      = 1;
    fn_res->value_length        = fn_val_len;
    fn_res->value_offset        = 24U;
    fn_res->indexed_flag        = 1;

    uint8_t *fn_payload_ptr = new_rec + cur + 24U;
    struct ntfs_attr_filename *fn = (struct ntfs_attr_filename *)fn_payload_ptr;
    fn->parent_directory = root_ref;
    fn->creation_time    = NTFS_DEFAULT_FILETIME;
    fn->altered_time     = NTFS_DEFAULT_FILETIME;
    fn->mft_changed_time = NTFS_DEFAULT_FILETIME;
    fn->read_time        = NTFS_DEFAULT_FILETIME;
    fn->allocated_size   = 0;
    fn->real_size        = 0;
    fn->flags            = si->dos_permissions;
    fn->reparse_value    = 0;
    fn->name_length      = (uint8_t)u16_len;
    fn->name_namespace   = NTFS_NAMESPACE_POSIX;
    memcpy(fn->name, u16_name, u16_len * 2U);
    cur += fn_attr_len;

    /* Attribute 2: $DATA (0x80, initially 0-byte resident) */
    struct ntfs_attr_resident *da_res = (struct ntfs_attr_resident *)(new_rec + cur);
    da_res->common.type         = NTFS_ATTR_DATA;
    da_res->common.length       = 24U;
    da_res->common.non_resident = 0;
    da_res->common.attr_id      = 2;
    da_res->value_length        = 0;
    da_res->value_offset        = 24U;
    cur += 24U;

    *(uint32_t *)(new_rec + cur)     = NTFS_ATTR_END;
    *(uint32_t *)(new_rec + cur + 4) = 0;
    hdr->bytes_in_use = cur + 8U;

    if (ntfs_write_mft_record(rec_num, new_rec) != 0) {
        return -1;
    }

    uint64_t child_ref = rec_num | ((uint64_t)seq_num << 48);
    if (ntfs_dir_add_root_entry(child_ref, fn_payload_ptr, fn_val_len, u16_name, u16_len) != 0) {
        return -1;
    }

    return 0;
}

int ntfs_write_or_create_file(const char *filename, const void *data, size_t size) {
    if (!g_vol.mounted || !filename) {
        return -1;
    }
    if (ntfs_find_file(filename, NULL) != 0) {
        if (ntfs_create_file(filename) != 0) {
            return -1;
        }
    }
    return ntfs_write_file(filename, data, size);
}

/* ============================================================================
 * 4. Bare-Metal NTFS 3.1 Formatter (ntfs_format_slot) & Linux Automount Engine
 * ============================================================================ */

static uint8_t ntfs_cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint64_t ntfs_generate_volume_uuid(int slot, const char *label) {
    uint32_t tsc_lo = 0, tsc_hi = 0;
    __asm__ volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
    uint64_t seed = ((uint64_t)tsc_hi << 32) | (uint64_t)tsc_lo;

    uint64_t rtc = ((uint64_t)ntfs_cmos_read(0x00))
                 | ((uint64_t)ntfs_cmos_read(0x02) << 8)
                 | ((uint64_t)ntfs_cmos_read(0x04) << 16)
                 | ((uint64_t)ntfs_cmos_read(0x07) << 24)
                 | ((uint64_t)ntfs_cmos_read(0x08) << 32)
                 | ((uint64_t)ntfs_cmos_read(0x09) << 40);

    uint64_t h = 0xCBF29CE484222325ULL ^ seed ^ (rtc * 0x9E3779B97F4A7C15ULL);
    h ^= (uint64_t)(uint32_t)(slot + 1) * 0xBF58476D1CE4E5B9ULL;

    const struct ata_drive_info *drv = ata_get_drive_by_slot(slot);
    if (drv) {
        for (size_t i = 0; drv->model[i] != '\0'; i++) {
            h ^= (uint8_t)drv->model[i];
            h *= 0x100000001B3ULL;
        }
        for (size_t i = 0; drv->serial[i] != '\0'; i++) {
            h ^= (uint8_t)drv->serial[i];
            h *= 0x100000001B3ULL;
        }
        h ^= drv->total_sectors;
    }
    if (label) {
        for (size_t i = 0; label[i] != '\0'; i++) {
            h ^= (uint8_t)label[i];
            h *= 0x100000001B3ULL;
        }
    }

    /* SplitMix64 finalizer */
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBULL;
    h = h ^ (h >> 31);
    if (h == 0) {
        h = 0x4449534D41534D01ULL;
    }
    return h;
}

static void fmt_mft_init(uint8_t *rec, uint32_t rec_num, uint16_t seq, uint16_t flags) {
    memset(rec, 0, NTFS_MAX_MFT_RECORD_SIZE);
    struct ntfs_mft_record_header *hdr = (struct ntfs_mft_record_header *)rec;
    memcpy(hdr->magic, NTFS_MFT_MAGIC, 4);
    hdr->usa_offset        = 0x30U;
    hdr->usa_count         = 3U;     /* USN + 2 sectors (1024 B) */
    hdr->lsn               = 0;
    hdr->sequence_number   = seq;
    hdr->hard_link_count   = 1;
    hdr->attrs_offset      = 0x38U;
    hdr->flags             = flags;
    hdr->bytes_in_use      = 0x38U;
    hdr->bytes_allocated   = NTFS_MAX_MFT_RECORD_SIZE;
    hdr->base_mft_record   = 0;
    hdr->next_attr_id      = 0;
    hdr->mft_record_number = rec_num;
}

static void fmt_mft_add_resident(uint8_t *rec, uint32_t type, const char *attr_name,
                                 const void *val, uint32_t val_len, uint8_t indexed) {
    struct ntfs_mft_record_header *hdr = (struct ntfs_mft_record_header *)rec;
    uint32_t cur = hdr->bytes_in_use;

    uint16_t u16_attr_name[16];
    size_t nlen = 0;
    uint16_t name_off = 0;
    uint16_t val_off  = 24U;

    if (attr_name && attr_name[0] != '\0') {
        nlen     = ntfs_ascii_to_utf16(attr_name, u16_attr_name, 16);
        name_off = 24U;
        val_off  = (uint16_t)ALIGN8(24U + (uint32_t)(nlen * 2U));
    }

    uint32_t total_len = (uint32_t)ALIGN8((uint32_t)val_off + val_len);
    if (cur + total_len + 8U > hdr->bytes_allocated) {
        return;
    }

    memset(rec + cur, 0, total_len);
    struct ntfs_attr_resident *res = (struct ntfs_attr_resident *)(rec + cur);
    res->common.type         = type;
    res->common.length       = total_len;
    res->common.non_resident = 0;
    res->common.name_length  = (uint8_t)nlen;
    res->common.name_offset  = name_off;
    res->common.flags        = 0;
    res->common.attr_id      = hdr->next_attr_id++;
    res->value_length        = val_len;
    res->value_offset        = val_off;
    res->indexed_flag        = indexed;

    if (nlen > 0) {
        memcpy(rec + cur + name_off, u16_attr_name, nlen * 2U);
    }
    if (val && val_len > 0) {
        memcpy(rec + cur + val_off, val, val_len);
    }

    hdr->bytes_in_use += total_len;
}

static void fmt_mft_add_stdinfo_and_fn(uint8_t *rec, const char *sys_name, uint32_t file_attrs) {
    struct ntfs_attr_stdinfo si;
    memset(&si, 0, sizeof(si));
    si.creation_time    = NTFS_DEFAULT_FILETIME;
    si.altered_time     = NTFS_DEFAULT_FILETIME;
    si.mft_changed_time = NTFS_DEFAULT_FILETIME;
    si.read_time        = NTFS_DEFAULT_FILETIME;
    si.dos_permissions  = file_attrs & ~NTFS_FILE_ATTR_DIRECTORY;
    fmt_mft_add_resident(rec, NTFS_ATTR_STANDARD_INFORMATION, NULL, &si, sizeof(si), 0);

    uint8_t fn_buf[196];
    memset(fn_buf, 0, sizeof(fn_buf));
    uint16_t u16[32];
    size_t ulen = ntfs_ascii_to_utf16(sys_name, u16, 32);

    struct ntfs_attr_filename *fn = (struct ntfs_attr_filename *)fn_buf;
    fn->parent_directory = NTFS_MFT_REC_ROOT | (5ULL << 48);
    fn->creation_time    = NTFS_DEFAULT_FILETIME;
    fn->altered_time     = NTFS_DEFAULT_FILETIME;
    fn->mft_changed_time = NTFS_DEFAULT_FILETIME;
    fn->read_time        = NTFS_DEFAULT_FILETIME;
    fn->flags            = file_attrs;
    fn->name_length      = (uint8_t)ulen;
    fn->name_namespace   = NTFS_NAMESPACE_WIN32_AND_DOS;
    memcpy(fn_buf + sizeof(struct ntfs_attr_filename), u16, ulen * 2U);

    uint32_t fn_len = (uint32_t)(sizeof(struct ntfs_attr_filename) + ulen * 2U);
    fmt_mft_add_resident(rec, NTFS_ATTR_FILE_NAME, NULL, fn_buf, fn_len, 1);
}

static void fmt_mft_add_nonresident(uint8_t *rec, uint32_t type,
                                    uint64_t start_lcn, uint64_t cluster_count,
                                    uint64_t real_size) {
    struct ntfs_mft_record_header *hdr = (struct ntfs_mft_record_header *)rec;
    uint32_t cur = hdr->bytes_in_use;

    struct ntfs_extent ext = { start_lcn, cluster_count };
    uint8_t rl_buf[32];
    int rl_len = ntfs_encode_runlist(&ext, 1, rl_buf, sizeof(rl_buf));
    if (rl_len <= 0) {
        return;
    }

    uint32_t total_len = (uint32_t)ALIGN8(64U + (uint32_t)rl_len);
    if (cur + total_len + 8U > hdr->bytes_allocated) {
        return;
    }

    memset(rec + cur, 0, total_len);
    struct ntfs_attr_nonresident *nr = (struct ntfs_attr_nonresident *)(rec + cur);
    nr->common.type         = type;
    nr->common.length       = total_len;
    nr->common.non_resident = 1;
    nr->common.name_length  = 0;
    nr->common.name_offset  = 64U;
    nr->common.flags        = 0;
    nr->common.attr_id      = hdr->next_attr_id++;
    nr->start_vcn           = 0;
    nr->last_vcn            = cluster_count - 1ULL;
    nr->runlist_offset      = 64U;
    nr->allocated_size      = cluster_count * 4096ULL;
    nr->real_size           = real_size;
    nr->initialized_size    = real_size;
    memcpy(rec + cur + 64U, rl_buf, (size_t)rl_len);

    hdr->bytes_in_use += total_len;
}

static void fmt_mft_add_small_i30_root(uint8_t *rec) {
    uint8_t ir_payload[sizeof(struct ntfs_index_root) + sizeof(struct ntfs_index_entry_header)];
    memset(ir_payload, 0, sizeof(ir_payload));

    struct ntfs_index_root *ir = (struct ntfs_index_root *)ir_payload;
    ir->attr_type                = NTFS_ATTR_FILE_NAME;
    ir->collation_rule           = 0x01U;
    ir->index_block_size         = 4096U;
    ir->clusters_per_index_block = 1;
    ir->index_hdr.entries_offset = sizeof(struct ntfs_index_header); /* 16 */
    ir->index_hdr.index_length   = sizeof(struct ntfs_index_header) + sizeof(struct ntfs_index_entry_header); /* 32 */
    ir->index_hdr.allocated_size = ir->index_hdr.index_length;
    ir->index_hdr.flags          = 0;

    struct ntfs_index_entry_header *end_ie =
        (struct ntfs_index_entry_header *)(ir_payload + sizeof(struct ntfs_index_root));
    end_ie->mft_reference = 0;
    end_ie->entry_length  = sizeof(struct ntfs_index_entry_header); /* 16 */
    end_ie->key_length    = 0;
    end_ie->index_flags   = NTFS_INDEX_ENTRY_END;
    end_ie->reserved      = 0;

    fmt_mft_add_resident(rec, NTFS_ATTR_INDEX_ROOT, "$I30", ir_payload, sizeof(ir_payload), 0);
}

static int fmt_mft_finish(uint8_t *rec) {
    struct ntfs_mft_record_header *hdr = (struct ntfs_mft_record_header *)rec;
    uint32_t cur = hdr->bytes_in_use;
    if (cur + 8U > hdr->bytes_allocated) {
        return -1;
    }
    *(uint32_t *)(rec + cur)     = NTFS_ATTR_END;
    *(uint32_t *)(rec + cur + 4) = 0;
    hdr->bytes_in_use = cur + 8U;
    return ntfs_usa_apply_fixup(rec, NTFS_MAX_MFT_RECORD_SIZE, hdr->usa_offset, hdr->usa_count);
}

/* Format an entire ATA disk slot with a clean NTFS 3.1 filesystem */
int ntfs_format_slot(int slot, const char *volume_label) {
    const struct ata_drive_info *drv = ata_get_drive_by_slot(slot);
    if (!drv || !drv->present || drv->total_sectors < 2048ULL) {
        return -1;
    }

    const uint32_t spc = 8U;                 /* 4096-byte clusters */
    const uint64_t bpc = 4096ULL;
    uint64_t vol_sectors = (drv->total_sectors - 1ULL) & ~7ULL;
    uint64_t total_clusters = vol_sectors / spc;

    /* Cluster layout:
     *   LCN 0        : $Boot (VBR at LBA 0)
     *   LCN 1        : $MFTMirr (mirror of MFT records 0..3)
     *   LCN 2..3     : $LogFile (2 clusters)
     *   LCN 4..19    : $MFT (16 clusters = 64 MFT records of 1024 B)
     *   LCN 20..51   : $UpCase (32 clusters = 128 KiB uppercase table)
     *   LCN 52..52+N : $Bitmap (cluster allocation bitmap) */
    const uint64_t mft_lcn      = 4ULL;
    const uint64_t mft_clusters = 16ULL;
    const uint64_t mirr_lcn     = 1ULL;
    const uint64_t log_lcn      = 2ULL;
    const uint64_t log_clusters = 2ULL;
    const uint64_t upc_lcn      = 20ULL;
    const uint64_t upc_clusters = 32ULL;
    const uint64_t bm_lcn       = 52ULL;

    uint64_t bm_bytes    = ALIGN8((total_clusters + 7ULL) / 8ULL);
    uint64_t bm_clusters = (bm_bytes + bpc - 1ULL) / bpc;
    if (bm_clusters == 0) bm_clusters = 1ULL;

    uint64_t reserved_clusters = bm_lcn + bm_clusters;
    if (total_clusters <= reserved_clusters + 8ULL) {
        return -1;
    }

    uint64_t uuid = ntfs_generate_volume_uuid(slot, volume_label);

    /* 1. Write 128 KiB $UpCase table at LCN 20..51 (LBA 160..415) */
    uint16_t *upcase = (uint16_t *)malloc(131072U);
    if (!upcase) {
        return -1;
    }
    for (uint32_t c = 0; c < 65536U; c++) {
        upcase[c] = (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32U) : (uint16_t)c;
    }
    if (ata_write_sectors_slot(slot, upc_lcn * spc, (uint32_t)(upc_clusters * spc), upcase) != 0) {
        free(upcase);
        return -1;
    }
    free(upcase);

    /* 2. Stream $Bitmap sectors to LCN 52..52+bm_clusters-1 (batched in 32 KiB chunks) */
    uint8_t *bm_chunk = (uint8_t *)malloc(32768U);
    if (!bm_chunk) {
        return -1;
    }
    uint64_t total_bm_secs = bm_clusters * spc;
    uint64_t sec_idx = 0;
    while (sec_idx < total_bm_secs) {
        uint64_t batch = total_bm_secs - sec_idx;
        if (batch > 64ULL) batch = 64ULL;
        memset(bm_chunk, 0, (size_t)batch * ATA_SECTOR_SIZE);

        for (uint64_t s = 0; s < batch; s++) {
            uint64_t global_sec = sec_idx + s;
            uint8_t *sec_ptr = bm_chunk + (size_t)s * ATA_SECTOR_SIZE;
            uint64_t bit_base = global_sec * ATA_SECTOR_SIZE * 8ULL;

            for (uint32_t b = 0; b < ATA_SECTOR_SIZE * 8U; b++) {
                uint64_t cl = bit_base + b;
                if (cl < reserved_clusters || cl >= total_clusters) {
                    sec_ptr[b >> 3] |= (uint8_t)(1U << (b & 7));
                }
            }
        }

        if (ata_write_sectors_slot(slot, bm_lcn * spc + sec_idx, (uint32_t)batch, bm_chunk) != 0) {
            free(bm_chunk);
            return -1;
        }
        sec_idx += batch;
    }
    free(bm_chunk);

    /* 3. Construct System MFT Records 0..15 (16 KiB) */
    uint8_t *mft_sys = (uint8_t *)malloc(16U * NTFS_MAX_MFT_RECORD_SIZE);
    if (!mft_sys) {
        return -1;
    }
    memset(mft_sys, 0, 16U * NTFS_MAX_MFT_RECORD_SIZE);

    uint32_t sys_flags = NTFS_FILE_ATTR_HIDDEN | NTFS_FILE_ATTR_SYSTEM;

    /* Record 0: $MFT */
    {
        uint8_t *r = mft_sys + 0U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 0, 1, NTFS_MFT_FLAG_IN_USE);
        fmt_mft_add_stdinfo_and_fn(r, "$MFT", sys_flags);
        fmt_mft_add_nonresident(r, NTFS_ATTR_DATA, mft_lcn, mft_clusters, mft_clusters * bpc);
        uint8_t mft_bm[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; /* Records 0..15 in use */
        fmt_mft_add_resident(r, NTFS_ATTR_BITMAP, NULL, mft_bm, sizeof(mft_bm), 0);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 1: $MFTMirr */
    {
        uint8_t *r = mft_sys + 1U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 1, 1, NTFS_MFT_FLAG_IN_USE);
        fmt_mft_add_stdinfo_and_fn(r, "$MFTMirr", sys_flags);
        fmt_mft_add_nonresident(r, NTFS_ATTR_DATA, mirr_lcn, 1ULL, bpc);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 2: $LogFile */
    {
        uint8_t *r = mft_sys + 2U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 2, 2, NTFS_MFT_FLAG_IN_USE);
        fmt_mft_add_stdinfo_and_fn(r, "$LogFile", sys_flags);
        fmt_mft_add_nonresident(r, NTFS_ATTR_DATA, log_lcn, log_clusters, log_clusters * bpc);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 3: $Volume (stores NTFS Volume Label = hardware tag VHD001 / HD001 / SD001!) */
    {
        uint8_t *r = mft_sys + 3U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 3, 3, NTFS_MFT_FLAG_IN_USE);
        fmt_mft_add_stdinfo_and_fn(r, "$Volume", sys_flags);

        uint16_t u16_lbl[32];
        size_t lbl_len = volume_label ? ntfs_ascii_to_utf16(volume_label, u16_lbl, 31) : 0;
        fmt_mft_add_resident(r, NTFS_ATTR_VOLUME_NAME, NULL, u16_lbl, (uint32_t)(lbl_len * 2U), 0);

        uint8_t vol_info[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 3, 1, 0, 0 }; /* NTFS 3.1 */
        fmt_mft_add_resident(r, NTFS_ATTR_VOLUME_INFORMATION, NULL, vol_info, sizeof(vol_info), 0);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 4: $AttrDef */
    {
        uint8_t *r = mft_sys + 4U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 4, 4, NTFS_MFT_FLAG_IN_USE);
        fmt_mft_add_stdinfo_and_fn(r, "$AttrDef", sys_flags);
        fmt_mft_add_resident(r, NTFS_ATTR_DATA, NULL, NULL, 0, 0);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 5: . (Root Directory with $I30 Small Index) */
    {
        uint8_t *r = mft_sys + 5U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 5, 5, NTFS_MFT_FLAG_IN_USE | NTFS_MFT_FLAG_IS_DIRECTORY);
        fmt_mft_add_stdinfo_and_fn(r, ".", sys_flags | NTFS_FILE_ATTR_DIRECTORY);
        fmt_mft_add_small_i30_root(r);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 6: $Bitmap */
    {
        uint8_t *r = mft_sys + 6U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 6, 6, NTFS_MFT_FLAG_IN_USE);
        fmt_mft_add_stdinfo_and_fn(r, "$Bitmap", sys_flags);
        fmt_mft_add_nonresident(r, NTFS_ATTR_DATA, bm_lcn, bm_clusters, bm_bytes);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 7: $Boot */
    {
        uint8_t *r = mft_sys + 7U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 7, 7, NTFS_MFT_FLAG_IN_USE);
        fmt_mft_add_stdinfo_and_fn(r, "$Boot", sys_flags);
        fmt_mft_add_nonresident(r, NTFS_ATTR_DATA, 0ULL, 1ULL, bpc);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 8: $BadClus */
    {
        uint8_t *r = mft_sys + 8U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 8, 8, NTFS_MFT_FLAG_IN_USE);
        fmt_mft_add_stdinfo_and_fn(r, "$BadClus", sys_flags);
        fmt_mft_add_resident(r, NTFS_ATTR_DATA, NULL, NULL, 0, 0);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 9: $Secure */
    {
        uint8_t *r = mft_sys + 9U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 9, 9, NTFS_MFT_FLAG_IN_USE);
        fmt_mft_add_stdinfo_and_fn(r, "$Secure", sys_flags);
        fmt_mft_add_resident(r, NTFS_ATTR_DATA, NULL, NULL, 0, 0);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 10: $UpCase */
    {
        uint8_t *r = mft_sys + 10U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 10, 10, NTFS_MFT_FLAG_IN_USE);
        fmt_mft_add_stdinfo_and_fn(r, "$UpCase", sys_flags);
        fmt_mft_add_nonresident(r, NTFS_ATTR_DATA, upc_lcn, upc_clusters, 131072ULL);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Record 11: $Extend */
    {
        uint8_t *r = mft_sys + 11U * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, 11, 11, NTFS_MFT_FLAG_IN_USE | NTFS_MFT_FLAG_IS_DIRECTORY);
        fmt_mft_add_stdinfo_and_fn(r, "$Extend", sys_flags | NTFS_FILE_ATTR_DIRECTORY);
        fmt_mft_add_small_i30_root(r);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Records 12..15: Reserved NTFS system records */
    for (uint32_t idx = 12U; idx < 16U; idx++) {
        uint8_t *r = mft_sys + idx * NTFS_MAX_MFT_RECORD_SIZE;
        fmt_mft_init(r, idx, (uint16_t)idx, NTFS_MFT_FLAG_IN_USE);
        struct ntfs_attr_stdinfo si;
        memset(&si, 0, sizeof(si));
        si.creation_time = NTFS_DEFAULT_FILETIME;
        si.dos_permissions = sys_flags;
        fmt_mft_add_resident(r, NTFS_ATTR_STANDARD_INFORMATION, NULL, &si, sizeof(si), 0);
        if (fmt_mft_finish(r) != 0) { free(mft_sys); return -1; }
    }

    /* Write Records 0..15 to $MFT (LCN 4, 32 sectors) and Records 0..3 to $MFTMirr (LCN 1, 8 sectors) */
    if (ata_write_sectors_slot(slot, mft_lcn * spc, 32U, mft_sys) != 0 ||
        ata_write_sectors_slot(slot, mirr_lcn * spc, 8U, mft_sys) != 0) {
        free(mft_sys);
        return -1;
    }
    free(mft_sys);

    /* Zero remaining preallocated $MFT slots (Records 16..63 = 48 KiB = 96 sectors) & $LogFile (16 sectors) */
    uint8_t *zero_blk = (uint8_t *)malloc(49152U);
    if (!zero_blk) {
        return -1;
    }
    memset(zero_blk, 0, 49152U);
    if (ata_write_sectors_slot(slot, mft_lcn * spc + 32ULL, 96U, zero_blk) != 0 ||
        ata_write_sectors_slot(slot, log_lcn * spc, (uint32_t)(log_clusters * spc), zero_blk) != 0) {
        free(zero_blk);
        return -1;
    }

    /* 4. Construct and write NTFS Volume Boot Record (VBR) at LBA 0 + Backup VBR at end of disk */
    struct ntfs_vbr *vbr = (struct ntfs_vbr *)zero_blk;
    memset(vbr, 0, ATA_SECTOR_SIZE);
    vbr->jump_boot[0]             = 0xEB;
    vbr->jump_boot[1]             = 0x52;
    vbr->jump_boot[2]             = 0x90;
    memcpy(vbr->oem_id, NTFS_OEM_ID, 8);
    vbr->bytes_per_sector         = ATA_SECTOR_SIZE;
    vbr->sectors_per_cluster      = (uint8_t)spc;
    vbr->reserved_sectors         = 0;
    vbr->media_descriptor         = 0xF8;
    vbr->sectors_per_track        = 63;
    vbr->number_of_heads          = 255;
    vbr->hidden_sectors           = 0;
    vbr->unused_drive_flags       = 0x00800080U;
    vbr->total_sectors            = vol_sectors;
    vbr->mft_lcn                  = mft_lcn;
    vbr->mft_mirr_lcn             = mirr_lcn;
    vbr->clusters_per_mft_record  = -10; /* 2^10 = 1024 bytes */
    vbr->clusters_per_index_block = 1;   /* 1 cluster = 4096 bytes */
    vbr->volume_serial_number     = uuid;
    vbr->boot_signature           = NTFS_BOOT_SIGNATURE;

    /* Write LCN 0 (VBR in sector 0 + 7 zeroed boot sectors) and backup VBR at last sector */
    if (ata_write_sectors_slot(slot, 0ULL, spc, zero_blk) != 0 ||
        ata_write_sectors_slot(slot, drv->total_sectors - 1ULL, 1U, zero_blk) != 0) {
        free(zero_blk);
        return -1;
    }

    free(zero_blk);
    return ata_flush_cache_slot(slot);
}

int ntfs_persist_fstab(const char *fstab_text) {
    if (!g_vol.mounted || !fstab_text) {
        return -1;
    }
    return ntfs_write_or_create_file(NTFS_PERSISTENT_FSTAB_FILE, fstab_text, strlen(fstab_text));
}

static void ntfs_generate_default_fstab(char *out, size_t max_len) {
    const char *tag = g_vol.volume_label[0] ? g_vol.volume_label
                    : ata_get_drive_info()->tag;
    out[0] = '\0';
    strncat(out, "# /etc/fstab: static file system information (Linux UUID/LABEL persistence)\n", max_len - strlen(out) - 1);
    strncat(out, "# <file system>          <mount point> <type> <options>                       <dump> <pass>\n", max_len - strlen(out) - 1);
    strncat(out, "ramfs                    /             ramfs  rw,relatime                     0      0\n", max_len - strlen(out) - 1);
    strncat(out, "UUID=", max_len - strlen(out) - 1);
    strncat(out, g_vol.uuid_str, max_len - strlen(out) - 1);
    strncat(out, "    /ntfs         ntfs   rw,sync,flush_ext,tag=", max_len - strlen(out) - 1);
    strncat(out, tag, max_len - strlen(out) - 1);
    strncat(out, "   0      1\n", max_len - strlen(out) - 1);
}

/* Parse drive tag of the form VHD001..VHD999, HD001..HD999, or SD001..SD999 */
static int ntfs_parse_drive_tag(const char *str, uint8_t *out_media_type, int *out_index) {
    if (!str) {
        return 0;
    }
    uint8_t mtype = 0;
    size_t pfx_len = 0;

    if (strncmp(str, "VHD", 3) == 0) {
        mtype = ATA_MEDIA_VHD;
        pfx_len = 3;
    } else if (strncmp(str, "SD", 2) == 0) {
        mtype = ATA_MEDIA_SSD;
        pfx_len = 2;
    } else if (strncmp(str, "HD", 2) == 0) {
        mtype = ATA_MEDIA_HDD;
        pfx_len = 2;
    } else {
        return 0;
    }

    if (str[pfx_len + 0] < '0' || str[pfx_len + 0] > '9' ||
        str[pfx_len + 1] < '0' || str[pfx_len + 1] > '9' ||
        str[pfx_len + 2] < '0' || str[pfx_len + 2] > '9' ||
        str[pfx_len + 3] != '\0') {
        return 0;
    }

    int val = (str[pfx_len + 0] - '0') * 100
            + (str[pfx_len + 1] - '0') * 10
            + (str[pfx_len + 2] - '0');
    if (val < 1 || val > 999) {
        return 0;
    }

    if (out_media_type) *out_media_type = mtype;
    if (out_index)      *out_index      = val;
    return 1;
}

int ntfs_probe_slot(int slot, struct ntfs_probe_info *out) {
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    const struct ata_drive_info *drv = ata_get_drive_by_slot(slot);
    if (!drv || !drv->present) {
        return -1;
    }

    /* Save any currently mounted slot so probing is non-destructive */
    int prev_mounted = g_vol.mounted;
    int prev_slot    = prev_mounted ? g_vol.slot : ata_get_active_slot();

    if (ntfs_init_slot(slot, 1) != 0) {
        if (prev_mounted && prev_slot >= 0) {
            (void)ntfs_init_slot(prev_slot, 1);
        } else if (prev_slot >= 0) {
            (void)ata_select_drive(prev_slot);
        }
        return -1;
    }

    out->valid_ntfs     = 1;
    out->part_start_lba = g_vol.part_start_lba;
    out->volume_serial  = g_vol.volume_serial;
    strcpy(out->uuid_str, g_vol.uuid_str);
    strcpy(out->volume_label, g_vol.volume_label);

    char fstab_check[512];
    int64_t rd = ntfs_read_file(NTFS_PERSISTENT_FSTAB_FILE, fstab_check, sizeof(fstab_check) - 1U);
    if (rd > 0) {
        fstab_check[rd] = '\0';
        if (strstr(fstab_check, "UUID=") != NULL && strstr(fstab_check, out->uuid_str) != NULL) {
            out->has_persistent_fstab = 1;
        }
    }

    g_vol.mounted = 0;
    if (prev_mounted && prev_slot >= 0) {
        (void)ntfs_init_slot(prev_slot, 1);
    } else if (prev_slot >= 0) {
        (void)ata_select_drive(prev_slot);
    }

    return 0;
}

/* Linux-style blkid + /etc/fstab persistent disk selection & first-boot auto-formatter */
int ntfs_automount(void) {
    struct ntfs_probe_info probes[ATA_MAX_DRIVES];
    memset(probes, 0, sizeof(probes));

    int any_drive = 0;
    for (int s = 0; s < ATA_MAX_DRIVES; s++) {
        const struct ata_drive_info *drv = ata_get_drive_by_slot(s);
        if (drv && drv->present) {
            any_drive = 1;
            (void)ntfs_probe_slot(s, &probes[s]);
        }
    }
    if (!any_drive) {
        return -1;
    }

    /* Step 1: Reconcile persistent tags (VHD001..999, HD001..999, SD001..999).
     * Drives that already carry a valid tag in NTFS $Volume retain their exact tag;
     * untagged drives get the lowest free 001..999 number for their hardware media type. */
    uint8_t has_saved_tag[ATA_MAX_DRIVES] = { 0, 0, 0, 0 };
    uint8_t claimed_mtype[ATA_MAX_DRIVES] = { 0, 0, 0, 0 };
    int     claimed_index[ATA_MAX_DRIVES] = { 0, 0, 0, 0 };

    for (int s = 0; s < ATA_MAX_DRIVES; s++) {
        if (probes[s].valid_ntfs) {
            uint8_t mtype = 0;
            int idx = 0;
            if (ntfs_parse_drive_tag(probes[s].volume_label, &mtype, &idx)) {
                has_saved_tag[s] = 1;
                claimed_mtype[s] = mtype;
                claimed_index[s] = idx;
                ata_set_drive_tag(s, probes[s].volume_label);
            }
        }
    }

    for (int s = 0; s < ATA_MAX_DRIVES; s++) {
        const struct ata_drive_info *drv = ata_get_drive_by_slot(s);
        if (!drv || !drv->present || has_saved_tag[s]) {
            continue;
        }
        int chosen_idx = 1;
        for (int candidate = 1; candidate <= 999; candidate++) {
            int taken = 0;
            for (int k = 0; k < ATA_MAX_DRIVES; k++) {
                if (claimed_index[k] == candidate && claimed_mtype[k] == drv->media_type) {
                    taken = 1;
                    break;
                }
            }
            if (!taken) {
                chosen_idx = candidate;
                break;
            }
        }
        char new_tag[8];
        ata_format_tag(drv->media_type, chosen_idx, new_tag);
        ata_set_drive_tag(s, new_tag);
        claimed_mtype[s] = drv->media_type;
        claimed_index[s] = chosen_idx;
    }

    /* Step 2 (Subsequent Boots): Prefer the drive carrying a matching persistent fstab UUID */
    int target_slot = -1;
    for (int s = 0; s < ATA_MAX_DRIVES; s++) {
        if (probes[s].valid_ntfs && probes[s].has_persistent_fstab) {
            target_slot = s;
            break;
        }
    }

    if (target_slot >= 0) {
        const struct ata_drive_info *drv = ata_get_drive_by_slot(target_slot);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("[  ");
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_puts("OK");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("  ] blkid: Matched persistent fstab disk ");
        vga_puts(drv->dev_name);
        vga_puts(" [");
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_puts(drv->tag);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("] UUID=");
        vga_puts(probes[target_slot].uuid_str);
        vga_putchar('\n');

        return ntfs_init_slot(target_slot, 0);
    }

    /* Step 3 (First Boot with pre-existing NTFS disk): Adopt & bind it in fstab */
    for (int s = 0; s < ATA_MAX_DRIVES; s++) {
        if (probes[s].valid_ntfs) {
            target_slot = s;
            break;
        }
    }

    if (target_slot >= 0) {
        const struct ata_drive_info *drv = ata_get_drive_by_slot(target_slot);
        if (ntfs_init_slot(target_slot, 0) != 0) {
            return -1;
        }
        if (!has_saved_tag[target_slot]) {
            (void)ntfs_set_volume_label(drv->tag);
        }
        char fstab_buf[512];
        ntfs_generate_default_fstab(fstab_buf, sizeof(fstab_buf));
        (void)ntfs_persist_fstab(fstab_buf);
        return 0;
    }

    /* Step 4 (First Boot with NO formatted NTFS disk): Find formattable drive & format in NTFS 3.1 */
    int fmt_slot = -1;
    for (int s = 0; s < ATA_MAX_DRIVES; s++) {
        const struct ata_drive_info *drv = ata_get_drive_by_slot(s);
        if (drv && drv->present && drv->total_sectors >= 2048ULL) {
            fmt_slot = s;
            break;
        }
    }

    if (fmt_slot < 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_puts("[WARN] storage: No formattable disk (>= 1 MiB) available.\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return -1;
    }

    const struct ata_drive_info *fdrv = ata_get_drive_by_slot(fmt_slot);
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("[FORMAT] First boot: Formatting ");
    vga_puts(fdrv->dev_name);
    vga_puts(" (");
    vga_puts(ata_media_type_name(fdrv->media_type));
    vga_puts(") as NTFS 3.1 [Tag: ");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    vga_puts(fdrv->tag);
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("]...\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    if (ntfs_format_slot(fmt_slot, fdrv->tag) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("[FAIL] mkfs.ntfs: Failed to format ");
        vga_puts(fdrv->dev_name);
        vga_puts("!\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return -1;
    }

    if (ntfs_init_slot(fmt_slot, 0) != 0) {
        return -1;
    }

    g_vol.newly_formatted = 1;

    char fstab_buf[512];
    ntfs_generate_default_fstab(fstab_buf, sizeof(fstab_buf));
    (void)ntfs_persist_fstab(fstab_buf);

    return 0;
}

/* ============================================================================
 * 5. Diagnostics & Console Output (ntfsinfo / blkid / ntfsls)
 * ============================================================================ */

static void ntfs_print_padded(const char *s, int width) {
    int len = (int)strlen(s);
    vga_puts(s);
    while (len < width) {
        vga_putchar(' ');
        len++;
    }
}

void ntfs_print_blkid(void) {
    if (ata_get_drive_count() == 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_puts("blkid: No ATA block devices detected.\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    for (int s = 0; s < ATA_MAX_DRIVES; s++) {
        const struct ata_drive_info *drv = ata_get_drive_by_slot(s);
        if (!drv || !drv->present) {
            continue;
        }
        struct ntfs_probe_info pr;
        (void)ntfs_probe_slot(s, &pr);

        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_puts(drv->dev_name);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts(": TAG=\"");
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_puts(drv->tag);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("\" MEDIA=\"");
        vga_puts(ata_media_type_name(drv->media_type));
        vga_puts("\"");

        if (pr.valid_ntfs) {
            vga_puts(" LABEL=\"");
            vga_puts(pr.volume_label[0] ? pr.volume_label : drv->tag);
            vga_puts("\" UUID=\"");
            vga_puts(pr.uuid_str);
            vga_puts("\" TYPE=\"ntfs\"");
            if (g_vol.mounted && g_vol.slot == s) {
                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                vga_puts(" [MOUNTED /ntfs]");
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            }
        } else {
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            vga_puts(" TYPE=\"unformatted\"");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
        vga_putchar('\n');
    }
}

void ntfs_print_info(void) {
    char num[24];

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("ATA / NTFS Storage Subsystem Status:\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("-------------------------------------------------------------\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    if (ata_get_drive_count() == 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_puts("  [WARN] No writable ATA disk detected. Filesystem not mounted.\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    for (int s = 0; s < ATA_MAX_DRIVES; s++) {
        const struct ata_drive_info *d = ata_get_drive_by_slot(s);
        if (!d || !d->present) {
            continue;
        }
        vga_puts("  Device ");
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_puts(d->dev_name);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts(" [");
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_puts(d->tag);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("]: ");
        vga_puts(d->model);
        vga_puts(" | ");
        vga_puts(ata_media_type_name(d->media_type));
        if (g_vol.mounted && g_vol.slot == s) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            vga_puts(" (ACTIVE)");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
        vga_putchar('\n');
    }

    const struct ata_drive_info *drv = ata_get_drive_info();
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("-------------------------------------------------------------\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    vga_puts("  Active Drive:       ");
    vga_puts(drv->dev_name);
    vga_puts(" [Tag: ");
    vga_puts(drv->tag);
    vga_puts("]\n");

    vga_puts("  Hardware Model:     ");
    vga_puts(drv->model);
    vga_puts(" (SN: ");
    vga_puts(drv->serial);
    vga_puts(")\n");

    vga_puts("  Media Class:        ");
    vga_puts(ata_media_type_name(drv->media_type));
    vga_putchar('\n');

    vga_puts("  Capacity (LBA48):   ");
    ntfs_u64_to_str(drv->capacity_gib, num);
    vga_puts(num);
    vga_putchar('.');
    if (drv->capacity_gib_hundredths < 10) vga_putchar('0');
    ntfs_u64_to_str(drv->capacity_gib_hundredths, num);
    vga_puts(num);
    vga_puts(" GiB (");
    ntfs_u64_to_str(drv->total_sectors, num);
    vga_puts(num);
    vga_puts(" sectors)\n");

    if (!g_vol.mounted) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_puts("  NTFS Status:        Not mounted (no valid NTFS VBR)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    vga_puts("  Filesystem:         NTFS 3.1 (Read/Write + 0xEA Cache Flush)\n");
    vga_puts("  Volume Label (Tag): ");
    vga_puts(g_vol.volume_label[0] ? g_vol.volume_label : drv->tag);
    vga_putchar('\n');

    vga_puts("  Volume UUID:        ");
    vga_puts(g_vol.uuid_str);
    vga_putchar('\n');

    vga_puts("  Bytes / Sector:     ");
    ntfs_u64_to_str(g_vol.bytes_per_sector, num);
    vga_puts(num);
    vga_puts("\n  Sectors / Cluster:  ");
    ntfs_u64_to_str(g_vol.sectors_per_cluster, num);
    vga_puts(num);
    vga_puts(" (");
    ntfs_u64_to_str(g_vol.bytes_per_cluster, num);
    vga_puts(num);
    vga_puts(" Bytes)\n");

    vga_puts("  $MFT Location:      LCN ");
    ntfs_u64_to_str(g_vol.mft_lcn, num);
    vga_puts(num);
    vga_puts(" -> LBA ");
    ntfs_u64_to_str(g_vol.mft_start_lba, num);
    vga_puts(num);
    vga_putchar('\n');

    vga_puts("  MFT Record Size:    ");
    ntfs_u64_to_str(g_vol.mft_record_size, num);
    vga_puts(num);
    vga_puts(" Bytes (");
    ntfs_u64_to_str(g_vol.mft_total_records, num);
    vga_puts(num);
    vga_puts(" total $MFT slots)\n");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("-------------------------------------------------------------\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

void ntfs_print_root_dir(void) {
    if (!g_vol.mounted) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        vga_puts("[WARN] No NTFS filesystem is currently mounted!\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    struct ntfs_file_info list[48];
    int cnt = ntfs_list_root(list, 48);
    if (cnt < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("[FAIL] Failed to read NTFS root directory!\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("NTFS Root Directory (MFT Record 5) [");
    vga_puts(g_vol.volume_label[0] ? g_vol.volume_label : ata_get_drive_info()->tag);
    vga_puts("]:\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("MFT#    Storage Mode    Size (Bytes)      Filename\n");
    vga_puts("-------------------------------------------------------------\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    if (cnt == 0) {
        vga_puts("(no user files in root directory)\n");
        return;
    }

    char num[24];
    for (int i = 0; i < cnt; i++) {
        ntfs_u64_to_str(list[i].mft_record, num);
        ntfs_print_padded(num, 8);

        if (list[i].is_directory) {
            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            ntfs_print_padded("<DIR>", 16);
        } else if (list[i].is_resident) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            ntfs_print_padded("Resident", 16);
        } else {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            ntfs_print_padded("Non-Resident", 16);
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        ntfs_u64_to_str(list[i].real_size, num);
        ntfs_print_padded(num, 18);

        vga_puts(list[i].name);
        if (list[i].is_directory) {
            vga_putchar('/');
        }
        vga_putchar('\n');
    }
}
