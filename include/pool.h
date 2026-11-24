#ifndef POOL_H
#define POOL_H

#include "queue.h"

#include <pthread.h>
#include <signal.h>

/*
 * 워커 스레드 풀. 각 워커는 큐에서 연결을 꺼내 handler 로 처리하고, 처리가 끝나면
 * 그 연결을 직접 close 한 뒤 다시 꺼낸다 (handler 는 fd 를 닫지 않는다).
 * 큐가 닫히면 들고 있던 연결만 끝내고 종료한다.
 */
typedef void (*conn_handler)(const struct conn_item *item, void *arg);

struct worker;

struct worker_pool {
    struct worker *workers;
    int n;
    struct conn_queue *q;
    conn_handler handler;
    void *arg;
    pthread_mutex_t lock;  /* 아래 필드와 각 워커의 fd/done 을 보호 */
    int forced;            /* pool_stop 이 강제로 끊기 시작함 */
};

/*
 * 워커 n 개를 띄운다. 워커에서는 SIGINT/SIGTERM 을 막아 두어
 * 종료 신호가 accept 중인 메인 스레드로만 가게 한다.
 * 성공 0. 실패하면 이미 띄운 워커를 정리하고 -1 (큐는 닫힌 상태가 됨).
 */
int pool_start(struct worker_pool *p, int n, struct conn_queue *q,
               conn_handler handler, void *arg);

/* 지금 처리 중인 연결 수 */
int pool_active(struct worker_pool *p);

/*
 * 큐를 닫고 워커가 모두 끝나길 기다린 뒤 join 한다.
 * timeout_ms 가 지나거나 *force_level 이 2 이상이 되면(두 번째 종료 신호)
 * 처리 중인 연결 소켓을 shutdown(SHUT_RDWR) 해서 워커가 빨리 빠져나오게 한다.
 * 워커가 CGI 를 기다리는 중이면 CGI 시간 제한까지는 더 걸릴 수 있다.
 * 반환: 강제로 끊은 연결 수 (0 이면 모두 정상으로 끝남).
 */
int pool_stop(struct worker_pool *p, int timeout_ms,
              const volatile sig_atomic_t *force_level);

#endif
