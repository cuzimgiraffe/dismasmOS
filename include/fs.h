#ifndef FS_H
#define FS_H

#include "types.h"

#define FS_MAX_FILES 32
#define FS_MAX_FILENAME 32
#define FS_MAX_FILESIZE 2048

struct fs_file {
    char name[FS_MAX_FILENAME];
    char data[FS_MAX_FILESIZE];
    size_t size;
    uint8_t used;
};

void fs_init(void);
int fs_create(const char *name);
int fs_write(const char *name, const char *data, size_t size);
struct fs_file *fs_find(const char *name);
void fs_list(void);
size_t fs_used_bytes(void);
size_t fs_file_count(void);

#endif
