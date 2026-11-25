.DEFAULT_GOAL := all

CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Werror
override CPPFLAGS += -Isrc/app -Isrc/http -Isrc/server -Isrc/static

COMMON_SOURCES = src/server/webserver.c src/server/epoll_loop.c src/server/client_session.c src/http/http.c src/server/connection.c src/server/shutdown.c
COMMON_HEADERS = src/server/webserver.h src/server/epoll_loop.h src/server/client_session.h src/server/session_app.h src/http/http.h src/server/connection.h src/server/shutdown.h
SOURCES = $(COMMON_SOURCES) src/app/app.c src/app/app_session.c src/app/cgi.c src/app/router.c src/static/static_file.c src/static/static_response.c
HEADERS = $(COMMON_HEADERS) src/app/app.h src/app/cgi.h src/app/router.h src/static/static_file.h src/static/static_response.h
PROXY_SOURCES = $(COMMON_SOURCES) src/proxy/main.c src/proxy/proxy_routes.c src/proxy/proxy_wire.c src/proxy/proxy_exchange.c
PROXY_HEADERS = $(COMMON_HEADERS) src/proxy/proxy_routes.h src/proxy/proxy_wire.h src/proxy/proxy_exchange.h

.PHONY: all help dev-build dev-shell dev-check test-unit test-integration test-deadlines test-shutdown test-epoll test-keepalive test-proxy test-proxy-unit test-cgi

all: webserver reverse-proxy

webserver: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(SOURCES) $(LDLIBS) -pthread -o $@

reverse-proxy: $(PROXY_SOURCES) $(PROXY_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(PROXY_SOURCES) $(LDLIBS) -o $@

test-unit:
	@set -eu; \
		http_bin=$$(mktemp); body_bin=$$(mktemp); server_bin=$$(mktemp); router_bin=$$(mktemp); static_bin=$$(mktemp); conn_bin=$$(mktemp); \
		trap 'rm -f "$$http_bin" "$$body_bin" "$$server_bin" "$$router_bin" "$$static_bin" "$$conn_bin"' EXIT HUP INT TERM; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_http.c src/http/http.c $(LDLIBS) -o "$$http_bin"; \
		$(UNIT_RUNNER) "$$http_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_body.c src/http/http.c $(LDLIBS) -o "$$body_bin"; \
		$(UNIT_RUNNER) "$$body_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_webserver.c src/app/app_session.c src/app/cgi.c src/http/http.c src/server/connection.c $(LDLIBS) -pthread -Wl,--wrap=recv -Wl,--wrap=send -o "$$server_bin"; \
		$(UNIT_RUNNER) "$$server_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_router.c src/app/app_session.c src/app/cgi.c src/app/router.c src/http/http.c src/server/connection.c src/static/static_response.c src/static/static_file.c $(LDLIBS) -pthread -Wl,--wrap=open -Wl,--wrap=fstat -o "$$router_bin"; \
		$(UNIT_RUNNER) "$$router_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_static_file.c src/static/static_file.c $(LDLIBS) -o "$$static_bin"; \
		$(UNIT_RUNNER) "$$static_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -DCONNECTION_TESTABLE -I. $(LDFLAGS) tests/test_connection.c src/server/connection.c $(LDLIBS) -o "$$conn_bin"; \
		$(UNIT_RUNNER) "$$conn_bin"

test-integration: webserver
	python3 tests/test_server.py ./webserver
	python3 tests/test_static.py ./webserver

test-deadlines:
	@set -eu; \
		exe=$$(mktemp); trap 'rm -f "$$exe"' EXIT HUP INT TERM; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -DPHASE1_HEADER_MS=250 -DPHASE1_IDLE_MS=300 \
			-DPHASE1_TOTAL_MS=1500 -DPHASE1_SEND_MS=500 $(LDFLAGS) $(SOURCES) $(LDLIBS) -pthread -o "$$exe"; \
		python3 tests/test_deadlines.py "$$exe"

test-shutdown: webserver
	python3 tests/test_shutdown.py ./webserver

test-epoll: webserver
	python3 tests/test_epoll.py ./webserver

test-keepalive: webserver
	python3 tests/test_keepalive.py ./webserver

test-proxy-unit:
	@set -eu; \
		exe=$$(mktemp); trap 'rm -f "$$exe"' EXIT HUP INT TERM; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/proxy $(LDFLAGS) tests/test_proxy_wire.c \
			src/proxy/proxy_wire.c src/http/http.c $(LDLIBS) -o "$$exe"; \
		$(UNIT_RUNNER) "$$exe"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc/proxy $(LDFLAGS) tests/test_proxy_routes.c \
			src/proxy/proxy_routes.c $(LDLIBS) -o "$$exe"; \
		$(UNIT_RUNNER) "$$exe"

test-proxy: test-proxy-unit reverse-proxy webserver
	python3 tests/test_proxy.py ./reverse-proxy
	python3 tests/test_proxy_processes.py ./reverse-proxy ./webserver
	python3 tests/test_proxy_reuse.py ./reverse-proxy

test-cgi: webserver
	python3 tests/test_cgi.py ./webserver

help:
	@printf '%s\n' \
		'Host: sh dev.sh build | shell | check' \
		'Optional host Make: make dev-build | dev-shell' \
		'Container: make | make dev-check' \
		'Unit tests: make test-unit' \
		'Integration tests: make test-integration' \
		'Reverse proxy tests: make test-proxy' \
		'Full gate: make test; memory checks: make sanitize | valgrind' \
		'Performance baseline: make benchmark' \
		'Run the server: ./webserver --port 8080 --root .'

dev-build:
	sh dev.sh build

dev-shell:
	sh dev.sh shell

dev-check:
	timeout --kill-after=5s 120s sh tests/environment/check.sh

include tests/phase1.mk
