#if AMALGAMATION
# define INTERNAL static
# define EXTERNAL static
# include "context.c"
# include "util/argparse.c"
# include "util/hash.c"
# include "util/string.c"
# include "backend/x86_64/instr.c"
# include "backend/x86_64/elf.c"
# include "backend/x86_64/abi.c"
# include "backend/x86_64/assemble.c"
# include "backend/compile.c"
# include "backend/graphviz/dot.c"
# include "optimizer/transform.c"
# include "optimizer/liveness.c"
# include "optimizer/optimize.c"
# include "preprocessor/tokenize.c"
# include "preprocessor/strtab.c"
# include "preprocessor/input.c"
# include "preprocessor/directive.c"
# include "preprocessor/preprocess.c"
# include "preprocessor/macro.c"
# include "parser/typetree.c"
# include "parser/symtab.c"
# include "parser/parse.c"
# include "parser/statement.c"
# include "parser/initializer.c"
# include "parser/expression.c"
# include "parser/declaration.c"
# include "parser/eval.c"
#else
# define INTERNAL
# define EXTERNAL extern
# include "backend/compile.h"
# include "optimizer/optimize.h"
# include "parser/parse.h"
# include "parser/symtab.h"
# include "parser/typetree.h"
# include "preprocessor/preprocess.h"
# include "preprocessor/input.h"
# include "preprocessor/macro.h"
# include "util/argparse.h"
# include <lacc/context.h>
# include <lacc/ir.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>

/*
 * Configurable location of implementation defined standard library
 * headers. This is set in the makefile, by default pointing to files
 * from the source tree under /include/stdlib/.
 */
#ifndef LACC_STDLIB_PATH
# define LACC_STDLIB_PATH "/usr/local/lib/lacc/include"
#endif

static const char *program;
static FILE *output;
static int optimization_level;
static int dump_symbols, dump_types;

static void help(const char *arg)
{
    fprintf(
        stderr,
        "Usage: %s [-(S|E|c)] [-v] [-fPIC] [-I <path>] [-o <file>] <file>\n",
        program);
    exit(1);
}

static void flag(const char *arg)
{
    switch (*arg) {
    case 'c':
        context.target = TARGET_x86_64_ELF;
        break;
    case 'S':
        context.target = TARGET_x86_64_ASM;
        break;
    case 'E':
        context.target = TARGET_NONE;
        break;
    case 'v':
        context.verbose += 1;
        break;
    case 'w':
        context.suppress_warning = 1;
        break;
    default:
        assert(0);
        break;
    }
}

static void option(const char *arg)
{
    if (!strcmp("-fPIC", arg)) {
        context.pic = 1;
    } else if (!strcmp("-fno-PIC", arg)) {
        context.pic = 0;
    } else assert(0);
}

static void open_output_handle(const char *file)
{
    output = fopen(file, "w");
    if (output == NULL) {
        fprintf(stderr, "Could not open output file '%s'.\n", file);
        exit(1);
    }
}

static void set_c_std(const char *std)
{
    if (!strcmp("c89", std)) {
        context.standard = STD_C89;
    } else if (!strcmp("c99", std)) {
        context.standard = STD_C99;
    } else if (!strcmp("c11", std)) {
        context.standard = STD_C11;
    } else {
        fprintf(stderr, "Unrecognized option %s.\n", std);
        exit(1);
    }
}

static void set_optimization_level(const char *level)
{
    assert(isdigit(level[2]));
    optimization_level = level[2] - '0';
}

static void set_dump_state(const char *arg)
{
    if (!strcmp("--dump-symbols", arg)) {
        dump_symbols = 1;
    } else {
        assert(!strcmp("--dump-types", arg));
        dump_types = 1;
    }
}

