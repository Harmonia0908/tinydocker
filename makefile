CC ?= cc
RUNTIME_DIR ?= /home/xanarry/tinydocker_runtime
CGROUP_PARENT ?= /sys/fs/cgroup/system.slice

CPPFLAGS += -D_GNU_SOURCE \
	-DTINYDOCKER_RUNTIME_DIR='"$(RUNTIME_DIR)"' \
	-DTINYDOCKER_CGROUP_PARENT='"$(CGROUP_PARENT)"'
CFLAGS ?= -O2 -g
LDLIBS ?= -lcrypto

WARNINGS := -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 \
	-Wno-format-nonliteral -Wno-missing-field-initializers -Wshadow \
	-Wconversion -Wstrict-prototypes -Wmissing-prototypes
STRICT_WARNINGS := $(WARNINGS) -Werror

CORE_SOURCES := core/safety.c core/cgroup_parse.c core/status_codec.c core/fs.c \
	core/process.c
RUNTIME_SOURCES := logger/log.c util/utils.c cmdparser/cmdparser.c \
	docker/cgroup.c docker/container.c docker/network.c docker/status_info.c \
	docker/volumes.c docker/workspace.c main.c
SOURCES := $(CORE_SOURCES) $(RUNTIME_SOURCES)
OBJECTS := $(SOURCES:%.c=build/%.o)
TEST_BINARY := build/test_core
SANITIZER_BINARY := build/test_core_sanitize
UNAME_S := $(shell uname -s)

.PHONY: all clean test sanitize static-check check privileged-test

all: tinydocker

ifeq ($(UNAME_S),Linux)
tinydocker: $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) $(LDLIBS) -o $@

build/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -c $< -o $@
else
tinydocker:
	@echo "tinydocker runtime build requires Linux (current platform: $(UNAME_S))." >&2
	@echo "Portable checks remain available: make check" >&2
	@false
endif

$(TEST_BINARY): tests/test_core.c $(CORE_SOURCES)
	mkdir -p build
	$(CC) $(CFLAGS) $(STRICT_WARNINGS) tests/test_core.c $(CORE_SOURCES) -o $@

test: $(TEST_BINARY)
	./$(TEST_BINARY)

$(SANITIZER_BINARY): tests/test_core.c $(CORE_SOURCES)
	mkdir -p build
	$(CC) -O1 -g $(STRICT_WARNINGS) -fsanitize=address,undefined \
		-fno-omit-frame-pointer tests/test_core.c $(CORE_SOURCES) -o $@

sanitize: $(SANITIZER_BINARY)
	./$(SANITIZER_BINARY)

ifeq ($(UNAME_S),Linux)
static-check:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(STRICT_WARNINGS) -fsyntax-only $(SOURCES)
else
static-check:
	$(CC) $(CFLAGS) $(STRICT_WARNINGS) -fsyntax-only \
		tests/test_core.c $(CORE_SOURCES)
endif

check: test static-check sanitize

privileged-test: tinydocker
	bash tests/run_privileged.sh

clean:
	rm -rf build
	rm -f tinydocker a.out *.o
