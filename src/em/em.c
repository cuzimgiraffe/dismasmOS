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

#define MODE_EDIT 0
#define MODE_COMMAND 1

enum em_file_type {
    FT_GENERIC = 0,
    FT_BASH,
    FT_PYTHON,
    FT_MARKDOWN
};

static char buffer[EM_BUF_SIZE];
static size_t buf_len = 0;
static size_t cursor_pos = 0;
static int mode = MODE_EDIT;
static int cur_file_type = FT_GENERIC;
static char cmd_buf[32];
static size_t cmd_len = 0;
static char msg_buf[80];
static const char *cur_filename = NULL;

/* 1. Bash Shell Scripting Dictionary */
static const char *dict_bash[] = {
    /* Builtins & Keywords */
    "if", "then", "else", "elif", "fi", "case", "esac", "for", "select", "while", "until",
    "do", "done", "in", "function", "time", "echo", "printf", "read", "cd", "pwd",
    "set", "unset", "export", "alias", "unalias", "source", "exec", "trap", "eval",
    "shift", "exit", "return", "declare", "typeset", "local", "readonly", "shopt",
    "test", "true", "false", "builtin", "command", "type", "hash", "bind", "help",
    "let", "complete", "compgen", "mapfile", "readarray", "enable", "disown", "suspend",
    "times", "umask", "wait", "kill", "bg", "fg", "jobs", "fc", "history",
    /* Coreutils & UNIX CLI */
    "cat", "grep", "sed", "awk", "cut", "sort", "uniq", "wc", "tr", "tee", "head", "tail",
    "find", "xargs", "chmod", "chown", "chgrp", "mkdir", "rmdir", "cp", "mv", "rm",
    "touch", "ln", "ls", "lsf", "ps", "top", "df", "du", "free", "sleep", "which",
    "whereis", "curl", "wget", "tar", "gzip", "gunzip", "zip", "unzip", "uname",
    "whoami", "id", "date", "basename", "dirname", "diff", "patch", "sudo", "su",
    "apt", "yum", "git", "make", "clear", "env", "ssh", "scp", "rsync", "ping",
    "systemctl", "reboot", "shutdown", "dmesg", "service", "makedir", "changes",
    /* Variables & Environment */
    "path", "home", "user", "shell", "term", "lang", "ifs", "reply", "args", "status",
    "stdin", "stdout", "stderr", "null", "pipe", "stream", "script", "file", "dir",
    "var", "name", "val", "value", "count", "line", "text", "string", "num", "loop",
    "item", "key", "flag", "opt", "option", "param", "error", "log", "temp", "tmp",
    "bin", "usr", "etc", "dev", "proc", "sys",
    NULL
};

/* 2. Python Language Dictionary */
static const char *dict_python[] = {
    /* Keywords */
    "and", "as", "assert", "async", "await", "break", "class", "continue", "def",
    "del", "elif", "else", "except", "finally", "for", "from", "global", "if",
    "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise",
    "return", "try", "while", "with", "yield", "true", "false", "none", "match", "case",
    /* Builtins & Common Methods */
    "print", "len", "range", "str", "int", "float", "list", "dict", "set", "tuple",
    "bool", "bytes", "type", "object", "input", "open", "read", "write", "close",
    "seek", "tell", "readline", "readlines", "append", "extend", "insert", "remove",
    "pop", "clear", "index", "count", "sort", "reverse", "copy", "keys", "values",
    "items", "get", "update", "split", "join", "strip", "lstrip", "rstrip", "replace",
    "find", "format", "lower", "upper", "title", "startswith", "endswith", "encode",
    "decode", "sum", "min", "max", "abs", "round", "pow", "enumerate", "zip", "map",
    "filter", "sorted", "reversed", "all", "any", "isinstance", "issubclass", "super",
    "self", "cls", "init", "main", "repr", "iter", "next", "dir", "help", "vars",
    "callable", "hasattr", "getattr", "setattr", "delattr", "property",
    /* Exceptions & Modules */
    "exception", "error", "valueerror", "typeerror", "indexerror", "keyerror",
    "ioerror", "oserror", "runtimeerror", "syntaxerror", "attributeerror",
    "nameerror", "importerror", "keyboardinterrupt", "sys", "os", "math", "re",
    "json", "time", "datetime", "random", "path", "collections", "itertools",
    "functools", "typing", "optional", "union", "subprocess", "argparse", "logging",
    "unittest", "threading", "socket",
    /* Common Identifiers */
    "data", "item", "result", "results", "response", "payload", "buffer", "stream",
    "chunk", "config", "params", "args", "kwargs", "target", "source", "node",
    "tree", "table", "column", "row", "size", "length", "total", "valid", "invalid",
    "success", "failure", "debug", "info", "warn", "warning",
    NULL
};

