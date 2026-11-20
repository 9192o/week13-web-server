.DEFAULT_GOAL := help

.PHONY: help dev-build dev-shell dev-check

help:
	@printf '%s\n' \
		'Host: sh dev.sh build | shell | check' \
		'Optional host Make: make dev-build | dev-shell' \
		'Container: make dev-check' \
		'The HTTP server build is not implemented yet.'

dev-build:
	sh dev.sh build

dev-shell:
	sh dev.sh shell

dev-check:
	timeout --kill-after=5s 120s sh tests/environment/check.sh
