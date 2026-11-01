# omc - oh-my-c build
CC      ?= cc
PKG     ?= pkg-config
OPT     ?= -O2
CFLAGS  ?= $(OPT) -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable -Wshadow=local \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIC -Isrc -Iinclude -MMD -MP
LDFLAGS += -pie -rdynamic -Wl,-z,relro,-z,now
LIBS    += $(shell $(PKG) --libs openssl 2>/dev/null || echo -lssl -lcrypto) -lz -lpthread -lm

SRC  := $(sort $(wildcard src/*.c src/*/*.c src/ui/tools/*.c))
OBJ  := $(patsubst src/%.c,build/obj/%.o,$(SRC)) build/obj/embedded.o
BIN  := build/omc

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

# ---- optional library build: `make lib` -> libomc.a, libomc.so, omc.pc (public API: include/omc.h) ----
LIB_OBJ    := $(filter-out build/obj/main.o,$(OBJ))
LIB_SOVER  := 0
LIB_STATIC := build/libomc.a
LIB_SHARED := build/libomc.so.$(LIB_SOVER)
OBJCOPY    ?= objcopy

lib: $(LIB_STATIC) $(LIB_SHARED) build/omc.pc

# static: prelink everything into one object, then keep only the public omc_* symbols global so
# internal names (json_*, sb_*, ...) can never clash with the host application's symbols
build/libomc.syms: include/omc.h
	@mkdir -p build
	@grep -o 'OMC_API [^(]*(' $< | sed -E 's/.*[ *]([a-z_0-9]+)\($$/\1/' | sort -u > $@

$(LIB_STATIC): $(LIB_OBJ) build/libomc.syms
	$(LD) -r -o build/omc-prelink.o $(LIB_OBJ)
	$(OBJCOPY) --keep-global-symbols=build/libomc.syms build/omc-prelink.o build/omc-lib.o
	rm -f $@ && $(AR) rcs $@ build/omc-lib.o

build/libomc.map: build/libomc.syms
	@{ echo '{ global:'; sed 's/$$/;/' $<; echo 'local: *; };'; } > $@

$(LIB_SHARED): $(LIB_OBJ) build/libomc.map
	$(CC) -shared -Wl,-soname,libomc.so.$(LIB_SOVER) -Wl,--version-script=build/libomc.map -Wl,-z,relro,-z,now \
	    -o $@ $(LIB_OBJ) $(LIBS)
	ln -sf libomc.so.$(LIB_SOVER) build/libomc.so

build/omc.pc: Makefile
	@printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: omc\nDescription: oh-my-c coding agent library\nVersion: %s\nLibs: -L$${libdir} -lomc\nLibs.private: %s\nCflags: -I$${includedir}\n' \
	    '$(PREFIX)' "$$(sed -n 's/.*OMC_VERSION "\(.*\)".*/\1/p' src/core/core.h)" '$(LIBS)' > $@

install-lib: lib
	install -Dm644 include/omc.h $(DESTDIR)$(PREFIX)/include/omc.h
	install -Dm644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/libomc.a
	install -Dm755 $(LIB_SHARED) $(DESTDIR)$(PREFIX)/lib/libomc.so.$(LIB_SOVER)
	ln -sf libomc.so.$(LIB_SOVER) $(DESTDIR)$(PREFIX)/lib/libomc.so
	install -Dm644 build/omc.pc $(DESTDIR)$(PREFIX)/lib/pkgconfig/omc.pc

# embedding example + its check (links the static library only)
build/examples/embed: examples/embed.c $(LIB_STATIC)
	@mkdir -p build/examples
	$(CC) -O2 -Wall -Wextra -Iinclude -o $@ $< $(LIB_STATIC) $(LIBS)

build/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Embedded assets (prompts, catalog) via a tiny in-tree generator: no external tools.
build/embedgen: build-tools/embedgen.c
	@mkdir -p build
	$(CC) -O2 -o $@ $<

EMBED_FILES := $(shell find prompts assets -type f 2>/dev/null | LC_ALL=C sort) README.md $(sort $(wildcard docs/*.md))
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
GOOGLE_OBJ := $(O)/ai/provider_google.o $(O)/ai/google_schema.o $(O)/ai/google_auth.o
T_test_providers_ag  := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/provider_transport.o $(O)/ai/provider_anthropic.o $(GOOGLE_OBJ)
T_test_providers_google := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/provider_transport.o $(GOOGLE_OBJ) $(O)/ai/google_cca.o \
                        $(O)/ai/provider_gemini_cli.o $(O)/ai/think_heal.o
T_test_providers_oai := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/provider_transport.o $(O)/ai/provider_openai_completions.o \
                        $(O)/ai/provider_openai_responses.o $(O)/ai/provider_ollama.o
T_test_http          := $(CORE_OBJ) $(NET_OBJ)
T_test_ws            := $(CORE_OBJ) $(NET_OBJ) $(O)/net/ws.o
T_test_providers_bedrock := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/provider_transport.o $(O)/ai/aws.o $(O)/ai/provider_bedrock.o
T_test_readfmt       := $(CORE_OBJ) $(O)/tools/readfmt.o
T_test_editmodes     := $(CORE_OBJ) $(O)/tools/diff.o $(O)/tools/fuzzy.o $(O)/tools/patch.o $(O)/tools/sloppy.o \
                        $(O)/text/regex.o $(O)/text/rxbt.o
T_test_cursor        := $(CORE_OBJ) $(O)/ai/pb.o $(O)/ai/cursor_proto.o
T_tui_demo           := $(T_test_tui)
T_test_chrome        := $(LIB_OBJ)
T_test_auth          := $(LIB_OBJ)
T_test_toolcards     := $(T_test_tui) $(patsubst src/%.c,$(O)/%.o,$(wildcard src/ui/tools/*.c)) $(O)/tools/outsink.o
T_test_outsink       := $(CORE_OBJ) $(O)/tools/outsink.o
T_test_judgment      := $(CORE_OBJ) $(O)/coding/judgment.o $(O)/prompt/template.o $(O)/text/yaml.o
T_test_memory        := $(LIB_OBJ)
UNIT_TESTS := test_text test_tui test_providers_ag test_providers_oai test_providers_bedrock test_readfmt test_editmodes \
              test_chrome test_providers_google test_toolcards test_cursor test_judgment test_auth test_memory test_outsink

.SECONDEXPANSION:
build/tests/%: tests/%.c $$(T_$$*)
	@mkdir -p build/tests
	$(CC) $(CFLAGS) -o $@ $< $(T_$*) $(LIBS)

test: $(addprefix build/tests/,$(UNIT_TESTS)) build/examples/embed
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; ./build/tests/$$t >/dev/null || { echo "   FAIL ($$t)"; exit 1; }; echo "   ok"; done
	@./tests/e2e/run.sh

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/omc

PREFIX ?= /usr/local

clean:
	rm -rf build

.PHONY: all lib install-lib test clean install
-include $(OBJ:.o=.d)
