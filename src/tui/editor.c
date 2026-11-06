/* editor.c - multi-line editor component with undo, kill/yank, history, paste markers and
 * autocomplete. */
#include "tui/editor.h"
#include "core/fs.h"
#include "tui/symbols.h"

#include <ctype.h>
#include <math.h>

enum { ACT_NONE, ACT_TYPE, ACT_OTHER };
#define UNDO_MAX 200
#define HISTORY_MAX 100

/* ---------- autocomplete result ---------- */

void tui_ac_result_add(tui_ac_result_t *r, const char *value, const char *label, const char *description) {
    tui_ac_result_add_icon(r, value, label, description, NULL);
}

void tui_ac_result_add_icon(tui_ac_result_t *r, const char *value, const char *label, const char *description,
                            const char *icon) {
    tui_select_item_t it = {xstrdup(value ? value : ""), label ? xstrdup(label) : NULL,
                            description && *description ? xstrdup(description) : NULL, icon && *icon ? xstrdup(icon) : NULL,
                            NULL, false};
    vec_push(&r->items, it);
}

void tui_ac_result_free(tui_ac_result_t *r) {
    for (size_t i = 0; i < r->items.n; i++) {
        free(r->items.v[i].value);
        free(r->items.v[i].label);
        free(r->items.v[i].description);
        free(r->items.v[i].icon);
        free(r->items.v[i].hint);
    }
    vec_free(&r->items);
    strvec_free(&r->ghosts);
    strvec_free(&r->actions);
    r->prefix_len = 0;
}

void tui_ac_result_add_action(tui_ac_result_t *r, const char *label, const char *description, const char *action) {
    while (r->actions.n < r->items.n) strvec_push(&r->actions, "");
    tui_ac_result_add(r, label, label, description);
    strvec_push(&r->actions, action);
}

/* ---------- slash-command scoring (upstream autocomplete.ts) ---------- */

static bool ac_subsequence(const char *q, const char *t) {
    size_t ql = strlen(q), tl = strlen(t);
    if (!ql) return true;
    if (ql > tl) return false;
    size_t qi = 0;
    for (size_t ti = 0; ti < tl && qi < ql; ti++)
        if (q[qi] == t[ti]) qi++;
    return qi == ql;
}

static double ac_subsequence_score(const char *q, const char *t) {
    if (!*q) return 1;
    if (!strcmp(t, q)) return 100;
    if (str_starts(t, q)) return 80;
    if (strstr(t, q)) return 60;
    size_t ql = strlen(q), qi = 0;
    int gaps = 0;
    long last = -1;
    for (size_t ti = 0; t[ti] && qi < ql; ti++)
        if (q[qi] == t[ti]) {
            if (last >= 0 && (long)ti - last > 1) gaps++;
            last = (long)ti;
            qi++;
        }
    if (qi != ql) return 0;
    return MAX(1, 40 - gaps * 5);
}

static double ac_command_score(const char *q, const char *t) {
    if (!*q) return 1;
    if (!strcmp(q, t)) return 1000;
    if (str_starts(t, q)) return 900;
    return ac_subsequence(q, t) ? ac_subsequence_score(q, t) : 0;
}

/* ---------- default provider ---------- */

tui_ac_default_t *tui_ac_default_new(const char *cwd) {
    tui_ac_default_t *p = xcalloc(1, sizeof *p);
    p->cwd = cwd ? xstrdup(cwd) : NULL;
    p->max_items = 50;
    return p;
}

void tui_ac_default_add_command(tui_ac_default_t *p, const char *name, const char *description) {
    tui_ac_default_add_command_icon(p, name, description, NULL);
}

void tui_ac_default_add_command_icon(tui_ac_default_t *p, const char *name, const char *description, const char *icon) {
    tui_ac_default_add_command_hint(p, name, description, icon, NULL);
}

void tui_ac_default_add_command_hint(tui_ac_default_t *p, const char *name, const char *description, const char *icon,
                                     const char *hint) {
    strvec_push(&p->commands, name);
    strvec_push(&p->descriptions, description ? description : "");
    strvec_push(&p->icons, icon ? icon : "");
    strvec_push(&p->hints, hint ? hint : "");
}

/* ---------- skill namespace (autocomplete.ts) ---------- */

#define SKILL_NS "skill:"

static bool is_skill_cmd(const char *name) { return str_starts(name, SKILL_NS); }

/* commandBreakoutTier: exact / leading-prefix tier */
static double breakout_tier(const char *q, const char *t) {
    if (!strcmp(q, t)) return 1000;
    if (str_starts(t, q)) return 900;
    return 0;
}

/* skillBareNameBreakoutTier: the bare name or one of its hyphen-delimited segments starts with the query */
static double skill_breakout_tier(const char *q, const char *bare) {
    size_t ql = strlen(q), bl = strlen(bare);
    if (!ql) return 0;
    if (!strcmp(q, bare)) return 1000;
    if (str_starts(bare, q)) return 900;
    size_t s = 0;
    while (s < bl) {
        while (s < bl && bare[s] != '-') s++;
        s++;
        if (s >= bl) break;
        if (!strncmp(bare + s, q, ql)) {
            size_t e = s;
            while (e < bl && bare[e] != '-') e++;
            return ql == e - s ? 1000 : 900;
        }
    }
    return 0;
}

/* findLeadingSlashCommandStart / findTrailingSlashCommandStart (/(?:^|\s)\/([^\s/]*)$/) */
static long leading_slash_start(const char *s) {
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    return s[i] == '/' ? (long)i : -1;
}

static long trailing_slash_start(const char *s) {
    size_t n = strlen(s), i = n;
    while (i > 0 && !isspace((unsigned char)s[i - 1]) && s[i - 1] != '/') i--;
    if (i == 0 || s[i - 1] != '/') return -1;
    size_t slash = i - 1;
    if (slash > 0 && !isspace((unsigned char)s[slash - 1])) return -1;
    return (long)slash;
}

static bool blank_span(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!isspace((unsigned char)s[i])) return false;
    return true;
}

/* midPromptSkillTokenMatches */
static bool mid_prompt_skill_matches(const char *q, const char *lname, const char *ldesc);

void tui_ac_default_add_alias(tui_ac_default_t *p, const char *command, const char *alias) {
    strvec_push(&p->alias_names, alias);
    strvec_push(&p->alias_targets, command);
}

void tui_ac_default_free(tui_ac_default_t *p) {
    if (!p) return;
    strvec_free(&p->alias_names);
    strvec_free(&p->alias_targets);
    strvec_free(&p->commands);
    strvec_free(&p->descriptions);
    strvec_free(&p->icons);
    strvec_free(&p->hints);
    for (size_t i = 0; i < p->actions.n; i++) {
        free(p->actions.v[i].id), free(p->actions.v[i].label);
        free(p->actions.v[i].description), free(p->actions.v[i].keywords);
    }
    vec_free(&p->actions);
    free(p->cwd);
    free(p);
}

void tui_ac_default_add_action(tui_ac_default_t *p, const char *id, const char *label, const char *description,
                               const char *keywords) {
    tui_ac_action_t a = {xstrdup(id), xstrdup(label), xstrdup(description ? description : ""),
                         xstrdup(keywords ? keywords : "")};
    vec_push(&p->actions, a);
}

/* ---------- `#` prompt actions and GitHub references (prompt-action-autocomplete.ts, github-ref-autocomplete.ts) ---------- */

static bool gh_boundary(char c) { return isspace((unsigned char)c) || c == '"' || c == '\'' || c == '`' || c == '(' || c == '<' || c == '='; }

/* getGithubRefSuggestions: a standalone `#<number>` (optionally `pr|pull|issue #<number>`) ending the text */
static bool github_ref(const char *t, tui_ac_result_t *out) {
    size_t n = strlen(t), d = n;
    while (d > 0 && isdigit((unsigned char)t[d - 1])) d--;
    if (d == n || d == 0 || t[d - 1] != '#' || t[d] == '0') return false;
    size_t h = d - 1;
    const char *num = t + d;
    /* qualifier form (the leftmost match): `<word><ws>#N`, the word at a boundary */
    size_t ws = h;
    while (ws > 0 && isspace((unsigned char)t[ws - 1])) ws--;
    int qual = 0; /* 1 pr, 2 issue */
    size_t start = h;
    if (ws < h) {
        static const char *const words[] = {"pr", "pull", "issue"};
        for (size_t i = 0; i < 3 && !qual; i++) {
            size_t wl = strlen(words[i]);
            if (ws < wl || strncasecmp(t + ws - wl, words[i], wl)) continue;
            size_t ws0 = ws - wl;
            if (ws0 == 0 || gh_boundary(t[ws0 - 1])) {
                qual = i == 2 ? 2 : 1;
                start = ws0;
            }
        }
    }
    if (!qual && h > 0 && !gh_boundary(t[h - 1])) return false;
    static const struct {
        const char *scheme, *label, *desc;
    } KINDS[] = {{"pr", "PR", "GitHub pull request"}, {"issue", "Issue", "GitHub issue"}};
    for (int k = 0; k < 2; k++) {
        if (qual && qual != k + 1) continue;
        char *value = xasprintf("%s://%s ", KINDS[k].scheme, num), *label = xasprintf("%s #%s", KINDS[k].label, num);
        tui_ac_result_add(out, value, label, KINDS[k].desc);
        free(value), free(label);
    }
    out->prefix_len = n - start;
    return true;
}

/* getPromptActionPrefix + the ranked action list */
static bool prompt_actions(const tui_ac_default_t *p, const char *t, tui_ac_result_t *out) {
    const char *hash = strrchr(t, '#');
    if (!hash || !p->actions.n || strpbrk(hash + 1, " \t\r\n\v\f")) return false;
    char *q = str_lower(hash + 1);
    size_t n = p->actions.n, m = 0;
    size_t *order = xcalloc(n, sizeof *order);
    double *score = xcalloc(n, sizeof *score);
    for (size_t i = 0; i < n; i++) {
        const tui_ac_action_t *a = &p->actions.v[i];
        char *s = xasprintf("%s %s %s", a->label, a->description, a->keywords), *ls = str_lower(s);
        if (ac_subsequence(q, ls)) {
            score[i] = ac_subsequence_score(q, ls);
            order[m++] = i;
        }
        free(s), free(ls);
    }
    for (size_t a = 1; a < m; a++) /* stable, score descending */
        for (size_t b = a; b > 0 && score[order[b]] > score[order[b - 1]]; b--) {
            size_t tmp = order[b];
            order[b] = order[b - 1];
            order[b - 1] = tmp;
        }
    for (size_t k = 0; k < m; k++)
        tui_ac_result_add_action(out, p->actions.v[order[k]].label, p->actions.v[order[k]].description,
                                 p->actions.v[order[k]].id);
    if (m) out->prefix_len = strlen(hash);
    free(order), free(score), free(q);
    return m > 0;
}

