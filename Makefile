CC      ?= cc
CFLAGS  ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS = -Iinclude

BUILD   ?= build
TARGET  ?= server

# main.c 를 제외한 모듈은 테스트에서도 링크한다.
LIB_SRCS  = src/config.c src/server.c src/http_parser.c src/http_response.c \
            src/url.c src/static.c src/body.c src/cgi.c
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

.PHONY: all test asan ftest ftest-slow clean

all: $(TARGET) $(CGI_BINS)

$(TARGET): $(LIB_OBJS) $(MAIN_OBJ)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.c $(LIB_OBJS) tests/test.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_OBJS) -o $@

test: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; ./$$t; done

# AddressSanitizer + LeakSanitizer + UBSan 빌드: make asan && ./server-asan
# 일반 빌드와 오브젝트가 섞이지 않도록 별도 디렉터리(build-asan)를 쓴다.
asan:
	$(ASAN_MAKE) all test

# 기능 테스트: ASan 서버를 임시 문서 루트로 띄워 시나리오 확인 (sanitizer 보고도 실패 처리)
# ftest 는 빠른 것만, ftest-slow 는 시간 제한 테스트(약 30초)까지.
ftest:
	$(ASAN_MAKE) all
	bash tests/functional.sh ./server-asan

ftest-slow:
	$(ASAN_MAKE) all
	bash tests/functional.sh ./server-asan --slow

www/cgi-bin/%: cgi-src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -rf build build-asan server server-asan $(CGI_BINS)
