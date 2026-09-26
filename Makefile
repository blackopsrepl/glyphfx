VERSION := $(shell cat VERSION)
CC ?= cc
CFLAGS ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
CPPFLAGS += -Isrc -Ibuild -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS ?=
LDLIBS += -lm

BUILD := build
SRCS := $(shell find src -name '*.c')
LIB_SRCS := $(filter-out src/main.c,$(SRCS))
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
LIB_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean check parity effects release debug test

all: $(BUILD)/glyphfx

$(BUILD)/glyphfx_version.h: VERSION
	@mkdir -p $(BUILD)
	@printf '#ifndef GLYPHFX_VERSION_H\n#define GLYPHFX_VERSION_H\n#define GLYPHFX_VERSION "%s"\n#endif\n' "$(VERSION)" > $@

$(BUILD)/%.o: src/%.c $(BUILD)/glyphfx_version.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/libglyphfx.a: $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(LIB_OBJS)

$(BUILD)/glyphfx: $(OBJS) $(BUILD)/libglyphfx.a
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

TEST_SRCS := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRCS))

$(BUILD)/test_%: tests/test_%.c $(BUILD)/libglyphfx.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(BUILD)/libglyphfx.a $(LDFLAGS) $(LDLIBS)

check: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "== $$t"; $$t || exit 1; done
	@echo "all unit tests passed"

test: check

parity: $(BUILD)/glyphfx
	@tools/parity/run_m0.sh

effects: $(BUILD)/glyphfx
	@fail=0; for f in tools/parity/cases/*.txt; do \
		e=$$(basename $$f .txt); \
		tools/parity/run_effects.sh $$e || fail=1; \
	done; exit $$fail

release: CFLAGS += -DNDEBUG
release: $(BUILD)/glyphfx
	strip $(BUILD)/glyphfx 2>/dev/null || true
	@echo "release: $(BUILD)/glyphfx"

debug:
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -std=c17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-Wall -Wextra -Wpedantic -Wconversion -Wshadow $(SRCS) -o $(BUILD)/glyphfx-debug $(LDLIBS)

clean:
	rm -rf $(BUILD)

-include $(DEPS)
