#ifndef NTFS_H
#define NTFS_H

#include "types.h"

/* ============================================================================
 * NTFS 3.1 On-Disk Structures (Freestanding x86_64, Little-Endian, packed)
 * ============================================================================ */

#define NTFS_OEM_ID                     "NTFS    "
#define NTFS_BOOT_SIGNATURE             0xAA55
#define NTFS_MFT_MAGIC                  "FILE"
#define NTFS_INDX_MAGIC                 "INDX"

/* System MFT Record Numbers (Metadata Files) */
#define NTFS_MFT_REC_MFT                0ULL   /* $MFT */
#define NTFS_MFT_REC_MFTMIRR            1ULL   /* $MFTMirr */
#define NTFS_MFT_REC_LOGFILE            2ULL   /* $LogFile */
#define NTFS_MFT_REC_VOLUME             3ULL   /* $Volume */
#define NTFS_MFT_REC_ATTRDEF            4ULL   /* $AttrDef */
#define NTFS_MFT_REC_ROOT               5ULL   /* . (Root Directory) */
#define NTFS_MFT_REC_BITMAP             6ULL   /* $Bitmap (Cluster Allocation) */
#define NTFS_MFT_REC_BOOT               7ULL   /* $Boot */
#define NTFS_MFT_REC_BADCLUS            8ULL   /* $BadClus */
#define NTFS_MFT_REC_SECURE             9ULL   /* $Secure */
#define NTFS_MFT_REC_UPCASE             10ULL  /* $UpCase */
#define NTFS_MFT_REC_EXTEND             11ULL  /* $Extend */
#define NTFS_MFT_REC_FIRST_USER         24ULL  /* First User MFT Slot */

/* MFT Record Header Flags */
#define NTFS_MFT_FLAG_IN_USE            0x0001
#define NTFS_MFT_FLAG_IS_DIRECTORY      0x0002

/* NTFS Attribute Types */
#define NTFS_ATTR_STANDARD_INFORMATION  0x00000010U
#define NTFS_ATTR_ATTRIBUTE_LIST        0x00000020U
#define NTFS_ATTR_FILE_NAME             0x00000030U
#define NTFS_ATTR_OBJECT_ID             0x00000040U
#define NTFS_ATTR_SECURITY_DESCRIPTOR   0x00000050U
#define NTFS_ATTR_VOLUME_NAME           0x00000060U
#define NTFS_ATTR_VOLUME_INFORMATION    0x00000070U
#define NTFS_ATTR_DATA                  0x00000080U
#define NTFS_ATTR_INDEX_ROOT            0x00000090U
#define NTFS_ATTR_INDEX_ALLOCATION      0x000000A0U
#define NTFS_ATTR_BITMAP                0x000000B0U
#define NTFS_ATTR_END                   0xFFFFFFFFU

/* Attribute Header Flags */
#define NTFS_ATTR_FLAG_COMPRESSED       0x0001
#define NTFS_ATTR_FLAG_ENCRYPTED        0x4000
#define NTFS_ATTR_FLAG_SPARSE           0x8000

/* $FILE_NAME Namespaces */
#define NTFS_NAMESPACE_POSIX            0
#define NTFS_NAMESPACE_WIN32            1
#define NTFS_NAMESPACE_DOS              2
#define NTFS_NAMESPACE_WIN32_AND_DOS    3

/* $FILE_NAME File Attributes */
#define NTFS_FILE_ATTR_READONLY         0x00000001U
#define NTFS_FILE_ATTR_HIDDEN           0x00000002U
#define NTFS_FILE_ATTR_SYSTEM           0x00000004U
#define NTFS_FILE_ATTR_ARCHIVE          0x00000020U
#define NTFS_FILE_ATTR_DIRECTORY        0x10000000U

/* Index Header & Index Entry Flags ($I30 B-Tree) */
#define NTFS_INDEX_FLAG_LARGE           0x01   /* Has $INDEX_ALLOCATION */
#define NTFS_INDEX_ENTRY_NODE           0x0001 /* Has Sub-Node VCN */
#define NTFS_INDEX_ENTRY_END            0x0002 /* Last Sentinel Entry */

