#ifndef PROC_H
#define PROC_H

#include "types.h"

#define MAX_PROCS 16
#define PROC_NAME_MAX 32
#define PROC_AGENT_MAX 32
#define PROC_COMMENT_MAX 64

enum proc_state {
    PROC_STOPPED = 0,
    PROC_RUNNING = 1
};

struct process {
    int pid;
    char name[PROC_NAME_MAX];
    char agent[PROC_AGENT_MAX];
    int state;
    uint32_t mem_kib;
    uint32_t mem_bytes;
    char comment[PROC_COMMENT_MAX];
    uint8_t used;
};

void proc_init(void);
int proc_start(const char *name);
int proc_kill(const char *name);
int proc_set_comment(const char *name, const char *comment);
void proc_status(const char *name);
void proc_status_all(void);
size_t proc_total_mem_kib(void);
size_t proc_active_count(void);

#endif
