CC      ?= cc
CFLAGS  ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS = -Iinclude
# 워커 스레드 풀 (컴파일과 링크 모두). CFLAGS 를 통째로 바꾸는 asan/ftest 빌드에도 붙도록 따로 둔다.
PTHREAD  = -pthread

BUILD   ?= build
TARGET  ?= server

# main.c 를 제외한 모듈은 테스트에서도 링크한다.
LIB_SRCS  = src/config.c src/server.c src/http_parser.c src/http_response.c \
            src/url.c src/static.c src/body.c src/cgi.c src/queue.c src/pool.c
MAIN_SRC  = src/main.c
TEST_SRCS = $(wildcard tests/test_*.c)

# www/cgi-bin 에 설치되는 CGI 프로그램 (소스는 cgi-src/)
CGI_BINS  = www/cgi-bin/adder

LIB_OBJS  = $(LIB_SRCS:%.c=$(BUILD)/%.o)
MAIN_OBJ  = $(MAIN_SRC:%.c=$(BUILD)/%.o)
TEST_BINS = $(TEST_SRCS:tests/%.c=$(BUILD)/tests/%)

ASAN_CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g -O1 \
              -fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_MAKE   = $(MAKE) BUILD=build-asan TARGET=server-asan CFLAGS="$(ASAN_CFLAGS)"

# 시간 제한을 짧게 줄인 ASan 빌드 (ftest 가 시간 제한 시나리오를 몇 초 안에 돌리려고).
# 값이 다르면 오브젝트도 달라지므로 디렉터리를 따로 쓴다.
# FAST_SEND_MS 는 100MB 전체 수신 시나리오가 그 안에 끝날 만큼은 되어야 한다.
FAST_HEADER_MS = 500
FAST_BODY_MS   = 1000
FAST_CGI_MS    = 500
FAST_KEEPALIVE_MS = 500
FAST_SEND_MS   = 2000
# FAST_SHUTDOWN_MS 는 FAST_SEND_MS 보다 짧아야 강제 종료 시나리오가 전송 제한보다 먼저 걸린다.
FAST_SHUTDOWN_MS = 1500
FAST_DEFS = -DHEADER_TIMEOUT_MS=$(FAST_HEADER_MS) -DBODY_TIMEOUT_MS=$(FAST_BODY_MS) \
            -DCGI_TIMEOUT_MS=$(FAST_CGI_MS) -DSEND_TIMEOUT_MS=$(FAST_SEND_MS) \
            -DKEEPALIVE_TIMEOUT_MS=$(FAST_KEEPALIVE_MS) \
            -DSHUTDOWN_TIMEOUT_MS=$(FAST_SHUTDOWN_MS)
FAST_MAKE = $(MAKE) BUILD=build-asan-fast TARGET=server-asan-fast \
            CFLAGS="$(ASAN_CFLAGS) $(FAST_DEFS)"
# functional.sh 가 기대 시간을 계산할 때 쓰는 값 (기본값은 실제 값)
FAST_ENV  = HEADER_MS=$(FAST_HEADER_MS) BODY_MS=$(FAST_BODY_MS) \
            CGI_MS=$(FAST_CGI_MS) SEND_MS=$(FAST_SEND_MS) KEEPALIVE_MS=$(FAST_KEEPALIVE_MS) \
            SHUTDOWN_MS=$(FAST_SHUTDOWN_MS)

.PHONY: all test asan ftest ftest-slow valgrind clean

all: $(TARGET) $(CGI_BINS)

$(TARGET): $(LIB_OBJS) $(MAIN_OBJ)
	$(CC) $(CFLAGS) $(PTHREAD) $^ -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PTHREAD) -c $< -o $@

$(BUILD)/tests/%: tests/%.c $(LIB_OBJS) tests/test.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PTHREAD) $< $(LIB_OBJS) -o $@

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

# AddressSanitizer + LeakSanitizer + UBSan 빌드: make asan && ./server-asan
# 일반 빌드와 오브젝트가 섞이지 않도록 별도 디렉터리(build-asan)를 쓴다.
asan:
	$(ASAN_MAKE) all test

# 기능 테스트: ASan 서버를 임시 문서 루트로 띄워 시나리오 확인 (sanitizer 보고도 실패 처리)
# ftest 는 시간 제한을 줄인 빌드로 시간 제한 시나리오까지 전부 (몇 초).
# ftest-slow 는 실제 시간 제한 값 그대로 (약 1분, 대부분 전송 제한 30초 대기).
ftest:
	$(FAST_MAKE) all
	$(FAST_ENV) bash tests/functional.sh ./server-asan-fast --timing

ftest-slow:
	$(ASAN_MAKE) all
	bash tests/functional.sh ./server-asan --timing

# valgrind: 일반 빌드 서버를 valgrind 아래에서 띄워 빠른 기능 테스트
# (메모리 오류·확실한 누수가 있으면 서버가 exit 99 → 실패)
valgrind: all
	bash tests/functional.sh tests/valgrind-server.sh

www/cgi-bin/%: cgi-src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf build build-asan build-asan-fast server server-asan server-asan-fast $(CGI_BINS)
