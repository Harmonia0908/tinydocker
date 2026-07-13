CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=

WARNINGS := -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 -Wno-format-nonliteral -Wshadow \
	-Wconversion -Wstrict-prototypes -Wmissing-prototypes
CORE_SOURCES := core/safety.c core/cgroup_parse.c core/status_codec.c core/fs.c \
	core/process.c
TEST_BINARY := build/test_core

.PHONY: all clean test

all: tinydocker

tinydocker:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -D_GNU_SOURCE \
		logger/*.c util/*.c cmdparser/*.c docker/*.c $(CORE_SOURCES) main.c \
		-lcrypto -o $@

$(TEST_BINARY): tests/test_core.c $(CORE_SOURCES)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -Werror \
		tests/test_core.c $(CORE_SOURCES) -o $@

test: $(TEST_BINARY)
	./$(TEST_BINARY)

clean:
	rm -rf build
	rm -f tinydocker a.out *.o