#define NTFS_MAX_MFT_RECORD_SIZE        1024U
#define NTFS_MAX_EXTENTS                32
#define NTFS_MAX_FILENAME               64
#define NTFS_PERSISTENT_FSTAB_FILE      ".fstab"

/* 1. NTFS Volume Boot Record (VBR) - strictly 512 bytes */
struct ntfs_vbr {
    uint8_t  jump_boot[3];              /* 0x00: EB 52 90 */
    char     oem_id[8];                 /* 0x03: "NTFS    " */
    uint16_t bytes_per_sector;          /* 0x0B: 512 */
    uint8_t  sectors_per_cluster;       /* 0x0D: 8 (4096 bytes/cluster) */
    uint16_t reserved_sectors;          /* 0x0E: 0 */
    uint8_t  zero1[5];                  /* 0x10: 0 */
    uint8_t  media_descriptor;          /* 0x15: 0xF8 */
    uint16_t zero2;                     /* 0x16: 0 */
    uint16_t sectors_per_track;         /* 0x18 */
    uint16_t number_of_heads;           /* 0x1A */
    uint32_t hidden_sectors;            /* 0x1C */
    uint32_t zero3;                     /* 0x20 */
    uint32_t unused_drive_flags;        /* 0x24: 0x00800080 */
    uint64_t total_sectors;             /* 0x28: Total sectors in NTFS volume */
    uint64_t mft_lcn;                   /* 0x30: Starting LCN of $MFT */
    uint64_t mft_mirr_lcn;              /* 0x38: Starting LCN of $MFTMirr */
    int8_t   clusters_per_mft_record;   /* 0x40: Negative => 2^(-val) bytes (-10 => 1024) */
    uint8_t  reserved0[3];              /* 0x41 */
    int8_t   clusters_per_index_block;  /* 0x44: Index block size in clusters */
    uint8_t  reserved1[3];              /* 0x45 */
    uint64_t volume_serial_number;      /* 0x48: 64-bit NTFS Volume UUID */
    uint32_t checksum;                  /* 0x50 */
    uint8_t  bootstrap_code[426];       /* 0x54 */
    uint16_t boot_signature;            /* 0x1FE: 0xAA55 */
} __attribute__((packed));

/* 2. MFT Record Header (48 bytes in NTFS 3.1) */
struct ntfs_mft_record_header {
    char     magic[4];                  /* 0x00: "FILE" */
    uint16_t usa_offset;                /* 0x04: Offset to Update Sequence Array */
    uint16_t usa_count;                 /* 0x06: Number of 16-bit entries (USN + sectors) */
    uint64_t lsn;                       /* 0x08: $LogFile Sequence Number */
    uint16_t sequence_number;           /* 0x10: Reuse sequence counter */
    uint16_t hard_link_count;           /* 0x12: Directory link count */
    uint16_t attrs_offset;              /* 0x14: Offset to first attribute */
    uint16_t flags;                     /* 0x16: 0x01=InUse, 0x02=Directory */
    uint32_t bytes_in_use;              /* 0x18: Used bytes in MFT record */
    uint32_t bytes_allocated;           /* 0x1C: Allocated size (1024) */
    uint64_t base_mft_record;           /* 0x20: 0 for base record */
    uint16_t next_attr_id;              /* 0x28: Next available attribute ID */
    uint16_t align_pad;                 /* 0x2A: NTFS 3.1 alignment padding */
    uint32_t mft_record_number;         /* 0x2C: Self MFT record index */
} __attribute__((packed));

/* 3. Common Attribute Header (16 bytes) */
struct ntfs_attr_header {
    uint32_t type;                      /* 0x00: e.g. 0x30 ($FILE_NAME), 0x80 ($DATA) */
    uint32_t length;                    /* 0x04: Total attribute length (8-byte aligned) */
    uint8_t  non_resident;              /* 0x08: 0 = Resident, 1 = Non-Resident (Runlist) */
    uint8_t  name_length;               /* 0x09: Name length in UTF-16 code units */
    uint16_t name_offset;               /* 0x0A: Offset to UTF-16 attribute name */
    uint16_t flags;                     /* 0x0C: Compressed / Sparse / Encrypted */
    uint16_t attr_id;                   /* 0x0E: Unique attribute ID in record */
} __attribute__((packed));

