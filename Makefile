.DEFAULT_GOAL := webserver

CC = gcc
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -Werror

.PHONY: help dev-build dev-shell dev-check

webserver: app.c webserver.c http.c app.h webserver.h http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) app.c webserver.c http.c $(LDLIBS) -o $@

help:
	@printf '%s\n' \
		'Host: sh dev.sh build | shell | check' \
		'Optional host Make: make dev-build | dev-shell' \
		'Container: make | make dev-check' \
		'Run the server: ./webserver'

dev-build:
	sh dev.sh build

dev-shell:
	sh dev.sh shell

dev-check:
	timeout --kill-after=5s 120s sh tests/environment/check.sh
