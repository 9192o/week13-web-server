CC       ?= cc
CFLAGS   ?= -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -g
CFLAGS   += -MMD -MP -pthread
CPPFLAGS += -Iinclude -Isrc/proxy
LDFLAGS  ?=
LDFLAGS  += -pthread

.DEFAULT_GOAL := all

EVENT_SRCS := src/event/event_backend.c src/event/event_poll.c src/event/event_epoll.c src/event/event_kqueue.c

EV_SERVER_SRCS := src/app/main_ev.c src/common/config.c src/common/config_file.c src/conn/conn.c src/conn/conn_signal.c src/conn/conn_net.c src/conn/conn_timer.c src/conn/conn_io.c src/conn/conn_loop.c src/app/server_ev.c src/app/server_cgi.c src/app/server_request.c src/proxy/proxy_route.c src/proxy/proxy_lifecycle.c src/proxy/proxy_connection.c src/proxy/proxy_relay.c src/proxy/proxy_pool.c src/proxy/proxy_wire.c src/http/http_upstream.c src/http/chunked.c src/common/util.c src/common/logger.c src/common/access_log.c src/http/http_request.c src/http/http_response.c src/static/static_file.c src/cgi/cgi.c src/cgi/cgi_task.c src/cgi/task_pool.c $(EVENT_SRCS)
EV_SERVER_OBJS := $(EV_SERVER_SRCS:.c=.o)
EV_SERVER_BIN  := server_ev

# Standalone proxy-only binary: same proxy core, no static/CGI/task pool.
PROXY_SERVER_SRCS := src/app/main_proxy.c src/common/config.c src/common/config_file.c src/conn/conn.c src/conn/conn_signal.c src/conn/conn_net.c src/conn/conn_timer.c src/conn/conn_io.c src/conn/conn_loop.c src/app/proxy_server.c src/proxy/proxy_route.c src/proxy/proxy_lifecycle.c src/proxy/proxy_connection.c src/proxy/proxy_relay.c src/proxy/proxy_pool.c src/proxy/proxy_wire.c src/http/http_upstream.c src/http/chunked.c src/common/util.c src/common/logger.c src/common/access_log.c src/http/http_request.c src/http/http_response.c $(EVENT_SRCS)
PROXY_SERVER_OBJS := $(PROXY_SERVER_SRCS:.c=.o)
PROXY_SERVER_BIN  := reverse-proxy

# Backend-only binary: static/CGI, no proxy code is linked.
BACKEND_SERVER_SRCS := src/app/main_backend.c src/common/config.c src/common/config_file.c src/conn/conn.c src/conn/conn_signal.c src/conn/conn_net.c src/conn/conn_timer.c src/conn/conn_io.c src/conn/conn_loop.c src/app/server_ev.c src/app/server_cgi.c src/app/server_request.c src/common/util.c src/common/logger.c src/common/access_log.c src/http/http_request.c src/http/http_response.c src/static/static_file.c src/cgi/cgi.c src/cgi/cgi_task.c src/cgi/task_pool.c $(EVENT_SRCS)
BACKEND_SERVER_OBJS := $(BACKEND_SERVER_SRCS:%.c=build/backend/%.o)
BACKEND_SERVER_BIN  := backend-server

# Phase 2 merged: the final server is the event-loop/task-pool server.
SERVER_SRCS := $(EV_SERVER_SRCS)
SERVER_OBJS := $(EV_SERVER_OBJS)
SERVER_BIN  := server

CGI_BINS := cgi-bin/echo cgi-bin/sleep cgi-bin/crash cgi-bin/term cgi-bin/partial cgi-bin/created cgi-bin/dupct cgi-bin/status204 cgi-bin/badstatus cgi-bin/nocontent cgi-bin/notmodified cgi-bin/reset
BACKEND_BIN := tests/backend

TEST_SRCS := tests/test_main.c tests/test_http_request.c tests/test_http_response.c tests/test_util.c tests/test_static_file.c tests/test_event_backend.c tests/test_task_pool.c tests/test_logger.c tests/test_chunked.c tests/test_http_upstream.c tests/test_config_file.c tests/test_proxy_route.c tests/test_proxy_pool.c
TEST_OBJS := $(TEST_SRCS:.c=.o)
TEST_BIN  := run_tests

