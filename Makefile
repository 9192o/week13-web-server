CC      ?= cc
CFLAGS  ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS = -Iinclude

BUILD   = build
TARGET  = server

# main.c 를 제외한 모듈은 테스트에서도 링크한다.
LIB_SRCS  = src/config.c src/server.c
MAIN_SRC  = src/main.c
TEST_SRCS = $(wildcard tests/test_*.c)

LIB_OBJS  = $(LIB_SRCS:%.c=$(BUILD)/%.o)
MAIN_OBJ  = $(MAIN_SRC:%.c=$(BUILD)/%.o)
TEST_BINS = $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%)

.PHONY: all test asan clean

all: $(TARGET)

$(TARGET): $(LIB_OBJS) $(MAIN_OBJ)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.c $(LIB_OBJS) tests/test.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_OBJS) -o $@

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

# AddressSanitizer + LeakSanitizer 빌드: make asan && ./server
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" all test

clean:
	rm -rf $(BUILD) $(TARGET)
