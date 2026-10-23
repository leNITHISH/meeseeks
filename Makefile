# meeseeks: crash-safe log-structured KV store
#
#   make            release build (lib, server, tests, bench)
#   make debug      -O0 -g build in build/debug
#   make asan       AddressSanitizer + UBSan build in build/asan
#   make tsan       ThreadSanitizer build in build/tsan
#   make test       run the test suite (release build)
#   make test-asan  run the test suite under ASan/UBSan
#   make test-tsan  run the concurrent tests under ThreadSanitizer
#   make bench      build and run the benchmarks
#   make clean

CC      ?= cc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion \
           -Wstrict-prototypes -Wmissing-prototypes
CPPFLAGS:= -Iinclude -Isrc -D_POSIX_C_SOURCE=200809L
LDLIBS  := -lpthread

MODE ?= release
ifeq ($(MODE),release)
  OPT := -O2 -DNDEBUG
else ifeq ($(MODE),debug)
  OPT := -O0 -g3
else ifeq ($(MODE),asan)
  OPT := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
         -fno-sanitize-recover=all
  LDFLAGS += -fsanitize=address,undefined
else ifeq ($(MODE),tsan)
  OPT := -O1 -g -fno-omit-frame-pointer -fsanitize=thread
  LDFLAGS += -fsanitize=thread
else
  $(error unknown MODE '$(MODE)')
endif

CFLAGS ?=
CFLAGS += $(CSTD) $(WARN) $(OPT)

B := build/$(MODE)

LIB_SRC := src/crc32.c src/record.c src/index.c src/kv.c
LIB_OBJ := $(LIB_SRC:src/%.c=$(B)/obj/%.o)
LIB     := $(B)/libmeeseeks.a

TESTS   := test_kv test_model test_crash test_concurrency test_server
TEST_BIN:= $(TESTS:%=$(B)/%)
SERVER  := $(B)/kvserver
BENCH   := $(B)/bench

.PHONY: all debug asan tsan test test-asan test-tsan bench clean

all: $(LIB) $(SERVER) $(TEST_BIN)

debug:
	$(MAKE) MODE=debug all

asan:
	$(MAKE) MODE=asan all

tsan:
	$(MAKE) MODE=tsan all

$(B)/obj/%.o: src/%.c $(wildcard src/*.h) include/kv.h | $(B)/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(SERVER): src/server.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DKV_SERVER_MAIN $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(B)/test_server: tests/test_server.c src/server.c tests/test_util.h $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_server.c src/server.c $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(B)/test_%: tests/test_%.c tests/test_util.h $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BENCH): bench/bench.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(B)/obj:
	mkdir -p $@

test: $(TEST_BIN)
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done
	@echo "all tests passed ($(MODE))"

test-asan:
	$(MAKE) MODE=asan test

# TSan only matters where threads share a store.
test-tsan:
	$(MAKE) MODE=tsan build/tsan/test_concurrency build/tsan/test_server
	TSAN_OPTIONS=halt_on_error=1 build/tsan/test_concurrency
	TSAN_OPTIONS=halt_on_error=1 build/tsan/test_server

bench: $(BENCH)
	$(BENCH)

clean:
	rm -rf build
