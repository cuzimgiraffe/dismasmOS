#include "wdid.h"

/* 1. LSF MANUAL ENTRY */
static const char * const doc_lsf_lines[] = {
    "NAME",
    "  lsf - List directory contents, file sizes, and permission attributes",
    "",
    "SYNOPSIS",
    "  lsf [flags] [directory]",
    "",
    "DESCRIPTION",
    "  The lsf command lists the files and subdirectories located within a directory.",
    "  By default, it lists entries in the current working directory, displaying",
    "  directories in cyan text and standard files in white text.",
    "",
    "FLAGS & OPTIONS",
    "  -s",
    "    Show file sizes. Displays each file's size in kilobytes (KiB) and raw",
    "    hexadecimal bytes (e.g. '1 KiB (0x00000042 B)').",
    "",
    "  -p",
    "    Show permissions. Displays Unix-style permission strings (e.g.,",
    "    'drwxr-xr-x' for directories and '-rw-r--r--' for standard files).",
    "",
    "  Combined Flags:",
    "    Flags can be combined together on the command line (e.g. 'lsf -sp /boot'",
    "    or 'lsf -s -p').",
    "",
    "PRACTICAL EXAMPLES",
    "  lsf",
    "    Lists entries in the current working directory.",
    "",
    "  lsf -s /home",
    "    Lists all files in /home along with their size in KiB and hex bytes.",
    "",
    "  lsf -sp /",
    "    Lists the root directory with both file sizes and permission attributes.",
    "",
    "SEE ALSO",
    "  cd, cat, changes, wdid"
};

/* 2. CD MANUAL ENTRY */
static const char * const doc_cd_lines[] = {
    "NAME",
    "  cd - Change the current working directory in the hierarchical filesystem",
    "",
    "SYNOPSIS",
    "  cd",
    "  cd ~",
    "  cd /",
    "  cd <directory_path>",
    "",
    "DESCRIPTION",
    "  The cd command changes the active working directory for the shell REPL.",
    "  All subsequent relative file and directory lookups (e.g. for cat, lsf,",
    "  touch, em) will be evaluated relative to this new directory location.",
    "",
    "PATH RESOLUTION RULES",
    "  - Running 'cd' without arguments or 'cd ~' switches directly to /home",
    "    (the standard user workspace).",
    "  - Running 'cd /' switches to the root directory.",
    "  - Relative paths (such as 'cd boot' or 'cd ..') are resolved iteratively",
    "    against the current working directory.",
    "  - The root traversal is clamped: navigating above '/' remains at '/'.",
    "",
    "ERROR HANDLING",
    "  If the target path does not exist or refers to a regular file instead of",
    "  a directory, cd prints an error message and sounds the 3000 Hz PC speaker",
    "  warning tone.",
    "",
    "PRACTICAL EXAMPLES",
    "  cd /shell",
    "    Switches to the /shell directory.",
    "",
    "  cd ..",
    "    Navigates up one level to the parent directory.",
    "",
    "  cd ~",
    "    Returns to /home.",
    "",
    "SEE ALSO",
    "  lsf, makedir, wdid"
};

/* 3. MAKEDIR MANUAL ENTRY */
static const char * const doc_makedir_lines[] = {
    "NAME",
    "  makedir - Create a new directory node in the in-memory RAM filesystem",
    "",
    "SYNOPSIS",
    "  makedir <directory_path>",
    "",
    "DESCRIPTION",
    "  The makedir command creates a new directory node within the RAMFS filesystem.",
    "  Once created, files can be stored inside the directory, and the shell can",
    "  navigate into it using the cd command.",
    "",
    "HOW IT WORKS UNDER THE HOOD",
    "  1. Resolves the requested path against the current working directory.",
    "  2. Checks whether an entry with the target path already exists in the file",
    "     table. If so, an error is reported.",
    "  3. Allocates an unused filesystem slot, sets its directory attribute flag",
    "     (is_dir = 1), and initializes its node parameters.",
    "  4. Automatically records a 'CREATED' audit event into the filesystem",
    "     audit log accessible via the 'changes' command.",
    "",
    "PRACTICAL EXAMPLES",
    "  makedir projects",
    "    Creates a directory named 'projects' in the current working directory.",
    "",
    "  makedir /home/notes",
    "    Creates a directory at absolute path /home/notes.",
    "",
    "SEE ALSO",
    "  cd, lsf, rm, changes, wdid"
};