static bool mid_prompt_skill_matches(const char *q, const char *lname, const char *ldesc) {
    if (str_starts(SKILL_NS, q)) return true;
    if (str_starts(q, SKILL_NS)) return ac_command_score(q, lname) > 0 || (*ldesc && ac_command_score(q, ldesc) > 0);
    return is_skill_cmd(lname) && skill_breakout_tier(q, lname + strlen(SKILL_NS)) > 0;
}

/* buildSlashCommandCompletions over the registered commands (collapseSkillNamespace applied unless `mid`,
 * which keeps skills only: buildMidPromptSkillCompletions). Items insert "/<name> " ("/skill:" for the
 * namespace row). */
static void slash_completions(tui_ac_default_t *p, const char *q, bool mid, tui_ac_result_t *out) {
    size_t n = p->commands.n;
    /* collapseSkillNamespace */
    bool collapse = !mid && !str_starts(q, SKILL_NS);
    bool approaches = str_starts(SKILL_NS, q);
    double command_tier = 0;
    size_t skill_count = 0;
    const char *skill_icon = NULL;
    if (collapse && !approaches) {
        for (size_t i = 0; i < n && command_tier < 1000; i++) {
            if (is_skill_cmd(p->commands.v[i])) continue;
            char *ln = str_lower(p->commands.v[i]);
            command_tier = MAX(command_tier, breakout_tier(q, ln));
            free(ln);
            for (size_t k = 0; k < p->alias_names.n; k++) {
                if (strcmp(p->alias_targets.v[k], p->commands.v[i])) continue;
                char *la = str_lower(p->alias_names.v[k]);
                command_tier = MAX(command_tier, breakout_tier(q, la));
                free(la);
            }
        }
    }
    /* candidate rows: registered commands, then the namespace row (index n) */
    double *score = xcalloc(n + 1, sizeof *score);
    size_t *order = xcalloc(n + 1, sizeof *order);
    bool *desc_only = xcalloc(n + 1, sizeof *desc_only);
    int *alias_hit = xcalloc(n + 1, sizeof *alias_hit);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        const char *name = p->commands.v[i];
        bool skill = is_skill_cmd(name);
        char *ln = str_lower(name), *ld = str_lower(p->descriptions.v[i]);
        bool keep = true;
        if (mid) keep = skill && mid_prompt_skill_matches(q, ln, ld);
        else if (collapse && skill) {
            skill_count++;
            if (!skill_icon && i < p->icons.n) skill_icon = p->icons.v[i];
            keep = !approaches && skill_breakout_tier(q, ln + strlen(SKILL_NS)) > command_tier;
        }
        alias_hit[i] = -1;
        if (!keep) {
            free(ln);
            free(ld);
            continue;
        }
        double ns = !*q && skill ? 950
                    : skill      ? MAX(ac_command_score(q, ln), skill_breakout_tier(q, ln + strlen(SKILL_NS)))
                                 : ac_command_score(q, ln);
        double ds = *ld && ac_subsequence(q, ld) ? ac_subsequence_score(q, ld) * 0.5 : 0;
        double s = ns > ds ? ns : ds;
        desc_only[i] = ns <= 0;
        free(ln);
        free(ld);
        /* aliases: a better alias hit shows the alias as the row (and accepts the command) */
        for (size_t k = 0; *q && !mid && k < p->alias_names.n; k++) {
            if (strcmp(p->alias_targets.v[k], name)) continue;
            char *la = str_lower(p->alias_names.v[k]);
            double as = ac_command_score(q, la);
            free(la);
            if (as <= 0) continue;
            desc_only[i] = false;
            if (as > s) {
                s = as;
                alias_hit[i] = (int)k;
            }
        }
        if (s <= 0) continue;
        score[i] = s;
        order[m++] = i;
    }
    bool group = collapse && skill_count && approaches;
    if (group) { /* the `/skill:` namespace row, scored like a skill command */
        score[n] = !*q ? 950 : ac_command_score(q, SKILL_NS);
        char *ds = xasprintf("%zu skill%s", skill_count, skill_count == 1 ? "" : "s");
        double d = ac_subsequence(q, ds) && *q ? ac_subsequence_score(q, ds) * 0.5 : 0;
        free(ds);
        if (d > score[n]) score[n] = d;
        alias_hit[n] = -1;
        if (score[n] > 0) order[m++] = n;
    }
    /* stable sort by score desc */
    for (size_t a = 1; a < m; a++)
        for (size_t b = a; b > 0 && score[order[b]] > score[order[b - 1]]; b--) {
            size_t t = order[b];
            order[b] = order[b - 1];
            order[b - 1] = t;
        }
    for (size_t k = 0; k < m && (int)out->items.n < p->max_items; k++) {
        size_t i = order[k];
        if (i == n) {
            char *d = xasprintf("%zu skill%s", skill_count, skill_count == 1 ? "" : "s");
            tui_ac_result_add_icon(out, "/" SKILL_NS, SKILL_NS, d, skill_icon);
            free(d);
            continue;
        }
        const char *shown = alias_hit[i] >= 0 ? p->alias_names.v[alias_hit[i]] : p->commands.v[i];
        char *val = xasprintf("/%s ", shown);
        char *live = p->describe ? p->describe(p->describe_ud, p->commands.v[i]) : NULL;
        const char *desc = live ? live : p->descriptions.v[i];
        const char *hint = i < p->hints.n ? p->hints.v[i] : "";
        char *full = *hint ? (*desc ? xasprintf("%s - %s", hint, desc) : xstrdup(hint)) : xstrdup(desc);
        tui_ac_result_add_icon(out, val, shown, full, i < p->icons.n ? p->icons.v[i] : NULL);
        free(full);
        free(live);
        free(val);
    }
    if (m) out->first_desc_match = order[0] < n && desc_only[order[0]];
    free(desc_only);
    free(alias_hit);
    free(score);
    free(order);
}

bool tui_ac_default_provider(void *ud, const char *line_before, bool first_line, bool forced, tui_ac_result_t *out) {
    tui_ac_default_t *p = ud;
    size_t len = strlen(line_before);
    /* slash commands (CombinedAutocompleteProvider.getSuggestions): a leading `/token` of the submitted
     * prompt completes command names (upstream buildSlashCommandCompletions ranking: exact 1000, prefix 900,
     * else subsequence tiers; a description subsequence scores half); a `/token` after prompt text looks up
     * skills only */
    long lead = leading_slash_start(line_before), trail = trailing_slash_start(line_before);
    bool text_before_trail = trail >= 0 && (!first_line || !blank_span(line_before, (size_t)trail));
    bool text_before_lead = lead >= 0 && !first_line;
    long start = text_before_trail ? trail : text_before_lead ? -1 : lead;
    if (start >= 0) {
        const char *cmd_text = line_before + start;
        const char *sp = strchr(cmd_text, ' ');
        bool mid = text_before_trail;
        if (!sp) {
            char *q = str_lower(cmd_text + 1);
            slash_completions(p, q, mid, out);
            free(q);
            if (out->items.n) {
                out->prefix_len = len - (size_t)start;
                out->slash = true;
                return true;
            }
            if (!mid && start == lead && !strchr(cmd_text + 1, '/')) return false;
        } else if (!mid && p->args) {
            /* "/cmd args": the command's argument completions first, then path completion */
            char *cmd = xstrndup(cmd_text + 1, (size_t)(sp - cmd_text - 1));
            const char *arg = sp + 1;
            int r = p->args(p->args_ud, cmd, arg, out);
            free(cmd);
            if (r < 0) return false;
            if (r > 0 && out->items.n) {
                out->prefix_len = strlen(arg);
                out->args = true;
                return true;
            }
        }
    }
    /* PromptActionAutocompleteProvider order: GitHub `#N` refs, `#` prompt actions (not inside slash-command
     * arguments, where the base provider's `@` mentions come first), then `@` file mentions */
    bool slash_args = lead >= 0 && !text_before_lead && strchr(line_before + lead, ' ');
    long at = tui_ac_at_prefix_start(line_before);
    if (slash_args && at >= 0 && tui_ac_at_suggestions(p->cwd, line_before + at, out)) return true;
    if (github_ref(line_before, out)) return true;
    if (!slash_args && prompt_actions(p, line_before, out)) return true;
    if (at >= 0) return tui_ac_at_suggestions(p->cwd, line_before + at, out);
    /* Tab: complete the path token before the cursor */
    size_t s = len;
    while (s > 0 && line_before[s - 1] != ' ' && line_before[s - 1] != '\t') s--;
    if (forced && s < len) return tui_ac_path_suggestions(p->cwd, line_before + s, out);
    return false;
}

char *tui_ac_default_inline_hint(void *ud, const char *line_before, bool first_line) {
    tui_ac_default_t *p = ud;
    if (!p || !first_line || line_before[0] != '/') return NULL;
    size_t sp = strcspn(line_before, " \t");
    if (!line_before[sp]) return NULL;
    char *cmd = xstrndup(line_before + 1, sp - 1);
    const char *arg = line_before + sp + 1;
    /* the first command with this name owns the hint: a registered argument hint (buildStaticInlineHint:
     * shown while no argument is typed), else the builtin's own hint logic */
    char *h = NULL;
    bool owned = false;
    for (size_t i = 0; i < p->commands.n && !owned; i++) {
        if (strcmp(p->commands.v[i], cmd)) continue;
        owned = true;
        if (i < p->hints.n && *p->hints.v[i]) h = blank_span(arg, strlen(arg)) ? xstrdup(p->hints.v[i]) : NULL;
        else if (p->inline_hint) h = p->inline_hint(p->args_ud, cmd, arg);
    }
    if (!owned && p->inline_hint) h = p->inline_hint(p->args_ud, cmd, arg);
    free(cmd);
    return h;
}

/* ---------- helpers ---------- */

static char *cur_line(tui_editor_t *ed) { return ed->lines.v[ed->cl]; }

static void push_undo(tui_editor_t *ed, int action);

