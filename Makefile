# wubulens -- the WuBu lens: a plain-language GPU + task manager, in C11.
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -Isrc
LDFLAGS ?= -ldl

SRC := src/perf.c src/proc.c src/gpu.c src/classify.c src/narrate.c src/snapshot.c src/session.c src/tui.c src/ops.c
BIN := wubulens

all: $(BIN)

$(BIN): $(SRC) src/main.c
	$(CC) $(CFLAGS) $(SRC) src/main.c -o $@ $(LDFLAGS)

tests/test_core: tests/test_core.c $(SRC)
	$(CC) $(CFLAGS) $(SRC) tests/test_core.c -o $@ $(LDFLAGS)

tests/test_ops: tests/test_ops.c src/ops.c
	$(CC) $(CFLAGS) tests/test_ops.c src/ops.c -o $@ $(LDFLAGS)

test: tests/test_core tests/test_ops
	./tests/test_core
	./tests/test_ops

clean:
	rm -f $(BIN) tests/test_core tests/test_ops

.PHONY: all test clean