/* 4. CHANGES MANUAL ENTRY */
static const char * const doc_changes_lines[] = {
    "NAME",
    "  changes - View the filesystem access and modification audit trail log",
    "",
    "SYNOPSIS",
    "  changes",
    "",
    "DESCRIPTION",
    "  The changes command inspects the system's internal circular filesystem audit",
    "  log. Every significant filesystem operation performed in dismasmOS is",
    "  automatically captured in real time for security and integrity tracking.",
    "",
    "LOGGED AUDIT EVENTS",
    "  - VIEW    : Recorded whenever a file is inspected via 'cat', searched",
    "              via 'grep', or opened in the 'em' text editor.",
    "  - EDITED  : Recorded whenever a file is modified and saved in 'em' or",
    "              overwritten using shell redirection (>).",
    "  - CREATED : Recorded when a new file is created via 'touch', a directory",
    "              via 'makedir', or when redirecting output into a new file.",
    "  - DELETED : Recorded whenever a file or directory is removed via 'rm'.",
    "",
    "RECORD FORMAT",
    "  Each audit entry displays the event type (in cyan), the acting user",
    "  ('root'), and the full resolved target path.",
    "",
    "SEE ALSO",
    "  touch, rm, em, echo, wdid"
};

/* 5. EM MANUAL ENTRY */
static const char * const doc_em_lines[] = {
    "NAME",
    "  em - Full-screen modal text editor with syntax and vocabulary validation",
    "",
    "SYNOPSIS",
    "  em <filename>",
    "",
    "DESCRIPTION",
    "  The em command launches the built-in full-screen text editor. It provides",
    "  a complete visual editing environment across the 80x25 VGA buffer with direct",
    "  writing, hardware arrow key cursor movement, modal commands via the Left Alt",
    "  key, and grammar dictionary validation.",
    "",
    "SCREEN LAYOUT",
    "  - Rows 0 through 22 : Main text editing canvas.",
    "  - Row 23            : Blue status bar showing active file mode, filename,",
    "                        cursor row and column coordinates, and file size.",
    "  - Row 24            : Semicolon command prompt bar (activated by Alt).",
    "",
    "OPERATING MODES",
    "  1. DIRECT WRITING MODE (Default):",
    "     Text characters, spaces, punctuation, and newlines are typed directly",
    "     into the buffer at the blinking hardware cursor position.",
    "     Use Arrow Keys to move the cursor freely through the text.",
    "",
    "  2. COMMAND MODE (Left Alt key):",
    "     Press the Left Alt key to open the command prompt on row 24.",
    "     Supported commands:",
    "       ;wsc  - Write, save changes to RAMFS, and close the editor.",
    "       ;s    - Save changes to RAMFS without closing the editor.",
    "       ;q    - Quit editor immediately without saving changes.",
    "       ;-m   - Hop cursor directly to the next detected typo/unknown word.",
    "     Pressing Alt or Escape again returns immediately to direct writing mode.",
    "",
    "SYNTAX & VOCABULARY VALIDATION",
    "  em inspects the file extension to load specialized language dictionaries:",
    "    - Bash Scripting (.sh, .bash, sh.cfg, aliases): Shell keywords.",
    "    - Python (.py, .pyw): Python keywords, builtins, and exceptions.",
    "    - Markdown (.md, .markdown, README): Markdown documentation vocabulary.",
    "    - C / System: C types, kernel structs, and x86 register tokens.",
    "  Recognized vocabulary words render in clean white text; unknown tokens and",
    "  typos are highlighted in bright red.",
    "",
    "SYSTEM FILE PROTECTION DIALOG",
    "  Opening sensitive system files located in /boot or /krnl activates an",
    "  interactive warning modal. Users must explicitly navigate with Arrow Keys",
    "  and confirm with [ OK ] before editing system assets.",
    "",
    "SEE ALSO",
    "  cat, touch, changes, wdid"
};