/* collapseToAtom for every completed token the host recognizes (skill chips) */
static void collapse_tokens(tui_editor_t *ed) {
    if (!ed->token_fn || ed->collapsing) return;
    ed->collapsing = true;
    char *text = NULL;
    for (size_t i = 0; i < ed->lines.n; i++) {
        size_t from = 0;
        for (;;) {
            if (!text) {
                sb_t b = SB_INIT;
                for (size_t k = 0; k < ed->lines.n; k++) sb_appendf(&b, "%s%s", k ? "\n" : "", ed->lines.v[k]);
                text = sb_detach(&b);
            }
            const char *line = ed->lines.v[i];
            size_t s, e;
            char *label = NULL, *exp = NULL, *style = NULL;
            if (!ed->token_fn(ed->token_ud, text, line, from, &s, &e, &label, &exp, &style)) break;
            size_t len = strlen(line);
            if (e > len || s >= e || (e == len && i + 1 == ed->lines.n)) {
                free(label);
                free(exp);
                free(style);
                break;
            }
            push_undo(ed, ACT_OTHER);
            tui_editor_paste_t pp = {0, label, exp, style};
            vec_push(&ed->pastes, pp);
            size_t ll = strlen(label);
            char *nl = xasprintf("%.*s%s%s", (int)s, line, label, line + e);
            if (ed->cl == i && ed->cc > s) ed->cc = ed->cc >= e ? ed->cc - (e - s) + ll : s + ll;
            free(ed->lines.v[i]);
            ed->lines.v[i] = nl;
            ed->last_action = ACT_NONE;
            from = s + ll;
            free(text);
            text = NULL;
        }
    }
    free(text);
    ed->collapsing = false;
}

static void notify_change(tui_editor_t *ed) {
    collapse_tokens(ed);
    if (ed->on_change) ed->on_change(ed, ed->cb_ud);
    tui_component_request_render(&ed->base);
}

static size_t prev_char(const char *s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static size_t next_char(const char *s, size_t i) {
    size_t n = strlen(s);
    if (i >= n) return n;
    i++;
    while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static void set_line(tui_editor_t *ed, size_t idx, char *s) {
    free(ed->lines.v[idx]);
    ed->lines.v[idx] = s;
}

static void insert_line(tui_editor_t *ed, size_t idx, char *s) {
    vec_push(&ed->lines, NULL);
    memmove(ed->lines.v + idx + 1, ed->lines.v + idx, (ed->lines.n - 1 - idx) * sizeof(char *));
    ed->lines.v[idx] = s;
}

static void remove_line(tui_editor_t *ed, size_t idx) {
    free(ed->lines.v[idx]);
    memmove(ed->lines.v + idx, ed->lines.v + idx + 1, (ed->lines.n - idx - 1) * sizeof(char *));
    ed->lines.n--;
}

static void snapshot_free(tui_editor_snapshot_t *s) { strvec_free(&s->lines); }

static void push_undo(tui_editor_t *ed, int action) {
    if (action == ACT_TYPE && ed->last_action == ACT_TYPE) return;
    ed->last_action = action;
    tui_editor_snapshot_t s = {{0}, ed->cl, ed->cc};
    for (size_t i = 0; i < ed->lines.n; i++) strvec_push(&s.lines, ed->lines.v[i]);
    if (ed->undo.n >= UNDO_MAX) {
        snapshot_free(&ed->undo.v[0]);
        memmove(ed->undo.v, ed->undo.v + 1, (ed->undo.n - 1) * sizeof(s));
        ed->undo.n--;
    }
    vec_push(&ed->undo, s);
}

void tui_editor_undo(tui_editor_t *ed) {
    if (!ed->undo.n) return;
    tui_editor_snapshot_t s = vec_pop(&ed->undo);
    strvec_free(&ed->lines);
    ed->lines = s.lines;
    ed->cl = MIN(s.cl, ed->lines.n - 1);
    ed->cc = MIN(s.cc, strlen(cur_line(ed)));
    ed->last_action = ACT_NONE;
    ed->preferred_col = -1;
    notify_change(ed);
}

static void close_ac(tui_editor_t *ed) {
    if (ed->ac_list) tui_component_destroy(&ed->ac_list->base);
    ed->ac_list = NULL;
    ed->ac_args = false;
    strvec_free(&ed->ac_ghosts);
    strvec_free(&ed->ac_actions);
}

/* raw insertion of text (may contain '\n') at the cursor */
static void insert_raw(tui_editor_t *ed, const char *text) {
    char *line = cur_line(ed);
    char *after = xstrdup(line + ed->cc);
    line[ed->cc] = 0;
    const char *p = text;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char *joined = xasprintf("%s%.*s", cur_line(ed), (int)n, p);
        set_line(ed, ed->cl, joined);
        ed->cc = strlen(joined);
        if (!nl) break;
        ed->cl++;
        insert_line(ed, ed->cl, xstrdup(""));
        ed->cc = 0;
        p = nl + 1;
    }
    char *fin = xasprintf("%s%s", cur_line(ed), after);
    set_line(ed, ed->cl, fin);
    free(after);
}

/* delete [from_line:from_col, to_line:to_col) and return it (owned) */
static char *delete_range(tui_editor_t *ed, size_t fl, size_t fc, size_t tl, size_t tc) {
    sb_t killed = SB_INIT;
    if (fl == tl) {
        char *l = ed->lines.v[fl];
        sb_appendn(&killed, l + fc, tc - fc);
        memmove(l + fc, l + tc, strlen(l + tc) + 1);
    } else {
        sb_append(&killed, ed->lines.v[fl] + fc);
        for (size_t i = fl + 1; i < tl; i++) {
            sb_putc(&killed, '\n');
            sb_append(&killed, ed->lines.v[i]);
        }
        sb_putc(&killed, '\n');
        sb_appendn(&killed, ed->lines.v[tl], tc);
        char *merged = xasprintf("%.*s%s", (int)fc, ed->lines.v[fl], ed->lines.v[tl] + tc);
        for (size_t i = tl; i > fl; i--) remove_line(ed, i);
        set_line(ed, fl, merged);
    }
    ed->cl = fl;
    ed->cc = fc;
    return sb_detach(&killed);
}

/* ---------- kill ring (kill-ring.ts) ---------- */

enum { KILL_NONE, KILL_KILL, KILL_YANK };
#define KILL_RING_MAX 60

/* Record killed text (owned). A kill right after another kill accumulates into the newest entry:
 * prepended for backward deletions, appended for forward ones. */
static void record_kill(tui_editor_t *ed, char *k, bool backward, int prev_state) {
    if (!k || !*k) {
        free(k);
        return;
    }
    if (prev_state == KILL_KILL && ed->kills.n) {
        char *last = ed->kills.v[ed->kills.n - 1];
        ed->kills.v[ed->kills.n - 1] = backward ? xasprintf("%s%s", k, last) : xasprintf("%s%s", last, k);
        free(last);
        free(k);
    } else {
        strvec_push_own(&ed->kills, k);
        if (ed->kills.n > KILL_RING_MAX) {
            free(ed->kills.v[0]);
            memmove(ed->kills.v, ed->kills.v + 1, (ed->kills.n - 1) * sizeof(char *));
            ed->kills.n--;
        }
    }
    ed->kill_state = KILL_KILL;
}

static const char *kill_peek(const tui_editor_t *ed) { return ed->kills.n ? ed->kills.v[ed->kills.n - 1] : NULL; }

/* move the newest entry to the front (yank-pop cycling) */
static void kill_rotate(tui_editor_t *ed) {
    if (ed->kills.n < 2) return;
    char *last = ed->kills.v[ed->kills.n - 1];
    memmove(ed->kills.v + 1, ed->kills.v, (ed->kills.n - 1) * sizeof(char *));
    ed->kills.v[0] = last;
}

/* ---------- paste markers ---------- */

/* If a known atom (paste marker / chip) ends exactly at `col`, return its start; else -1. */
static long marker_ending_at(const tui_editor_t *ed, const char *line, size_t col) {
    if (col == 0) return -1;
    for (size_t i = 0; i < ed->pastes.n; i++) {
        const char *m = ed->pastes.v[i].marker;
        size_t ml = strlen(m);
        if (ml <= col && !memcmp(line + col - ml, m, ml)) return (long)(col - ml);
    }
    return -1;
}

static long marker_starting_at(const tui_editor_t *ed, const char *line, size_t col) {
    for (size_t i = 0; i < ed->pastes.n; i++) {
        const char *m = ed->pastes.v[i].marker;
        size_t ml = strlen(m);
        if (!strncmp(line + col, m, ml)) return (long)(col + ml);
    }
    return -1;
}

static void paste_free(tui_editor_paste_t *p) {
    free(p->marker);
    free(p->content);
    free(p->style);
}

void tui_editor_insert_atom(tui_editor_t *ed, const char *label, const char *expansion, const char *style) {
    ed->hist_index = -1;
    push_undo(ed, ACT_OTHER);
    tui_editor_paste_t pp = {0, xstrdup(label), xstrdup(expansion), style ? xstrdup(style) : NULL};
    vec_push(&ed->pastes, pp);
    insert_raw(ed, label);
    insert_raw(ed, " "); /* omp insertAtom: `${label} ` */
    ed->last_action = ACT_OTHER;
    ed->preferred_col = -1;
    notify_change(ed);
}

void tui_editor_set_paste_hook(tui_editor_t *ed, bool (*fn)(void *ud, tui_editor_t *ed, const char *text), void *ud) {
    ed->paste_hook = fn;
    ed->paste_hook_ud = ud;
}

void tui_editor_set_token_hook(tui_editor_t *ed,
                               bool (*fn)(void *ud, const char *text, const char *line, size_t from, size_t *start, size_t *end,
                                          char **label, char **expansion, char **style),
                               void *ud) {
    ed->token_fn = fn;
    ed->token_ud = ud;
}

void tui_editor_set_large_paste_hook(tui_editor_t *ed,
                                     bool (*fn)(void *ud, tui_editor_t *ed, const char *text, size_t line_count),
                                     void *ud) {
    ed->large_paste_hook = fn;
    ed->large_paste_ud = ud;
}

static void texts_clear(tui_editor_t *ed) {
    for (size_t i = 0; i < ed->texts.n; i++) {
        free(ed->texts.v[i].label);
        free(ed->texts.v[i].content);
    }
    ed->texts.n = 0;
    ed->text_counter = 0;
}

int tui_editor_insert_text_attachment(tui_editor_t *ed, const char *content, const char *expansion, const char *style) {
    int n = ++ed->text_counter;
    tui_editor_text_att_t t = {n, xasprintf("%s #%d", tui_sym("chip.paste"), n), xstrdup(content),
                               1 + str_count(content, "\n"), utf8_strlen(content)};
    vec_push(&ed->texts, t);
    tui_editor_insert_atom(ed, t.label, expansion ? expansion : content, style);
    return n;
}

/* paint styled atoms (omp chip style: bold + identity color, then a reset the text coloring re-opens) */
static char *style_atoms(const tui_editor_t *ed, char *s, bool surface) {
    for (size_t i = 0; i < ed->pastes.n; i++) {
        const tui_editor_paste_t *p = &ed->pastes.v[i];
        if (!p->style || !strstr(s, p->marker)) continue;
        /* a pill (style with a background, skill chips) restores the default background too */
        bool pill = strstr(p->style, "[48") != NULL;
        char *rep = xasprintf("\x1b[1m%s%s%s", p->style, p->marker,
                              surface ? (pill ? "\x1b[22m\x1b[39m\x1b[49m" : "\x1b[22m\x1b[39m") : "\x1b[0m");
        char *t = str_replace(s, p->marker, rep);
        free(rep);
        free(s);
        s = t;
    }
    return s;
}

static char *sanitize_paste(const char *text) {
    sb_t b = SB_INIT;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r') {
            sb_putc(&b, '\n');
            if (p[1] == '\n') p++;
        } else if (c == '\t') {
            tui_sb_spaces(&b, TUI_TAB_WIDTH);
        } else if (c == '\n' || c >= 0x20) {
            if (c != 0x7f) sb_putc(&b, (char)c);
        }
    }
    char *s = sb_detach(&b);
    char *clean = utf8_sanitize(s, strlen(s));
    free(s);
    return clean;
}

