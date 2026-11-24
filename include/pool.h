#ifndef POOL_H
#define POOL_H

#include "queue.h"

#include <pthread.h>

/*
 * 워커 스레드 풀. 각 워커는 큐에서 연결을 꺼내 handler 로 처리하고 다시 꺼낸다.
 * 큐가 닫히면 들고 있던 연결만 끝내고 종료한다.
 */
typedef void (*conn_handler)(const struct conn_item *item, void *arg);

struct worker_pool {
    pthread_t *threads;
    int n;
    struct conn_queue *q;
    conn_handler handler;
    void *arg;
};

/*
 * 워커 n 개를 띄운다. 워커에서는 SIGINT/SIGTERM 을 막아 두어
 * 종료 신호가 accept 중인 메인 스레드로만 가게 한다.
 * 성공 0. 실패하면 이미 띄운 워커를 정리하고 -1 (큐는 닫힌 상태가 됨).
 */
int pool_start(struct worker_pool *p, int n, struct conn_queue *q,
               conn_handler handler, void *arg);

/* 큐를 닫고 모든 워커가 끝날 때까지 기다린다 (join). */
void pool_stop(struct worker_pool *p);

#endif