/* 6. CAT MANUAL ENTRY */
static const char * const doc_cat_lines[] = {
    "NAME",
    "  cat - Concatenate and stream raw file contents to standard output",
    "",
    "SYNOPSIS",
    "  cat <filename>",
    "",
    "DESCRIPTION",
    "  The cat command reads the raw textual data of a designated file from the",
    "  RAMFS filesystem and writes it directly to the VGA console display.",
    "",
    "HOW IT WORKS UNDER THE HOOD",
    "  1. Resolves the filename against the current working directory.",
    "  2. Locates the file node in the in-memory file table.",
    "  3. Validates that the node is a regular data file, not a directory.",
    "  4. Appends a 'VIEW' audit entry into the system audit log.",
    "  5. Streams the byte buffer to the screen using vga_puts().",
    "",
    "ERROR HANDLING",
    "  If the file does not exist or represents a directory, cat prints",
    "  'cat: file not found' and triggers the 3000 Hz PC speaker error beep.",
    "",
    "PRACTICAL EXAMPLES",
    "  cat /etc/os-release",
    "    Displays operating system release information.",
    "",
    "  cat /shell/motd",
    "    Displays the system message of the day.",
    "",
    "SEE ALSO",
    "  grep, echo, em, changes, wdid"
};

/* 7. ECHO MANUAL ENTRY */
static const char * const doc_echo_lines[] = {
    "NAME",
    "  echo - Print text arguments to console or redirect output into a file",
    "",
    "SYNOPSIS",
    "  echo [text ...]",
    "  echo [text ...] > <destination_file>",
    "",
    "DESCRIPTION",
    "  The echo command prints its arguments to the terminal screen separated by",
    "  spaces and terminated with a newline.",
    "",
    "  When the redirection operator ('>') is used, echo redirects its text",
    "  directly into the specified destination file instead of printing to the",
    "  screen.",
    "",
    "FILE REDIRECTION (>)",
    "  - If the target file does not exist, echo automatically creates it and",
    "    records a 'CREATED' audit event in the system audit log.",
    "  - If the target file already exists, echo overwrites the file contents with",
    "    the new text payload and records an 'EDITED' audit event.",
    "  - Leading and trailing quotation marks around the text are cleanly stripped.",
    "",
    "PRACTICAL EXAMPLES",
    "  echo Hello dismasmOS!",
    "    Prints 'Hello dismasmOS!' to the active console.",
    "",
    "  echo System configuration test > /home/config.txt",
    "    Writes the text string into /home/config.txt.",
    "",
    "SEE ALSO",
    "  cat, touch, changes, wdid"
};

/* 8. TOUCH MANUAL ENTRY */
static const char * const doc_touch_lines[] = {
    "NAME",
    "  touch - Create an empty file node in the in-memory RAM filesystem",
    "",
    "SYNOPSIS",
    "  touch <filename>",
    "",
    "DESCRIPTION",
    "  The touch command creates a new empty (0-byte) file in the RAM filesystem",
    "  if the specified file does not already exist. If the file already exists,",
    "  touch succeeds cleanly without altering existing data.",
    "",
    "HOW IT WORKS UNDER THE HOOD",
    "  1. Resolves the file path against the current working directory.",
    "  2. Searches the filesystem table for an existing matching entry.",
    "  3. If not found, allocates an available file slot, sets its size to 0,",
    "     and initializes its node properties.",
    "  4. Records a 'CREATED' audit event in the filesystem audit log.",
    "",
    "PRACTICAL EXAMPLES",
    "  touch document.txt",
    "    Creates an empty file named document.txt in the active directory.",
    "",
    "  touch /home/todo.md",
    "    Creates an empty markdown file at /home/todo.md.",
    "",
    "SEE ALSO",
    "  rm, echo, em, changes, wdid"
};

/* 9. RM MANUAL ENTRY */
static const char * const doc_rm_lines[] = {
    "NAME",
    "  rm - Remove a file or directory node from the in-memory filesystem",
    "",
    "SYNOPSIS",
    "  rm <target_path>",
    "",
    "DESCRIPTION",
    "  The rm command deletes a designated file or directory from the RAMFS file table.",
    "  Once deleted, the allocated slot is marked free and becomes available for",
    "  subsequent file creation.",
    "",
    "HOW IT WORKS UNDER THE HOOD",
    "  1. Resolves the target path against the active working directory.",
    "  2. Searches the filesystem table for a matching node.",
    "  3. Zeroes out the file structure and marks the entry slot as inactive.",
    "  4. Records a 'DELETED' audit entry into the audit trail log.",
    "",
    "ERROR HANDLING",
    "  If the specified file or directory does not exist, rm prints an error",
    "  message and triggers the 3000 Hz PC speaker error beep.",
    "",
    "PRACTICAL EXAMPLES",
    "  rm old_file.txt",
    "    Deletes old_file.txt from the current directory.",
    "",
    "  rm /home/notes",
    "    Removes the notes directory.",
    "",
    "SEE ALSO",
    "  touch, makedir, changes, wdid"
};

