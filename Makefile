# omc - oh-my-c build (Linux and macOS)
CC      ?= cc
PKG     ?= pkg-config
OPT     ?= -O2
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
# per-platform build tree: the source dir may be shared between machines (e.g. a macOS host and a
# Linux VM), so objects and generated tools of one platform must never be picked up by the other
O       := build/obj/$(UNAME_S)-$(UNAME_M)
CC_IS_CLANG := $(shell $(CC) --version 2>/dev/null | grep -qi clang && echo 1)

CFLAGS  ?= $(OPT) -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIC -Isrc -Iinclude -MMD -MP
ifneq ($(CC_IS_CLANG),1)
CFLAGS  += -Wshadow=local
endif

ifeq ($(UNAME_S),Darwin)
# Apple ld: PIE is the default; no -z options. OpenSSL comes from Homebrew (keg-only) or pkg-config.
LDFLAGS += -Wl,-export_dynamic
OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)
SSL_PC  := $(if $(OPENSSL_PREFIX),PKG_CONFIG_PATH=$(OPENSSL_PREFIX)/lib/pkgconfig:$$PKG_CONFIG_PATH )
SSL_CFLAGS := $(shell $(SSL_PC)$(PKG) --cflags openssl 2>/dev/null || { [ -n "$(OPENSSL_PREFIX)" ] && echo -I$(OPENSSL_PREFIX)/include; })
SSL_LIBS   := $(shell $(SSL_PC)$(PKG) --libs openssl 2>/dev/null || { [ -n "$(OPENSSL_PREFIX)" ] && echo -L$(OPENSSL_PREFIX)/lib; echo -lssl -lcrypto; })
else
LDFLAGS += -pie -rdynamic -Wl,-z,relro,-z,now
SSL_CFLAGS := $(shell $(PKG) --cflags openssl 2>/dev/null)
SSL_LIBS   := $(shell $(PKG) --libs openssl 2>/dev/null || echo -lssl -lcrypto)
endif
CFLAGS  += $(SSL_CFLAGS)
LIBS    += $(SSL_LIBS) -lz -lpthread -lm

