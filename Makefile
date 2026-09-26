# glyphfx Makefile
# Pure C17 build system with colorized output

# ============== Colors & Symbols ==============
GREEN   := \033[92m
EMERALD := \033[38;2;16;185;129m
CYAN    := \033[96m
YELLOW  := \033[93m
MAGENTA := \033[95m
RED     := \033[91m
GRAY    := \033[90m
BOLD    := \033[1m
RESET   := \033[0m

CHECK    := ✓
CROSS    := ✗
ARROW    := ▸
PROGRESS := →

# ============== Project Metadata ==============
VERSION := $(shell cat VERSION)

CC      ?= cc
AR      ?= ar
PREFIX  ?= /usr/local
CFLAGS  ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
CPPFLAGS += -Isrc -Ibuild -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS ?=
LDLIBS  += -lm

BUILD       := build
SRCS        := $(shell find src -name '*.c')
LIB_SRCS    := $(filter-out src/main.c,$(SRCS))
OBJS        := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
LIB_OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRCS))
DEPS        := $(OBJS:.o=.d)
TEST_SRCS   := $(wildcard tests/test_*.c)
TEST_BINS   := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRCS))
EFFECT_CASES := $(wildcard tools/parity/cases/*.txt)

# ============== Phony Targets ==============
.PHONY: all build help banner check test parity effects release static install debug clean version \
        bump-patch bump-minor bump-major bump-dry

# ============== Default Target ==============
.DEFAULT_GOAL := build

# ============== Banner ==============
banner:
	@printf "$(EMERALD)$(BOLD)%s$(RESET)\n" ' ██████╗ ██╗  ██╗   ██╗██████╗ ██╗  ██╗███████╗██╗  ██╗'
	@printf "$(EMERALD)$(BOLD)%s$(RESET)\n" '██╔════╝ ██║  ╚██╗ ██╔╝██╔══██╗██║  ██║██╔════╝╚██╗██╔╝'
	@printf "$(EMERALD)$(BOLD)%s$(RESET)\n" '██║  ███╗██║   ╚████╔╝ ██████╔╝███████║█████╗   ╚███╔╝ '
	@printf "$(EMERALD)$(BOLD)%s$(RESET)\n" '██║   ██║██║    ╚██╔╝  ██╔═══╝ ██╔══██║██╔══╝   ██╔██╗ '
	@printf "$(EMERALD)$(BOLD)%s$(RESET)\n" '╚██████╔╝███████╗██║   ██║     ██║  ██║██║     ██╔╝ ██╗'
	@printf "$(EMERALD)$(BOLD)%s$(RESET)\n" ' ╚═════╝ ╚══════╝╚═╝   ╚═╝     ╚═╝  ╚═╝╚═╝     ╚═╝  ╚═╝'
	@printf "  $(GRAY)v$(VERSION)$(RESET) $(EMERALD)terminal text effects, in C17$(RESET)\n\n"

# ============== Section Header ==============
define section
	@printf "\n$(CYAN)$(BOLD)╔══════════════════════════════════════════════════╗$(RESET)\n"
	@printf "$(CYAN)$(BOLD)║ %-48s ║$(RESET)\n" "$(1)"
	@printf "$(CYAN)$(BOLD)╚══════════════════════════════════════════════════╝$(RESET)\n\n"
endef

# ============== Help ==============
help: banner
	@printf "$(BOLD)Usage:$(RESET) make $(YELLOW)<target>$(RESET)\n\n"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "build" "compile $(BUILD)/glyphfx (default)"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "check" "run the C unit tests"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "parity" "byte-exact M0 option matrix vs the ttfx oracle"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "effects" "byte-exact frame parity for every effect"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "debug" "ASan/UBSan build"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "release" "stripped release build"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "static" "statically linked build"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "install" "install to PREFIX ($(PREFIX))"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "version" "print the version"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "bump-{patch,minor,major}" "cut a release with commit-and-tag-version"
	@printf "  $(CYAN)%-24s$(RESET) %s\n" "clean" "remove $(BUILD)/"
	@printf "\n$(GRAY)  CC=$(CC)  CFLAGS=$(CFLAGS)$(RESET)\n"

# ============== Build ==============
all: build

build: banner
	$(call section,Building glyphfx)
	@$(MAKE) --no-print-directory $(BUILD)/glyphfx

$(BUILD)/glyphfx_version.h: VERSION
	@mkdir -p $(BUILD)
	@printf '#ifndef GLYPHFX_VERSION_H\n#define GLYPHFX_VERSION_H\n#define GLYPHFX_VERSION "%s"\n#endif\n' "$(VERSION)" > $@

$(BUILD)/%.o: src/%.c $(BUILD)/glyphfx_version.h
	@mkdir -p $(dir $@)
	@printf "$(GRAY)  $(PROGRESS) %s$(RESET)\n" "$<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/libglyphfx.a: $(LIB_OBJS)
	@mkdir -p $(dir $@)
	@$(AR) rcs $@ $(LIB_OBJS)

$(BUILD)/glyphfx: $(OBJS) $(BUILD)/libglyphfx.a
	@printf "$(GRAY)  $(PROGRESS) linking %s$(RESET)\n" "$@"
	@$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
	@printf "$(GREEN)$(CHECK) Built $(BOLD)$(BUILD)/glyphfx$(RESET)\n"

# ============== Tests ==============
test: check

check: banner $(TEST_BINS)
	$(call section,Unit Tests)
	@fail=0; for t in $(TEST_BINS); do \
		printf "$(GRAY)  $(PROGRESS) %s$(RESET)\n" "$$t"; \
		$$t || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then printf "$(GREEN)$(CHECK) all unit tests passed$(RESET)\n"; \
	else printf "$(RED)$(CROSS) unit tests failed$(RESET)\n" && exit 1; fi

$(BUILD)/test_%: tests/test_%.c $(BUILD)/libglyphfx.a
	@mkdir -p $(dir $@)
	@$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(BUILD)/libglyphfx.a $(LDFLAGS) $(LDLIBS)

# ============== Parity ==============
parity: banner $(BUILD)/glyphfx
	$(call section,M0 Parity Matrix)
	@tools/parity/run_m0.sh && printf "$(GREEN)$(CHECK) M0 parity passed$(RESET)\n" \
		|| (printf "$(RED)$(CROSS) M0 parity failed$(RESET)\n" && exit 1)

effects: banner $(BUILD)/glyphfx
	$(call section,Effect Parity)
	@fail=0; for f in $(EFFECT_CASES); do \
		e=$$(basename $$f .txt); \
		tools/parity/run_effects.sh $$e || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then printf "$(GREEN)$(CHECK) all effects byte-exact$(RESET)\n"; \
	else printf "$(RED)$(CROSS) effect parity failed$(RESET)\n" && exit 1; fi

# ============== Release Builds ==============
release: CFLAGS += -DNDEBUG
release: banner $(BUILD)/glyphfx
	$(call section,Release Build)
	@strip $(BUILD)/glyphfx 2>/dev/null || true
	@printf "$(GREEN)$(CHECK) stripped $(BOLD)$(BUILD)/glyphfx$(RESET)\n"

static: banner $(OBJS)
	$(call section,Static Build)
	@$(CC) $(CPPFLAGS) $(CFLAGS) -static -o $(BUILD)/glyphfx-static $(OBJS) $(LDFLAGS) $(LDLIBS)
	@printf "$(GREEN)$(CHECK) $(BOLD)$(BUILD)/glyphfx-static$(RESET)\n"

debug: banner
	$(call section,Sanitizer Build)
	@mkdir -p $(BUILD)
	@$(CC) $(CPPFLAGS) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-Wall -Wextra -Wpedantic -Wconversion -Wshadow $(SRCS) -o $(BUILD)/glyphfx-debug $(LDLIBS)
	@printf "$(GREEN)$(CHECK) $(BOLD)$(BUILD)/glyphfx-debug$(RESET)\n"

install: banner $(BUILD)/glyphfx
	$(call section,Installing)
	@install -d $(DESTDIR)$(PREFIX)/bin
	@install -m 0755 $(BUILD)/glyphfx $(DESTDIR)$(PREFIX)/bin/glyphfx
	@printf "$(GREEN)$(CHECK) $(DESTDIR)$(PREFIX)/bin/glyphfx$(RESET)\n"

# ============== Version Management ==============
version:
	@printf "$(CYAN)Version:$(RESET) $(YELLOW)$(BOLD)$(VERSION)$(RESET)\n"

bump-patch: banner
	$(call section,Release $(ARROW) patch)
	@npx commit-and-tag-version --release-as patch

bump-minor: banner
	$(call section,Release $(ARROW) minor)
	@npx commit-and-tag-version --release-as minor

bump-major: banner
	$(call section,Release $(ARROW) major)
	@npx commit-and-tag-version --release-as major

bump-dry:
	@npx commit-and-tag-version --dry-run

# ============== Clean ==============
clean: banner
	$(call section,Cleaning)
	@rm -rf $(BUILD)
	@printf "$(GREEN)$(CHECK) removed $(BUILD)/$(RESET)\n"

-include $(DEPS)
