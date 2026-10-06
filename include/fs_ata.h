#ifndef FS_ATA_H
#define FS_ATA_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

int ata_wait_bsy(void);
int ata_wait_drq(void);
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);

int fs_ata_save_file(const char *name8, const void *data, uint32_t size);
int fs_ata_load_file(const char *name8, void *dest);
void fs_ata_list_toc(void);

extern uint8_t toc_buffer[512];
extern uint32_t last_io_lba;
extern uint8_t last_ata_status;

#ifdef __cplusplus
}
#endif

#endif
