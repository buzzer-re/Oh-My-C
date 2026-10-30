# omc - oh-my-c build
CC      ?= cc
PKG     ?= pkg-config
OPT     ?= -O2
CFLAGS  ?= $(OPT) -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable -Wshadow=local \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -Isrc -MMD -MP
LDFLAGS += -pie -rdynamic -Wl,-z,relro,-z,now
LIBS    += $(shell $(PKG) --libs openssl 2>/dev/null || echo -lssl -lcrypto) -lz -lpthread -lm

SRC  := $(sort $(wildcard src/*.c src/*/*.c))
OBJ  := $(patsubst src/%.c,build/obj/%.o,$(SRC)) build/obj/embedded.o
BIN  := build/omc

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Embedded assets (prompts, catalog) via a tiny in-tree generator: no external tools.
build/embedgen: build-tools/embedgen.c
	@mkdir -p build
	$(CC) -O2 -o $@ $<

EMBED_FILES := $(shell find prompts assets -type f 2>/dev/null | LC_ALL=C sort)
build/embedded.c: build/embedgen $(EMBED_FILES)
	@./build/embedgen $@ $(EMBED_FILES)

build/obj/embedded.o: build/embedded.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Unit tests link only the objects they exercise (several stub catalog/auth symbols).
O := build/obj
CORE_OBJ := $(O)/core/core.o $(O)/core/json.o $(O)/core/fs.o $(O)/core/proc.o $(O)/core/embed.o $(O)/embedded.o
NET_OBJ  := $(O)/net/http.o
T_test_text          := $(CORE_OBJ) $(patsubst src/%.c,$(O)/%.o,$(wildcard src/text/*.c))
T_test_tui           := $(CORE_OBJ) $(patsubst src/%.c,$(O)/%.o,$(wildcard src/tui/*.c))
T_test_providers_ag  := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/provider_anthropic.o $(O)/ai/provider_google.o
T_test_providers_oai := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/provider_openai_completions.o \
                        $(O)/ai/provider_openai_responses.o $(O)/ai/provider_ollama.o
T_test_http          := $(CORE_OBJ) $(NET_OBJ)
T_test_providers_bedrock := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/aws.o $(O)/ai/provider_bedrock.o
T_test_readfmt       := $(CORE_OBJ) $(O)/tools/readfmt.o
T_tui_demo           := $(T_test_tui)
UNIT_TESTS := test_text test_tui test_providers_ag test_providers_oai test_providers_bedrock test_readfmt

.SECONDEXPANSION:
build/tests/%: tests/%.c $$(T_$$*)
	@mkdir -p build/tests
	$(CC) $(CFLAGS) -o $@ $< $(T_$*) $(LIBS)

test: $(addprefix build/tests/,$(UNIT_TESTS))
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; ./build/tests/$$t >/dev/null || { echo "   FAIL ($$t)"; exit 1; }; echo "   ok"; done
	@./tests/e2e/run.sh

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/omc

PREFIX ?= /usr/local

clean:
	rm -rf build

.PHONY: all test clean install
-include $(OBJ:.o=.d)