/* 3. Markdown Documentation Dictionary */
static const char *dict_markdown[] = {
    /* Document & Formatting Vocabulary */
    "markdown", "heading", "header", "subheading", "title", "summary", "abstract",
    "overview", "architecture", "specification", "specifications", "implementation",
    "manual", "reference", "references", "guide", "guidelines", "changelog", "changes",
    "features", "usage", "installation", "requirements", "prerequisites", "setup",
    "configuration", "build", "compile", "running", "run", "test", "tests", "testing",
    "benchmark", "license", "contributing", "author", "version", "release", "notes",
    "description", "details", "example", "examples", "sample", "code", "syntax",
    "table", "tables", "list", "lists", "link", "links", "url", "image", "images",
    "badge", "badges", "quote", "quotes", "blockquote", "block", "blocks", "codeblock",
    "inline", "bold", "italic", "strikethrough", "section", "sections", "subsection",
    "item", "items", "bullet", "numbered", "task", "tasks", "todo", "warning", "note",
    "tip", "important", "caution", "notice", "output", "input", "parameter", "parameters",
    "argument", "arguments", "status", "error", "result", "command", "commands",
    "terminal", "console", "shell", "bash", "python", "commit", "push", "pull",
    "branch", "repo", "repository", "github", "git", "text", "file", "files",
    "directory", "project", "system", "readme", "doc", "docs", "documentation",
    "api", "cli", "web", "server", "client", "platform", "kernel", "hardware",
    "software", "memory", "cpu", "device", "driver", "process", "thread", "network",
    "packet", "payload", "buffer", "byte", "bytes", "bit", "bits", "kib", "mib", "gib",
    "ram", "rom", "vga", "bios", "boot", "bootloader", "grub", "entry", "registers",
    "assembly", "source", "binary", "target", "default", "active", "enabled",
    "disabled", "passed", "check", "checklist", "verified", "step", "steps", "stage",
    "stages", "module", "modules", "library", "package", "dependency", "dependencies",
    "standard", "format", "layout", "report", "performance", "clean", "update",
    "fixed", "removed", "added",
    NULL
};

/* 4. C & Kernel System Dictionary */
static const char *dict_c_system[] = {
    "int", "char", "void", "return", "if", "else", "while", "for", "include", "define",
    "struct", "main", "static", "const", "unsigned", "signed", "sizeof", "typedef", "extern",
    "auto", "break", "continue", "default", "do", "double", "enum", "float", "goto", "long",
    "short", "switch", "union", "volatile", "case", "asm", "multiboot", "entry", "section",
    "align", "global", "type", "skip", "quad", "word", "byte", "vga", "pic", "idt", "gdt",
    "kbd", "io", "proc", "fs", "service", "prf", "memrep",
    NULL
};

/* 5. Common Multilingual Words (English & German) */
static const char *dict_common[] = {
    "the", "be", "to", "of", "and", "a", "in", "that", "have", "i", "it", "for", "not",
    "on", "with", "he", "as", "you", "do", "at", "this", "but", "his", "by", "from",
    "they", "we", "say", "her", "she", "or", "an", "will", "my", "one", "all", "would",
    "there", "their", "what", "so", "up", "out", "about", "who", "get", "which", "go",
    "me", "when", "make", "can", "like", "time", "no", "just", "him", "know", "take",
    "people", "into", "year", "your", "good", "some", "could", "them", "see", "other",
    "than", "then", "now", "look", "only", "come", "its", "over", "think", "also",
    "back", "after", "use", "two", "how", "our", "work", "first", "well", "way", "even",
    "new", "want", "because", "any", "these", "give", "day", "most", "us", "root",
    "der", "die", "das", "und", "ist", "in", "den", "von", "zu", "mit", "sich", "des",
    "auf", "fuer", "im", "dem", "nicht", "ein", "eine", "einer", "eines", "einem", "einen",
    "als", "auch", "es", "an", "werden", "aus", "er", "hat", "dass", "sie", "nach",
    "wird", "bei", "um", "am", "sind", "noch", "wie", "ueber", "wir", "war", "hier",
    "hallo", "welt", "datei", "zeile", "speichern", "ende", "fehler", "neu", "bit",
    "prozesse", "prozess", "ja", "nein", "gut", "sehr", "mehr", "immer", "wenn", "dann",
    "nur", "alle", "kann", "man", "dazu", "machen", "sehen", "zeigen", "unter", "oben",
    NULL
};

