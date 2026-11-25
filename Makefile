# omc - oh-my-c build (Linux and macOS; Windows: `make windows`, a MinGW-w64 cross build, see docs/windows.md)
CC      ?= cc
PKG     ?= pkg-config
OPT     ?= -O2
ifeq ($(WINDOWS),1)
# Windows x64 cross build. MINGW is the toolchain prefix; the posix-threads gcc variant is preferred where the distro
# ships both (Debian/Ubuntu's mingw-w64); MSYS2's plain gcc is the posix one already.
MINGW   ?= x86_64-w64-mingw32-
CC      := $(shell command -v $(MINGW)gcc-posix >/dev/null 2>&1 && echo $(MINGW)gcc-posix || echo $(MINGW)gcc)
WINDRES ?= $(MINGW)windres
HOSTCC  ?= cc
UNAME_S := Windows
UNAME_M := x86_64
EXE     := .exe
else
HOSTCC  ?= $(CC)
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
EXE     :=
endif
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

ifeq ($(UNAME_S),Windows)
# MinGW-w64: POSIX headers omc needs and the CRT lacks come from src/plat/win/include (poll.h, sys/socket.h, termios.h,
# zlib.h, openssl/*.h, ...), backed by src/plat/win/*.c over Win32, Winsock, Schannel and CNG. Every TU sees
# plat/win/posix.h first. Only system DLLs are imported: libgcc, libssp and winpthreads are linked statically.
CFLAGS  := $(filter-out -fPIC,$(CFLAGS)) -Isrc/plat/win/include -include plat/win/posix.h \
           -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -D__USE_MINGW_ANSI_STDIO=1 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNOGDI
LDFLAGS += -static
TEST_LDFLAGS := -static
# libssp only where the toolchain ships it: newer mingw-w64 runtimes carry the stack protector themselves (MinGW-Builds
# gcc 15, GitHub's Windows runners)
SSP_LIB := $(if $(filter libssp.a,$(shell $(CC) -print-file-name=libssp.a 2>/dev/null)),,-lssp)
LIBS    += -lws2_32 -lsecur32 -lcrypt32 -lbcrypt -lncrypt -ladvapi32 -luser32 -lshell32 -lole32 -luserenv -ldbghelp $(SSP_LIB)
SSL_LIBS :=
else ifeq ($(UNAME_S),Darwin)
# Apple ld: PIE is the default; no -z options. OpenSSL comes from Homebrew (keg-only) or pkg-config.
LDFLAGS += -Wl,-export_dynamic
# unit tests link a subset of the objects, leaving some __attribute__((weak)) references undefined; ELF resolves those
# to NULL at link time, Apple ld needs them looked up at load time (dyld then also leaves a missing weak symbol NULL)
TEST_LDFLAGS := -Wl,-undefined,dynamic_lookup
OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)
SSL_PC  := $(if $(OPENSSL_PREFIX),PKG_CONFIG_PATH=$(OPENSSL_PREFIX)/lib/pkgconfig:$$PKG_CONFIG_PATH )
SSL_CFLAGS := $(shell $(SSL_PC)$(PKG) --cflags openssl 2>/dev/null || { [ -n "$(OPENSSL_PREFIX)" ] && echo -I$(OPENSSL_PREFIX)/include; })
SSL_LIBS   := $(shell $(SSL_PC)$(PKG) --libs openssl 2>/dev/null || { [ -n "$(OPENSSL_PREFIX)" ] && echo -L$(OPENSSL_PREFIX)/lib; echo -lssl -lcrypto; })
# release binaries (build-tools/ci.sh dist): OpenSSL linked in statically, so the binary needs no Homebrew on the Mac
ifeq ($(STATIC_OPENSSL),1)
SSL_LIBS   := $(OPENSSL_PREFIX)/lib/libssl.a $(OPENSSL_PREFIX)/lib/libcrypto.a
endif
else
LDFLAGS += -pie -rdynamic -Wl,-z,relro,-z,now
SSL_CFLAGS := $(shell $(PKG) --cflags openssl 2>/dev/null)
SSL_LIBS   := $(shell $(PKG) --libs openssl 2>/dev/null || echo -lssl -lcrypto)
endif
CFLAGS  += $(SSL_CFLAGS)
ifeq ($(UNAME_S),Windows)
LIBS    := -lpthread -lm $(LIBS)
else
LIBS    += $(SSL_LIBS) -lz -lpthread -lm
endif

