CC       ?= cc
CFLAGS   ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -g
CFLAGS   += -MMD -MP
CPPFLAGS += -Iinclude
LDFLAGS  ?=

.DEFAULT_GOAL := all

SERVER_SRCS := src/main.c src/config.c src/server.c src/util.c src/http_request.c src/http_response.c src/static_file.c src/cgi.c
SERVER_OBJS := $(SERVER_SRCS:.c=.o)
SERVER_BIN  := server

CGI_BINS := cgi-bin/echo cgi-bin/sleep cgi-bin/crash

TEST_SRCS := tests/test_main.c tests/test_http_request.c tests/test_http_response.c tests/test_util.c
TEST_OBJS := $(TEST_SRCS:.c=.o)
TEST_BIN  := run_tests

TEST_SUPPORT_SRCS := src/http_request.c src/http_response.c src/config.c src/util.c
TEST_SUPPORT_OBJS := $(TEST_SUPPORT_SRCS:.c=.o)

DEPS := $(SERVER_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(TEST_SUPPORT_OBJS:.o=.d)
-include $(DEPS)

SAN_CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -g -MMD -MP -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_LDFLAGS := -fsanitize=address,undefined

.PHONY: all cgi test integration sanitize fdtest load linux-test clean

all: $(SERVER_BIN) cgi

cgi: $(CGI_BINS)

cgi-bin/%: cgi-bin/%.c
	$(CC) $(CFLAGS) -o $@ $<

$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(CFLAGS) $(SERVER_OBJS) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_OBJS) $(TEST_SUPPORT_OBJS)
	$(CC) $(CFLAGS) $(TEST_OBJS) $(TEST_SUPPORT_OBJS) -o $@ $(LDFLAGS)

integration: $(SERVER_BIN) cgi
	sh tests/integration.sh

sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) CFLAGS="$(SAN_CFLAGS)" LDFLAGS="$(SAN_LDFLAGS)" all test integration

fdtest: $(SERVER_BIN) cgi
	sh tests/fd_leak.sh

load: $(SERVER_BIN)
	sh tests/load.sh

linux-test:
	docker run --rm -v "$(PWD)":/src -w /src gcc:13 \
		sh -c "apt-get update && apt-get install -y curl netcat-openbsd && make test && make integration"

clean:
	rm -f $(SERVER_OBJS) $(SERVER_BIN) $(CGI_BINS) $(TEST_OBJS) $(TEST_BIN) $(DEPS) src/http_request.o src/http_response.o src/static_file.o
