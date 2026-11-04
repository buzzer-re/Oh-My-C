/* test_slash.c - the shared slash-command registry: parsing, alias lookup, allowArgs matching, omp's
 * argument parsers (compact modes, shake modes, subcommands) and description key placeholders. */
#include "coding/slashcmd.h"

#include <stdio.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static void test_parse(void) {
    slash_parsed_t p;
    CHECK(slash_parse("/compact soft  focus here ", &p));
    CHECK(str_eq(p.name, "compact") && str_eq(p.args, "soft  focus here"));
    slash_parsed_free(&p);
    CHECK(slash_parse("/force:bash ls", &p)); /* ':' separates like whitespace */
    CHECK(str_eq(p.name, "force") && str_eq(p.args, "bash ls"));
    slash_parsed_free(&p);
    CHECK(!slash_parse("/", &p));
    CHECK(!slash_parse("hello", &p));
}

static void test_registry(void) {
    CHECK(slash_count() > 30);
    CHECK(str_eq(slash_at(0)->name, "settings")); /* upstream order (security is not implemented) */
    CHECK(slash_lookup("models") == slash_lookup("model"));
    CHECK(slash_lookup("rewind") == slash_lookup("branch"));
    CHECK(slash_lookup("q") == slash_lookup("quit"));
    CHECK(!slash_lookup("help") && !slash_lookup("thinking") && !slash_lookup("name"));
    CHECK(slash_lookup("providers") == slash_lookup("setup") && !slash_lookup("security")); /* never advertise what omc lacks */
    CHECK(!slash_lookup("ratchet") && !slash_lookup("omfg") && !slash_lookup("stats"));
    /* TUI only, like omp: no text handler */
    CHECK(slash_lookup("debug") && !slash_lookup("debug")->handle && slash_lookup("pause") && !slash_lookup("pause")->handle);
    CHECK(slash_lookup("record") && !slash_lookup("record")->handle);
    CHECK(slash_lookup("migrate") && str_eq(slash_at(slash_count() - 1)->name, "migrate"));
    const slash_cmd_t *a = slash_lookup("advisor");
    bool configure = false;
    for (size_t i = 0; i < a->nsubs; i++) configure |= str_eq(a->subs[i].name, "configure");
    CHECK(a->nsubs >= 3 && !configure);
    const slash_cmd_t *c = slash_lookup("compact");
    CHECK(c->nsubs == 1 && str_eq(c->subs[0].name, "soft") && str_eq(c->subs[0].usage, "[focus]"));
    CHECK(str_eq(c->acp_input_hint, "[soft] [focus]"));
    CHECK(str_eq(slash_lookup("resume")->inline_hint, "[session id]"));
    slash_parsed_t p;
    CHECK(!slash_match("/new now", &p));     /* no allowArgs: goes to the model */
    CHECK(!slash_match("/model gpt", &p));   /* omp's /model takes no arguments */
    CHECK(slash_match("/models", &p) == slash_lookup("model"));
    slash_parsed_free(&p);
    CHECK(slash_match("/handoff focus on tests", &p) == slash_lookup("handoff"));
    CHECK(str_eq(p.args, "focus on tests"));
    slash_parsed_free(&p);
    CHECK(!slash_match("/notacommand", &p));
}

static void test_args(void) {
    const char *mode;
    char *focus, *err;
    CHECK(slash_parse_compact_args("", &mode, &focus, &err) && !mode && !focus);
    CHECK(slash_parse_compact_args("Soft keep the API notes", &mode, &focus, &err) && str_eq(mode, "soft") &&
          str_eq(focus, "keep the API notes"));
    free(focus);
    CHECK(slash_parse_compact_args("softly please", &mode, &focus, &err) && !mode && str_eq(focus, "softly please"));
    free(focus);
    CHECK(slash_parse_compact_args("snapcompact", &mode, &focus, &err) && str_eq(mode, "snapcompact") && !focus);
    CHECK(!slash_parse_compact_args("snapcompact x", &mode, &focus, &err));
    CHECK(str_eq(err, "/compact snapcompact does not take focus instructions (it archives history without an LLM summary)."));
    free(err);
    err = NULL;
    CHECK(str_eq(slash_parse_shake_mode("", &err), "elide"));
    CHECK(str_eq(slash_parse_shake_mode(" IMAGES ", &err), "images"));
    CHECK(!slash_parse_shake_mode("bogus", &err) && str_eq(err, "Unknown /shake mode \"bogus\". Use elide, images, or thinking."));
    free(err);
    char *verb, *rest;
    slash_subcommand("  Dump   raw ", &verb, &rest);
    CHECK(str_eq(verb, "dump") && str_eq(rest, "raw"));
    free(verb);
    free(rest);
    char *d = slash_format_description("x {key:shift+tab} y", NULL);
    CHECK(str_eq(d, "x shift+tab y"));
    free(d);
    char *n = slash_locale_int(1234567);
    CHECK(str_eq(n, "1,234,567"));
    free(n);
}

int main(void) {
    test_parse();
    test_registry();
    test_args();
    if (fails) return 1;
    printf("test_slash: ok\n");
    return 0;
}
