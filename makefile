CC ?= cc
RUNTIME_BUILD_DIR ?= build
RUNTIME_TARGET ?= tinydocker
RUNTIME_DIR ?= /home/xanarry/tinydocker_runtime
CGROUP_PARENT ?= /sys/fs/cgroup/system.slice

CPPFLAGS += -D_GNU_SOURCE \
	-DTINYDOCKER_RUNTIME_DIR='"$(RUNTIME_DIR)"' \
	-DTINYDOCKER_CGROUP_PARENT='"$(CGROUP_PARENT)"'
CFLAGS ?= -O2 -g
LDLIBS ?= -lcrypto

LANGUAGE_FLAGS := -std=c11
WARNING_FLAGS := -Wall -Wextra -Wpedantic -Wformat=2 \
	-Wno-format-nonliteral -Wno-missing-field-initializers -Wshadow \
	-Wconversion -Wstrict-prototypes -Wmissing-prototypes
COMPILE_FLAGS := $(LANGUAGE_FLAGS) $(WARNING_FLAGS)
STRICT_COMPILE_FLAGS := $(COMPILE_FLAGS) -Werror
DEPENDENCY_FLAGS := -MMD -MP

DEBUG_CFLAGS ?= -O0 -g3
RELEASE_CFLAGS ?= -O2 -DNDEBUG
DEBUG_BUILD_DIR := build/debug
RELEASE_BUILD_DIR := build/release
DEBUG_TARGET := $(DEBUG_BUILD_DIR)/tinydocker
RELEASE_TARGET := $(RELEASE_BUILD_DIR)/tinydocker

CORE_SOURCES := core/safety.c core/cgroup_parse.c core/status_codec.c \
	core/network_state.c core/fs.c core/process.c
SUPPORT_SOURCES := logger/log.c util/utils.c
COMMAND_SOURCES := cmdparser/cmdparser.c
DOCKER_SOURCES := docker/cgroup.c docker/container.c docker/network.c \
	docker/status_info.c docker/volumes.c docker/workspace.c
ENTRYPOINT_SOURCES := main.c
RUNTIME_SOURCES := $(SUPPORT_SOURCES) $(COMMAND_SOURCES) \
	$(DOCKER_SOURCES) $(ENTRYPOINT_SOURCES)
