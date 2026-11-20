.DEFAULT_GOAL := webserver

CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Werror

.PHONY: help dev-build dev-shell dev-check test-unit test-integration

webserver: app.c webserver.c http.c app.h webserver.h http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) app.c webserver.c http.c $(LDLIBS) -o $@

test-unit:
	@set -eu; \
		http_bin=$$(mktemp); body_bin=$$(mktemp); server_bin=$$(mktemp); \
		trap 'rm -f "$$http_bin" "$$body_bin" "$$server_bin"' EXIT HUP INT TERM; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_http.c http.c $(LDLIBS) -o "$$http_bin"; \
		"$$http_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_body.c http.c $(LDLIBS) -o "$$body_bin"; \
		"$$body_bin"; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) tests/test_webserver.c http.c $(LDLIBS) -Wl,--wrap=recv -Wl,--wrap=send -o "$$server_bin"; \
		"$$server_bin"

test-integration: webserver
	@set -eu; python3 tests/test_server.py ./webserver

help:
	@printf '%s\n' \
		'Host: sh dev.sh build | shell | check' \
		'Optional host Make: make dev-build | dev-shell' \
		'Container: make | make dev-check' \
		'Unit tests: make test-unit' \
		'Integration tests: make test-integration' \
		'Run the server: ./webserver'

dev-build:
	sh dev.sh build

dev-shell:
	sh dev.sh shell

dev-check:
	timeout --kill-after=5s 120s sh tests/environment/check.sh
