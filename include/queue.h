#ifndef QUEUE_H
#define QUEUE_H

#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

/* 접근 로그용 "ip:port" 문자열 크기 */
#define CONN_PEER_MAX (INET_ADDRSTRLEN + 8)

/* accept 스레드가 워커에게 넘기는 연결 하나 */
struct conn_item {
    int  fd;
    char peer[CONN_PEER_MAX];
};

/*
 * 고정 크기 연결 큐 (원형 버퍼, mutex + cond).
 * 넣는 쪽은 accept 스레드 하나, 꺼내는 쪽은 워커 여러 개.
 */
struct conn_queue {
    struct conn_item *items;
    int cap;
    int head;            /* 다음에 꺼낼 위치 */
    int len;             /* 들어 있는 개수 */
    int closed;          /* queue_close 이후 1 */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
};

enum queue_result {
    QUEUE_OK = 0,
    QUEUE_CLOSED = -1,   /* queue_close 이후 */
    QUEUE_TIMEOUT = -2   /* 시간 안에 자리가 나지 않음 */
};

/* cap 칸짜리 큐를 만든다. 성공 0, 실패 -1 (자원은 남기지 않음). */
int queue_init(struct conn_queue *q, int cap);

/* 큐 자원 해제. 남은 항목의 fd 는 닫지 않는다 (queue_drain 먼저). */
void queue_destroy(struct conn_queue *q);

/*
 * item 을 넣는다. 가득 차 있으면 timeout_ms 동안 자리가 나길 기다린다.
 * 반환: QUEUE_OK, QUEUE_TIMEOUT(호출자가 종료 플래그를 보고 다시 시도), QUEUE_CLOSED.
 */
enum queue_result queue_push(struct conn_queue *q, const struct conn_item *item,
                             int timeout_ms);

/*
 * 항목 하나를 꺼낸다. 비어 있으면 들어오거나 닫힐 때까지 기다린다.
 * 닫힌 뒤에는 남은 항목이 있어도 QUEUE_CLOSED (대기 중인 연결은 처리하지 않음).
 */
enum queue_result queue_pop(struct conn_queue *q, struct conn_item *out);

/* 닫고, 기다리는 모든 스레드를 깨운다. 이후 push/pop 은 QUEUE_CLOSED. */
void queue_close(struct conn_queue *q);

/* 남은 항목을 모두 빼서 fd 를 close 한다. 닫은 개수를 돌려준다. */
int queue_drain(struct conn_queue *q);

#endif
