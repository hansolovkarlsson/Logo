#ifndef COMPAT_H
#define COMPAT_H

// Shims for the POSIX/GNU libc functions mingw-w64 doesn't provide, so
// the Windows build compiles from exactly the same sources as macOS and
// Linux rather than needing its own #ifdef'd copies of the callers.
//
// Everything here is _WIN32-only by design: macOS and Linux keep using
// their own libc versions, and this header compiles to nothing at all
// there. Deliberately header-only/static inline -- there is no
// compat.c, because adding one would put it in the Makefile's
// $(wildcard src/*.c) and so into every test binary's link line too,
// including the three (test_lexer/test_parser/test_bytecode) that are
// pkg-config-free by design.
//
// The third Windows gap, open_memstream, is NOT here: both of its
// callers wanted "disassemble this chunk into a string" rather than a
// stream, so that one is bytecode_disassemble_to_string() in
// bytecode.h/c instead -- a portable helper, not a shim.

#ifdef _WIN32

#include <string.h>
#include <time.h>

// strcasestr is a GNU extension (glibc + macOS both have it, mingw
// doesn't). Sole caller is interpreter.c's TO...END block scanner.
// strncasecmp itself IS declared by mingw's <string.h>, so only the
// search loop needs supplying.
static inline char *logo_strcasestr(const char *haystack, const char *needle) {
    if (*needle == '\0') return (char *)haystack;
    size_t needle_len = strlen(needle);
    for (; *haystack != '\0'; haystack++) {
        if (strncasecmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
    }
    return NULL;
}
#define strcasestr logo_strcasestr

// localtime_r (vm.c's TIME and DATE) -- the Windows CRT spells the same
// thread-safe conversion localtime_s, with the two arguments the other
// way round and an errno_t return instead of the struct tm *.
static inline struct tm *logo_localtime_r(const time_t *timep, struct tm *result) {
    return localtime_s(result, timep) == 0 ? result : NULL;
}
#define localtime_r logo_localtime_r

#endif // _WIN32

#endif // COMPAT_H