void tui_editor_paste(tui_editor_t *ed, const char *text) {
    char *clean = sanitize_paste(text ? text : "");
    if (!*clean) {
        free(clean);
        return;
    }
    if ((clean[0] == '/' || clean[0] == '~' || clean[0] == '.') && ed->cc > 0) {
        char prev = cur_line(ed)[ed->cc - 1];
        if (isalnum((unsigned char)prev) || prev == '_') {
            char *t = xasprintf(" %s", clean);
            free(clean);
            clean = t;
        }
    }
    size_t nlines = 1 + str_count(clean, "\n");
    size_t nchars = utf8_strlen(clean);
    /* the host may take a marker-sized paste (large-paste menu / text-attachment chip) */
    if ((nlines > 10 || nchars > 1000) && ed->large_paste_hook &&
        ed->large_paste_hook(ed->large_paste_ud, ed, clean, nlines)) {
        free(clean);
        return;
    }
    ed->hist_index = -1;
    push_undo(ed, ACT_OTHER);
    if (nlines > 10 || nchars > 1000) {
        int id = ++ed->paste_counter;
        char *marker = nlines > 10 ? xasprintf("[Paste #%d, +%zu lines]", id, nlines)
                                   : xasprintf("[Paste #%d, %zu chars]", id, nchars);
        tui_editor_paste_t pp = {id, marker, clean, NULL};
        vec_push(&ed->pastes, pp);
        insert_raw(ed, marker);
    } else {
        insert_raw(ed, clean);
        free(clean);
    }
    ed->last_action = ACT_OTHER;
    ed->preferred_col = -1;
    notify_change(ed);
}

/* ---------- layout ---------- */

typedef struct {
    size_t line, start, end;
} seg_t;
typedef VEC(seg_t) segvec_t;

static void layout(const tui_editor_t *ed, int W, segvec_t *segs) {
    if (W < 1) W = 1;
    for (size_t li = 0; li < ed->lines.n; li++) {
        const char *s = ed->lines.v[li];
        size_t n = strlen(s);
        size_t seg_start = 0, last_break = 0;
        int col = 0;
        size_t i = 0;
        while (i < n) {
            uint32_t cp;
            int k = utf8_decode(s + i, n - i, &cp);
            int cw = tui_cp_width(cp);
            if (col + cw > W && i > seg_start) {
                size_t brk = last_break > seg_start ? last_break : i;
                seg_t sg = {li, seg_start, brk};
                vec_push(segs, sg);
                seg_start = brk;
                last_break = 0;
                col = tui_visible_width_n(s + seg_start, i - seg_start);
            }
            col += cw;
            i += (size_t)k;
            if (cp == ' ') last_break = i;
        }
        seg_t sg = {li, seg_start, n};
        vec_push(segs, sg);
    }
}

static size_t cursor_seg(const tui_editor_t *ed, const segvec_t *segs) {
    for (size_t i = 0; i < segs->n; i++) {
        const seg_t *s = &segs->v[i];
        if (s->line != ed->cl) continue;
        bool last_of_line = i + 1 == segs->n || segs->v[i + 1].line != s->line;
        if (ed->cc >= s->start && (ed->cc < s->end || (last_of_line && ed->cc == s->end))) return i;
    }
    return 0;
}

static int effective_layout_width(const tui_editor_t *ed, int width) {
    int content = MAX(1, width - 2 * ed->padding_x);
    return MAX(1, content - 1);
}

/* ---------- movement ---------- */

static size_t offset_for_col(const char *s, size_t start, size_t end, int target, bool is_last) {
    int col = 0;
    size_t i = start;
    while (i < end) {
        uint32_t cp;
        int k = utf8_decode(s + i, end - i, &cp);
        int cw = tui_cp_width(cp);
        if (col + cw > target) break;
        col += cw;
        i += (size_t)k;
    }
    if (!is_last && i >= end && end > start) i = prev_char(s, end);
    return i;
}

/* Move cursor by visual rows; returns false if at the boundary. */
static bool move_vertical(tui_editor_t *ed, int dir) {
    segvec_t segs = {0};
    layout(ed, ed->layout_width > 0 ? ed->layout_width : 79, &segs);
    size_t si = cursor_seg(ed, &segs);
    const seg_t *cs = &segs.v[si];
    int col = tui_visible_width_n(ed->lines.v[ed->cl] + cs->start, ed->cc - cs->start);
    if (ed->preferred_col < 0) ed->preferred_col = col;
    long ti = (long)si + dir;
    if (ti < 0 || ti >= (long)segs.n) {
        vec_free(&segs);
        return false;
    }
    const seg_t *ts = &segs.v[ti];
    bool is_last = (size_t)ti + 1 == segs.n || segs.v[ti + 1].line != ts->line;
    ed->cl = ts->line;
    ed->cc = offset_for_col(ed->lines.v[ts->line], ts->start, ts->end, ed->preferred_col, is_last);
    vec_free(&segs);
    return true;
}

static bool on_first_visual_line(tui_editor_t *ed) {
    segvec_t segs = {0};
    layout(ed, ed->layout_width > 0 ? ed->layout_width : 79, &segs);
    bool r = cursor_seg(ed, &segs) == 0;
    vec_free(&segs);
    return r;
}

static bool on_last_visual_line(tui_editor_t *ed) {
    segvec_t segs = {0};
    layout(ed, ed->layout_width > 0 ? ed->layout_width : 79, &segs);
    bool r = cursor_seg(ed, &segs) + 1 == segs.n;
    vec_free(&segs);
    return r;
}

static int char_class(const char *s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t') return 0;
    if (isalnum(c) || c == '_' || c >= 0x80) return 1;
    return 2;
}

static void word_left(tui_editor_t *ed) {
    if (ed->cc == 0) {
        if (ed->cl > 0) {
            ed->cl--;
            ed->cc = strlen(cur_line(ed));
        }
        return;
    }
    const char *s = cur_line(ed);
    size_t i = ed->cc;
    while (i > 0 && char_class(s, prev_char(s, i)) == 0) i = prev_char(s, i);
    if (i > 0) {
        int cls = char_class(s, prev_char(s, i));
        while (i > 0 && char_class(s, prev_char(s, i)) == cls) i = prev_char(s, i);
    }
    ed->cc = i;
}

static size_t word_right_pos(const char *s, size_t i) {
    size_t n = strlen(s);
    while (i < n && char_class(s, i) == 0) i = next_char(s, i);
    if (i < n) {
        int cls = char_class(s, i);
        while (i < n && char_class(s, i) == cls) i = next_char(s, i);
    }
    return i;
}

static void word_right(tui_editor_t *ed) {
    const char *s = cur_line(ed);
    if (ed->cc >= strlen(s)) {
        if (ed->cl + 1 < ed->lines.n) {
            ed->cl++;
            ed->cc = 0;
        }
        return;
    }
    ed->cc = word_right_pos(s, ed->cc);
}

/* ---------- history ---------- */

void tui_editor_add_history(tui_editor_t *ed, const char *text) {
    if (!text || !*text) return;
    if (ed->history.n && !strcmp(ed->history.v[ed->history.n - 1], text)) return;
    if (ed->history.n >= HISTORY_MAX) {
        free(ed->history.v[0]);
        memmove(ed->history.v, ed->history.v + 1, (ed->history.n - 1) * sizeof(char *));
        ed->history.n--;
    }
    strvec_push(&ed->history, text);
}

static void set_text_internal(tui_editor_t *ed, const char *text) {
    strvec_free(&ed->lines);
    ed->lines = str_split(text ? text : "", "\n");
    if (!ed->lines.n) strvec_push(&ed->lines, "");
    ed->cl = ed->lines.n - 1;
    ed->cc = strlen(cur_line(ed));
    ed->preferred_col = -1;
}

static void history_nav(tui_editor_t *ed, int dir) {
    /* dir -1 = older, +1 = newer */
    int n = (int)ed->history.n;
    if (!n) return;
    int idx = ed->hist_index;
    if (dir < 0) {
        if (idx + 1 >= n) return;
        if (idx == -1) {
            free(ed->hist_draft);
            ed->hist_draft = tui_editor_get_text(ed);
        }
        idx++;
    } else {
        if (idx < 0) return;
        idx--;
    }
    ed->hist_index = idx;
    push_undo(ed, ACT_OTHER);
    set_text_internal(ed, idx < 0 ? (ed->hist_draft ? ed->hist_draft : "") : ed->history.v[n - 1 - idx]);
    notify_change(ed);
}

/* ---------- autocomplete ---------- */

/* #hasOnlyWhitespaceBeforeCursorLine: the cursor's line starts the submitted prompt */
static bool lines_above_blank(const tui_editor_t *ed) {
    for (size_t i = 0; i < ed->cl; i++)
        if (!blank_span(ed->lines.v[i], strlen(ed->lines.v[i]))) return false;
    return true;
}

static void refresh_ac(tui_editor_t *ed, bool forced) {
    if (!ed->ac_fn) return;
    char *before = xstrndup(cur_line(ed), ed->cc);
    tui_ac_result_t r;
    memset(&r, 0, sizeof r);
    bool ok = ed->ac_fn(ed->ac_ud, before, lines_above_blank(ed), forced, &r);
    free(before);
    if (!ok || !r.items.n || r.prefix_len > ed->cc) {
        tui_ac_result_free(&r);
        close_ac(ed);
        return;
    }
    close_ac(ed);
    ed->ac_list = tui_select_list_new(ed->ac_max_visible);
    ed->ac_list->filter_enabled = false;
    if (r.slash) {
        /* SLASH_COMMAND_SELECT_LIST_LAYOUT */
        ed->ac_list->layout.min_primary = 12;
        ed->ac_list->layout.max_primary = 32;
        ed->ac_list->layout.wrap_description = true;
        ed->ac_list->layout.max_description_rows = 2;
    }
    ed->ac_list->base.tui = ed->base.tui;
    /* upstream creates a fresh list per update (#createAutocompleteList): the selection restarts at the top */
    for (size_t i = 0; i < r.items.n; i++)
        tui_select_list_add_item(ed->ac_list, r.items.v[i].value, r.items.v[i].label, r.items.v[i].description,
                                 r.items.v[i].icon, false);
    for (size_t i = 0; i < r.items.n; i++) strvec_push(&ed->ac_ghosts, i < r.ghosts.n ? r.ghosts.v[i] : "");
    for (size_t i = 0; i < r.items.n; i++) strvec_push(&ed->ac_actions, i < r.actions.n ? r.actions.v[i] : "");
    ed->ac_args = r.args;
    ed->ac_list->selected = 0;
    ed->ac_enter_accepts = !r.first_desc_match;
    ed->ac_prefix_len = r.prefix_len;
    tui_ac_result_free(&r);
}

