#ifndef CONTEXT_H
#define CONTEXT_H
#if !defined(INTERNAL) || !defined(EXTERNAL)
# error Missing amalgamation macros
#endif

#include <stddef.h>

enum target {
    TARGET_NONE,
    TARGET_IR_DOT,
    TARGET_x86_64_ASM,
    TARGET_x86_64_BIN,
    TARGET_x86_64_ELF
};

enum cstd {
    STD_C89,
    STD_C99,
    STD_C11
};

/* Global information about translation unit. */
INTERNAL struct context {
    int errors;
    int verbose;
    int suppress_warning;
    unsigned int pic : 1;            /* position independent code */
    enum target target;
    enum cstd standard;
} context;

/*
 * Output diagnostics info to stdout. No-op if context.verbose is not
 * set.
 */
INTERNAL void verbose(const char *, ...);

/*
 * Output warning to stderr. No-op if context.suppress_warning is set.
 */
INTERNAL void warning(const char *, ...);

/* Output error to stderr. */
INTERNAL void error(const char *, ...);

#endif
