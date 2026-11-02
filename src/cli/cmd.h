/* cmd.h - the subcommand framework: a port of omp's pi-utils/cli.ts (Command.parse over node:util parseArgs in
 * strict mode, renderRootHelp / renderCommandHelp, the CliUsageError report) plus the command table of
 * cli-commands.ts. Commands declare their flags/args/examples as data; help, completions and parse errors all
 * come from that one table. */
#ifndef OMC_CLI_CMD_H
#define OMC_CLI_CMD_H

#include "core/core.h"
#include "core/json.h"

#define CLI_BIN "omc"

typedef enum { CF_BOOL, CF_STR, CF_INT } cf_kind_t;

typedef struct {
    const char *name; /* long name without `--` */
    cf_kind_t kind;
    char ch;          /* short option character, 0 for none */
    const char *desc;
    const char *const *options; /* NULL-terminated allowed values (string flags), or NULL */
    bool multiple;
    const char *def;  /* default ("true" for booleans, decimal for integers), or NULL */
    bool required;
} cflag_t;

typedef struct {
    const char *name; /* lower-case; rendered upper-case */
    const char *desc;
    bool required, multiple;
    const char *const *options;
} carg_t;

typedef struct cli_cmd cli_cmd_t;

/* one parse of a command line against a command's declarations */
typedef struct {
    const cli_cmd_t *cmd;
    struct cli_fv {
        bool set;          /* given on the command line (or defaulted) */
        bool given;        /* given on the command line */
        strvec_t vals;     /* string values (last one wins unless multiple) */
        long long num;     /* integers */
    } *fv;
    size_t nflags;
    strvec_t pos;          /* positionals in order */
    strvec_t *argv_of;     /* per declared arg: its value(s) */
    size_t nargs;
} cparse_t;

struct cli_cmd {
    const char *name;
    const char *desc;
    const char *const *aliases; /* NULL-terminated or NULL */
    bool hidden;
    const cflag_t *flags;       /* {0}-terminated or NULL */
    const carg_t *args;         /* {0}-terminated or NULL */
    const char *const *examples;
    int (*run)(cparse_t *p);    /* parsed (strict) commands */
    int (*run_raw)(int argc, char **argv); /* commands that parse their own argv (launch, acp, __complete) */
};

/* flag / arg accessors (name = the declared long name) */
bool cp_bool(const cparse_t *p, const char *name);
const char *cp_str(const cparse_t *p, const char *name); /* last value, default, or NULL */
const strvec_t *cp_strs(const cparse_t *p, const char *name); /* every value (multiple) */
bool cp_int(const cparse_t *p, const char *name, long long *out); /* false when unset */
const char *cp_arg(const cparse_t *p, const char *name);
const strvec_t *cp_arg_multi(const cparse_t *p, const char *name); /* NULL when none */

/* A CliUsageError thrown from run(): `error: <msg>`, the usage line and the help hint on stderr; returns 1. */
int cp_usage_error(const cparse_t *p, const char *fmt, ...) PRINTF_LIKE(2, 3);

/* the registered commands (cli-commands.ts order) */
const cli_cmd_t *cli_commands(size_t *n);
const cli_cmd_t *cli_find_command(const char *name); /* exact name or alias */
const cli_cmd_t *cli_launch_command(void);

/* pi-utils run(): help/usage handling + strict parse + run. argv[0] is the command id. */
int cli_run_command(const cli_cmd_t *c, int argc, char **argv);
void cli_render_command_help(FILE *f, const cli_cmd_t *c);
void cli_render_root_help(FILE *f);
char *cli_usage_line(const cli_cmd_t *c); /* "$ omc token PROVIDER [FLAGS]" (owned) */

/* chalk: colored only when stdout is a terminal (FORCE_COLOR / NO_COLOR / TERM=dumb honored), owned */
char *cli_red(const char *s);
char *cli_yellow(const char *s);
char *cli_dim(const char *s);
char *cli_bold(const char *s);
char *cli_green(const char *s);
char *cli_cyan(const char *s);
char *cli_blue(const char *s);

/* extra root-help text (help-extra.ts) */
char *cli_extra_help(void); /* owned */

#endif
