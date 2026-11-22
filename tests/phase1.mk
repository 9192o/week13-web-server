# Included by the root Makefile after the server targets.
PHASE1_SANITIZE_FLAGS = -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie
PHASE1_VALGRIND = valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=91 --track-fds=yes

.PHONY: test sanitize valgrind benchmark

test: test-unit test-integration test-deadlines

sanitize:
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(MAKE) -B test CFLAGS='$(CFLAGS) $(PHASE1_SANITIZE_FLAGS)'

valgrind:
	$(MAKE) -B test-unit CFLAGS='$(CFLAGS) -g' UNIT_RUNNER='$(PHASE1_VALGRIND)'

benchmark:
	@set -eu; \
		exe=$$(mktemp); trap 'rm -f "$$exe"' EXIT HUP INT TERM; \
		$(CC) $(CPPFLAGS) $(CFLAGS) -O2 $(LDFLAGS) $(SOURCES) $(LDLIBS) -o "$$exe"; \
		python3 tests/benchmark_phase1.py "$$exe" --log-file /dev/null
