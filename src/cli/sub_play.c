/* sub_play.c - `omc play [file] [-s speed] [-i idle-limit]` (port of omp commands/play.ts): replay a /record
 * session recording in the terminal. */
#include "cli/cli_int.h"
#include "cli/cmd.h"

#include "coding/record.h"
#include "core/fs.h"

#include <math.h>
#include <stdlib.h>
#include <unistd.h>

const cflag_t PLAY_FLAGS[] = {
    {.name = "speed", .kind = CF_STR, .ch = 's', .desc = "Playback speed multiplier (default: 1)"},
    {.name = "idle-limit", .kind = CF_STR, .ch = 'i', .desc = "Cap pauses between frames to this many seconds"},
    {0}};
const carg_t PLAY_ARGS[] = {{.name = "file", .desc = "Recording to play (default: the newest /record capture)", .required = false}, {0}};
const char *const PLAY_EXAMPLES[] = {"omc play", "omc play /tmp/omp-recordings/2026-09-22T10-00-00-1a2b3c4d.ompcast -s 2 -i 1", NULL};

/* parsePositive: NaN when absent, -1 when not a positive finite number */
static double parse_positive(const char *v) {
    if (!v) return NAN;
    char *end = NULL;
    double d = strtod(v, &end);
    while (end && *end == ' ') end++;
    if (!end || end == v || *end || !isfinite(d) || d <= 0) return -1;
    return d;
}

int cmd_play(cparse_t *p) {
    double speed = parse_positive(cp_str(p, "speed"));
    if (speed < 0) return cp_usage_error(p, "--speed must be a positive number");
    if (isnan(speed)) speed = 1;
    double idle = parse_positive(cp_str(p, "idle-limit"));
    if (idle < 0) return cp_usage_error(p, "--idle-limit must be a positive number");
    if (!isatty(STDOUT_FILENO)) return cp_usage_error(p, "omc play needs an interactive terminal");
    const char *arg = cp_arg(p, "file");
    char *file = NULL;
    if (arg && *arg) {
        char *cwd = get_cwd();
        file = path_abs(arg, cwd);
        free(cwd);
    } else file = record_latest();
    if (!file) {
        char *dir = record_dir();
        int rc = cp_usage_error(p, "no recordings in %s; start one with /record", dir);
        free(dir);
        return rc;
    }
    size_t n = 0;
    char *text = fs_read(file, &n);
    if (!text) {
        int rc = cp_usage_error(p, "recording not found: %s", file);
        free(file);
        return rc;
    }
    char *err = NULL;
    json_t *rec = record_parse(text, n, &err);
    free(text);
    if (!rec) {
        int rc = cp_usage_error(p, "%s: %s", file, err);
        free(err);
        free(file);
        return rc;
    }
    record_play(rec, speed, isnan(idle) ? -1 : (int64_t)(idle * 1000));
    json_free(rec);
    free(file);
    return 0;
}