SOURCES := $(CORE_SOURCES) $(RUNTIME_SOURCES)
CORE_HEADERS := $(wildcard core/*.h)
SUPPORT_HEADERS := $(wildcard logger/*.h util/*.h)
COMMAND_HEADERS := $(wildcard cmdparser/*.h)
DOCKER_HEADERS := $(wildcard docker/*.h)
RUNTIME_HEADERS := $(wildcard runtime/*.h)
HEADERS := $(CORE_HEADERS) $(SUPPORT_HEADERS) $(COMMAND_HEADERS) \
	$(DOCKER_HEADERS) $(RUNTIME_HEADERS)
OBJECTS := $(patsubst %.c,$(RUNTIME_BUILD_DIR)/%.o,$(SOURCES))
DEPENDENCIES := $(OBJECTS:.o=.d)
TEST_BINARY := build/test_core
PARSER_TEST_BINARY := build/test_cmdparser
RUNTIME_STATE_TEST_BINARY := build/test_runtime_state
SANITIZER_BINARY := build/test_core_sanitize
override TEST_RUNTIME_DIR := runtime
override TEST_CGROUP_PARENT := cgroup
OPENSSL_CFLAGS ?= $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LDFLAGS ?= $(shell pkg-config --libs-only-L openssl 2>/dev/null)
RUNTIME_STATE_SOURCES := $(CORE_SOURCES) $(SUPPORT_SOURCES) \
	docker/cgroup.c docker/network.c docker/status_info.c
UNAME_S := $(shell uname -s)
CLANG_FORMAT ?= clang-format
FILES ?=

.PHONY: all debug release help clean test sanitize static-check check \
	privileged-test format format-check format-config-check

all: $(RUNTIME_TARGET)

debug:
	$(MAKE) RUNTIME_BUILD_DIR="$(DEBUG_BUILD_DIR)" \
		RUNTIME_TARGET="$(DEBUG_TARGET)" \
		CFLAGS="$(DEBUG_CFLAGS)" all

release:
	$(MAKE) RUNTIME_BUILD_DIR="$(RELEASE_BUILD_DIR)" \
		RUNTIME_TARGET="$(RELEASE_TARGET)" \
		CFLAGS="$(RELEASE_CFLAGS)" all

ifeq ($(UNAME_S),Linux)
$(RUNTIME_TARGET): $(OBJECTS)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJECTS) $(LDLIBS) -o $@

$(RUNTIME_BUILD_DIR)/%.o: %.c makefile
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMPILE_FLAGS) $(DEPENDENCY_FLAGS) \
		-c $< -o $@
else
$(RUNTIME_TARGET):
	@echo "tinydocker runtime build requires Linux (current platform: $(UNAME_S))." >&2
	@echo "Portable checks remain available: make check" >&2
	@false
endif

$(TEST_BINARY): tests/test_core.c $(CORE_SOURCES) $(CORE_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(STRICT_COMPILE_FLAGS) tests/test_core.c \
		$(CORE_SOURCES) -o $@

$(PARSER_TEST_BINARY): tests/test_cmdparser.c cmdparser/cmdparser.c \
		core/safety.c $(COMMAND_HEADERS) core/safety.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(STRICT_COMPILE_FLAGS) \
		tests/test_cmdparser.c cmdparser/cmdparser.c core/safety.c -o $@

$(RUNTIME_STATE_TEST_BINARY): tests/test_runtime_state.c \
		$(RUNTIME_STATE_SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CPPFLAGS) \
		-UTINYDOCKER_RUNTIME_DIR \
		-DTINYDOCKER_RUNTIME_DIR='"$(TEST_RUNTIME_DIR)"' \
		-UTINYDOCKER_CGROUP_PARENT \
		-DTINYDOCKER_CGROUP_PARENT='"$(TEST_CGROUP_PARENT)"' \
		$(OPENSSL_CFLAGS) $(CFLAGS) $(STRICT_COMPILE_FLAGS) \
		tests/test_runtime_state.c $(RUNTIME_STATE_SOURCES) \
		$(OPENSSL_LDFLAGS) $(LDLIBS) -o $@

ifeq ($(UNAME_S),Linux)
TEST_DEPENDENCIES := $(TEST_BINARY) $(PARSER_TEST_BINARY) \
	$(RUNTIME_STATE_TEST_BINARY) tinydocker
else
TEST_DEPENDENCIES := $(TEST_BINARY) $(RUNTIME_STATE_TEST_BINARY)
endif

test: $(TEST_DEPENDENCIES)
	./$(TEST_BINARY)
	./$(RUNTIME_STATE_TEST_BINARY)
ifeq ($(UNAME_S),Linux)
	./$(PARSER_TEST_BINARY)
	bash tests/test_cli_blackbox.sh
endif

$(SANITIZER_BINARY): tests/test_core.c $(CORE_SOURCES)
	mkdir -p build
	$(CC) -O1 -g $(STRICT_COMPILE_FLAGS) -fsanitize=address,undefined \
		-fno-omit-frame-pointer tests/test_core.c $(CORE_SOURCES) -o $@

sanitize: $(SANITIZER_BINARY)
	./$(SANITIZER_BINARY)

ifeq ($(UNAME_S),Linux)
static-check:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(STRICT_COMPILE_FLAGS) \
		-fsyntax-only $(SOURCES)
else
static-check:
	$(CC) $(CFLAGS) $(STRICT_COMPILE_FLAGS) -fsyntax-only \
		tests/test_core.c $(CORE_SOURCES)
endif

check: test static-check sanitize

privileged-test: tinydocker
	bash tests/run_privileged.sh

format-config-check:
	$(CLANG_FORMAT) --style=file --dump-config >/dev/null

format-check: format-config-check
	@test -n "$(strip $(FILES))" || { \
		echo "set FILES to the C sources or headers to check" >&2; \
		exit 2; \
	}
	$(CLANG_FORMAT) --style=file --dry-run --Werror $(FILES)

format: format-config-check
	@test -n "$(strip $(FILES))" || { \
		echo "set FILES to the C sources or headers to format" >&2; \
		exit 2; \
	}
	$(CLANG_FORMAT) --style=file -i $(FILES)

help:
	@printf '%s\n' 'make                 Build the default Linux runtime (tinydocker)'
	@printf '%s\n' 'make debug           Build build/debug/tinydocker with -O0 -g3'
	@printf '%s\n' 'make release         Build build/release/tinydocker with -O2 -DNDEBUG'
	@printf '%s\n' 'make test            Run non-privileged behavior tests'
	@printf '%s\n' 'make static-check    Compile checked sources with strict warnings'
	@printf '%s\n' 'make sanitize        Run portable ASan/UBSan tests'
	@printf '%s\n' 'make check           Run test, static-check, and sanitize'
	@printf '%s\n' 'make privileged-test Run explicitly opted-in Linux integration tests'
	@printf '%s\n' 'make format-check FILES="..."  Check selected C files only'

clean:
	rm -rf build
	rm -f tinydocker a.out *.o

-include $(DEPENDENCIES)
