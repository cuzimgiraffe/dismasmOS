#include "em.h"
#include "vga.h"
#include "kbd.h"
#include "fs.h"
#include "string.h"
#include "types.h"

#define EM_BUF_SIZE 2048
#define EM_TEXT_ROWS 23
#define EM_STATUS_ROW 23
#define EM_CMD_ROW 24

#define MODE_NORMAL 0
#define MODE_INSERT 1
#define MODE_COMMAND 2

static char buffer[EM_BUF_SIZE];
static size_t buf_len = 0;
static size_t cursor_pos = 0;
static int mode = MODE_NORMAL;
static char cmd_buf[32];
static size_t cmd_len = 0;
static char msg_buf[80];
static const char *cur_filename = NULL;

static const char *dict[] = {
    "int", "char", "void", "return", "if", "else", "while", "for", "include", "define",
    "struct", "main", "static", "const", "unsigned", "signed", "sizeof", "typedef", "extern",
    "auto", "break", "continue", "default", "do", "double", "enum", "float", "goto", "long",
    "short", "switch", "union", "volatile", "case", "asm", "multiboot", "entry", "section",
    "align", "global", "type", "skip", "quad", "word", "byte",
    "echo", "cat", "ls", "grep", "touch", "uname", "reboot", "help", "lang", "clear",
    "pr", "em", "kill", "start", "status", "memrep",
    "service", "prf", "system", "shell", "kernel", "agent", "comment", "process",
    "the", "be", "to", "of", "and", "a", "in", "that", "have", "i", "it", "for", "not",
    "on", "with", "he", "as", "you", "do", "at", "this", "but", "his", "by", "from",
    "they", "we", "say", "her", "she", "or", "an", "will", "my", "one", "all", "would",
    "there", "their", "what", "so", "up", "out", "about", "who", "get", "which", "go",
    "me", "when", "make", "can", "like", "time", "no", "just", "him", "know", "take",
    "people", "into", "year", "your", "good", "some", "could", "them", "see", "other",
    "than", "then", "now", "look", "only", "come", "its", "over", "think", "also",
    "back", "after", "use", "two", "how", "our", "work", "first", "well", "way", "even",
    "new", "want", "because", "any", "these", "give", "day", "most", "us", "file", "text",
    "test", "root", "name", "data", "error", "write", "read", "open", "close", "memory",
    "line", "code", "exit", "buffer", "kib", "ram", "cpu", "bios", "vga", "boot",
    "bytes", "size", "null", "true", "false", "page", "table", "ptr", "hello", "world",
    "der", "die", "das", "und", "ist", "in", "den", "von", "zu", "mit", "sich", "des",
    "auf", "fuer", "im", "dem", "nicht", "ein", "eine", "einer", "eines", "einem", "einen",
    "als", "auch", "es", "an", "werden", "aus", "er", "hat", "dass", "sie", "nach",
    "wird", "bei", "um", "am", "sind", "noch", "wie", "ueber", "wir", "war", "hier",
    "hallo", "welt", "datei", "zeile", "speichern", "ende", "fehler", "neu", "bit",
    "prozesse", "prozess", "ja", "nein", "gut", "sehr", "mehr", "immer", "wenn", "dann",
    "nur", "alle", "kann", "man", "dazu", "machen", "sehen", "zeigen", "unter", "oben",
    NULL
};

static int is_word_char(char c) {
    return isalpha(c);
}

static int is_typo(const char *w, size_t len) {
    if (len < 2 || len >= 32) {
        return 0;
    }
    char lower[32];
    for (size_t i = 0; i < len; i++) {
        lower[i] = (char)tolower(w[i]);
    }
    lower[len] = '\0';

    for (size_t i = 0; dict[i] != NULL; i++) {
        if (strcmp(lower, dict[i]) == 0) {
            return 0;
        }
    }
    return 1;
}

