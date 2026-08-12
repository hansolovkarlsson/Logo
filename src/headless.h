#ifndef LOGO_HEADLESS_H
#define LOGO_HEADLESS_H

// Runs `path` with no GTK window/event loop at all -- bin/logomotive
// --headless script.logo (see main.c's own consume_headless_flag).
// Returns a process exit code: 0 on a normal VM_RUN_HALTED finish
// (even if the script itself printed runtime errors -- same convention
// every other headless entry point in this codebase already uses),
// nonzero if the file can't be read, fails to parse, or hits a
// suspension with no well-defined headless meaning (a bare AWAIT/YIELD
// with no enclosing LAUNCH). See headless.c's own file comment for the
// full suspend-handling rules.
int run_headless_script(const char *prog, const char *path);

#endif
