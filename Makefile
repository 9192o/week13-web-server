.DEFAULT_GOAL := webserver

CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Werror
SOURCES = app.c webserver.c http.c router.c static_file.c static_response.c
HEADERS = app.h webserver.h http.h router.h static_file.h static_response.h

.PHONY: help dev-build dev-shell dev-check test-unit test-integration

webserver: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(SOURCES) $(LDLIBS) -o $@

test-unit:
	@set -eu; \
		http_bin=$$(mktemp); body_bin=$$(mktemp); server_bin=$$(mktemp); router_bin=$$(mktemp); static_bin=$$(mktemp); \
		trap 'rm -f "$$http_bin" "$$body_bin" "$$server_bin" "$$router_bin" "$$static_bin"' EXIT HUP INT TERM; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_http.c http.c $(LDLIBS) -o "$$http_bin"; \
		"$$http_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_body.c http.c $(LDLIBS) -o "$$body_bin"; \
		"$$body_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_webserver.c http.c $(LDLIBS) -Wl,--wrap=recv -Wl,--wrap=send -o "$$server_bin"; \
		"$$server_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_router.c router.c http.c static_response.c static_file.c $(LDLIBS) -Wl,--wrap=open -Wl,--wrap=fstat -o "$$router_bin"; \
		"$$router_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_static_file.c static_file.c $(LDLIBS) -o "$$static_bin"; \
		"$$static_bin"

test-integration: webserver
	python3 tests/test_server.py ./webserver
	python3 tests/test_static.py ./webserver

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