static void maybe_trigger_ac(tui_editor_t *ed) {
    if (!ed->ac_fn) return;
    if (ed->ac_list) {
        refresh_ac(ed, false);
        return;
    }
    const char *line = cur_line(ed);
    bool trig = false;
    bool above_blank = lines_above_blank(ed);
    if (above_blank && leading_slash_start(line) >= 0) trig = true; /* command names, then their arguments */
    if (!trig) { /* #isInMidPromptSkillSlashContext: a `/token` after prompt text looks up skills */
        char *before = xstrndup(line, ed->cc);
        long t = trailing_slash_start(before);
        if (t >= 0 && !(above_blank && leading_slash_start(before) >= 0) && (!above_blank || !blank_span(before, (size_t)t)))
            trig = true;
        free(before);
    }
    if (!trig) { /* an `@` file token, or a `#` prompt action / reference (`#[^\s#]*$`) */
        char *before = xstrndup(line, ed->cc);
        const char *hash = strrchr(before, '#');
        trig = tui_ac_at_prefix_start(before) >= 0 || (hash && !strpbrk(hash + 1, " \t\r\n\v\f"));
        free(before);
    }
    if (trig) refresh_ac(ed, false);
}

static void accept_ac(tui_editor_t *ed) {
    const tui_select_item_t *it = ed->ac_list ? tui_select_list_selected(ed->ac_list) : NULL;
    if (!it) {
        close_ac(ed);
        return;
    }
    size_t sel = ed->ac_list->selected;
    const char *action = sel < ed->ac_actions.n ? ed->ac_actions.v[sel] : "";
    if (*action) { /* `#` prompt action: drop the typed `#query` (undo keeps it for undoPastTransientText), run it */
        char *act = xstrdup(action);
        size_t start = ed->cc - MIN(ed->ac_prefix_len, ed->cc);
        char *prefix = xstrndup(cur_line(ed) + start, ed->cc - start);
        if (strcmp(act, "undo")) {
            char *line = cur_line(ed);
            set_line(ed, ed->cl, xasprintf("%.*s%s", (int)start, line, line + ed->cc));
            ed->cc = start;
        }
        close_ac(ed);
        notify_change(ed);
        if (ed->on_action) ed->on_action(ed, act, prefix, ed->action_ud);
        free(prefix);
        free(act);
        return;
    }
    char *val = xstrdup(it->value);
    push_undo(ed, ACT_OTHER);
    size_t start = ed->cc - MIN(ed->ac_prefix_len, ed->cc);
    char *line = cur_line(ed);
    char *nl = xasprintf("%.*s%s%s", (int)start, line, val, line + ed->cc);
    ed->cc = start + strlen(val);
    set_line(ed, ed->cl, nl);
    close_ac(ed);
    if (str_ends(val, "/")) refresh_ac(ed, true);
    else if (!strcmp(val, "/" SKILL_NS)) refresh_ac(ed, false); /* the namespace row reopens on the skills */
    free(val);
    ed->last_action = ACT_OTHER;
    notify_change(ed);
}

/* ---------- key handling ---------- */

static void do_submit(tui_editor_t *ed) {
    if (ed->disable_submit) return;
    char *expanded = tui_editor_get_expanded_text(ed);
    char *trimmed = str_trim(expanded);
    free(expanded);
    tui_editor_add_history(ed, trimmed);
    tui_editor_clear(ed);
    if (ed->on_submit) ed->on_submit(ed, trimmed, ed->cb_ud);
    free(trimmed);
}

static void delete_backward(tui_editor_t *ed) {
    if (ed->cc == 0) {
        if (ed->cl == 0) return;
        push_undo(ed, ACT_OTHER);
        size_t pl = strlen(ed->lines.v[ed->cl - 1]);
        free(delete_range(ed, ed->cl - 1, pl, ed->cl, 0));
        return;
    }
    push_undo(ed, ACT_OTHER);
    long ms = marker_ending_at(ed, cur_line(ed), ed->cc);
    size_t from = ms >= 0 ? (size_t)ms : prev_char(cur_line(ed), ed->cc);
    free(delete_range(ed, ed->cl, from, ed->cl, ed->cc));
}

static void delete_forward(tui_editor_t *ed) {
    const char *l = cur_line(ed);
    size_t n = strlen(l);
    if (ed->cc >= n) {
        if (ed->cl + 1 >= ed->lines.n) return;
        push_undo(ed, ACT_OTHER);
        free(delete_range(ed, ed->cl, n, ed->cl + 1, 0));
        return;
    }
    push_undo(ed, ACT_OTHER);
    long me = marker_starting_at(ed, l, ed->cc);
    size_t to = me >= 0 ? (size_t)me : next_char(l, ed->cc);
    free(delete_range(ed, ed->cl, ed->cc, ed->cl, to));
}

/* join with the previous line (cursor at column 0), recording the newline as a backward kill */
static void kill_join_previous(tui_editor_t *ed, int prev) {
    if (ed->cl == 0) return;
    push_undo(ed, ACT_OTHER);
    record_kill(ed, delete_range(ed, ed->cl - 1, strlen(ed->lines.v[ed->cl - 1]), ed->cl, 0), true, prev);
}

/* join with the next line (cursor at the end), recording the newline as a forward kill */
static void kill_join_next(tui_editor_t *ed, int prev) {
    if (ed->cl + 1 >= ed->lines.n) return;
    push_undo(ed, ACT_OTHER);
    record_kill(ed, delete_range(ed, ed->cl, strlen(cur_line(ed)), ed->cl + 1, 0), false, prev);
}

static void delete_word_backward(tui_editor_t *ed, int prev) {
    if (ed->cc == 0) {
        kill_join_previous(ed, prev);
        return;
    }
    push_undo(ed, ACT_OTHER);
    size_t end = ed->cc;
    long ms = marker_ending_at(ed, cur_line(ed), ed->cc);
    if (ms >= 0) ed->cc = (size_t)ms;
    else word_left(ed);
    record_kill(ed, delete_range(ed, ed->cl, ed->cc, ed->cl, end), true, prev);
}

static void delete_word_forward(tui_editor_t *ed, int prev) {
    const char *l = cur_line(ed);
    if (ed->cc >= strlen(l)) {
        kill_join_next(ed, prev);
        return;
    }
    push_undo(ed, ACT_OTHER);
    long me = marker_starting_at(ed, l, ed->cc);
    size_t to = me >= 0 ? (size_t)me : word_right_pos(l, ed->cc);
    record_kill(ed, delete_range(ed, ed->cl, ed->cc, ed->cl, to), false, prev);
}

/* Remove the newest kill-ring entry if it sits right before the cursor (#deleteYankedText). */
static bool delete_yanked(tui_editor_t *ed) {
    const char *y = kill_peek(ed);
    if (!y) return false;
    strvec_t parts = str_split(y, "\n");
    bool ok = false;
    size_t k = parts.n ? parts.n : 1;
    if (ed->cl + 1 >= k) {
        size_t sl = ed->cl - (k - 1), sc = 0;
        if (k == 1) {
            size_t yl = strlen(y);
            ok = ed->cc >= yl && !memcmp(cur_line(ed) + ed->cc - yl, y, yl);
            sc = ok ? ed->cc - yl : 0;
        } else {
            const char *first = ed->lines.v[sl], *lastp = parts.v[k - 1];
            size_t fl = strlen(first), pl = strlen(parts.v[0]);
            ok = fl >= pl && !strcmp(first + fl - pl, parts.v[0]) && ed->cc == strlen(lastp) &&
                 !memcmp(cur_line(ed), lastp, ed->cc);
            for (size_t i = 1; ok && i + 1 < k; i++) ok = !strcmp(ed->lines.v[sl + i], parts.v[i]);
            sc = ok ? fl - pl : 0;
        }
        if (ok) free(delete_range(ed, sl, sc, ed->cl, ed->cc));
    }
    strvec_free(&parts);
    return ok;
}

/* ---------- character jump ---------- */

/* Jump to the next (dir 1) / previous (dir -1) occurrence of `ch`, searching across lines and skipping
 * the cursor position (#jumpToChar); no match leaves the cursor in place. */
static void jump_to_char(tui_editor_t *ed, const char *ch, int dir) {
    size_t chl = strlen(ch);
    if (!chl) return;
    for (long li = (long)ed->cl; li >= 0 && li < (long)ed->lines.n; li += dir) {
        const char *line = ed->lines.v[li];
        size_t n = strlen(line);
        bool cur = (size_t)li == ed->cl;
        long found = -1;
        if (dir > 0) {
            size_t from = cur ? ed->cc + 1 : 0;
            if (from <= n) {
                const char *p = strstr(line + from, ch);
                if (p) found = p - line;
            }
        } else {
            /* JS lastIndexOf(ch, cursorCol - 1): a negative start searches from 0 */
            long from = cur ? (long)ed->cc - 1 : (long)n;
            if (from < 0) from = 0;
            for (long i = MIN(from, (long)n - (long)chl); i >= 0; i--)
                if (!memcmp(line + i, ch, chl)) {
                    found = i;
                    break;
                }
        }
        if (found >= 0) {
            ed->cl = (size_t)li;
            ed->cc = (size_t)found;
            return;
        }
    }
}

static void insert_typed(tui_editor_t *ed, const char *text, int prev_last) {
    /* #insertCharacter undo coalescing: consecutive word typing is one undo unit; a chunk with whitespace
     * records its own state and the next word starts a new unit */
    bool word = !strpbrk(text, " \t\r\n\v\f");
    if (!word || prev_last != ACT_TYPE) push_undo(ed, ACT_OTHER);
    ed->last_action = word ? ACT_TYPE : ACT_NONE;
    insert_raw(ed, text);
    ed->hist_index = -1;
}