TEST_SUPPORT_SRCS := src/http/http_request.c src/http/http_response.c src/http/http_upstream.c src/http/chunked.c src/common/config.c src/common/config_file.c src/common/util.c src/common/logger.c src/common/access_log.c src/static/static_file.c src/cgi/task_pool.c src/proxy/proxy_route.c src/proxy/proxy_pool.c $(EVENT_SRCS)
TEST_SUPPORT_OBJS := $(TEST_SUPPORT_SRCS:.c=.o)

DEPS := $(SERVER_OBJS:.o=.d) $(EV_SERVER_OBJS:.o=.d) $(PROXY_SERVER_OBJS:.o=.d) $(BACKEND_SERVER_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(TEST_SUPPORT_OBJS:.o=.d)
-include $(DEPS)

SAN_CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -g -MMD -MP -pthread -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_LDFLAGS := -fsanitize=address,undefined -pthread

.PHONY: all cgi backend test integration integration-s1 integration-s2 integration-p3 proxy-load proxy-bin-test proxy-front-test demo-proxy shutdown-test sanitize fdtest load linux-test clean

all: $(SERVER_BIN) $(EV_SERVER_BIN) $(PROXY_SERVER_BIN) $(BACKEND_SERVER_BIN) cgi

backend: $(BACKEND_BIN)

$(BACKEND_BIN): tests/backend.c
	$(CC) $(CFLAGS) -o $@ $<

cgi: $(CGI_BINS)

cgi-bin/%: cgi-bin/%.c
	$(CC) $(CFLAGS) -o $@ $<

$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(CFLAGS) $(SERVER_OBJS) -o $@ $(LDFLAGS)

$(EV_SERVER_BIN): $(EV_SERVER_OBJS)
	$(CC) $(CFLAGS) $(EV_SERVER_OBJS) -o $@ $(LDFLAGS)

$(PROXY_SERVER_BIN): $(PROXY_SERVER_OBJS)
	$(CC) $(CFLAGS) $(PROXY_SERVER_OBJS) -o $@ $(LDFLAGS)

$(BACKEND_SERVER_BIN): $(BACKEND_SERVER_OBJS)
	$(CC) $(CFLAGS) $(BACKEND_SERVER_OBJS) -o $@ $(LDFLAGS)

build/backend/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DBACKEND_ONLY -c $< -o $@

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

integration-s1: $(EV_SERVER_BIN) cgi
	sh tests/integration_s1.sh

integration-s2: $(EV_SERVER_BIN) cgi
	sh tests/integration_s2.sh

integration-p3: $(SERVER_BIN) backend
	sh tests/integration_p3.sh

proxy-load: $(SERVER_BIN) backend
	sh tests/load_proxy.sh

proxy-bin-test: $(PROXY_SERVER_BIN) backend
	sh tests/integration_proxy_bin.sh

proxy-front-test: $(BACKEND_SERVER_BIN) $(PROXY_SERVER_BIN) $(BACKEND_BIN) cgi
	sh tests/integration_proxy_front.sh

demo-proxy: $(BACKEND_SERVER_BIN) $(PROXY_SERVER_BIN) cgi
	sh examples/proxy-first/run.sh

shutdown-test: $(SERVER_BIN) cgi
	sh tests/shutdown.sh

sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) CFLAGS="$(SAN_CFLAGS)" LDFLAGS="$(SAN_LDFLAGS)" all test integration integration-s1 integration-s2 integration-p3 shutdown-test

fdtest: $(SERVER_BIN) cgi
	sh tests/fd_leak.sh

load: $(SERVER_BIN)
	sh tests/load.sh

linux-test:
	docker run --rm -v "$(PWD)":/src -w /src gcc:13 \
		sh -c "apt-get update && apt-get install -y curl netcat-openbsd apache2-utils && make test && make integration && make integration-s1 && make integration-s2 && make integration-p3 && make proxy-load && make proxy-front-test && make shutdown-test"

clean:
	rm -rf build/backend $(BACKEND_SERVER_BIN)
	rm -f $(SERVER_OBJS) $(EV_SERVER_OBJS) $(PROXY_SERVER_OBJS) $(SERVER_BIN) $(EV_SERVER_BIN) $(PROXY_SERVER_BIN) $(CGI_BINS) $(BACKEND_BIN) $(TEST_OBJS) $(TEST_BIN) $(DEPS) src/http_request.o src/http_response.o src/static_file.o src/main.o src/server.o src/main.d src/server.d
