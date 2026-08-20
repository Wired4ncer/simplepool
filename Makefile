# simplepool Makefile
# Pure C11. Deps: sqlite3, libcurl, pthread. cJSON is vendored under src/cjson/.

CC      ?= cc
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin

UNAME_S := $(shell uname -s)

# --- Platform-specific include / lib paths -----------------------------------
# macOS: prefer Homebrew's prefix; fall back to /opt/homebrew (Apple Silicon)
# and /usr/local (Intel). Linux: rely on system paths.
ifeq ($(UNAME_S),Darwin)
    BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
    ifeq ($(BREW_PREFIX),)
        ifneq ($(wildcard /opt/homebrew),)
            BREW_PREFIX := /opt/homebrew
        else
            BREW_PREFIX := /usr/local
        endif
    endif
    PLATFORM_CFLAGS  := -I$(BREW_PREFIX)/include \
                        -I$(BREW_PREFIX)/opt/sqlite/include \
                        -I$(BREW_PREFIX)/opt/curl/include \
                        -I$(BREW_PREFIX)/opt/hiredis/include
    PLATFORM_LDFLAGS := -L$(BREW_PREFIX)/lib \
                        -L$(BREW_PREFIX)/opt/sqlite/lib \
                        -L$(BREW_PREFIX)/opt/curl/lib \
                        -L$(BREW_PREFIX)/opt/hiredis/lib
else
    PLATFORM_CFLAGS  :=
    PLATFORM_LDFLAGS :=
endif

# --- Flags -------------------------------------------------------------------
WARNFLAGS := -Wall -Wextra -Werror -Wpedantic -Wshadow -Wstrict-prototypes
HARDEN    := -fstack-protector-strong -D_FORTIFY_SOURCE=2
# Strict -std=c11 hides POSIX functions (clock_gettime, localtime_r, ...) behind
# feature-test macros; request POSIX.1-2008 so they're declared on glibc.
POSIX     := -D_POSIX_C_SOURCE=200809L

CFLAGS  ?= -std=c11 $(WARNFLAGS) -O2 -g $(HARDEN) $(POSIX) \
           -Iinclude -Isrc -Isrc/cjson $(PLATFORM_CFLAGS)
LDFLAGS ?= $(PLATFORM_LDFLAGS)
LDLIBS  ?= -lsqlite3 -lcurl -lhiredis -lpthread

BUILD_DIR := build
BIN       := $(BUILD_DIR)/simplepool

# --- Build provenance --------------------------------------------------------
# Baked into the binary so `simplepool --version` states which commit is
# actually running. Reading the checkout at runtime instead would answer a
# different question: a tree gets patched or moves on past the last `make`,
# and from then on its HEAD is not what the running process was built from.
# Empty outside a git checkout (release tarball) — reported as "unknown".
VERSION    := 0.1.0
GIT_COMMIT := $(shell git rev-parse HEAD 2>/dev/null)
GIT_BRANCH := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null)
GIT_DIRTY  := $(shell git status --porcelain --untracked-files=no 2>/dev/null | head -1)
VERSION_H  := $(BUILD_DIR)/version_gen.h

# Sources compiled in this wave. More modules land in later waves.
SRCS := src/main.c src/log.c src/config.c src/coinbase.c \
        src/share.c src/sha256.c src/stratum.c src/store.c \
        src/pplns.c \
        src/bitcoind.c src/broadcast.c src/thunder.c src/version.c \
        src/cjson/cJSON.c
OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean test format install help FORCE

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# version.o is the one object whose correctness depends on git state rather
# than on any file it includes, so no ordinary prerequisite can invalidate it:
# commit a change and it would keep reporting the previous hash.
#
# The git state therefore goes through a generated header, rewritten only when
# its content actually differs. Recompiling version.o unconditionally would
# work too, but it would relink the binary on every single `make` — so `make`
# would never be a no-op, every invocation would produce a new mtime, and any
# deploy that restarts on "the binary changed" would restart forever. Writing
# to a temp file and moving it only on a real difference keeps `make` idempotent
# while still catching a new commit.
$(VERSION_H): FORCE
	@mkdir -p $(dir $@)
	@printf '#define SIMPLEPOOL_VERSION "%s"\n#define SIMPLEPOOL_GIT_COMMIT "%s"\n#define SIMPLEPOOL_GIT_BRANCH "%s"\n#define SIMPLEPOOL_GIT_DIRTY %s\n' \
		'$(VERSION)' '$(GIT_COMMIT)' '$(GIT_BRANCH)' '$(if $(GIT_DIRTY),1,0)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(BUILD_DIR)/src/version.o: src/version.c $(VERSION_H)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -include $(VERSION_H) -MMD -MP -c $< -o $@

FORCE:

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR)

include tests/test_share.mk
include tests/test_bitcoind.mk
include tests/test_stratum.mk
include tests/test_store.mk
include tests/test_coinbase.mk
include tests/test_broadcast.mk
include tests/test_thunder.mk
include tests/test_pplns.mk

test: build/test_share build/test_bitcoind build/test_stratum build/test_store build/test_coinbase build/test_broadcast build/test_thunder build/test_pplns
	./build/test_share
	./build/test_bitcoind
	./build/test_stratum
	./build/test_store
	./build/test_coinbase
	./build/test_broadcast
	./build/test_thunder
	./build/test_pplns

format:
	@if command -v clang-format >/dev/null 2>&1; then \
		find src include tests -type f \( -name '*.c' -o -name '*.h' \) \
			-not -path 'src/cjson/*' \
			-print0 | xargs -0 clang-format -i ; \
		echo "formatted"; \
	else \
		echo "clang-format not installed; skipping"; \
	fi

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/simplepool

help:
	@echo "Targets: all clean test format install"
	@echo "  PREFIX=$(PREFIX)  CC=$(CC)  UNAME_S=$(UNAME_S)"
