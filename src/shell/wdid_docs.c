#include "wdid.h"

extern const struct wdid_entry wdid_doc_wdid;
extern const struct wdid_entry wdid_doc_help;
extern const struct wdid_entry wdid_doc_clear;
extern const struct wdid_entry wdid_doc_colr;
extern const struct wdid_entry wdid_doc_lang;
extern const struct wdid_entry wdid_doc_uname;
extern const struct wdid_entry wdid_doc_memrep;
extern const struct wdid_entry wdid_doc_shutdown;
extern const struct wdid_entry wdid_doc_reboot;

extern const struct wdid_entry wdid_doc_heap;
extern const struct wdid_entry wdid_doc_calc;
extern const struct wdid_entry wdid_doc_dump;
extern const struct wdid_entry wdid_doc_reg;
extern const struct wdid_entry wdid_doc_asm;
extern const struct wdid_entry wdid_doc_pr;

extern const struct wdid_entry wdid_doc_lsf;
extern const struct wdid_entry wdid_doc_cd;
extern const struct wdid_entry wdid_doc_makedir;
extern const struct wdid_entry wdid_doc_changes;
extern const struct wdid_entry wdid_doc_em;
extern const struct wdid_entry wdid_doc_cat;
extern const struct wdid_entry wdid_doc_echo;
extern const struct wdid_entry wdid_doc_touch;
extern const struct wdid_entry wdid_doc_rm;
extern const struct wdid_entry wdid_doc_grep;

/* Operator && manual entry */
static const char * const doc_chain_lines[] = {
    "NAME",
    "  && - Sequential command chaining operator with fail-fast execution",
    "",
    "SYNOPSIS",
    "  <command1> && <command2> [&& <command3> ...]",
    "",
    "DESCRIPTION",
    "  The && operator enables chaining multiple shell commands together on a",
    "  single line. Commands are executed sequentially from left to right.",
    "",
    "FAIL-FAST BEHAVIOR",
    "  - If command1 succeeds (returns status 0), execution automatically advances",
    "    to command2.",
    "  - If any command in the chain fails (returns non-zero), execution of all",
    "    subsequent commands in the pipeline is immediately aborted, and the 1 ms",
    "    3000 Hz PC speaker error tone sounds.",
    "",
    "PRACTICAL EXAMPLES",
    "  touch test.txt && echo Data > test.txt && cat test.txt",
    "    Creates test.txt, writes 'Data' into it, and prints its contents.",
    "",
    "  cd /boot && lsf -sp",
    "    Switches to /boot and immediately lists its contents with permissions.",
    "",
    "SEE ALSO",
    "  help, wdid"
};

static const struct wdid_entry wdid_doc_chain = {
    "&&", "chain", "Sequential command chaining operator with fail-fast execution",
    doc_chain_lines, sizeof(doc_chain_lines) / sizeof(doc_chain_lines[0])
};

const struct wdid_entry * const wdid_entries[] = {
    &wdid_doc_wdid,
    &wdid_doc_help,
    &wdid_doc_clear,
    &wdid_doc_colr,
    &wdid_doc_lang,
    &wdid_doc_uname,
    &wdid_doc_memrep,
    &wdid_doc_shutdown,
    &wdid_doc_reboot,
    &wdid_doc_heap,
    &wdid_doc_calc,
    &wdid_doc_dump,
    &wdid_doc_reg,
    &wdid_doc_asm,
    &wdid_doc_pr,
    &wdid_doc_lsf,
    &wdid_doc_cd,
    &wdid_doc_makedir,
    &wdid_doc_changes,
    &wdid_doc_em,
    &wdid_doc_cat,
    &wdid_doc_echo,
    &wdid_doc_touch,
    &wdid_doc_rm,
    &wdid_doc_grep,
    &wdid_doc_chain
};

const size_t wdid_entry_count = sizeof(wdid_entries) / sizeof(wdid_entries[0]);
