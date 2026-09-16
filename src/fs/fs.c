#include "fs.h"
#include "string.h"
#include "vga.h"

static struct fs_file files[FS_MAX_FILES];
static struct fs_change change_log[FS_MAX_CHANGES];
static size_t change_count = 0;

void fs_log_change(const char *type, const char *user, const char *filename) {
    if (change_count >= FS_MAX_CHANGES) {
        /* Shift older entries */
        for (size_t i = 0; i < FS_MAX_CHANGES - 1; i++) {
            change_log[i] = change_log[i + 1];
        }
        change_count = FS_MAX_CHANGES - 1;
    }
    strncpy(change_log[change_count].type, type, sizeof(change_log[change_count].type) - 1);
    change_log[change_count].type[sizeof(change_log[change_count].type) - 1] = '\0';

    strncpy(change_log[change_count].user, user, sizeof(change_log[change_count].user) - 1);
    change_log[change_count].user[sizeof(change_log[change_count].user) - 1] = '\0';

    strncpy(change_log[change_count].filename, filename, sizeof(change_log[change_count].filename) - 1);
    change_log[change_count].filename[sizeof(change_log[change_count].filename) - 1] = '\0';

    change_count++;
}

void fs_show_changes(void) {
    if (change_count == 0) {
        vga_puts("No filesystem changes recorded yet.\n");
        return;
    }
    for (size_t i = 0; i < change_count; i++) {
        const char *verb = "Viewed";
        if (strcmp(change_log[i].type, "EDITED") == 0) {
            verb = "Edited";
        } else if (strcmp(change_log[i].type, "DELETED") == 0) {
            verb = "Deleted";
        } else if (strcmp(change_log[i].type, "CREATED") == 0) {
            verb = "Created";
        }

        vga_puts("[");
        if (strcmp(change_log[i].type, "VIEW") == 0) {
            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        } else if (strcmp(change_log[i].type, "EDITED") == 0) {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        } else if (strcmp(change_log[i].type, "CREATED") == 0) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        } else {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        }
        vga_puts(change_log[i].type);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("]-[");
        vga_set_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
        vga_puts(change_log[i].user);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("] - ");
        vga_puts(verb);
        vga_puts(" ");
        vga_puts(change_log[i].filename);
        vga_putchar('\n');
    }
}

void fs_resolve_path(const char *cwd, const char *path, char *out_path) {
    char temp[FS_MAX_PATH * 2];

    if (!path || path[0] == '\0') {
        if (!cwd || cwd[0] == '\0') {
            strcpy(out_path, "/");
        } else {
            strncpy(out_path, cwd, FS_MAX_PATH - 1);
            out_path[FS_MAX_PATH - 1] = '\0';
        }
        return;
    }

    if (path[0] == '/') {
        strncpy(temp, path, sizeof(temp) - 1);
        temp[sizeof(temp) - 1] = '\0';
    } else {
        if (!cwd || strcmp(cwd, "/") == 0) {
            temp[0] = '/';
            temp[1] = '\0';
            strncat(temp, path, sizeof(temp) - 2);
        } else {
            strncpy(temp, cwd, sizeof(temp) - 1);
            temp[sizeof(temp) - 1] = '\0';
            size_t clen = strlen(temp);
            if (clen < sizeof(temp) - 2) {
                temp[clen] = '/';
                temp[clen + 1] = '\0';
                strncat(temp, path, sizeof(temp) - clen - 2);
            }
        }
    }

    /* Tokenize components and resolve . and .. */
    char *comps[16];
    int comp_count = 0;
    char *p = temp;

    while (*p) {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *start = p;
        while (*p && *p != '/') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }

        if (strcmp(start, ".") == 0) {
            continue;
        } else if (strcmp(start, "..") == 0) {
            if (comp_count > 0) {
                comp_count--;
            }
        } else {
            if (comp_count < 16) {
                comps[comp_count++] = start;
            }
        }
    }

    if (comp_count == 0) {
        strcpy(out_path, "/");
        return;
    }

    out_path[0] = '\0';
    for (int i = 0; i < comp_count; i++) {
        size_t len = strlen(out_path);
        if (len + 1 + strlen(comps[i]) < FS_MAX_PATH) {
            out_path[len] = '/';
            out_path[len + 1] = '\0';
            strcat(out_path, comps[i]);
        }
    }
}

static struct fs_file *fs_find_slot(void) {
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            return &files[i];
        }
    }
    return NULL;
}

struct fs_file *fs_find(const char *path) {
    if (!path) return NULL;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && strcmp(files[i].path, path) == 0) {
            return &files[i];
        }
    }
    return NULL;
}

