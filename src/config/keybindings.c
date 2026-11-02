/* keybindings.c - user keybinding overrides (port of the config half of omp's app-keybindings.ts). */
#include "config/keybindings.h"
#include "core/fs.h"
#include "text/yaml.h"

/* KEYBINDING_NAME_MIGRATIONS */
static const char *const MIGRATIONS[][2] = {
    {"interrupt", "app.interrupt"},
    {"clear", "app.clear"},
    {"exit", "app.exit"},
    {"suspend", "app.suspend"},
    {"displayReset", "app.display.reset"},
    {"cycleThinkingLevel", "app.thinking.cycle"},
    {"cycleModelForward", "app.model.cycleForward"},
    {"cycleModelBackward", "app.model.cycleBackward"},
    {"selectModel", "app.model.select"},
    {"selectModelTemporary", "app.model.selectTemporary"},
    {"togglePlanMode", "app.plan.toggle"},
    {"historySearch", "app.history.search"},
    {"expandTools", "app.tools.expand"},
    {"toggleThinking", "app.thinking.toggle"},
    {"externalEditor", "app.editor.external"},
    {"followUp", "app.message.followUp"},
    {"retry", "app.retry"},
    {"dequeue", "app.message.dequeue"},
    {"pasteImage", "app.clipboard.pasteImage"},
    {"pasteTextRaw", "app.clipboard.pasteTextRaw"},
    {"copyLine", "app.clipboard.copyLine"},
    {"copyPrompt", "app.clipboard.copyPrompt"},
    {"newSession", "app.session.new"},
    {"tree", "app.session.tree"},
    {"fork", "app.session.fork"},
    {"resume", "app.session.resume"},
    {"observeSessions", "app.session.observe"},
    {"toggleSTT", "app.stt.toggle"},
    {"cursorUp", "tui.editor.cursorUp"},
    {"cursorDown", "tui.editor.cursorDown"},
    {"cursorLeft", "tui.editor.cursorLeft"},
    {"cursorRight", "tui.editor.cursorRight"},
    {"cursorWordLeft", "tui.editor.cursorWordLeft"},
    {"cursorWordRight", "tui.editor.cursorWordRight"},
    {"cursorLineStart", "tui.editor.cursorLineStart"},
    {"cursorLineEnd", "tui.editor.cursorLineEnd"},
    {"jumpForward", "tui.editor.jumpForward"},
    {"jumpBackward", "tui.editor.jumpBackward"},
    {"pageUp", "tui.editor.pageUp"},
    {"pageDown", "tui.editor.pageDown"},
    {"deleteCharBackward", "tui.editor.deleteCharBackward"},
    {"deleteCharForward", "tui.editor.deleteCharForward"},
    {"deleteWordBackward", "tui.editor.deleteWordBackward"},
    {"deleteWordForward", "tui.editor.deleteWordForward"},
    {"deleteToLineStart", "tui.editor.deleteToLineStart"},
    {"deleteToLineEnd", "tui.editor.deleteToLineEnd"},
    {"yank", "tui.editor.yank"},
    {"yankPop", "tui.editor.yankPop"},
    {"undo", "tui.editor.undo"},
    {"newLine", "tui.input.newLine"},
    {"submit", "tui.input.submit"},
    {"tab", "tui.input.tab"},
    {"copy", "tui.input.copy"},
    {"selectUp", "tui.select.up"},
    {"selectDown", "tui.select.down"},
    {"selectPageUp", "tui.select.pageUp"},
    {"selectPageDown", "tui.select.pageDown"},
    {"selectConfirm", "tui.select.confirm"},
    {"selectCancel", "tui.select.cancel"},
    {"toggleSessionNamedFilter", "app.session.togglePath"},
};

static const char *migrated_name(const char *key) {
    for (size_t i = 0; i < ARRAY_LEN(MIGRATIONS); i++)
        if (!strcmp(MIGRATIONS[i][0], key)) return MIGRATIONS[i][1];
    return NULL;
}

/* toKeybindingsConfig: strings and all-string arrays only */
static bool valid_value(const json_t *v) {
    if (json_is_str(v)) return true;
    if (!json_is_arr(v)) return false;
    json_t *it;
    json_arr_foreach(v, i, it) if (!json_is_str(it)) return false;
    return true;
}

json_t *kbcfg_migrate_names(const json_t *raw, bool *migrated) {
    json_t *out = json_obj();
    if (migrated) *migrated = false;
    if (!json_is_obj(raw)) return out;
    const char *key;
    json_t *val;
    json_obj_foreach(raw, i, key, val) {
        if (!valid_value(val)) continue;
        const char *nk = migrated_name(key);
        if (nk && migrated) *migrated = true;
        json_set(out, nk ? nk : key, json_clone(val));
    }
    return out;
}

