#ifndef TASK_POOL_H
#define TASK_POOL_H

#include <stddef.h>
#include <stdint.h>

typedef struct task_pool task_pool_t;

/* Task returns a result pointer. The event loop owns/frees the result. */
typedef void *(*task_fn)(void *arg);

task_pool_t *task_pool_create(int workers, int queue_size);
void task_pool_destroy(task_pool_t *pool);

/* Stop accepting jobs and join workers. Drain results afterwards. */
void task_pool_shutdown(task_pool_t *pool);

/*
 * Submit a job. Returns 0 on success, -1 with errno:
 *   EAGAIN   queue full
 *   ECANCELED pool is stopping
 */
int task_pool_submit(task_pool_t *pool, uint64_t id, task_fn fn, void *arg);

/* Read end of the completion pipe; add it to the event loop with EV_READ. */
int task_pool_completion_fd(const task_pool_t *pool);

/*
 * Pop one completion. Returns 1 when a result was popped, 0 when empty,
 * -1 on error. The caller owns `data`.
 */
int task_pool_pop_result(task_pool_t *pool, uint64_t *id, void **data);

/* Number of queued jobs (not including running/completed jobs). */
int task_pool_pending(const task_pool_t *pool);

#endif