/* 3a. Resident Attribute Header (24 bytes) */
struct ntfs_attr_resident {
    struct ntfs_attr_header common;
    uint32_t value_length;              /* 0x10: Exact payload byte length */
    uint16_t value_offset;              /* 0x14: Offset to payload */
    uint8_t  indexed_flag;              /* 0x16: 1 if indexed in directory */
    uint8_t  padding;                   /* 0x17 */
} __attribute__((packed));

/* 3b. Non-Resident Attribute Header (64 bytes) */
struct ntfs_attr_nonresident {
    struct ntfs_attr_header common;
    uint64_t start_vcn;                 /* 0x10: Starting Virtual Cluster Number */
    uint64_t last_vcn;                  /* 0x18: Ending Virtual Cluster Number */
    uint16_t runlist_offset;            /* 0x20: Offset to compressed runlist */
    uint16_t compression_unit;          /* 0x22: 0 for uncompressed */
    uint32_t padding;                   /* 0x24: 0 */
    uint64_t allocated_size;            /* 0x28: Allocated cluster size in bytes */
    uint64_t real_size;                 /* 0x30: Logical file size in bytes */
    uint64_t initialized_size;          /* 0x38: Initialized data length in bytes */
} __attribute__((packed));

/* 4. $STANDARD_INFORMATION Payload (48 bytes) */
struct ntfs_attr_stdinfo {
    uint64_t creation_time;
    uint64_t altered_time;
    uint64_t mft_changed_time;
    uint64_t read_time;
    uint32_t dos_permissions;
    uint32_t max_versions;
    uint32_t version_num;
    uint32_t class_id;
} __attribute__((packed));

/* 5. $FILE_NAME Attribute Payload (66-byte fixed header + UTF-16LE name) */
struct ntfs_attr_filename {
    uint64_t parent_directory;          /* Low 48 bits: Parent MFT Record, High 16 bits: Seq */
    uint64_t creation_time;
    uint64_t altered_time;
    uint64_t mft_changed_time;
    uint64_t read_time;
    uint64_t allocated_size;
    uint64_t real_size;
    uint32_t flags;
    uint32_t reparse_value;
    uint8_t  name_length;               /* Number of UTF-16LE code units */
    uint8_t  name_namespace;            /* POSIX / Win32 / DOS */
    uint16_t name[];                    /* UTF-16LE filename characters */
} __attribute__((packed));

/* 6. Directory Index Structures ($INDEX_ROOT 0x90 & $INDEX_ALLOCATION 0xA0) */
struct ntfs_index_header {
    uint32_t entries_offset;            /* Relative to start of ntfs_index_header */
    uint32_t index_length;              /* Used bytes including header */
    uint32_t allocated_size;            /* Available bytes */
    uint8_t  flags;                     /* 0x00 = Small Index, 0x01 = Large Index */
    uint8_t  reserved[3];
} __attribute__((packed));

struct ntfs_index_root {
    uint32_t attr_type;                 /* 0x30 ($FILE_NAME) */
    uint32_t collation_rule;            /* 0x01 (FILENAME) */
    uint32_t index_block_size;          /* Typically 4096 */
    int8_t   clusters_per_index_block;  /* Typically 1 */
    uint8_t  reserved[3];
    struct ntfs_index_header index_hdr;
} __attribute__((packed));

struct ntfs_indx_block_header {
    char     magic[4];                  /* "INDX" */
    uint16_t usa_offset;                /* 0x28 (40) */
    uint16_t usa_count;                 /* Sectors + 1 (e.g. 9 for 4096 B) */
    uint64_t lsn;
    uint64_t index_block_vcn;
    struct ntfs_index_header index_hdr; /* Starts at offset 0x18 (24) */
} __attribute__((packed));