bool tui_editor_handle_key(tui_editor_t *ed, const tui_key_t *k) {
    /* autocomplete navigation */
    if (ed->ac_list) {
        if (k->kind == TUI_KEY_ESCAPE && !k->mods) {
            close_ac(ed);
            tui_component_request_render(&ed->base);
            return true;
        }
        if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down") || tui_kb(k, "tui.select.pageUp") ||
            tui_kb(k, "tui.select.pageDown")) {
            tui_component_handle_key(&ed->ac_list->base, k);
            tui_component_request_render(&ed->base);
            return true;
        }
        if (tui_kb(k, "tui.input.submit") && !ed->ac_enter_accepts) {
            /* the top suggestion only matched a description: Enter submits the typed text */
            close_ac(ed);
        } else if (tui_kb(k, "tui.input.submit") && ed->ac_args) {
            /* #selectedSlashArgumentIsAlreadyTyped: Enter submits a fully typed argument (unless its
             * usage still names a required `<arg>`), otherwise it applies the completion only */
            const tui_select_item_t *it = tui_select_list_selected(ed->ac_list);
            const char *gh = it && ed->ac_list->selected < ed->ac_ghosts.n ? ed->ac_ghosts.v[ed->ac_list->selected] : "";
            size_t pl = MIN(ed->ac_prefix_len, ed->cc);
            char *typed = xstrndup(cur_line(ed) + ed->cc - pl, pl);
            char *tt = str_trim(typed), *vt = it ? str_trim(it->value) : xstrdup("");
            bool required = false;
            int depth = 0;
            for (const char *g = gh; *g; g++) {
                if (*g == '[') depth++;
                else if (*g == ']' && depth) depth--;
                else if (*g == '<' && !depth) required = true;
            }
            bool done = it && *tt && !strcmp(tt, vt) && !required && strlen(cur_line(ed)) == ed->cc;
            free(typed);
            free(tt);
            free(vt);
            if (done) close_ac(ed);
            else {
                accept_ac(ed);
                return true;
            }
        } else if (tui_kb(k, "tui.input.tab") || tui_kb(k, "tui.input.submit")) {
            bool submit = tui_kb(k, "tui.input.submit");
            accept_ac(ed);
            /* like upstream: Enter on a slash-command completion runs the command */
            if (submit && lines_above_blank(ed) && !ed->ac_list) {
                char *line = cur_line(ed);
                size_t n = line ? strlen(line) : 0;
                while (n && line[n - 1] == ' ') n--;
                if (line && line[0] == '/' && !memchr(line, ' ', n)) do_submit(ed);
            }
            return true;
        }
    }

    int prev_kill = ed->kill_state;
    ed->kill_state = KILL_NONE;

    /* character jump mode: the next printable key is the target; the jump keys cancel; any other
     * key cancels and is handled normally */
    if (ed->jump_mode) {
        int dir = ed->jump_mode;
        ed->jump_mode = 0;
        if (tui_kb(k, "tui.editor.jumpForward") || tui_kb(k, "tui.editor.jumpBackward")) return true;
        const char *jt = tui_key_text(k);
        if (k->kind == TUI_KEY_CHAR && jt && *jt) {
            close_ac(ed);
            jump_to_char(ed, jt, dir);
            ed->preferred_col = -1;
            tui_component_request_render(&ed->base);
            return true;
        }
    }

    bool changed = true, moved = false, retrigger = false;
    int prev_last = ed->last_action;
    ed->last_action = ACT_NONE;

    if (k->kind == TUI_KEY_PASTE) {
        if (ed->paste_hook && ed->paste_hook(ed->paste_hook_ud, ed, k->text ? k->text : "")) return true;
        tui_editor_paste(ed, k->text ? k->text : "");
        maybe_trigger_ac(ed);
        return true;
    } else if (tui_kb(k, "tui.editor.undo")) {
        ed->last_action = prev_last;
        tui_editor_undo(ed);
        close_ac(ed);
        return true;
    } else if (tui_kb(k, "tui.input.submit")) {
        do_submit(ed);
        return true;
    } else if (tui_kb(k, "tui.input.newLine") || tui_key_matches(k, "alt+enter") ||
               (tui_key_matches(k, "ctrl+enter") && !tui_kb(k, "tui.input.submit"))) {
        /* alt+enter / ctrl+enter are upstream's hard-coded newline fallbacks */
        push_undo(ed, ACT_OTHER);
        insert_raw(ed, "\n");
        close_ac(ed);
    } else if (tui_kb(k, "tui.input.tab")) {
        if (ed->ac_fn) refresh_ac(ed, true);
        changed = false;
    } else if (tui_kb(k, "tui.editor.deleteToLineEnd")) {
        size_t n = strlen(cur_line(ed));
        if (ed->cc >= n) kill_join_next(ed, prev_kill);
        else {
            push_undo(ed, ACT_OTHER);
            record_kill(ed, delete_range(ed, ed->cl, ed->cc, ed->cl, n), false, prev_kill);
        }
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.deleteToLineStart")) {
        if (ed->cc == 0) kill_join_previous(ed, prev_kill);
        else {
            push_undo(ed, ACT_OTHER);
            record_kill(ed, delete_range(ed, ed->cl, 0, ed->cl, ed->cc), true, prev_kill);
        }
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.deleteWordBackward")) {
        delete_word_backward(ed, prev_kill);
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.deleteWordForward")) {
        delete_word_forward(ed, prev_kill);
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.yank")) {
        if (kill_peek(ed)) {
            push_undo(ed, ACT_OTHER);
            insert_raw(ed, kill_peek(ed));
            ed->kill_state = KILL_YANK;
        } else changed = false;
    } else if (tui_kb(k, "tui.editor.yankPop")) {
        /* only right after a yank: swap the yanked text for the next older entry */
        if (prev_kill == KILL_YANK && ed->kills.n > 1) {
            push_undo(ed, ACT_OTHER);
            if (delete_yanked(ed)) {
                kill_rotate(ed);
                insert_raw(ed, kill_peek(ed));
            }
            ed->kill_state = KILL_YANK;
        } else changed = false;
    } else if (tui_kb(k, "tui.editor.deleteCharBackward") || tui_key_matches(k, "shift+backspace")) {
        delete_backward(ed);
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.deleteCharForward") || tui_key_matches(k, "shift+delete")) {
        delete_forward(ed);
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.jumpForward") || tui_kb(k, "tui.editor.jumpBackward")) {
        ed->jump_mode = tui_kb(k, "tui.editor.jumpForward") ? 1 : -1;
        changed = false;
    } else if (tui_kb(k, "tui.editor.cursorLeft")) {
        if (ed->cc > 0) {
            long ms = marker_ending_at(ed, cur_line(ed), ed->cc);
            ed->cc = ms >= 0 ? (size_t)ms : prev_char(cur_line(ed), ed->cc);
        } else if (ed->cl > 0) {
            ed->cl--;
            ed->cc = strlen(cur_line(ed));
        }
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorRight")) {
        const char *l = cur_line(ed);
        if (ed->cc < strlen(l)) {
            long me = marker_starting_at(ed, l, ed->cc);
            ed->cc = me >= 0 ? (size_t)me : next_char(l, ed->cc);
        } else if (ed->cl + 1 < ed->lines.n) {
            ed->cl++;
            ed->cc = 0;
        }
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorWordLeft")) {
        word_left(ed);
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorWordRight")) {
        word_right(ed);
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorLineStart")) {
        ed->cc = 0;
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorLineEnd")) {
        ed->cc = strlen(cur_line(ed));
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorUp")) {
        if (on_first_visual_line(ed) && (tui_editor_is_empty(ed) || ed->hist_index >= 0) && ed->history.n) {
            history_nav(ed, -1);
            return true;
        }
        if (!move_vertical(ed, -1)) ed->cc = 0;
        tui_component_request_render(&ed->base);
        return true;
    } else if (tui_kb(k, "tui.editor.cursorDown")) {
        if (ed->hist_index >= 0 && on_last_visual_line(ed)) {
            history_nav(ed, 1);
            return true;
        }
        if (!move_vertical(ed, 1)) ed->cc = strlen(cur_line(ed));
        tui_component_request_render(&ed->base);
        return true;
    } else if (tui_kb(k, "tui.editor.pageUp") || tui_kb(k, "tui.editor.pageDown")) {
        int dir = tui_kb(k, "tui.editor.pageUp") ? -1 : 1;
        int rows = ed->max_height > 0 ? ed->max_height : 10;
        for (int i = 0; i < rows; i++)
            if (!move_vertical(ed, dir)) break;
        tui_component_request_render(&ed->base);
        return true;
    } else if (tui_key_text(k) && k->kind == TUI_KEY_CHAR) {
        insert_typed(ed, tui_key_text(k), prev_last);
        retrigger = true;
    } else {
        ed->last_action = prev_last;
        return false;
    }

    if (moved) {
        ed->preferred_col = -1;
        if (ed->ac_list) refresh_ac(ed, false);
        tui_component_request_render(&ed->base);
        return true;
    }
    ed->preferred_col = -1;
    if (retrigger) maybe_trigger_ac(ed);
    if (changed) notify_change(ed);
    else tui_component_request_render(&ed->base);
    return true;
}

void tui_editor_set_row_hook(tui_editor_t *ed, char *(*fn)(void *ud, tui_editor_t *ed, const char *row), void *ud) {
    ed->row_hook = fn;
    ed->row_hook_ud = ud;
}

/* ---------- rendering ---------- */

static void border_line(sb_t *b, int width, int slot, const char *label) {
    const tui_theme_t *th = tui_theme();
    sb_t inner = SB_INIT;
    if (label) {
        sb_append(&inner, "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 ");
        sb_append(&inner, label);
        sb_putc(&inner, ' ');
    }
    int w = tui_visible_width(sb_cstr(&inner));
    for (int i = w; i < width; i++) sb_append(&inner, "\xe2\x94\x80");
    char *tr = tui_truncate_to_width(sb_cstr(&inner), width, "");
    tui_fg(b, th, (tui_tc_t)slot, tr);
    free(tr);
    sb_free(&inner);
}

/* ---------- composer chrome rendering (omp components/editor.ts render) ---------- */

#define PLACEHOLDER_MIN_GAP 2

/* upstream Theme.fgResolved: color the text, re-opening it after nested fg/full resets */
static char *text_colored(const tui_editor_t *ed, const char *s) {
    if (!ed->text_fg[0]) return xstrdup(s);
    sb_t b = SB_INIT;
    sb_append(&b, ed->text_fg);
    for (const char *p = s; *p;) {
        if (!strncmp(p, "\x1b[0m", 4) || !strncmp(p, "\x1b[39m", 5)) {
            size_t n = p[2] == '0' ? 4 : 5;
            sb_appendn(&b, p, n);
            sb_append(&b, ed->text_fg);
            p += n;
        } else {
            sb_putc(&b, *p++);
        }
    }
    sb_append(&b, "\x1b[39m");
    return sb_detach(&b);
}

