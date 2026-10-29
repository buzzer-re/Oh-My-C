# omc - oh-my-c build
CC      ?= cc
PKG     ?= pkg-config
OPT     ?= -O2
CFLAGS  ?= $(OPT) -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable -Wshadow=local \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -Isrc -MMD -MP
LDFLAGS += -pie -Wl,-z,relro,-z,now
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
	./build/embedgen $@ $(EMBED_FILES)

build/obj/embedded.o: build/embedded.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

TEST_SRC := $(wildcard tests/*.c)
TEST_BIN := $(patsubst tests/%.c,build/tests/%,$(TEST_SRC))
LIB_OBJ  := $(filter-out build/obj/main.o,$(OBJ))

build/tests/%: tests/%.c $(LIB_OBJ)
	@mkdir -p build/tests
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJ) $(LIBS)

test: $(TEST_BIN)
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/omc

PREFIX ?= /usr/local

clean:
	rm -rf build

.PHONY: all test clean install
-include $(OBJ:.o=.d)
