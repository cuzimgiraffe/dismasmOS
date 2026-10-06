#ifndef WDID_H
#define WDID_H

#include "types.h"

struct wdid_entry {
    const char *name;
    const char *alias;
    const char *summary;
    const char * const *lines;
    size_t line_count;
};

int cmd_wdid(int argc, char **argv);
const struct wdid_entry *wdid_find(const char *name);

extern const struct wdid_entry * const wdid_entries[];
extern const size_t wdid_entry_count;

#endif