struct ntfs_index_entry_header {
    uint64_t mft_reference;             /* Low 48 bits: File MFT Record, High 16 bits: Seq */
    uint16_t entry_length;              /* Total entry length (8-byte aligned) */
    uint16_t key_length;                /* Length of $FILE_NAME payload (unpadded) */
    uint16_t index_flags;               /* 0x01 = Subnode, 0x02 = End Sentinel */
    uint16_t reserved;
} __attribute__((packed));

/* Decoded Runlist Extent (Physical Cluster Range) */
struct ntfs_extent {
    uint64_t lcn;                       /* Physical Logical Cluster Number on volume */
    uint64_t cluster_count;             /* Number of contiguous clusters */
};

/* Mounted NTFS Volume Parameters */
struct ntfs_volume_info {
    uint8_t  mounted;
    uint8_t  newly_formatted;           /* 1 if formatted automatically on this boot */
    int      slot;                      /* Active ATA drive slot (0..3) */
    uint64_t part_start_lba;            /* 0 for raw volume, or MBR partition start LBA */
    uint16_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t mft_record_size;
    uint32_t index_block_size;
    uint64_t total_sectors;
    uint64_t total_clusters;
    uint64_t mft_lcn;
    uint64_t mft_mirr_lcn;
    uint64_t mft_start_lba;
    uint64_t mft_total_records;
    uint64_t volume_serial;
    char     uuid_str[17];              /* 16-char uppercase hex NTFS UUID */
    char     volume_label[32];          /* NTFS $Volume $VOLUME_NAME (e.g. VHD001) */
};

/* Non-destructive blkid Probe Summary for an ATA Drive Slot */
struct ntfs_probe_info {
    uint8_t  valid_ntfs;
    uint8_t  has_persistent_fstab;
    uint64_t part_start_lba;
    uint64_t volume_serial;
    char     uuid_str[17];
    char     volume_label[32];
};

/* File Entry Metadata for Directory Listing & Search */
struct ntfs_file_info {
    uint64_t mft_record;
    char     name[NTFS_MAX_FILENAME];
    uint64_t real_size;
    uint64_t allocated_size;
    uint8_t  is_directory;
    uint8_t  is_resident;
};

/* ============================================================================
 * Public NTFS Filesystem & Linux-Style Persistent Automount API
 * ============================================================================ */
int ntfs_init(void);
int ntfs_init_slot(int slot, int quiet);
int ntfs_automount(void);
int ntfs_format_slot(int slot, const char *volume_label);
int ntfs_probe_slot(int slot, struct ntfs_probe_info *out);

int ntfs_is_mounted(void);
const struct ntfs_volume_info *ntfs_get_volume_info(void);

int  ntfs_get_volume_label(char *out, size_t max_len);
int  ntfs_set_volume_label(const char *label);
void ntfs_format_uuid_hex(uint64_t serial, char out_hex17[17]);
int  ntfs_persist_fstab(const char *fstab_text);

/* Low-Level MFT & Runlist Operations */
int ntfs_read_mft_record(uint64_t record_num, void *buf_out);
int ntfs_write_mft_record(uint64_t record_num, const void *buf_in);
int ntfs_decode_runlist(const uint8_t *runlist, size_t max_bytes,
                        struct ntfs_extent *extents, int max_extents);
int ntfs_encode_runlist(const struct ntfs_extent *extents, int extent_count,
                        uint8_t *out_buf, size_t max_bytes);

/* File Read & Persistent Write Operations */
int     ntfs_find_file(const char *filename, struct ntfs_file_info *out_info);
int64_t ntfs_read_file(const char *filename, void *buffer, size_t max_size);
int     ntfs_write_file(const char *filename, const void *data, size_t size);
int     ntfs_create_file(const char *filename);
int     ntfs_write_or_create_file(const char *filename, const void *data, size_t size);
int     ntfs_list_root(struct ntfs_file_info *entries, int max_entries);

/* Diagnostics & Console Output */
void ntfs_print_info(void);
void ntfs_print_blkid(void);
void ntfs_print_root_dir(void);

#endif /* NTFS_H */