/* orderKeybindingsConfig: registry order first, then unknown keys in their order */
static json_t *ordered(const json_t *cfg) {
    json_t *out = json_obj();
    for (size_t i = 0; i < tui_keybindings_action_count(); i++) {
        const char *a = tui_keybindings_action_at(i);
        json_t *v = json_get(cfg, a);
        if (v) json_set(out, a, json_clone(v));
    }
    const char *key;
    json_t *val;
    json_obj_foreach(cfg, i, key, val) if (!json_get(out, key)) json_set(out, key, json_clone(val));
    return out;
}

/* JSONC -> JSON: drop // and block comments outside strings, and trailing commas */
static char *strip_jsonc(const char *s, size_t n) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') {
            sb_putc(&b, c);
            for (i++; i < n; i++) {
                sb_putc(&b, s[i]);
                if (s[i] == '\\' && i + 1 < n) sb_putc(&b, s[++i]);
                else if (s[i] == '"') break;
            }
        } else if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            if (i < n) sb_putc(&b, '\n');
        } else if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            for (i += 2; i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'); i++) {
            }
            i++;
        } else if (c == ',') {
            size_t j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n')) j++;
            if (j < n && (s[j] == '}' || s[j] == ']')) continue;
            sb_putc(&b, c);
        } else {
            sb_putc(&b, c);
        }
    }
    return sb_detach(&b);
}

/* loadRawConfig: parsed value, or NULL when missing/unreadable/invalid */
static json_t *load_raw(const char *path) {
    size_t len = 0;
    char *data = fs_read_limit(path, 1 << 20, &len, NULL);
    if (!data) return NULL;
    json_t *j = NULL;
    if (str_ends(path, ".json")) {
        char *clean = strip_jsonc(data, len);
        j = json_parse(clean, strlen(clean), NULL);
        free(clean);
    } else {
        j = yaml_parse(data, len, NULL);
    }
    free(data);
    return j;
}

json_t *kbcfg_load_dir(const char *agent_dir, bool write_back, char **path_out) {
    char *yml = path_join(agent_dir, "keybindings.yml");
    char *yaml = path_join(agent_dir, "keybindings.yaml");
    char *jsn = path_join(agent_dir, "keybindings.json");
    const char *read = yml, *wb = yml; /* resolveKeybindingsConfigPaths */
    if (fs_exists(yml)) {
    } else if (fs_exists(yaml)) read = wb = yaml;
    else if (fs_exists(jsn)) read = jsn;
    json_t *raw = load_raw(read);
    json_t *cfg;
    const char *persisted = read;
    if (!raw) {
        cfg = json_obj();
    } else {
        bool migrated = false;
        cfg = kbcfg_migrate_names(raw, &migrated);
        if (write_back && (migrated || strcmp(wb, read))) {
            json_t *o = ordered(cfg);
            char *text = json_len(o) ? yaml_dump(o) : xstrdup("{}");
            size_t tl = strlen(text);
            if (tl && text[tl - 1] == '\n') text[tl - 1] = 0; /* stringifyYamlConfig: no final newline */
            if (fs_write(wb, text, strlen(text)) == 0) persisted = wb;
            free(text);
            json_free(o);
        }
    }
    json_free(raw);
    if (path_out) *path_out = xstrdup(persisted);
    free(yml);
    free(yaml);
    free(jsn);
    return cfg;
}

json_t *kbcfg_load_merged(const char *agent_dir, const char *inherited_dir) {
    json_t *profile = kbcfg_load_dir(agent_dir, true, NULL);
    if (!inherited_dir) return profile;
    char *a = path_abs(agent_dir, NULL), *b = path_abs(inherited_dir, NULL);
    bool same = a && b && !strcmp(a, b);
    free(a);
    free(b);
    if (same) return profile;
    /* read-only: a named profile never writes migration output into the default profile's dir */
    json_t *merged = kbcfg_load_dir(inherited_dir, false, NULL);
    const char *key;
    json_t *val;
    json_obj_foreach(profile, i, key, val) json_set(merged, key, json_clone(val));
    json_free(profile);
    return merged;
}

void kbcfg_apply(tui_keybindings_t *kb, const char *agent_dir, const char *inherited_dir) {
    json_t *cfg = kbcfg_load_merged(agent_dir, inherited_dir);
    tui_keybindings_set_user(kb, cfg);
    json_free(cfg);
}

void kbcfg_init_global(void) { kbcfg_apply(tui_keybindings_global(), omc_agent_dir(), NULL); }