static int composer_padding(const tui_editor_t *ed) {
    return ed->composer->default_padding_x(ed->theme_padding_x);
}

static const char *composer_gutter(const tui_editor_t *ed) {
    if (ed->composer->side_borders) return NULL;
    return ed->gutter ? ed->gutter : ed->composer->default_gutter;
}

/* editor.ts render: the dropdown shows max(3, min(autocompleteMaxVisible, viewport rows - editor rows - 2)) items */
static void ac_render(tui_editor_t *ed, int width, strvec_t *out, size_t first_row) {
    if (!ed->ac_list) return;
    int rows = ed->base.tui ? tui_term(ed->base.tui)->rows : 0;
    if (rows <= 0) rows = 24;
    ed->ac_list->max_visible = MAX(3, MIN(ed->ac_max_visible, rows - (int)(out->n - first_row) - 2));
    tui_component_render(&ed->ac_list->base, width, out);
}

void tui_editor_set_autocomplete_max_visible(tui_editor_t *ed, double n) {
    ed->ac_max_visible = isfinite(n) ? MAX(3, MIN(20, (int)floor(n))) : 10;
}

static void composer_render(tui_editor_t *ed, int width, strvec_t *out) {
    size_t first_row = out->n;
    const tui_composer_style_t *st = ed->composer;
    tui_component_t *c = &ed->base;
    int px = composer_padding(ed);
    bool side = st->side_borders;
    int chrome = st->side_chrome_width(px);
    const char *gsrc = composer_gutter(ed);
    int gw = gsrc ? MIN(tui_visible_width(gsrc), MAX(0, width - 2 * chrome)) : 0;
    char *g_first = gw ? tui_slice_cols(gsrc, 0, gw) : NULL;
    sb_t gc = SB_INIT;
    tui_sb_spaces(&gc, gw);
    char *g_cont = sb_detach(&gc);
    int lcw = MAX(0, width - 2 * chrome - gw);
    int W = MAX(1, lcw - (side && px == 0 ? 1 : 0));
    ed->layout_width = W;

    segvec_t segs = {0};
    layout(ed, W, &segs);
    size_t csi = cursor_seg(ed, &segs);
    int total = (int)segs.n;
    int vis = ed->max_height > 0 ? MIN(ed->max_height, total) : total;
    if (ed->scroll > (int)csi) ed->scroll = (int)csi;
    if ((int)csi >= ed->scroll + vis) ed->scroll = (int)csi - vis + 1;
    if (ed->scroll > total - vis) ed->scroll = total - vis;
    if (ed->scroll < 0) ed->scroll = 0;

    tui_composer_ctx_t ctx = {.width = width, .padding_x = px, .border = ed->chrome_border, .accent = ed->chrome_accent,
                              .surface = ed->chrome_surface, .ud = ed->chrome_ud};
    char *top = NULL;
    if (st->status_attachment != TUI_ATTACH_NONE && ed->top_fn) {
        int tw = 0;
        top = ed->top_fn(ed->top_ud, MAX(0, width - chrome * 2), &tw);
        ctx.top_content = top;
        ctx.top_width = tw;
    }
    char *row = st->render_top(&ctx);
    if (row) strvec_push_own(out, row);

    bool empty = tui_editor_is_empty(ed);
    char *ph = empty && !ed->ac_list && ed->placeholder_fn ? ed->placeholder_fn(ed->placeholder_ud) : NULL;
    if (ph && !*ph) {
        free(ph);
        ph = NULL;
    }
    int phw = ph ? tui_visible_width(ph) : 0;
    /* dim ghost text after an end-of-line cursor (slash-command argument hints) */
    char *ghost = !ph && c->focused ? tui_editor_ghost(ed) : NULL;

    for (int vi = ed->scroll; vi < ed->scroll + vis; vi++) {
        const seg_t *sg = &segs.v[vi];
        const char *line = ed->lines.v[sg->line];
        char *text = xstrndup(line + sg->start, sg->end - sg->start);
        int dw = tui_visible_width(text);
        const char *gutter = !gw ? "" : vi == ed->scroll ? g_first : g_cont;
        bool has_cursor = (size_t)vi == csi && c->focused;
        int overflow = 0;
        if (!side && dw > lcw) {
            char *t = tui_slice_cols(text, 0, lcw);
            free(text);
            text = t;
            dw = tui_visible_width(text);
        }
        sb_t d = SB_INIT;
        if (has_cursor) {
            size_t cpos = MIN(ed->cc - sg->start, strlen(text));
            const char *after = text + cpos;
            if (ed->use_terminal_cursor) {
                int avail = MAX(0, lcw - dw);
                if (!*after && ph && avail - phw >= PLACEHOLDER_MIN_GAP) {
                    sb_appendn(&d, text, cpos);
                    sb_append(&d, TUI_CURSOR_MARKER);
                    tui_sb_spaces(&d, avail - phw);
                    sb_append(&d, ph);
                    dw += avail;
                } else if (!*after && !side && dw >= lcw && cpos > 0) {
                    size_t last = cpos;
                    while (last > 0 && ((unsigned char)text[last - 1] & 0xC0) == 0x80) last--;
                    if (last > 0) last--;
                    sb_appendn(&d, text, last);
                    sb_append(&d, TUI_CURSOR_MARKER);
                    sb_append(&d, text + last);
                } else {
                    sb_appendn(&d, text, cpos);
                    sb_append(&d, TUI_CURSOR_MARKER);
                    sb_append(&d, after);
                    if (!*after && ghost && avail > 0) {
                        char *g = tui_truncate_to_width(ghost, avail, NULL);
                        char *gd = tui_fgs(NULL, TUI_TC_DIM, g);
                        sb_append(&d, gd);
                        dw += tui_visible_width(g);
                        free(gd);
                        free(g);
                    }
                }
            } else if (*after) {
                size_t nx = next_char(text, cpos);
                sb_appendn(&d, text, cpos);
                sb_append(&d, TUI_CURSOR_MARKER);
                sb_append(&d, "\x1b[7m");
                sb_appendn(&d, after, nx - cpos);
                sb_append(&d, "\x1b[0m");
                sb_append(&d, text + nx);
            } else if (!side && dw + 1 > lcw && cpos > 0) {
                size_t last = cpos;
                while (last > 0 && ((unsigned char)text[last - 1] & 0xC0) == 0x80) last--;
                if (last > 0) last--;
                sb_appendn(&d, text, last);
                sb_append(&d, TUI_CURSOR_MARKER);
                sb_append(&d, "\x1b[7m");
                sb_append(&d, text + last);
                sb_append(&d, "\x1b[0m");
            } else {
                sb_appendn(&d, text, cpos);
                sb_append(&d, TUI_CURSOR_MARKER);
                sb_append(&d, ed->eol_cursor ? ed->eol_cursor : "\x1b[7m \x1b[0m");
                dw += 1;
                int avail = MAX(0, lcw - dw);
                if (ph && avail - phw >= PLACEHOLDER_MIN_GAP) {
                    tui_sb_spaces(&d, avail - phw);
                    sb_append(&d, ph);
                    dw += avail;
                } else if (ghost && avail > 0) {
                    char *g = tui_truncate_to_width(ghost, avail, NULL);
                    char *gd = tui_fgs(NULL, TUI_TC_DIM, g);
                    sb_append(&d, gd);
                    dw += tui_visible_width(g);
                    free(gd);
                    free(g);
                }
                if (dw > lcw && px > 0) overflow = dw - lcw;
            }
        } else {
            if (dw > lcw) {
                char *t = tui_truncate_to_width(text, lcw, NULL);
                free(text);
                text = t;
                dw = tui_visible_width(text);
            }
            sb_append(&d, text);
        }
        char *disp = style_atoms(ed, sb_detach(&d), st->filled_surface);
        if (ed->row_hook) {
            char *decorated = ed->row_hook(ed->row_hook_ud, ed, disp);
            if (decorated) {
                free(disp);
                disp = decorated;
            }
        }
        char *rendered = st->filled_surface ? xstrdup(disp) : text_colored(ed, disp);
        tui_composer_row_t r = {.ctx = &ctx,
                                .text = rendered,
                                .pad = MAX(0, lcw - dw),
                                .gutter = gutter,
                                .is_last_row = vi == ed->scroll + vis - 1,
                                .cursor_overflow = overflow};
        st->render_row(&r, out);
        free(rendered);
        free(disp);
        free(text);
    }
    row = st->render_bottom(&ctx);
    if (row) strvec_push_own(out, row);
    free(top);
    free(ph);
    free(ghost);
    free(g_first);
    free(g_cont);
    vec_free(&segs);
    ac_render(ed, width, out, first_row);
}

static void editor_render(tui_component_t *c, int width, strvec_t *out) {
    tui_editor_t *ed = (tui_editor_t *)c;
    if (ed->composer) {
        composer_render(ed, width, out);
        return;
    }
    size_t first_row = out->n;
    int content_w = MAX(1, width - 2 * ed->padding_x);
    int W = effective_layout_width(ed, width);
    ed->layout_width = W;
    segvec_t segs = {0};
    layout(ed, W, &segs);
    size_t csi = cursor_seg(ed, &segs);
    int total = (int)segs.n;
    int vis = ed->max_height > 0 ? MIN(ed->max_height, total) : total;
    if (ed->scroll > (int)csi) ed->scroll = (int)csi;
    if ((int)csi >= ed->scroll + vis) ed->scroll = (int)csi - vis + 1;
    if (ed->scroll > total - vis) ed->scroll = total - vis;
    if (ed->scroll < 0) ed->scroll = 0;

    sb_t b = SB_INIT;
    char lab[48];
    if (ed->scroll > 0) snprintf(lab, sizeof lab, "\xe2\x86\x91 %d more", ed->scroll);
    border_line(&b, width, ed->border_slot, ed->scroll > 0 ? lab : NULL);
    strvec_push_own(out, sb_detach(&b));

    bool empty = tui_editor_is_empty(ed);
    for (int vi = ed->scroll; vi < ed->scroll + vis; vi++) {
        const seg_t *sg = &segs.v[vi];
        const char *line = ed->lines.v[sg->line];
        sb_t l = SB_INIT;
        tui_sb_spaces(&l, ed->padding_x);
        int used;
        if ((size_t)vi == csi && c->focused) {
            sb_appendn(&l, line + sg->start, ed->cc - sg->start);
            sb_append(&l, TUI_CURSOR_MARKER);
            size_t after = ed->cc;
            if (ed->cc < sg->end) {
                size_t nx = next_char(line, ed->cc);
                sb_append(&l, "\x1b[7m");
                sb_appendn(&l, line + ed->cc, nx - ed->cc);
                sb_append(&l, "\x1b[27m");
                after = nx;
                sb_appendn(&l, line + after, sg->end - after);
            } else {
                sb_append(&l, "\x1b[7m \x1b[27m");
                if (empty && ed->placeholder) {
                    char *ph = tui_truncate_to_width(ed->placeholder, MAX(0, content_w - 1), NULL);
                    char *d = tui_dim(ph);
                    sb_append(&l, d);
                    free(d);
                    free(ph);
                }
            }
        } else {
            sb_appendn(&l, line + sg->start, sg->end - sg->start);
            if (empty && ed->placeholder && !c->focused) {
                char *ph = tui_truncate_to_width(ed->placeholder, content_w, NULL);
                char *d = tui_dim(ph);
                sb_append(&l, d);
                free(d);
                free(ph);
            }
        }
        used = tui_visible_width(sb_cstr(&l));
        if (used > width) { /* only at absurdly small widths */
            char *t = tui_truncate_to_width(sb_cstr(&l), width, "");
            sb_free(&l);
            strvec_push_own(out, t);
            continue;
        }
        tui_sb_spaces(&l, width - used);
        strvec_push_own(out, sb_detach(&l));
    }
    int below = total - (ed->scroll + vis);
    if (below > 0) snprintf(lab, sizeof lab, "\xe2\x86\x93 %d more", below);
    border_line(&b, width, ed->border_slot, below > 0 ? lab : NULL);
    strvec_push_own(out, sb_detach(&b));
    vec_free(&segs);
    ac_render(ed, width, out, first_row);
}

