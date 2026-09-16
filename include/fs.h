#ifndef FS_H
#define FS_H

#include "types.h"

#define FS_MAX_FILES 64
#define FS_MAX_PATH 64
#define FS_MAX_FILESIZE 2048
#define FS_MAX_CHANGES 64

struct fs_file {
    char path[FS_MAX_PATH];
    char data[FS_MAX_FILESIZE];
    size_t size;
    uint8_t is_dir;
    uint8_t is_system;
    uint8_t used;
};

struct fs_change {
    char type[10];     /* VIEW, EDITED, DELETED, CREATED */
    char user[16];     /* root */
    char filename[FS_MAX_PATH];
};

void fs_init(void);
int fs_create(const char *path);
int fs_mkdir(const char *path);
int fs_write(const char *path, const char *data, size_t size);
int fs_delete(const char *path);
struct fs_file *fs_find(const char *path);
int fs_is_dir(const char *path);
int fs_is_system(const char *path);

void fs_resolve_path(const char *cwd, const char *path, char *out_path);
void fs_list(const char *dir_path, int show_size, int show_perm);
size_t fs_used_bytes(void);
size_t fs_file_count(void);

void fs_log_change(const char *type, const char *user, const char *filename);
void fs_show_changes(void);

#endif
