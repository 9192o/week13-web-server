.DEFAULT_GOAL := webserver

CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Werror
SOURCES = app.c webserver.c http.c router.c static_file.c static_response.c connection.c
HEADERS = app.h webserver.h http.h router.h static_file.h static_response.h connection.h

.PHONY: help dev-build dev-shell dev-check test-unit test-integration test-deadlines

webserver: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(SOURCES) $(LDLIBS) -o $@

test-unit:
	@set -eu; \
		http_bin=$$(mktemp); body_bin=$$(mktemp); server_bin=$$(mktemp); router_bin=$$(mktemp); static_bin=$$(mktemp); conn_bin=$$(mktemp); \
		trap 'rm -f "$$http_bin" "$$body_bin" "$$server_bin" "$$router_bin" "$$static_bin" "$$conn_bin"' EXIT HUP INT TERM; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_http.c http.c $(LDLIBS) -o "$$http_bin"; \
		"$$http_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_body.c http.c $(LDLIBS) -o "$$body_bin"; \
		"$$body_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_webserver.c http.c connection.c $(LDLIBS) -Wl,--wrap=recv -Wl,--wrap=send -Wl,--wrap=setsockopt -o "$$server_bin"; \
		"$$server_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_router.c router.c http.c static_response.c static_file.c $(LDLIBS) -Wl,--wrap=open -Wl,--wrap=fstat -o "$$router_bin"; \
		"$$router_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_static_file.c static_file.c $(LDLIBS) -o "$$static_bin"; \
		"$$static_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -DCONNECTION_TESTABLE -I. $(LDFLAGS) tests/test_connection.c connection.c $(LDLIBS) -o "$$conn_bin"; \
		"$$conn_bin"

test-integration: webserver
	python3 tests/test_server.py ./webserver
	python3 tests/test_static.py ./webserver

test-deadlines:
	@set -eu; \
		exe=$$(mktemp); trap 'rm -f "$$exe"' EXIT HUP INT TERM; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -DPHASE1_HEADER_MS=250 -DPHASE1_IDLE_MS=300 \
			-DPHASE1_TOTAL_MS=1500 -DPHASE1_SEND_MS=500 $(LDFLAGS) $(SOURCES) $(LDLIBS) -o "$$exe"; \
		python3 tests/test_deadlines.py "$$exe"

help:
	@printf '%s\n' \
		'Host: sh dev.sh build | shell | check' \
		'Optional host Make: make dev-build | dev-shell' \
		'Container: make | make dev-check' \
		'Unit tests: make test-unit' \
		'Integration tests: make test-integration' \
		'Run the server: ./webserver --port 8080 --root .'

dev-build:
	sh dev.sh build

dev-shell:
	sh dev.sh shell

dev-check:
	timeout --kill-after=5s 120s sh tests/environment/check.sh