static void detect_file_type(void) {
    cur_file_type = FT_GENERIC;
    if (!cur_filename) {
        return;
    }

    size_t len = strlen(cur_filename);

    /* Extension analysis */
    if (len >= 3 && strcmp(cur_filename + len - 3, ".sh") == 0) {
        cur_file_type = FT_BASH;
        return;
    }
    if (len >= 5 && strcmp(cur_filename + len - 5, ".bash") == 0) {
        cur_file_type = FT_BASH;
        return;
    }
    if (strstr(cur_filename, "sh.cfg") || strstr(cur_filename, "aliases") || strstr(cur_filename, "motd")) {
        cur_file_type = FT_BASH;
        return;
    }

    if (len >= 3 && strcmp(cur_filename + len - 3, ".py") == 0) {
        cur_file_type = FT_PYTHON;
        return;
    }
    if (len >= 4 && strcmp(cur_filename + len - 4, ".pyw") == 0) {
        cur_file_type = FT_PYTHON;
        return;
    }

    if (len >= 3 && strcmp(cur_filename + len - 3, ".md") == 0) {
        cur_file_type = FT_MARKDOWN;
        return;
    }
    if (len >= 9 && strcmp(cur_filename + len - 9, ".markdown") == 0) {
        cur_file_type = FT_MARKDOWN;
        return;
    }
    if (strstr(cur_filename, "README") || strstr(cur_filename, "readme") ||
        strstr(cur_filename, "CHANGES") || strstr(cur_filename, "changes")) {
        cur_file_type = FT_MARKDOWN;
        return;
    }

    /* Content shebang inspection */
    if (buf_len >= 2 && buffer[0] == '#' && buffer[1] == '!') {
        if (strstr(buffer, "bash") || strstr(buffer, "sh")) {
            cur_file_type = FT_BASH;
            return;
        }
        if (strstr(buffer, "python")) {
            cur_file_type = FT_PYTHON;
            return;
        }
    }
}

static int is_word_char(char c) {
    return isalpha(c);
}

