#include "fs.h"
#include "string.h"
#include "vga.h"

static struct fs_file files[FS_MAX_FILES];

void fs_init(void) {
    memset(files, 0, sizeof(files));

    fs_create("readme.txt");
    const char *readme_text = "Welcome to dismasmOS.\nA minimal and efficient x86 CLI operating system.\nEnjoy pure bare-metal performance.\n";
    fs_write("readme.txt", readme_text, strlen(readme_text));

    fs_create("kernel.sys");
    const char *sys_text = "OS_NAME=dismasmOS\nVERSION=1.0\nARCH=x86_32\nBOOT=MULTIBOOT1\nSHELL=BUILTIN\n";
    fs_write("kernel.sys", sys_text, strlen(sys_text));

    fs_create("hardware.txt");
    const char *hw_text = "CPU=x86 Protected Mode\nVIDEO=VGA Text 80x25\nINPUT=PS2 Keyboard IRQ1\nPIC=8259 Remapped\n";
    fs_write("hardware.txt", hw_text, strlen(hw_text));
}

int fs_create(const char *name) {
    if (fs_find(name) != NULL) {
        return -1;
    }

    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            files[i].used = 1;
            strncpy(files[i].name, name, FS_MAX_FILENAME - 1);
            files[i].name[FS_MAX_FILENAME - 1] = '\0';
            files[i].size = 0;
            files[i].data[0] = '\0';
            return 0;
        }
    }
    return -1;
}

struct fs_file *fs_find(const char *name) {
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && strcmp(files[i].name, name) == 0) {
            return &files[i];
        }
    }
    return NULL;
}

int fs_write(const char *name, const char *data, size_t size) {
    struct fs_file *file = fs_find(name);
    if (!file) {
        return -1;
    }
    if (size >= FS_MAX_FILESIZE) {
        size = FS_MAX_FILESIZE - 1;
    }
    memcpy(file->data, data, size);
    file->data[size] = '\0';
    file->size = size;
    return 0;
}

void fs_list(void) {
    char num_buf[16];
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used) {
            vga_puts(files[i].name);
            vga_puts("  ");
            itoa((int)files[i].size, num_buf);
            vga_puts(num_buf);
            vga_puts(" B\n");
        }
    }
}

size_t fs_used_bytes(void) {
    size_t total = 0;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used) {
            total += files[i].size;
        }
    }
    return total;
}

size_t fs_file_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used) {
            count++;
        }
    }
    return count;
}