int fs_is_dir(const char *path) {
    if (strcmp(path, "/") == 0) {
        return 1;
    }
    struct fs_file *f = fs_find(path);
    return (f && f->is_dir);
}

int fs_is_system(const char *path) {
    if (!path) return 0;
    if (strncmp(path, "/boot", 5) == 0 || strncmp(path, "/krnl", 5) == 0) {
        return 1;
    }
    struct fs_file *f = fs_find(path);
    return (f && f->is_system);
}

int fs_mkdir(const char *path) {
    if (fs_find(path) != NULL || strcmp(path, "/") == 0) {
        return -1;
    }
    struct fs_file *f = fs_find_slot();
    if (!f) {
        return -1;
    }
    f->used = 1;
    f->is_dir = 1;
    f->size = 0;
    f->data[0] = '\0';
    if (strncmp(path, "/boot", 5) == 0 || strncmp(path, "/krnl", 5) == 0) {
        f->is_system = 1;
    } else {
        f->is_system = 0;
    }
    strncpy(f->path, path, FS_MAX_PATH - 1);
    f->path[FS_MAX_PATH - 1] = '\0';
    return 0;
}

int fs_create(const char *path) {
    if (fs_find(path) != NULL) {
        return -1;
    }
    struct fs_file *f = fs_find_slot();
    if (!f) {
        return -1;
    }
    f->used = 1;
    f->is_dir = 0;
    f->size = 0;
    f->data[0] = '\0';
    if (strncmp(path, "/boot", 5) == 0 || strncmp(path, "/krnl", 5) == 0) {
        f->is_system = 1;
    } else {
        f->is_system = 0;
    }
    strncpy(f->path, path, FS_MAX_PATH - 1);
    f->path[FS_MAX_PATH - 1] = '\0';
    return 0;
}

int fs_write(const char *path, const char *data, size_t size) {
    struct fs_file *file = fs_find(path);
    if (!file || file->is_dir) {
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

int fs_delete(const char *path) {
    struct fs_file *file = fs_find(path);
    if (!file) {
        return -1;
    }
    file->used = 0;
    file->path[0] = '\0';
    file->data[0] = '\0';
    file->size = 0;
    return 0;
}

static void print_pad(const char *s, int pad) {
    int len = (int)strlen(s);
    vga_puts(s);
    while (len < pad) {
        vga_putchar(' ');
        len++;
    }
}

static void print_hex_val(uint64_t val, int pad) {
    char hex[17];
    utoa_hex(val, hex);
    char buf[20];
    strcpy(buf, "0x");
    int z = 8 - (int)strlen(hex);
    for (int i = 0; i < z; i++) {
        strcat(buf, "0");
    }
    strcat(buf, hex);
    print_pad(buf, pad);
}

void fs_list(const char *dir_path, int show_size, int show_perm) {
    char base[FS_MAX_PATH];
    strncpy(base, dir_path, FS_MAX_PATH - 1);
    base[FS_MAX_PATH - 1] = '\0';

    size_t base_len = strlen(base);
    int is_root = (strcmp(base, "/") == 0);

    if (show_perm || show_size) {
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        if (show_perm && show_size) {
            print_pad("Permissions", 14);
            print_pad("Rights", 18);
            print_pad("Size (KiB / B)", 18);
            print_pad("Size (Hex)", 14);
            vga_puts("Name\n");
            vga_puts("----------------------------------------------------------------------\n");
        } else if (show_perm) {
            print_pad("Permissions", 14);
            print_pad("Rights", 20);
            vga_puts("Name\n");
            vga_puts("--------------------------------------------------\n");
        } else {
            print_pad("Size (KiB / B)", 18);
            print_pad("Size (Hex)", 14);
            vga_puts("Name\n");
            vga_puts("--------------------------------------------------\n");
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }

    size_t count = 0;

    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            continue;
        }
        if (strcmp(files[i].path, base) == 0) {
            continue;
        }

        const char *rel_name = NULL;

        if (is_root) {
            if (files[i].path[0] == '/') {
                const char *after = files[i].path + 1;
                if (strchr(after, '/') == NULL) {
                    rel_name = after;
                }
            }
        } else {
            if (strncmp(files[i].path, base, base_len) == 0 && files[i].path[base_len] == '/') {
                const char *after = files[i].path + base_len + 1;
                if (strchr(after, '/') == NULL) {
                    rel_name = after;
                }
            }
        }

        if (!rel_name || *rel_name == '\0') {
            continue;
        }

        count++;

        if (show_perm) {
            if (files[i].is_dir) {
                vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                print_pad("drwxr-xr-x", 14);
            } else if (files[i].is_system) {
                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                print_pad("-rwxr-xr-x", 14);
            } else {
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                print_pad("-rw-r--r--", 14);
            }
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            print_pad("[root:admin rwx]", 18);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }

        if (show_size) {
            if (files[i].is_dir) {
                vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                print_pad("<DIR>", 18);
                print_pad("-", 14);
            } else {
                char sbuf[32];
                char nbuf[16];
                uint64_t kib = (files[i].size + 1023) / 1024;
                itoa((int)kib, nbuf);
                strcpy(sbuf, nbuf);
                strcat(sbuf, " KiB (");
                itoa((int)files[i].size, nbuf);
                strcat(sbuf, nbuf);
                strcat(sbuf, " B)");
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                print_pad(sbuf, 18);
                print_hex_val(files[i].size, 14);
            }
        }

        if (files[i].is_dir) {
            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            vga_puts(rel_name);
            vga_puts("/\n");
        } else if (files[i].is_system) {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            vga_puts(rel_name);
            vga_putchar('\n');
        } else {
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_puts(rel_name);
            vga_putchar('\n');
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }

    if (count == 0 && (show_perm || show_size)) {
        vga_puts("(empty directory)\n");
    }
}

size_t fs_used_bytes(void) {
    size_t total = 0;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && !files[i].is_dir) {
            total += files[i].size;
        }
    }
    return total;
}