static void define_macro(const char *arg)
{
    static char line[1024];
    char *sep;

    if (strlen(arg) + 20 > sizeof(line)) {
        fprintf(stderr, "Macro definition exceeds length limit.\n");
        exit(1);
    }

    sep = strchr(arg, '=');
    if (sep) {
        sprintf(line, "#define %s", arg);
        *(line + 8 + (sep - arg)) = ' ';
    } else {
        sprintf(line, "#define %s 1", arg);
    }

    inject_line(line);
}

static char *parse_program_arguments(int argc, char *argv[])
{
    int c;
    char *input = NULL;
    struct option optv[] = {
        {"-S", &flag},
        {"-E", &flag},
        {"-c", &flag},
        {"-v", &flag},
        {"-w", &flag},
        {"-fPIC", &option},
        {"-fno-PIC", &option},
        {"--help", &help},
        {"-o:", &open_output_handle},
        {"-I:", &add_include_search_path},
        {"-O0", &set_optimization_level},
        {"-O1", &set_optimization_level},
        {"-O2", &set_optimization_level},
        {"-O3", &set_optimization_level},
        {"-std=", &set_c_std},
        {"-D:", &define_macro},
        {"--dump-symbols", &set_dump_state},
        {"--dump-types", &set_dump_state}
    };

    program = argv[0];
    output = stdout;
    context.standard = STD_C89;

    /* OpenBSD defaults to -fPIC unless explicitly turned off.  */
#ifdef __OpenBSD__
    context.pic = 1;
#endif

    context.target = TARGET_IR_DOT;

    c = parse_args(sizeof(optv)/sizeof(optv[0]), optv, argc, argv);
    if (c == argc - 1) {
        input = argv[c];
    } else if (c < argc - 1) {
        help(argv[0]);
        exit(1);
    }

    return input;
}

/*
 * Register compiler internal builtin symbols, that are assumed to
 * exists by standard library headers.
 */
static void register_builtin_declarations(void)
{
    inject_line("void *memcpy(void *dest, const void *src, unsigned long n);");
    inject_line("void __builtin_va_start(void);");
    inject_line("void __builtin_va_arg(void);");
    inject_line(
        "typedef struct {"
        "   unsigned int gp_offset;"
        "   unsigned int fp_offset;"
        "   void *overflow_arg_area;"
        "   void *reg_save_area;"
        "} __builtin_va_list[1];");
}

/*
 * Add default search paths last, with lowest priority. These are
 * searched after anything specified with -I, and in the order listed.
 */
static void add_include_search_paths(void)
{
    add_include_search_path("/usr/local/include");
    add_include_search_path(LACC_STDLIB_PATH);
#ifdef __linux__
    add_include_search_path("/usr/include/x86_64-linux-gnu");
#endif
    add_include_search_path("/usr/include");
}

int main(int argc, char *argv[])
{
    char *path;
    struct definition *def;
    const struct symbol *sym;

    init_preprocessing();
    path = parse_program_arguments(argc, argv);
    set_input_file(path);
    register_builtin_definitions(context.standard);
    add_include_search_paths();
    set_compile_target(output, path);

    if (context.target == TARGET_NONE) {
        preprocess(output);
    } else {
        push_scope(&ns_ident);
        push_scope(&ns_tag);
        register_builtin_declarations();
        push_optimization(optimization_level);

        while ((def = parse()) != NULL) {
            if (context.errors) {
                error("Aborting because of previous %s.",
                    (context.errors > 1) ? "errors" : "error");
                break;
            }

            optimize(def);
            compile(def);
        }

        while ((sym = yield_declaration(&ns_ident)) != NULL) {
            declare(sym);
        }

        if (dump_symbols) {
            output_symbols(stdout, &ns_ident);
            output_symbols(stdout, &ns_tag);
        }

        flush();
        pop_optimization();
        clear_types(dump_types ? stdout : NULL);
        pop_scope(&ns_tag);
        pop_scope(&ns_ident);
    }

    clear_preprocessing();
    if (output != stdout) {
        fclose(output);
    }

    return context.errors;
}