static void draw_cell(size_t col, size_t row, char c, uint8_t fg, uint8_t bg) {
    if (col >= VGA_WIDTH || row >= VGA_HEIGHT) {
        return;
    }
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    uint8_t attr = (uint8_t)(fg | (bg << 4));
    vga[row * VGA_WIDTH + col] = (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

static void get_cursor_row_col(size_t *out_row, size_t *out_col) {
    size_t r = 0;
    size_t c = 0;
    for (size_t i = 0; i < cursor_pos && i < buf_len; i++) {
        if (buffer[i] == '\n') {
            r++;
            c = 0;
        } else {
            c++;
            if (c >= VGA_WIDTH) {
                c = 0;
                r++;
            }
        }
    }
    *out_row = r;
    *out_col = c;
}

static void em_render(void) {
    for (size_t r = 0; r < EM_TEXT_ROWS; r++) {
        for (size_t c = 0; c < VGA_WIDTH; c++) {
            draw_cell(c, r, ' ', VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
    }

    size_t r = 0;
    size_t c = 0;
    size_t i = 0;

    while (i < buf_len && r < EM_TEXT_ROWS) {
        if (is_word_char(buffer[i])) {
            size_t w_start = i;
            while (i < buf_len && is_word_char(buffer[i])) {
                i++;
            }
            size_t w_len = i - w_start;
            int typo = is_typo(buffer + w_start, w_len);

            for (size_t k = 0; k < w_len && r < EM_TEXT_ROWS; k++) {
                uint8_t fg = typo ? VGA_COLOR_LIGHT_RED : VGA_COLOR_WHITE;
                draw_cell(c, r, buffer[w_start + k], fg, VGA_COLOR_BLACK);
                c++;
                if (c >= VGA_WIDTH) {
                    c = 0;
                    r++;
                }
            }
        } else if (buffer[i] == '\n') {
            r++;
            c = 0;
            i++;
        } else {
            draw_cell(c, r, buffer[i], VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            c++;
            if (c >= VGA_WIDTH) {
                c = 0;
                r++;
            }
            i++;
        }
    }

    for (size_t col = 0; col < VGA_WIDTH; col++) {
        draw_cell(col, EM_STATUS_ROW, ' ', VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    }

    char status[80];
    status[0] = '\0';
    if (mode == MODE_NORMAL) {
        strcat(status, "[NORMAL] ");
    } else if (mode == MODE_INSERT) {
        strcat(status, "[INSERT] ");
    } else {
        strcat(status, "[COMMAND] ");
    }
    strcat(status, cur_filename);
    strcat(status, " | ");

    char num[16];
    size_t cur_r = 0;
    size_t cur_c = 0;
    get_cursor_row_col(&cur_r, &cur_c);
    itoa((int)cur_r + 1, num);
    strcat(status, num);
    strcat(status, ":");
    itoa((int)cur_c + 1, num);
    strcat(status, num);
    strcat(status, " | ");
    itoa((int)buf_len, num);
    strcat(status, num);
    strcat(status, " B | ;q ;s ;wsc ;-m");

    for (size_t col = 0; status[col] && col < VGA_WIDTH; col++) {
        draw_cell(col, EM_STATUS_ROW, status[col], VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    }

    for (size_t col = 0; col < VGA_WIDTH; col++) {
        draw_cell(col, EM_CMD_ROW, ' ', VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }

    if (mode == MODE_COMMAND) {
        draw_cell(0, EM_CMD_ROW, ';', VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        for (size_t col = 0; col < cmd_len && col < VGA_WIDTH - 1; col++) {
            draw_cell(col + 1, EM_CMD_ROW, cmd_buf[col], VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
        vga_update_cursor((int)cmd_len + 1, EM_CMD_ROW);
    } else {
        for (size_t col = 0; msg_buf[col] && col < VGA_WIDTH; col++) {
            draw_cell(col, EM_CMD_ROW, msg_buf[col], VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        }
        if (cur_r < EM_TEXT_ROWS) {
            vga_update_cursor((int)cur_c, (int)cur_r);
        } else {
            vga_update_cursor(0, EM_TEXT_ROWS - 1);
        }
    }
}

static void em_save(void) {
    if (!fs_find(cur_filename)) {
        if (fs_create(cur_filename) != 0) {
            strcpy(msg_buf, "error: cannot create file");
            return;
        }
    }
    fs_write(cur_filename, buffer, buf_len);
    char num[16];
    strcpy(msg_buf, "saved ");
    strcat(msg_buf, cur_filename);
    strcat(msg_buf, " (");
    itoa((int)buf_len, num);
    strcat(msg_buf, num);
    strcat(msg_buf, " B)");
}

static void em_hop_error(void) {
    size_t search_start = cursor_pos;
    if (search_start < buf_len && is_word_char(buffer[search_start])) {
        while (search_start < buf_len && is_word_char(buffer[search_start])) {
            search_start++;
        }
    }

    size_t i = search_start;
    while (i < buf_len) {
        if (is_word_char(buffer[i])) {
            size_t w_start = i;
            while (i < buf_len && is_word_char(buffer[i])) {
                i++;
            }
            if (is_typo(buffer + w_start, i - w_start)) {
                cursor_pos = w_start;
                strcpy(msg_buf, "hop to error: '");
                size_t len = i - w_start;
                if (len > 16) len = 16;
                char w_sub[20];
                strncpy(w_sub, buffer + w_start, len);
                w_sub[len] = '\0';
                strcat(msg_buf, w_sub);
                strcat(msg_buf, "'");
                return;
            }
        } else {
            i++;
        }
    }

    i = 0;
    while (i < search_start) {
        if (is_word_char(buffer[i])) {
            size_t w_start = i;
            while (i < search_start && is_word_char(buffer[i])) {
                i++;
            }
            if (is_typo(buffer + w_start, i - w_start)) {
                cursor_pos = w_start;
                strcpy(msg_buf, "hop to error (wrap): '");
                size_t len = i - w_start;
                if (len > 16) len = 16;
                char w_sub[20];
                strncpy(w_sub, buffer + w_start, len);
                w_sub[len] = '\0';
                strcat(msg_buf, w_sub);
                strcat(msg_buf, "'");
                return;
            }
        } else {
            i++;
        }
    }

    strcpy(msg_buf, "no typos found");
}

static int execute_em_command(void) {
    cmd_buf[cmd_len] = '\0';
    if (strcmp(cmd_buf, "q") == 0 || strcmp(cmd_buf, ";q") == 0) {
        return 1;
    } else if (strcmp(cmd_buf, "s") == 0 || strcmp(cmd_buf, ";s") == 0) {
        em_save();
    } else if (strcmp(cmd_buf, "wsc") == 0 || strcmp(cmd_buf, ";wsc") == 0) {
        em_save();
        return 1;
    } else if (strcmp(cmd_buf, "-m") == 0 || strcmp(cmd_buf, ";-m") == 0) {
        em_hop_error();
    } else {
        strcpy(msg_buf, "unknown command: ;");
        strcat(msg_buf, cmd_buf);
    }
    mode = MODE_NORMAL;
    cmd_len = 0;
    return 0;
}

void em_run(const char *filename) {
    cur_filename = filename;
    buf_len = 0;
    cursor_pos = 0;
    mode = MODE_NORMAL;
    cmd_len = 0;
    msg_buf[0] = '\0';

    struct fs_file *f = fs_find(filename);
    if (f) {
        buf_len = f->size;
        if (buf_len >= EM_BUF_SIZE) {
            buf_len = EM_BUF_SIZE - 1;
        }
        memcpy(buffer, f->data, buf_len);
        buffer[buf_len] = '\0';
    } else {
        buffer[0] = '\0';
        buf_len = 0;
    }

    em_render();

    while (1) {
        char c = kbd_getchar();

        if (mode == MODE_COMMAND) {
            if (c == 27) {
                mode = MODE_NORMAL;
                cmd_len = 0;
                msg_buf[0] = '\0';
            } else if (c == '\n') {
                if (execute_em_command()) {
                    break;
                }
            } else if (c == '\b') {
                if (cmd_len > 0) {
                    cmd_len--;
                } else {
                    mode = MODE_NORMAL;
                }
            } else if ((uint8_t)c >= 32 && (uint8_t)c != 127) {
                if (cmd_len < sizeof(cmd_buf) - 1) {
                    cmd_buf[cmd_len++] = c;
                }
            }
        } else if (mode == MODE_NORMAL) {
            if (c == 'i') {
                mode = MODE_INSERT;
                msg_buf[0] = '\0';
            } else if (c == ';') {
                mode = MODE_COMMAND;
                cmd_len = 0;
                msg_buf[0] = '\0';
            } else if (c == 'x') {
                if (cursor_pos < buf_len) {
                    for (size_t k = cursor_pos; k < buf_len - 1; k++) {
                        buffer[k] = buffer[k + 1];
                    }
                    buf_len--;
                    buffer[buf_len] = '\0';
                }
            } else if (c == 'h') {
                if (cursor_pos > 0) {
                    cursor_pos--;
                }
            } else if (c == 'l') {
                if (cursor_pos < buf_len) {
                    cursor_pos++;
                }
            } else if (c == 'k') {
                size_t p = cursor_pos;
                while (p > 0 && buffer[p - 1] != '\n') {
                    p--;
                }
                if (p > 0) {
                    size_t prev_line_end = p - 1;
                    size_t prev_line_start = prev_line_end;
                    while (prev_line_start > 0 && buffer[prev_line_start - 1] != '\n') {
                        prev_line_start--;
                    }
                    size_t col_offset = cursor_pos - p;
                    size_t prev_line_len = prev_line_end - prev_line_start;
                    if (col_offset > prev_line_len) {
                        col_offset = prev_line_len;
                    }
                    cursor_pos = prev_line_start + col_offset;
                }
            } else if (c == 'j') {
                size_t p = cursor_pos;
                while (p < buf_len && buffer[p] != '\n') {
                    p++;
                }
                if (p < buf_len) {
                    size_t line_start = cursor_pos;
                    while (line_start > 0 && buffer[line_start - 1] != '\n') {
                        line_start--;
                    }
                    size_t col_offset = cursor_pos - line_start;
                    size_t next_line_start = p + 1;
                    size_t next_line_end = next_line_start;
                    while (next_line_end < buf_len && buffer[next_line_end] != '\n') {
                        next_line_end++;
                    }
                    size_t next_line_len = next_line_end - next_line_start;
                    if (col_offset > next_line_len) {
                        col_offset = next_line_len;
                    }
                    cursor_pos = next_line_start + col_offset;
                }
            }
        } else if (mode == MODE_INSERT) {
            if (c == 27) {
                mode = MODE_NORMAL;
            } else if (c == '\b') {
                if (cursor_pos > 0) {
                    for (size_t k = cursor_pos - 1; k < buf_len - 1; k++) {
                        buffer[k] = buffer[k + 1];
                    }
                    buf_len--;
                    cursor_pos--;
                    buffer[buf_len] = '\0';
                }
            } else if (c == '\n') {
                if (buf_len < EM_BUF_SIZE - 1) {
                    for (size_t k = buf_len; k > cursor_pos; k--) {
                        buffer[k] = buffer[k - 1];
                    }
                    buffer[cursor_pos] = '\n';
                    buf_len++;
                    cursor_pos++;
                    buffer[buf_len] = '\0';
                }
            } else if ((uint8_t)c >= 32 && (uint8_t)c != 127) {
                if (buf_len < EM_BUF_SIZE - 1) {
                    for (size_t k = buf_len; k > cursor_pos; k--) {
                        buffer[k] = buffer[k - 1];
                    }
                    buffer[cursor_pos] = c;
                    buf_len++;
                    cursor_pos++;
                    buffer[buf_len] = '\0';
                }
            }
        }

        em_render();
    }
}
