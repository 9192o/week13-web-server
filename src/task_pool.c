#include "task_pool.h"

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint64_t id;
    task_fn fn;
    void *arg;
} task_job_t;

typedef struct task_result {
    uint64_t id;
    void *data;
    struct task_result *next;
} task_result_t;

struct task_pool {
    pthread_mutex_t job_mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    task_job_t *jobs;
    int queue_size;
    int head;
    int count;
    int shutdown;

    pthread_mutex_t result_mutex;
    task_result_t *result_head;
    task_result_t *result_tail;

    int completion_pipe[2];

    pthread_t *threads;
    int workers;
    int started;
};

static void push_result(task_pool_t *pool, uint64_t id, void *data) {
    task_result_t *r = malloc(sizeof *r);
    if (r == NULL) {
        log_error("task_pool: result allocation failed");
        return;
    }
    r->id = id;
    r->data = data;
    r->next = NULL;

    pthread_mutex_lock(&pool->result_mutex);
    if (pool->result_tail != NULL)
        pool->result_tail->next = r;
    else
        pool->result_head = r;
    pool->result_tail = r;
    pthread_mutex_unlock(&pool->result_mutex);

    char byte = 'x';
    ssize_t n = write(pool->completion_pipe[1], &byte, 1);
    (void)n; /* EAGAIN is fine: the pipe already signals pending results. */
}

static void *worker_main(void *arg) {
    task_pool_t *pool = arg;

    for (;;) {
        pthread_mutex_lock(&pool->job_mutex);
        while (pool->count == 0 && !pool->shutdown)
            pthread_cond_wait(&pool->not_empty, &pool->job_mutex);

        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&pool->job_mutex);
            break;
        }

        task_job_t job = pool->jobs[pool->head];
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count--;
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->job_mutex);

        void *data = job.fn(job.arg);
        push_result(pool, job.id, data);
    }
    return NULL;
}

static int make_pipe(int fds[2]) {
    if (pipe(fds) < 0)
        return -1;

    for (int i = 0; i < 2; i++) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fds[0]);
            close(fds[1]);
            return -1;
        }
        (void)fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    }
    return 0;
}

task_pool_t *task_pool_create(int workers, int queue_size) {
    if (workers <= 0 || queue_size <= 0) {
        errno = EINVAL;
        return NULL;
    }

    task_pool_t *pool = calloc(1, sizeof *pool);
    if (pool == NULL)
        return NULL;

    pool->queue_size = queue_size;
    pool->workers = workers;
    pool->completion_pipe[0] = -1;
    pool->completion_pipe[1] = -1;

    pool->jobs = calloc((size_t)queue_size, sizeof *pool->jobs);
    pool->threads = calloc((size_t)workers, sizeof *pool->threads);
    if (pool->jobs == NULL || pool->threads == NULL) {
        free(pool->jobs);
        free(pool->threads);
        free(pool);
        errno = ENOMEM;
        return NULL;
    }

    if (pthread_mutex_init(&pool->job_mutex, NULL) != 0 ||
        pthread_mutex_init(&pool->result_mutex, NULL) != 0) {
        free(pool->jobs);
        free(pool->threads);
        free(pool);
        errno = ENOMEM;
        return NULL;
    }
    if (pthread_cond_init(&pool->not_empty, NULL) != 0 ||
        pthread_cond_init(&pool->not_full, NULL) != 0) {
        pthread_mutex_destroy(&pool->job_mutex);
        pthread_mutex_destroy(&pool->result_mutex);
        free(pool->jobs);
        free(pool->threads);
        free(pool);
        errno = ENOMEM;
        return NULL;
    }

    if (make_pipe(pool->completion_pipe) < 0) {
        pthread_cond_destroy(&pool->not_empty);
        pthread_cond_destroy(&pool->not_full);
        pthread_mutex_destroy(&pool->job_mutex);
        pthread_mutex_destroy(&pool->result_mutex);
        free(pool->jobs);
        free(pool->threads);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < workers; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_main, pool) != 0) {
            pthread_mutex_lock(&pool->job_mutex);
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->not_empty);
            pthread_mutex_unlock(&pool->job_mutex);

            for (int j = 0; j < pool->started; j++)
                pthread_join(pool->threads[j], NULL);

            close(pool->completion_pipe[0]);
            close(pool->completion_pipe[1]);
            pthread_cond_destroy(&pool->not_empty);
            pthread_cond_destroy(&pool->not_full);
            pthread_mutex_destroy(&pool->job_mutex);
            pthread_mutex_destroy(&pool->result_mutex);
            free(pool->jobs);
            free(pool->threads);
            free(pool);
            errno = EAGAIN;
            return NULL;
        }
        pool->started++;
    }

    return pool;
}

void task_pool_destroy(task_pool_t *pool) {
    if (pool == NULL)
        return;

    task_pool_shutdown(pool);

    task_result_t *r = pool->result_head;
    while (r != NULL) {
        task_result_t *next = r->next;
        /* The caller owns result data; drain results before destroy. */
        free(r);
        r = next;
    }

    close(pool->completion_pipe[0]);
    close(pool->completion_pipe[1]);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    pthread_mutex_destroy(&pool->job_mutex);
    pthread_mutex_destroy(&pool->result_mutex);
    free(pool->jobs);
    free(pool->threads);
    free(pool);
}

void task_pool_shutdown(task_pool_t *pool) {
    if (pool == NULL)
        return;

    pthread_mutex_lock(&pool->job_mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->job_mutex);

    for (int i = 0; i < pool->started; i++)
        pthread_join(pool->threads[i], NULL);
    pool->started = 0;
}

int task_pool_submit(task_pool_t *pool, uint64_t id, task_fn fn, void *arg) {
    if (pool == NULL || fn == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->job_mutex);
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->job_mutex);
        errno = ECANCELED;
        return -1;
    }
    if (pool->count == pool->queue_size) {
        pthread_mutex_unlock(&pool->job_mutex);
        errno = EAGAIN;
        return -1;
    }

    int tail = (pool->head + pool->count) % pool->queue_size;
    pool->jobs[tail].id = id;
    pool->jobs[tail].fn = fn;
    pool->jobs[tail].arg = arg;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->job_mutex);
    return 0;
}

int task_pool_completion_fd(const task_pool_t *pool) {
    if (pool == NULL) {
        errno = EINVAL;
        return -1;
    }
    return pool->completion_pipe[0];
}

int task_pool_pop_result(task_pool_t *pool, uint64_t *id, void **data) {
    if (pool == NULL || id == NULL || data == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&pool->result_mutex);
    task_result_t *r = pool->result_head;
    if (r == NULL) {
        pthread_mutex_unlock(&pool->result_mutex);
        return 0;
    }

    pool->result_head = r->next;
    if (pool->result_head == NULL)
        pool->result_tail = NULL;
    pthread_mutex_unlock(&pool->result_mutex);

    *id = r->id;
    *data = r->data;
    free(r);
    return 1;
}

int task_pool_pending(const task_pool_t *pool) {
    if (pool == NULL) {
        errno = EINVAL;
        return -1;
    }

    task_pool_t *p = (task_pool_t *)pool;
    pthread_mutex_lock(&p->job_mutex);
    int count = p->count;
    pthread_mutex_unlock(&p->job_mutex);
    return count;
}