size_t fs_file_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && !files[i].is_dir) {
            count++;
        }
    }
    return count;
}

void fs_init(void) {
    memset(files, 0, sizeof(files));
    change_count = 0;

    /* Base directories */
    fs_mkdir("/home");
    fs_mkdir("/boot");
    fs_mkdir("/krnl");
    fs_mkdir("/shell");
    fs_mkdir("/etc");

    /* Boot files in /boot */
    fs_create("/boot/grub.cfg");
    const char *grub_cfg = "set timeout=5\nset default=0\nmenuentry \"dismasmOS 1.2 x86_64\" {\n    multiboot /krnl/kernel.bin\n    boot\n}\n";
    fs_write("/boot/grub.cfg", grub_cfg, strlen(grub_cfg));

    fs_create("/boot/boot.s");
    const char *boot_s = "[bits 32]\nglobal _start\n_start:\n    cli\n    jmp kernel_entry\n";
    fs_write("/boot/boot.s", boot_s, strlen(boot_s));

    /* Kernel files in /krnl */
    fs_create("/krnl/kernel.sys");
    const char *kernel_sys = "OS_NAME=dismasmOS\nVERSION=1.2\nARCH=x86_64\nMODE=LONG_MODE_64BIT\nKERNEL_BASE=0x00100000\nPAGING=IDENTITY_1GIB\n";
    fs_write("/krnl/kernel.sys", kernel_sys, strlen(kernel_sys));

    fs_create("/krnl/system.map");
    const char *sys_map = "00100000 T _start\n00100100 T kmain\n00101000 T idt_init\n00102000 T vga_init\n";
    fs_write("/krnl/system.map", sys_map, strlen(sys_map));

    /* Shell files in /shell */
    fs_create("/shell/sh.cfg");
    const char *sh_cfg = "PROMPT=root@admin@dismasmOS\nCOLOR=DEFAULT\nHISTORY=ENABLED\nAUTO_COMPLETE=OFF\n";
    fs_write("/shell/sh.cfg", sh_cfg, strlen(sh_cfg));

    fs_create("/shell/motd");
    const char *motd = "Welcome to dismasmOS 1.2 64-Bit Microkernel Shell.\nType 'help' for available commands.\n";
    fs_write("/shell/motd", motd, strlen(motd));

    fs_create("/shell/aliases");
    const char *aliases = "alias cls=clear\nalias lsf=ls\nalias dir=lsf\n";
    fs_write("/shell/aliases", aliases, strlen(aliases));

    /* Etc files in /etc */
    fs_create("/etc/os-release");
    const char *os_release = "NAME=\"dismasmOS\"\nVERSION=\"1.2\"\nID=dismasm\nPRETTY_NAME=\"dismasmOS 1.2 x86_64\"\n";
    fs_write("/etc/os-release", os_release, strlen(os_release));

    fs_create("/etc/hostname");
    const char *hostname = "dismasm-box\n";
    fs_write("/etc/hostname", hostname, strlen(hostname));

    fs_create("/etc/fstab");
    const char *fstab = "# <file system> <mount point> <type> <options>\nramfs           /             ramfs  rw,relatime 0 0\n";
    fs_write("/etc/fstab", fstab, strlen(fstab));
}