SRC  := $(sort $(wildcard src/*.c src/*/*.c src/ui/tools/*.c))
OBJ  := $(patsubst src/%.c,$(O)/%.o,$(SRC)) $(O)/embedded.o
BIN  := build/omc

all: $(BIN)

$(O)/omc: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

# build/omc is always this platform's binary (refreshed on every make)
$(BIN): $(O)/omc FORCE
	@cmp -s $< $@ || cp $< $@

FORCE:

# ---- optional library build: `make lib` -> libomc.a, libomc.so, omc.pc (public API: include/omc.h) ----
LIB_OBJ    := $(filter-out $(O)/main.o,$(OBJ))
LIB_SOVER  := 0
LIB_STATIC := build/libomc.a
ifeq ($(UNAME_S),Darwin)
LIB_SHARED := build/libomc.$(LIB_SOVER).dylib
else
LIB_SHARED := build/libomc.so.$(LIB_SOVER)
endif
OBJCOPY    ?= objcopy

lib: $(LIB_STATIC) $(LIB_SHARED) build/omc.pc

# static: prelink everything into one object, then keep only the public omc_* symbols global so
# internal names (json_*, sb_*, ...) can never clash with the host application's symbols
build/libomc.syms: include/omc.h
	@mkdir -p build
	@grep -o 'OMC_API [^(]*(' $< | sed -E 's/.*[ *]([a-z_0-9]+)\($$/\1/' | sort -u > $@

ifeq ($(UNAME_S),Darwin)
# Mach-O symbols carry a leading underscore
build/libomc.exports: build/libomc.syms
	@sed 's/^/_/' $< > $@

$(LIB_STATIC): $(LIB_OBJ) build/libomc.exports
	$(LD) -r -exported_symbols_list build/libomc.exports -o build/omc-lib.o $(LIB_OBJ)
	rm -f $@ && $(AR) rcs $@ build/omc-lib.o

$(LIB_SHARED): $(LIB_OBJ) build/libomc.exports
	$(CC) -dynamiclib -install_name @rpath/libomc.$(LIB_SOVER).dylib -Wl,-exported_symbols_list,build/libomc.exports \
	    -o $@ $(LIB_OBJ) $(LIBS)
	ln -sf libomc.$(LIB_SOVER).dylib build/libomc.dylib
else
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
endif

build/omc.pc: Makefile
	@printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: omc\nDescription: oh-my-c coding agent library\nVersion: %s\nLibs: -L$${libdir} -lomc\nLibs.private: %s\nCflags: -I$${includedir}\n' \
	    '$(PREFIX)' "$$(sed -n 's/.*OMC_VERSION "\(.*\)".*/\1/p' src/core/core.h)" '$(LIBS)' > $@

install-lib: lib
	mkdir -p $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m644 include/omc.h $(DESTDIR)$(PREFIX)/include/omc.h
	install -m644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/libomc.a
	install -m755 $(LIB_SHARED) $(DESTDIR)$(PREFIX)/lib/$(notdir $(LIB_SHARED))
	ln -sf $(notdir $(LIB_SHARED)) $(DESTDIR)$(PREFIX)/lib/$(if $(filter Darwin,$(UNAME_S)),libomc.dylib,libomc.so)
	install -m644 build/omc.pc $(DESTDIR)$(PREFIX)/lib/pkgconfig/omc.pc

# embedding example + its check (links the static library only)
build/examples/embed: examples/embed.c $(LIB_STATIC)
	@mkdir -p build/examples
	$(CC) -O2 -Wall -Wextra -Iinclude -o $@ $< $(LIB_STATIC) $(LIBS)

$(O)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Embedded assets (prompts, catalog) via a tiny in-tree generator: no external tools.
$(O)/embedgen: build-tools/embedgen.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -o $@ $<

EMBED_FILES := $(shell find prompts assets -type f 2>/dev/null | LC_ALL=C sort) README.md $(sort $(wildcard docs/*.md))
$(O)/embedded.c: $(O)/embedgen $(EMBED_FILES)
	@./$(O)/embedgen $@ $(EMBED_FILES)

$(O)/embedded.o: $(O)/embedded.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Unit tests link only the objects they exercise (several stub catalog/auth symbols).
CORE_OBJ := $(O)/core/core.o $(O)/core/json.o $(O)/core/fs.o $(O)/core/proc.o $(O)/core/embed.o $(O)/embedded.o
NET_OBJ  := $(O)/net/http.o
CODEC_OBJ := $(patsubst src/%.c,$(O)/%.o,$(wildcard src/img/*.c))
IMG_OBJ  := $(O)/ai/ai_images.o $(CODEC_OBJ)
T_test_text          := $(CORE_OBJ) $(patsubst src/%.c,$(O)/%.o,$(wildcard src/text/*.c))
T_test_tui           := $(CORE_OBJ) $(patsubst src/%.c,$(O)/%.o,$(wildcard src/tui/*.c)) $(O)/text/walker.o $(O)/text/glob.o $(CODEC_OBJ)
GOOGLE_OBJ := $(O)/ai/provider_google.o $(O)/ai/google_schema.o $(O)/ai/google_auth.o
T_test_providers_ag  := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/retry.o $(O)/ai/stream_guard.o $(O)/ai/service_tier.o $(O)/ai/provider_transport.o $(O)/ai/provider_anthropic.o $(O)/ai/anthropic_slow.o $(GOOGLE_OBJ) $(IMG_OBJ)
T_test_providers_google := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/retry.o $(O)/ai/stream_guard.o $(O)/ai/service_tier.o $(O)/ai/provider_transport.o $(GOOGLE_OBJ) $(O)/ai/google_cca.o \
                        $(O)/ai/provider_gemini_cli.o $(O)/ai/think_heal.o $(IMG_OBJ)
T_test_providers_oai := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/retry.o $(O)/ai/stream_guard.o $(O)/ai/service_tier.o $(O)/ai/provider_transport.o $(O)/ai/provider_openai_completions.o \
                        $(O)/ai/provider_openai_responses.o $(O)/ai/provider_ollama.o $(O)/ai/compat_detect.o
T_test_http          := $(CORE_OBJ) $(NET_OBJ)
T_test_ws            := $(CORE_OBJ) $(NET_OBJ) $(O)/net/ws.o
T_test_providers_bedrock := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/retry.o $(O)/ai/stream_guard.o $(O)/ai/service_tier.o $(O)/ai/provider_transport.o $(O)/ai/aws.o $(O)/ai/provider_bedrock.o
T_test_readfmt       := $(CORE_OBJ) $(O)/tools/readfmt.o
T_test_editmodes     := $(CORE_OBJ) $(O)/tools/diff.o $(O)/tools/fuzzy.o $(O)/tools/patch.o $(O)/tools/sloppy.o $(O)/tools/hashline.o \
                        $(O)/text/regex.o $(O)/text/rxbt.o
T_test_cursor        := $(CORE_OBJ) $(O)/ai/pb.o $(O)/ai/cursor_proto.o
T_tui_demo           := $(T_test_tui)
T_test_chrome        := $(LIB_OBJ)
T_test_auth          := $(LIB_OBJ)
T_test_catalog       := $(LIB_OBJ)
T_test_model_resolver := $(LIB_OBJ)
T_test_compaction_point := $(LIB_OBJ)
T_test_toolcards     := $(T_test_tui) $(patsubst src/%.c,$(O)/%.o,$(wildcard src/ui/tools/*.c)) $(O)/tools/outsink.o
T_test_outsink       := $(CORE_OBJ) $(O)/tools/outsink.o
T_test_judgment      := $(CORE_OBJ) $(O)/coding/judgment.o $(O)/prompt/template.o $(O)/text/yaml.o
T_test_memory        := $(LIB_OBJ)
T_test_planreview    := $(LIB_OBJ)
T_test_btw           := $(LIB_OBJ)
T_test_queue         := $(LIB_OBJ)
T_test_export        := $(LIB_OBJ)
T_test_dump          := $(LIB_OBJ)
T_test_panels        := $(LIB_OBJ)
T_test_img           := $(CORE_OBJ) $(CODEC_OBJ)
T_test_img_jpeg      := $(CORE_OBJ) $(CODEC_OBJ)
T_test_img_webp      := $(CORE_OBJ) $(CODEC_OBJ)
T_test_guards        := $(LIB_OBJ)
T_test_keybindings   := $(CORE_OBJ) $(O)/tui/keys.o $(O)/config/keybindings.o $(O)/text/yaml.o
T_test_slash         := $(LIB_OBJ)
T_test_signals       := $(LIB_OBJ)
T_test_tree          := $(LIB_OBJ)
T_test_approval      := $(LIB_OBJ)
T_test_commands      := $(LIB_OBJ)
T_test_compaction    := $(LIB_OBJ)
T_test_tiers         := $(LIB_OBJ)
T_test_workspace     := $(LIB_OBJ)
T_test_agenthub      := $(LIB_OBJ)
T_test_mcp           := $(LIB_OBJ)
T_test_loop          := $(LIB_OBJ)
T_test_usage         := $(LIB_OBJ)
T_test_extracmds     := $(LIB_OBJ)
T_test_display       := $(LIB_OBJ)
T_test_turn_recovery := $(LIB_OBJ)
T_test_settings_audit := $(CORE_OBJ)
UNIT_TESTS := test_text test_tui test_providers_ag test_providers_oai test_providers_bedrock test_readfmt test_editmodes \
              test_chrome test_providers_google test_toolcards test_cursor test_judgment test_auth test_memory test_outsink test_catalog test_model_resolver test_compaction_point \
              test_planreview test_img test_img_jpeg test_img_webp test_btw test_queue test_guards test_keybindings test_slash test_commands test_export test_dump test_signals \
              test_panels test_tree test_approval test_compaction test_tiers test_workspace test_agenthub test_mcp test_loop test_usage test_extracmds test_display test_turn_recovery test_settings_audit

.SECONDEXPANSION:
$(O)/tests/%: tests/%.c $$(T_$$*)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $< $(T_$*) $(LIBS)

test: $(addprefix $(O)/tests/,$(UNIT_TESTS)) build/examples/embed $(BIN)
	@set -e; for t in $(UNIT_TESTS); do echo "== $$t"; ./$(O)/tests/$$t >/dev/null || { echo "   FAIL ($$t)"; exit 1; }; echo "   ok"; done
	@OMC_BIN=$(CURDIR)/$(O)/omc ./tests/e2e/run.sh

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m755 $(BIN) $(DESTDIR)$(PREFIX)/bin/omc

# per-user install like omp's (~/.local/bin/omc); `make install PREFIX=/usr/local` (with sudo) for a system-wide one
PREFIX ?= $(HOME)/.local

clean:
	rm -rf build

.PHONY: all lib install-lib test clean install FORCE
-include $(OBJ:.o=.d)