SRC  := $(sort $(wildcard src/*.c src/*/*.c src/ui/tools/*.c))
ifeq ($(UNAME_S),Windows)
SRC  += $(sort $(wildcard src/plat/win/*.c))
endif
OBJ  := $(patsubst src/%.c,$(O)/%.o,$(SRC)) $(O)/embedded.o
BIN  := build/omc$(EXE)

all: $(BIN)

ifeq ($(UNAME_S),Windows)
# application manifest (UTF-8 code page, long paths, Windows 10 compatibility) and version info
OBJ  += $(O)/plat/win/omc_rc.o
$(O)/plat/win/omc_rc.o: src/plat/win/omc.rc src/plat/win/omc.manifest src/core/core.h
	@mkdir -p $(dir $@)
	$(WINDRES) $(foreach v,1 2 3,-DOMC_V$(word $(v),MAJOR MINOR PATCH)=$(word $(v),$(subst ., ,$(VERSION)))) -Isrc/plat/win -i $< -o $@
endif

# `make windows [lib]`: the same targets cross-built for Windows x64 (build/omc.exe, build/libomc.dll)
windows:
	@$(MAKE) --no-print-directory WINDOWS=1 all

$(O)/omc$(EXE): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

# build/omc is always this platform's binary (refreshed on every make)
$(BIN): $(O)/omc$(EXE) FORCE
	@cmp -s $< $@ || cp $< $@

FORCE:

# ---- optional library build: `make lib` -> libomc.a, libomc.so, omc.pc (public API: include/omc.h) ----
LIB_OBJ    := $(filter-out $(O)/main.o $(O)/plat/win/omc_rc.o,$(OBJ))
LIB_SOVER  := 0
LIB_STATIC := build/libomc.a
ifeq ($(UNAME_S),Darwin)
LIB_SHARED := build/libomc.$(LIB_SOVER).dylib
else ifeq ($(UNAME_S),Windows)
# a tree of its own: the Windows build shares build/ with the Linux one it is cross-compiled beside
LIB_STATIC := build/windows/libomc.a
LIB_SHARED := build/windows/libomc.dll
else
LIB_SHARED := build/libomc.so.$(LIB_SOVER)
endif
OBJCOPY    ?= objcopy
PYTHON     ?= python3
PC_FILE    := $(if $(filter Windows,$(UNAME_S)),build/windows/omc.pc,build/omc.pc)
# Windows: relocatable, the root is the directory above the .pc (the release zip and install.ps1 -Lib keep it in lib\)
PC_PREFIX  := $(if $(filter Windows,$(UNAME_S)),$${pcfiledir}/..,$(PREFIX))

lib: $(LIB_STATIC) $(LIB_SHARED) $(PC_FILE)

# static: prelink everything into one object, then keep only the public omc_* symbols global so
# internal names (json_*, sb_*, ...) can never clash with the host application's symbols
build/libomc.syms: include/omc.h
	@mkdir -p build
	@grep -o 'OMC_API [^(]*(' $< | sed -E 's/.*[ *]([a-z_0-9]+)\($$/\1/' | grep '^omc_' | sort -u > $@

ifeq ($(UNAME_S),Windows)
# COFF: the same prelink as ELF for the static library; the DLL exports exactly the omc_* API (a .def file) and comes
# with its import library, build/libomc.dll.a. Both carry their own libgcc/winpthreads, so only system DLLs are needed.
$(LIB_STATIC): $(LIB_OBJ) build/libomc.syms
	@mkdir -p $(dir $@)
	$(MINGW)ld -r -o $(O)/omc-prelink.o $(LIB_OBJ)
	$(MINGW)objcopy --keep-global-symbols=build/libomc.syms $(O)/omc-prelink.o $(O)/omc-lib.o
	rm -f $@ && $(MINGW)ar rcs $@ $(O)/omc-lib.o

build/windows/libomc.def: build/libomc.syms
	@mkdir -p $(dir $@)
	@{ echo 'LIBRARY libomc.dll'; echo 'EXPORTS'; sed 's/^/    /' $<; } > $@

$(LIB_SHARED): $(LIB_OBJ) build/windows/libomc.def
	$(CC) -shared -static -o $@ $(LIB_OBJ) build/windows/libomc.def -Wl,--out-implib,build/windows/libomc.dll.a $(LIBS)
else ifeq ($(UNAME_S),Darwin)
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

$(PC_FILE): Makefile
	@printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: omc\nDescription: oh-my-c coding agent library\nVersion: %s\nLibs: -L$${libdir} -lomc\nLibs.private: %s\nCflags: -I$${includedir}\n' \
	    '$(PC_PREFIX)' '$(VERSION)' '$(LIBS)' > $@

install-lib: lib
	mkdir -p $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m644 include/omc.h $(DESTDIR)$(PREFIX)/include/omc.h
	install -m644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/libomc.a
	install -m755 $(LIB_SHARED) $(DESTDIR)$(PREFIX)/lib/$(notdir $(LIB_SHARED))
	ln -sf $(notdir $(LIB_SHARED)) $(DESTDIR)$(PREFIX)/lib/$(if $(filter Darwin,$(UNAME_S)),libomc.dylib,libomc.so)
	install -m644 build/omc.pc $(DESTDIR)$(PREFIX)/lib/pkgconfig/omc.pc

# embedding example + its check (links the static library only)
build/examples/embed$(EXE): examples/embed.c $(LIB_STATIC)
	@mkdir -p build/examples
	$(CC) -O2 -Wall -Wextra -Iinclude -o $@ $< $(LIB_STATIC) $(LIBS) $(if $(filter Windows,$(UNAME_S)),-static)

$(O)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Embedded assets (prompts, catalog) via a tiny in-tree generator: no external tools.
$(O)/embedgen: build-tools/embedgen.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -o $@ $<

EMBED_FILES := $(shell find prompts assets -type f 2>/dev/null | LC_ALL=C sort) README.md $(sort $(wildcard docs/*.md))
$(O)/embedded.c: $(O)/embedgen $(EMBED_FILES)
	@./$(O)/embedgen $@ $(EMBED_FILES)

$(O)/embedded.o: $(O)/embedded.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Unit tests link only the objects they exercise (several stub catalog/auth symbols).
FUZZ_SRC := $(sort $(wildcard tests/fuzz/fuzz_*.c))
CORE_OBJ := $(O)/core/core.o $(O)/core/json.o $(O)/core/fs.o $(O)/core/proc.o $(O)/core/embed.o $(O)/embedded.o
ifeq ($(UNAME_S),Windows)
# the platform layer under every test (its POSIX regex needs text/regex.o, which only the full object list has)
CORE_OBJ += $(filter-out $(O)/plat/win/regex.o,$(patsubst src/%.c,$(O)/%.o,$(wildcard src/plat/win/*.c)))
endif
NET_OBJ  := $(O)/net/http.o
CODEC_OBJ := $(patsubst src/%.c,$(O)/%.o,$(wildcard src/img/*.c))
IMG_OBJ  := $(O)/ai/ai_images.o $(CODEC_OBJ)
T_test_text          := $(CORE_OBJ) $(patsubst src/%.c,$(O)/%.o,$(wildcard src/text/*.c))
T_test_tui           := $(CORE_OBJ) $(patsubst src/%.c,$(O)/%.o,$(wildcard src/tui/*.c)) $(O)/text/walker.o $(O)/text/glob.o $(CODEC_OBJ)
GOOGLE_OBJ := $(O)/ai/provider_google.o $(O)/ai/google_schema.o $(O)/ai/google_auth.o
T_test_providers_ag  := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/leaked_thinking.o $(O)/ai/think_heal.o $(O)/ai/retry.o $(O)/ai/stream_guard.o $(O)/ai/service_tier.o $(O)/ai/provider_transport.o $(O)/ai/provider_anthropic.o $(O)/ai/anthropic_slow.o $(O)/ai/anthropic_threads.o $(GOOGLE_OBJ) $(IMG_OBJ)
T_test_providers_google := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/leaked_thinking.o $(O)/ai/think_heal.o $(O)/ai/retry.o $(O)/ai/stream_guard.o $(O)/ai/service_tier.o $(O)/ai/provider_transport.o $(GOOGLE_OBJ) $(O)/ai/google_cca.o \
                        $(O)/ai/provider_gemini_cli.o $(IMG_OBJ)
T_test_providers_oai := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/leaked_thinking.o $(O)/ai/think_heal.o $(O)/ai/retry.o $(O)/ai/stream_guard.o $(O)/ai/service_tier.o $(O)/ai/provider_transport.o $(O)/ai/provider_openai_completions.o $(O)/ai/toolschema.o \
                        $(O)/ai/provider_openai_responses.o $(O)/ai/codex_version.o $(O)/ai/responses_ws.o $(O)/net/ws.o $(O)/ai/provider_ollama.o $(O)/ai/compat_detect.o
T_test_http          := $(CORE_OBJ) $(NET_OBJ)
T_test_ws            := $(CORE_OBJ) $(NET_OBJ) $(O)/net/ws.o
T_test_providers_bedrock := $(CORE_OBJ) $(NET_OBJ) $(O)/ai/ai_common.o $(O)/ai/leaked_thinking.o $(O)/ai/think_heal.o $(O)/ai/retry.o $(O)/ai/stream_guard.o $(O)/ai/service_tier.o $(O)/ai/provider_transport.o $(O)/ai/aws.o $(O)/ai/provider_bedrock.o
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
T_test_toolcards     := $(T_test_tui) $(patsubst src/%.c,$(O)/%.o,$(wildcard src/ui/tools/*.c)) $(O)/tools/outsink.o $(O)/tools/jobtext.o $(O)/tools/readsel.o $(O)/text/htmlform.o $(O)/text/regex.o $(O)/text/rxbt.o $(O)/tools/todotargets.o
T_test_outsink       := $(CORE_OBJ) $(O)/tools/outsink.o $(O)/tools/jobtext.o
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
T_test_toolcmd       := $(LIB_OBJ)
T_test_magic         := $(LIB_OBJ)
T_test_commands      := $(LIB_OBJ)
T_test_compaction    := $(LIB_OBJ)
T_test_tiers         := $(LIB_OBJ)
T_test_workspace     := $(LIB_OBJ)
T_test_agenthub      := $(LIB_OBJ)
T_test_mcp           := $(LIB_OBJ)
T_test_loop          := $(LIB_OBJ)
T_test_usage         := $(LIB_OBJ)
T_test_kiro          := $(LIB_OBJ)
T_test_extracmds     := $(LIB_OBJ)
T_test_display       := $(LIB_OBJ)
T_test_turn_recovery := $(LIB_OBJ)
T_test_dcr           := $(LIB_OBJ)
T_test_settings_audit := $(CORE_OBJ)
T_test_update        := $(CORE_OBJ) $(NET_OBJ) $(O)/core/tarx.o $(O)/coding/selfupdate.o
T_test_htmlform      := $(LIB_OBJ)
T_test_fuzz_regress  := $(LIB_OBJ) $(patsubst tests/fuzz/%.c,$(O)/tests/fuzz/%.o,$(FUZZ_SRC))
UNIT_TESTS := test_text test_tui test_providers_ag test_providers_oai test_providers_bedrock test_readfmt test_editmodes \
              test_chrome test_providers_google test_toolcards test_cursor test_judgment test_auth test_memory test_outsink test_catalog test_model_resolver test_compaction_point \
              test_planreview test_img test_img_jpeg test_img_webp test_btw test_queue test_guards test_keybindings test_slash test_commands test_export test_dump test_signals \
              test_panels test_tree test_approval test_compaction test_tiers test_workspace test_agenthub test_mcp test_loop test_usage test_extracmds test_display test_turn_recovery test_dcr test_settings_audit test_htmlform test_update test_kiro test_toolcmd test_magic test_fuzz_regress

.SECONDEXPANSION:
$(O)/tests/%: tests/%.c $$(T_$$*)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $< $(T_$*) $(LIBS) $(TEST_LDFLAGS)

ifeq ($(UNAME_S),Windows)
$(O)/tests/%.exe: tests/%.c $$(T_$$*)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $< $(T_$*) $(LIBS) $(TEST_LDFLAGS)

build/windows/conpty_drive.exe: tests/windows/conpty_drive.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -static -o $@ $<

build/windows/fake_omc.exe: tests/windows/fake_omc.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -static -o $@ $<

# linked as docs/windows.md tells users to: -lomc picks the import library (libomc.dll.a) over libomc.a
build/windows/embed_dll.exe: examples/embed.c $(LIB_SHARED)
	$(CC) -O2 -Wall -Wextra -Iinclude -DOMC_DLL -o $@ $< -Lbuild/windows -lomc

# everything tests/windows/run-tests.ps1 runs: `make WINDOWS=1 test-build`
test-build: $(BIN) $(addprefix $(O)/tests/,$(addsuffix .exe,$(UNIT_TESTS))) build/windows/conpty_drive.exe build/windows/fake_omc.exe \
            build/examples/embed.exe lib build/windows/embed_dll.exe
endif

build/examples/main_twice$(EXE): tests/e2e/main_twice.c $(LIB_STATIC)
	@mkdir -p build/examples
	$(CC) -O2 -Wall -Wextra -Iinclude -o $@ $< $(LIB_STATIC) $(LIBS)

# Never the real ~/.omc or ~/.omp: each run gets a fresh HOME and agent dir under the build dir, and no launch looks for
# a new omc release (OMC_NO_UPDATE_CHECK; tests/e2e/update.sh checks the update paths against a local mock).
# TEST_TMP: the tests' TMPDIR is a fresh directory holding a `.git` entry, so project discovery (skills, commands,
# context files, which walk ancestors up to the repo root, else to /) stops there instead of reaching the real
# home's .claude/.agents dirs when TMPDIR lies under it; removed after a passing run
TEST_TMP = tmp=$$(mktemp -d "$${TMPDIR:-/tmp}"/omc-test.XXXXXX); tmp=$$(cd "$$tmp" && pwd -P); mkdir "$$tmp/.git"; \
	export TMPDIR="$$tmp";
test: $(addprefix $(O)/tests/,$(UNIT_TESTS)) build/examples/embed build/examples/main_twice $(BIN)
	@set -e; home=$$(mktemp -d "$(CURDIR)/$(O)/test-home.XXXXXX"); $(TEST_TMP) \
	export HOME="$$home" OMC_CODING_AGENT_DIR="$$home/.omc/agent" PI_CODING_AGENT_DIR="$$home/.omp/agent" OMC_PLATFORM=linux OMC_NO_UPDATE_CHECK=1; \
	for t in $(UNIT_TESTS); do echo "== $$t"; ./$(O)/tests/$$t >/dev/null || { echo "   FAIL ($$t)"; exit 1; }; echo "   ok"; done; \
	OMC_BIN=$(CURDIR)/$(O)/omc ./tests/e2e/run.sh; \
	rm -rf "$$tmp"

# unit tests only (CI on platforms where the e2e suite's pty and process assumptions are not yet verified)
test-unit: $(addprefix $(O)/tests/,$(UNIT_TESTS))
	@set -e; home=$$(mktemp -d "$(CURDIR)/$(O)/test-home.XXXXXX"); $(TEST_TMP) \
	export HOME="$$home" OMC_CODING_AGENT_DIR="$$home/.omc/agent" PI_CODING_AGENT_DIR="$$home/.omp/agent" OMC_PLATFORM=linux OMC_NO_UPDATE_CHECK=1; \
	for t in $(UNIT_TESTS); do echo "== $$t"; ./$(O)/tests/$$t >/dev/null || { echo "   FAIL ($$t)"; exit 1; }; echo "   ok"; done; \
	rm -rf "$$tmp"

# ---- libFuzzer harnesses (opt-in, clang only): `make fuzz` -> build/fuzz/fuzz_<name>; tests/fuzz/run.sh runs them ----
# A separate object tree built with clang + libFuzzer/ASan/UBSan; the normal build and `make test` never touch it.
FUZZ_CC  ?= clang-21
FUZZ_BIN := $(patsubst tests/fuzz/%.c,build/fuzz/%,$(FUZZ_SRC))
fuzz:
	@$(MAKE) --no-print-directory O=build/fuzz/obj CC=$(FUZZ_CC) \
	    OPT="-O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined" fuzz-bins
fuzz-bins: $(FUZZ_BIN)
build/fuzz/%: tests/fuzz/%.c tests/fuzz/fuzz.h $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fsanitize=fuzzer -o $@ $< $(LIB_OBJ) $(LIBS)

# the fuzz harnesses again, built normally (gcc, no libFuzzer): test_fuzz_regress replays tests/fuzz/regress/<name>/*
$(O)/tests/fuzz/%.o: tests/fuzz/%.c tests/fuzz/fuzz.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DFUZZ_REGRESS -DFUZZ_NAME=$(patsubst fuzz_%,%,$*) -c -o $@ $<

# release packaging. Each release carries a prebuilt binary per platform and the source:
#   make dist      build/dist/omc-<version>-<os>-<arch>.tar.gz (+ .sha256): the binary, README and omp's license notice
#   make dist-src  build/dist/omc-<version>.tar.gz (+ .sha256): `git archive` of HEAD (committed files, under omc-<version>/)
VERSION   := $(shell sed -n 's/^\#define OMC_VERSION "\(.*\)"/\1/p' src/core/core.h)
DIST_OS   := $(if $(filter Darwin,$(UNAME_S)),macos,$(if $(filter Windows,$(UNAME_S)),windows,linux))
DIST_ARCH := $(if $(filter aarch64 arm64,$(UNAME_M)),arm64,$(UNAME_M))
DIST_NAME := omc-$(VERSION)-$(DIST_OS)-$(DIST_ARCH)
SRC_NAME  := omc-$(VERSION)
SHA256    := { command -v sha256sum >/dev/null && sha256sum "$$f" || shasum -a 256 "$$f"; }

version:
	@echo $(VERSION)

ifeq ($(UNAME_S),Windows)
# Windows: a zip, as install.ps1 downloads it: omc.exe, README, omp's license notice, lib\ (libomc.dll, its import
# library libomc.dll.a, libomc.a, libomc.def for MSVC's lib.exe, a relocatable omc.pc) and include\omc.h
dist: $(BIN) lib
	@rm -rf build/dist/$(DIST_NAME) && mkdir -p build/dist/$(DIST_NAME)/lib build/dist/$(DIST_NAME)/include
	cp $(BIN) README.md build/dist/$(DIST_NAME)/
	cp prompts/UPSTREAM-LICENSE build/dist/$(DIST_NAME)/OMP-LICENSE
	cp $(LIB_SHARED) build/windows/libomc.dll.a $(LIB_STATIC) build/windows/libomc.def $(PC_FILE) build/dist/$(DIST_NAME)/lib/
	cp include/omc.h build/dist/$(DIST_NAME)/include/
	rm -f build/dist/$(DIST_NAME).zip && $(PYTHON) -I build-tools/zipdir.py build/dist/$(DIST_NAME).zip build/dist $(DIST_NAME)
	cd build/dist && f=$(DIST_NAME).zip && $(SHA256) | sed 's/ [*]/  /' > $$f.sha256  # MSYS's sha256sum marks binary mode
	@cat build/dist/$(DIST_NAME).zip.sha256
else
dist: $(BIN)
	@rm -rf build/dist/$(DIST_NAME) && mkdir -p build/dist/$(DIST_NAME)
	cp $(BIN) README.md build/dist/$(DIST_NAME)/
	cp prompts/UPSTREAM-LICENSE build/dist/$(DIST_NAME)/OMP-LICENSE
	tar -C build/dist -czf build/dist/$(DIST_NAME).tar.gz $(DIST_NAME)
	cd build/dist && f=$(DIST_NAME).tar.gz && $(SHA256) > $$f.sha256
	@cat build/dist/$(DIST_NAME).tar.gz.sha256
endif

dist-src: FORCE
	@git rev-parse --verify -q HEAD >/dev/null || { echo "make dist-src: needs a git checkout" >&2; exit 1; }
	@git diff --quiet HEAD -- || echo "make dist-src: note: uncommitted changes are not in the archive" >&2
	@mkdir -p build/dist
	git archive --format=tar --prefix=$(SRC_NAME)/ HEAD | gzip -9n > build/dist/$(SRC_NAME).tar.gz
	cd build/dist && f=$(SRC_NAME).tar.gz && $(SHA256) > $$f.sha256
	@cat build/dist/$(SRC_NAME).tar.gz.sha256

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m755 $(BIN) $(DESTDIR)$(PREFIX)/bin/omc

# per-user install like omp's (~/.local/bin/omc); `make install PREFIX=/usr/local` (with sudo) for a system-wide one
PREFIX ?= $(HOME)/.local

clean:
	rm -rf build

.PHONY: all lib install-lib test test-unit fuzz fuzz-bins version dist dist-src clean install FORCE windows test-build
-include $(OBJ:.o=.d)
