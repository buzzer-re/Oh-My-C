/* judgment: question validation, state rendering, keyword prompts and reply parsing (text.ts behavior) */
#include "coding/judgment.h"

#include <stdio.h>

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static json_t *J(const char *s) {
    json_t *j = json_parse_str(s);
    if (!j) fprintf(stderr, "bad json: %s\n", s);
    return j;
}

int main(void) {
    char *err = NULL;
    /* word matching */
    CHECK(judgment_index_of_word("The answer: YES.", "yes") == 12);
    CHECK(judgment_index_of_word("eyes", "yes") == -1);
    CHECK(judgment_parse_yes_no("no, not true") == 0);
    CHECK(judgment_parse_yes_no("Yes") == 1);
    CHECK(judgment_parse_yes_no("maybe") == -1);
    CHECK(judgment_parse_score("level 2.", 3) == 2);
    CHECK(judgment_parse_score("v2 or 1.5 then 7 then 1", 3) == 1);
    CHECK(judgment_parse_score("none", 3) == -1);

    /* validation */
    json_t *q = judgment_parse_questions(J("{\"c\":{\"type\":\"choice\",\"instructions\":\"i\",\"criteria\":{\"a\":null}}}"), &err);
    CHECK(!q && err && strstr(err, "choice question \"c\" needs at least two options"));
    free(err);
    q = judgment_parse_questions(J("{}"), &err);
    CHECK(!q && strstr(err, "questions must contain at least one question"));
    free(err);
    CHECK(!judgment_check_state(J("\"\""), &err) && strstr(err, "state must not be empty"));
    free(err);
    CHECK(!judgment_check_state(J("3"), &err));
    free(err);

    json_t *qs = judgment_parse_questions(
        J("{\"fruit\":{\"type\":\"choice\",\"instructions\":\"Which?\",\"criteria\":{\"high\":null,\"xhigh\":\"very\"}},"
          "\"ok\":{\"type\":\"bool\",\"instructions\":\"Fine?\",\"criteria\":{\"true\":\"good\"}},"
          "\"lvl\":{\"type\":\"score\",\"instructions\":\"How?\",\"criteria\":[\"lo\",\"mid\",\"hi\"]}}"),
        &err);
    CHECK(qs && !err);

    /* prompt rendering */
    char *sys = NULL, *user = NULL;
    judgment_render_prompt(J("{\"title\":\"a<b\",\"bad key\":[1,2]}"), qs, &sys, &user);
    CHECK(strstr(sys, "Question `fruit`: Which?"));
    CHECK(strstr(sys, "- `xhigh`: very"));
    CHECK(strstr(sys, "YES: good"));
    CHECK(strstr(sys, "- `2`: hi"));
    CHECK(strstr(user, "State:\n<title>a&lt;b</title>\n<field name=\"bad key\">\n"));
    CHECK(strstr(user, "Answer one line per question, `<question id>: <answer>`."));
    free(sys);
    free(user);
    char *st = judgment_render_state(J("\"plain\""));
    CHECK(!strcmp(st, "<state>plain</state>"));
    free(st);

    /* reply parsing: multi-question lines, longer label wins a tie */
    json_t *a = judgment_parse_reply(qs, "Sure.\n- fruit: xhigh\n`ok` = yes\nlvl - 1\n", &err);
    CHECK(a && !err);
    CHECK(!strcmp(json_get_str(json_get(a, "fruit"), "choice"), "xhigh"));
    CHECK(json_get_int(json_get(json_get(a, "fruit"), "probabilities"), "high", -1) == 0);
    CHECK(json_get_int(json_get(a, "ok"), "bool", -1) == 1);
    CHECK(json_get_int(json_get(a, "lvl"), "score", -1) == 1);
    CHECK(!strcmp(json_get_str(json_get(a, "ok"), "type"), "bool"));
    json_free(a);
    a = judgment_parse_reply(qs, "fruit: high\nok: no", &err);
    CHECK(!a && err && strstr(err, "judgment \"lvl\": no answer line for question"));
    free(err);
    json_t *one = judgment_parse_questions(J("{\"b\":{\"type\":\"bool\",\"instructions\":\"x\"}}"), &err);
    a = judgment_parse_reply(one, "NO", &err);
    CHECK(a && json_get_int(json_get(a, "b"), "bool", -1) == 0);
    json_free(a);
    a = judgment_parse_reply(one, "hmm", &err);
    CHECK(!a && strstr(err, "no yes/no in reply: \"hmm\""));
    free(err);
    char *r = judgment_retry_system("SYS", true);
    CHECK(strstr(r, "SYS") && strstr(r, "labels, one line per question id"));
    free(r);
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