static bool editor_key(tui_component_t *c, const tui_key_t *k) { return tui_editor_handle_key((tui_editor_t *)c, k); }

static void editor_destroy(tui_component_t *c) {
    tui_editor_t *ed = (tui_editor_t *)c;
    close_ac(ed);
    strvec_free(&ed->lines);
    for (size_t i = 0; i < ed->undo.n; i++) snapshot_free(&ed->undo.v[i]);
    vec_free(&ed->undo);
    strvec_free(&ed->kills);
    strvec_free(&ed->history);
    free(ed->hist_draft);
    for (size_t i = 0; i < ed->pastes.n; i++) paste_free(&ed->pastes.v[i]);
    vec_free(&ed->pastes);
    texts_clear(ed);
    vec_free(&ed->texts);
    free(ed->placeholder);
    free(ed->gutter);
    free(ed);
}

static const tui_component_vt EDITOR_VT = {"editor", editor_render, editor_key, NULL, editor_destroy};

tui_editor_t *tui_editor_new(void) {
    tui_editor_t *ed = xcalloc(1, sizeof *ed);
    tui_component_init(&ed->base, &EDITOR_VT);
    ed->theme_padding_x = -1;
    strvec_push(&ed->lines, "");
    ed->preferred_col = -1;
    ed->hist_index = -1;
    ed->ac_max_visible = 10;
    ed->border_slot = TUI_TC_BORDER;
    return ed;
}

void tui_editor_set_text(tui_editor_t *ed, const char *text) {
    push_undo(ed, ACT_OTHER);
    char *clean = sanitize_paste(text ? text : "");
    set_text_internal(ed, clean);
    free(clean);
    ed->hist_index = -1;
    notify_change(ed);
}

char *tui_editor_get_text(const tui_editor_t *ed) { return strvec_join(&ed->lines, "\n"); }

char *tui_editor_get_expanded_text(const tui_editor_t *ed) {
    char *text = tui_editor_get_text(ed);
    for (size_t i = 0; i < ed->pastes.n; i++) {
        if (!strstr(text, ed->pastes.v[i].marker)) continue;
        char *r = str_replace(text, ed->pastes.v[i].marker, ed->pastes.v[i].content);
        free(text);
        text = r;
    }
    return text;
}

void tui_editor_insert_text(tui_editor_t *ed, const char *text) {
    push_undo(ed, ACT_OTHER);
    char *clean = sanitize_paste(text ? text : "");
    insert_raw(ed, clean);
    free(clean);
    ed->preferred_col = -1;
    notify_change(ed);
}

/* #matchesTransientUndoSnapshot: a snapshot taken while `transient` was being typed at `start` */
static bool transient_snapshot(const tui_editor_t *ed, const tui_editor_snapshot_t *s, const char *transient,
                               size_t start, const char *before, const char *after) {
    if (s->cl != ed->cl || s->lines.n != ed->lines.n) return false;
    if (s->cc < start || s->cc - start >= strlen(transient)) return false;
    for (size_t i = 0; i < s->lines.n; i++)
        if (i != ed->cl && strcmp(s->lines.v[i], ed->lines.v[i])) return false;
    char *want = xasprintf("%s%.*s%s", before, (int)(s->cc - start), transient, after);
    bool ok = !strcmp(s->lines.v[s->cl], want);
    free(want);
    return ok;
}

void tui_editor_undo_past_transient(tui_editor_t *ed, const char *transient) {
    size_t tl = strlen(transient);
    const char *line = cur_line(ed);
    if (!tl || ed->cc < tl || strncmp(line + ed->cc - tl, transient, tl)) {
        tui_editor_undo(ed);
        return;
    }
    size_t start = ed->cc - tl;
    char *before = xstrndup(line, start), *after = xstrdup(line + ed->cc);
    ed->hist_index = -1;
    ed->last_action = ACT_NONE;
    ed->preferred_col = -1;
    set_line(ed, ed->cl, xasprintf("%s%s", before, after));
    ed->cc = start;
    while (ed->undo.n && transient_snapshot(ed, &ed->undo.v[ed->undo.n - 1], transient, start, before, after)) {
        snapshot_free(&ed->undo.v[ed->undo.n - 1]);
        ed->undo.n--;
    }
    free(before);
    free(after);
    if (!ed->undo.n) {
        notify_change(ed);
        return;
    }
    tui_editor_undo(ed);
}

static void move_to(tui_editor_t *ed, size_t line, bool end) {
    ed->last_action = ACT_NONE;
    close_ac(ed);
    ed->cl = line;
    ed->cc = end ? strlen(cur_line(ed)) : 0;
    ed->preferred_col = -1;
    notify_change(ed);
}

void tui_editor_move_to_message_start(tui_editor_t *ed) { move_to(ed, 0, false); }
void tui_editor_move_to_message_end(tui_editor_t *ed) { move_to(ed, ed->lines.n - 1, true); }
void tui_editor_move_to_line_start(tui_editor_t *ed) { move_to(ed, ed->cl, false); }
void tui_editor_move_to_line_end(tui_editor_t *ed) { move_to(ed, ed->cl, true); }

void tui_editor_remember_draft(tui_editor_t *ed) {
    char *t = tui_editor_get_text(ed);
    char *tt = str_trim(t);
    if (*tt) tui_editor_add_history(ed, t);
    free(tt);
    free(t);
}

void tui_editor_submit(tui_editor_t *ed) { do_submit(ed); }

char *tui_editor_current_line(const tui_editor_t *ed) { return xstrdup(ed->lines.v[ed->cl]); }

bool tui_editor_is_empty(const tui_editor_t *ed) { return ed->lines.n == 1 && ed->lines.v[0][0] == 0; }

void tui_editor_clear(tui_editor_t *ed) {
    strvec_free(&ed->lines);
    strvec_push(&ed->lines, "");
    ed->cl = ed->cc = 0;
    ed->scroll = 0;
    ed->preferred_col = -1;
    ed->hist_index = -1;
    for (size_t i = 0; i < ed->pastes.n; i++) paste_free(&ed->pastes.v[i]);
    ed->pastes.n = 0;
    texts_clear(ed);
    for (size_t i = 0; i < ed->undo.n; i++) snapshot_free(&ed->undo.v[i]);
    ed->undo.n = 0;
    ed->last_action = ACT_NONE;
    close_ac(ed);
    notify_change(ed);
}

void tui_editor_set_on_submit(tui_editor_t *ed, tui_editor_submit_fn fn, void *ud) {
    ed->on_submit = fn;
    ed->cb_ud = ud;
}

void tui_editor_set_on_change(tui_editor_t *ed, tui_editor_change_fn fn) { ed->on_change = fn; }

void tui_editor_set_autocomplete(tui_editor_t *ed, tui_autocomplete_fn fn, void *ud) {
    ed->ac_fn = fn;
    ed->ac_ud = ud;
    close_ac(ed);
}

void tui_editor_set_inline_hint(tui_editor_t *ed, char *(*fn)(void *ud, const char *line_before, bool first_line), void *ud) {
    ed->hint_fn = fn;
    ed->hint_ud = ud;
}

/* editor.ts #getInlineHint: the selected completion's hint while the list is open, else the
 * provider's hint for the text before the cursor; only at the end of the line */
char *tui_editor_ghost(tui_editor_t *ed) {
    const char *line = cur_line(ed);
    if (ed->cc != strlen(line)) return NULL;
    if (ed->ac_list) {
        size_t sel = ed->ac_list->selected;
        return sel < ed->ac_ghosts.n && *ed->ac_ghosts.v[sel] ? xstrdup(ed->ac_ghosts.v[sel]) : NULL;
    }
    if (!ed->hint_fn) return NULL;
    char *h = ed->hint_fn(ed->hint_ud, line, lines_above_blank(ed));
    if (h && !*h) {
        free(h);
        h = NULL;
    }
    return h;
}

void tui_editor_set_placeholder(tui_editor_t *ed, const char *text) {
    free(ed->placeholder);
    ed->placeholder = text ? xstrdup(text) : NULL;
}

bool tui_editor_autocomplete_open(const tui_editor_t *ed) { return ed->ac_list != NULL; }

void tui_editor_set_composer(tui_editor_t *ed, const char *shape) {
    ed->composer = shape ? tui_composer_style(shape) : NULL;
    ed->scroll = 0;
    tui_component_request_render(&ed->base);
}

void tui_editor_set_chrome_colors(tui_editor_t *ed, tui_composer_color_fn border, tui_composer_color_fn accent,
                                  tui_composer_color_fn surface, void *ud) {
    ed->chrome_border = border;
    ed->chrome_accent = accent;
    ed->chrome_surface = surface;
    ed->chrome_ud = ud;
}

void tui_editor_set_top_provider(tui_editor_t *ed, char *(*fn)(void *ud, int avail, int *width), void *ud) {
    ed->top_fn = fn;
    ed->top_ud = ud;
}

void tui_editor_set_placeholder_fn(tui_editor_t *ed, char *(*fn)(void *ud), void *ud) {
    ed->placeholder_fn = fn;
    ed->placeholder_ud = ud;
}