/* 10. GREP MANUAL ENTRY */
static const char * const doc_grep_lines[] = {
    "NAME",
    "  grep - Search for text patterns line-by-line within a file",
    "",
    "SYNOPSIS",
    "  grep <pattern> <filename>",
    "",
    "DESCRIPTION",
    "  The grep command scans the lines of a specified text file and prints each line",
    "  that contains the given search pattern substring.",
    "",
    "HOW IT WORKS UNDER THE HOOD",
    "  1. Resolves the target filename and locates the file node in RAMFS.",
    "  2. Records a 'VIEW' audit event in the filesystem audit log.",
    "  3. Parses the file data into newline-delimited lines.",
    "  4. Scans each line for the pattern substring using strstr().",
    "  5. Prints matching lines directly to the VGA console display.",
    "",
    "ERROR HANDLING",
    "  If the file cannot be found or is a directory, grep prints an error",
    "  and sounds the 3000 Hz PC speaker warning tone.",
    "",
    "PRACTICAL EXAMPLES",
    "  grep VERSION /etc/os-release",
    "    Searches for lines containing 'VERSION' in /etc/os-release.",
    "",
    "  grep root /shell/sh.cfg",
    "    Prints lines referencing 'root' in the shell configuration.",
    "",
    "SEE ALSO",
    "  cat, em, changes, wdid"
};

const struct wdid_entry wdid_doc_lsf = {
    "lsf", "ls", "List directory contents, file sizes, and permission attributes",
    doc_lsf_lines, sizeof(doc_lsf_lines) / sizeof(doc_lsf_lines[0])
};

const struct wdid_entry wdid_doc_cd = {
    "cd", NULL, "Change current working directory in the hierarchical filesystem",
    doc_cd_lines, sizeof(doc_cd_lines) / sizeof(doc_cd_lines[0])
};

const struct wdid_entry wdid_doc_makedir = {
    "makedir", "mkdir", "Create a new directory node in the in-memory RAM filesystem",
    doc_makedir_lines, sizeof(doc_makedir_lines) / sizeof(doc_makedir_lines[0])
};

const struct wdid_entry wdid_doc_changes = {
    "changes", "audit", "View the filesystem access and modification audit trail log",
    doc_changes_lines, sizeof(doc_changes_lines) / sizeof(doc_changes_lines[0])
};

const struct wdid_entry wdid_doc_em = {
    "em", "edit", "Full-screen modal text editor with syntax & vocabulary validation",
    doc_em_lines, sizeof(doc_em_lines) / sizeof(doc_em_lines[0])
};

const struct wdid_entry wdid_doc_cat = {
    "cat", NULL, "Concatenate and stream raw file contents to standard output",
    doc_cat_lines, sizeof(doc_cat_lines) / sizeof(doc_cat_lines[0])
};

const struct wdid_entry wdid_doc_echo = {
    "echo", NULL, "Print text arguments to console or redirect output into a file",
    doc_echo_lines, sizeof(doc_echo_lines) / sizeof(doc_echo_lines[0])
};

const struct wdid_entry wdid_doc_touch = {
    "touch", NULL, "Create an empty file node in the in-memory RAM filesystem",
    doc_touch_lines, sizeof(doc_touch_lines) / sizeof(doc_touch_lines[0])
};

const struct wdid_entry wdid_doc_rm = {
    "rm", "del", "Remove a file or directory node from the in-memory filesystem",
    doc_rm_lines, sizeof(doc_rm_lines) / sizeof(doc_rm_lines[0])
};

const struct wdid_entry wdid_doc_grep = {
    "grep", "find", "Search for text patterns line-by-line within a file",
    doc_grep_lines, sizeof(doc_grep_lines) / sizeof(doc_grep_lines[0])
};