static int is_in_dict(const char *w, const char **d) {
    if (!d) return 0;
    for (size_t i = 0; d[i] != NULL; i++) {
        if (strcmp(w, d[i]) == 0) {
            return 1;
        }
    }
    return 0;
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

    /* Common multilingual vocabulary */
    if (is_in_dict(lower, dict_common)) {
        return 0;
    }

    /* Grammar-specific syntax dictionaries */
    if (cur_file_type == FT_BASH) {
        if (is_in_dict(lower, dict_bash)) return 0;
    } else if (cur_file_type == FT_PYTHON) {
        if (is_in_dict(lower, dict_python)) return 0;
    } else if (cur_file_type == FT_MARKDOWN) {
        if (is_in_dict(lower, dict_markdown)) return 0;
    }

    /* C and system terms */
    if (is_in_dict(lower, dict_c_system)) {
        return 0;
    }

    /* Fallback cross-dictionary match for generic text files */
    if (cur_file_type == FT_GENERIC) {
        if (is_in_dict(lower, dict_bash) ||
            is_in_dict(lower, dict_python) ||
            is_in_dict(lower, dict_markdown)) {
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

    /* Status row */
    for (size_t col = 0; col < VGA_WIDTH; col++) {
        draw_cell(col, EM_STATUS_ROW, ' ', VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    }

    char status[80];
    status[0] = '\0';
    if (mode == MODE_EDIT) {
        if (cur_file_type == FT_BASH) {
            strcat(status, "[EDIT:BASH] ");
        } else if (cur_file_type == FT_PYTHON) {
            strcat(status, "[EDIT:PYTHON] ");
        } else if (cur_file_type == FT_MARKDOWN) {
            strcat(status, "[EDIT:MARKDOWN] ");
        } else {
            strcat(status, "[EDIT] ");
        }
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
    strcat(status, " B | Alt: cmd (;wsc ;q ;s ;-m)");

    for (size_t col = 0; status[col] && col < VGA_WIDTH; col++) {
        draw_cell(col, EM_STATUS_ROW, status[col], VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    }

    /* Command / message row */
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
    fs_log_change("EDITED", "root", cur_filename);
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
                strcpy(msg_buf, "hop to typo: '");
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
                strcpy(msg_buf, "hop to typo (wrap): '");
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
    const char *cmd = cmd_buf;
    if (cmd[0] == ';') {
        cmd++;
    }
    while (*cmd == ' ') {
        cmd++;
    }

    if (strcmp(cmd, "q") == 0) {
        return 1;
    } else if (strcmp(cmd, "s") == 0) {
        em_save();
    } else if (strcmp(cmd, "wsc") == 0 || strcmp(cmd, "wq") == 0) {
        em_save();
        return 1;
    } else if (strcmp(cmd, "-m") == 0) {
        em_hop_error();
    } else {
        strcpy(msg_buf, "unknown command: ;");
        strcat(msg_buf, cmd_buf);
    }
    mode = MODE_EDIT;
    cmd_len = 0;
    return 0;
}

static void em_move_left(void) {
    if (cursor_pos > 0) {
        cursor_pos--;
    }
}

static void em_move_right(void) {
    if (cursor_pos < buf_len) {
        cursor_pos++;
    }
}

static void em_move_up(void) {
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
}

static void em_move_down(void) {
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

static void em_move_home(void) {
    while (cursor_pos > 0 && buffer[cursor_pos - 1] != '\n') {
        cursor_pos--;
    }
}

static void em_move_end(void) {
    while (cursor_pos < buf_len && buffer[cursor_pos] != '\n') {
        cursor_pos++;
    }
}

static void em_delete_char(void) {
    if (cursor_pos < buf_len) {
        for (size_t k = cursor_pos; k < buf_len - 1; k++) {
            buffer[k] = buffer[k + 1];
        }
        buf_len--;
        buffer[buf_len] = '\0';
    }
}

static void em_backspace(void) {
    if (cursor_pos > 0) {
        for (size_t k = cursor_pos - 1; k < buf_len - 1; k++) {
            buffer[k] = buffer[k + 1];
        }
        buf_len--;
        cursor_pos--;
        buffer[buf_len] = '\0';
    }
}

static void em_insert_char(char c) {
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

int em_confirm_system_file(const char *filename) {
    if (!fs_is_system(filename)) {
        return 1;
    }

    int selected = 1; /* 0 = OK, 1 = EXIT (default to safe option) */

    while (1) {
        vga_clear();

        /* Draw a centered dialog box (rows 4..18, cols 8..72) */
        for (int r = 4; r <= 18; r++) {
            for (int c = 8; c <= 72; c++) {
                char ch = ' ';
                uint8_t fg = VGA_COLOR_WHITE;
                uint8_t bg = VGA_COLOR_BLUE;

                if (r == 4 || r == 18) {
                    ch = '-';
                    fg = VGA_COLOR_LIGHT_CYAN;
                } else if (c == 8 || c == 72) {
                    ch = '|';
                    fg = VGA_COLOR_LIGHT_CYAN;
                }
                draw_cell(c, r, ch, fg, bg);
            }
        }

        const char *title = "[!] WARNING: CRITICAL SYSTEM FILE [!]";
        int tlen = (int)strlen(title);
        for (int i = 0; i < tlen; i++) {
            draw_cell(40 - tlen / 2 + i, 5, title[i], VGA_COLOR_YELLOW, VGA_COLOR_BLUE);
        }

        const char *line1 = "You are attempting to open a vital system file:";
        const char *line3 = "Modifying this file may cause severe system instability,";
        const char *line4 = "kernel panics, or prevent dismasmOS from booting!";
        const char *line5 = "Please think twice and ensure you understand the risks.";
        const char *line6 = "Are you sure you want to proceed?";

        for (int i = 0; line1[i]; i++) draw_cell(12 + i, 7, line1[i], VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        for (int i = 0; filename[i] && i < 56; i++) {
            draw_cell(14 + i, 8, filename[i], VGA_COLOR_YELLOW, VGA_COLOR_BLUE);
        }
        for (int i = 0; line3[i]; i++) draw_cell(12 + i, 10, line3[i], VGA_COLOR_LIGHT_RED, VGA_COLOR_BLUE);
        for (int i = 0; line4[i]; i++) draw_cell(12 + i, 11, line4[i], VGA_COLOR_LIGHT_RED, VGA_COLOR_BLUE);
        for (int i = 0; line5[i]; i++) draw_cell(12 + i, 13, line5[i], VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLUE);
        for (int i = 0; line6[i]; i++) draw_cell(12 + i, 14, line6[i], VGA_COLOR_WHITE, VGA_COLOR_BLUE);

        /* OK Button */
        uint8_t ok_fg = (selected == 0) ? VGA_COLOR_BLACK : VGA_COLOR_WHITE;
        uint8_t ok_bg = (selected == 0) ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_DARK_GREY;
        const char *ok_str = "   [ OK ]   ";
        for (int i = 0; ok_str[i]; i++) {
            draw_cell(22 + i, 16, ok_str[i], ok_fg, ok_bg);
        }

        /* EXIT Button */
        uint8_t ex_fg = (selected == 1) ? VGA_COLOR_WHITE : VGA_COLOR_LIGHT_GREY;
        uint8_t ex_bg = (selected == 1) ? VGA_COLOR_RED : VGA_COLOR_DARK_GREY;
        const char *ex_str = "  [ EXIT ]  ";
        for (int i = 0; ex_str[i]; i++) {
            draw_cell(44 + i, 16, ex_str[i], ex_fg, ex_bg);
        }

        vga_update_cursor(selected == 0 ? 27 : 49, 16);

        int key = kbd_getchar();
        if (key == KEY_LEFT) {
            selected = 0;
        } else if (key == KEY_RIGHT) {
            selected = 1;
        } else if (key == '\t' || key == KEY_UP || key == KEY_DOWN) {
            selected = 1 - selected;
        } else if (key == '\n') {
            vga_clear();
            return (selected == 0);
        } else if (key == 27) {
            vga_clear();
            return 0;
        }
    }
}

void em_run(const char *filename) {
    if (!em_confirm_system_file(filename)) {
        return;
    }

    cur_filename = filename;
    buf_len = 0;
    cursor_pos = 0;
    mode = MODE_EDIT; /* Direct writing mode */
    cmd_len = 0;
    msg_buf[0] = '\0';

    /* Log file view */
    fs_log_change("VIEW", "root", filename);

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

    detect_file_type();

    em_render();

    while (1) {
        int c = kbd_getchar();

        if (mode == MODE_COMMAND) {
            if (c == 27 || c == KEY_ALT) {
                mode = MODE_EDIT;
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
                    mode = MODE_EDIT;
                }
            } else if (c >= 32 && c <= 126) {
                if (cmd_len < sizeof(cmd_buf) - 1) {
                    cmd_buf[cmd_len++] = (char)c;
                }
            }
        } else {
            /* MODE_EDIT (Direct Writing) */
            if (c == KEY_ALT) {
                mode = MODE_COMMAND;
                cmd_len = 0;
                msg_buf[0] = '\0';
            } else if (c == KEY_UP) {
                em_move_up();
            } else if (c == KEY_DOWN) {
                em_move_down();
            } else if (c == KEY_LEFT) {
                em_move_left();
            } else if (c == KEY_RIGHT) {
                em_move_right();
            } else if (c == KEY_HOME) {
                em_move_home();
            } else if (c == KEY_END) {
                em_move_end();
            } else if (c == KEY_DELETE) {
                em_delete_char();
            } else if (c == '\b') {
                em_backspace();
            } else if (c == '\n') {
                em_insert_char('\n');
            } else if (c == '\t') {
                em_insert_char(' ');
                em_insert_char(' ');
                em_insert_char(' ');
                em_insert_char(' ');
            } else if ((c >= 32 && c <= 255) && c != 127) {
                em_insert_char((char)c);
            }
        }

        em_render();
    }
}
