/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lkl-host-ops.h"
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "LKL host failure: %s\n", #c); abort(); } } while (0)
struct lkl_sem { pthread_mutex_t mutex; pthread_cond_t cond; unsigned count; };
struct lkl_mutex { pthread_mutex_t mutex; };
struct lkl_tls_key { pthread_key_t key; };
struct timer { dispatch_source_t source; dispatch_queue_t queue; void (*fn)(void); };

static void host_print(const char *s, int n) { fwrite(s, 1, n, stderr); }
static void host_panic(void) { abort(); }
static struct lkl_sem *sem_alloc(int count)
{
    struct lkl_sem *s = calloc(1, sizeof(*s));
    REQUIRE(s);
    REQUIRE(pthread_mutex_init(&s->mutex, NULL) == 0);
    REQUIRE(pthread_cond_init(&s->cond, NULL) == 0);
    s->count = count;
    return s;
}
static void sem_free(struct lkl_sem *s)
{
    REQUIRE(pthread_cond_destroy(&s->cond) == 0);
    REQUIRE(pthread_mutex_destroy(&s->mutex) == 0);
    free(s);
}
static void sem_up(struct lkl_sem *s)
{
    REQUIRE(pthread_mutex_lock(&s->mutex) == 0);
    ++s->count;
    REQUIRE(pthread_cond_signal(&s->cond) == 0);
    REQUIRE(pthread_mutex_unlock(&s->mutex) == 0);
}
static void sem_down(struct lkl_sem *s)
{
    REQUIRE(pthread_mutex_lock(&s->mutex) == 0);
    while (!s->count) REQUIRE(pthread_cond_wait(&s->cond, &s->mutex) == 0);
    --s->count;
    REQUIRE(pthread_mutex_unlock(&s->mutex) == 0);
}
static struct lkl_mutex *mutex_alloc(int recursive)
{
    struct lkl_mutex *m = malloc(sizeof(*m));
    pthread_mutexattr_t attr;
    REQUIRE(m);
    REQUIRE(pthread_mutexattr_init(&attr) == 0);
    if (recursive) REQUIRE(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
    REQUIRE(pthread_mutex_init(&m->mutex, &attr) == 0);
    REQUIRE(pthread_mutexattr_destroy(&attr) == 0);
    return m;
}
static void mutex_free(struct lkl_mutex *m) { REQUIRE(pthread_mutex_destroy(&m->mutex) == 0); free(m); }
static void mutex_lock(struct lkl_mutex *m) { REQUIRE(pthread_mutex_lock(&m->mutex) == 0); }
static void mutex_unlock(struct lkl_mutex *m) { REQUIRE(pthread_mutex_unlock(&m->mutex) == 0); }

struct thread_start { void (*fn)(void *); void *arg; };
static void *thread_run(void *arg)
{
    struct thread_start start = *(struct thread_start *)arg;
    free(arg);
    start.fn(start.arg);
    return NULL;
}
static lkl_thread_t thread_create(void (*fn)(void *), void *arg)
{
    pthread_t thread;
    struct thread_start *start = malloc(sizeof(*start));
    if (!start) return 0;
    *start = (struct thread_start){fn, arg};
    if (pthread_create(&thread, NULL, thread_run, start)) { free(start); return 0; }
    return (unsigned long)thread;
}
static void thread_detach(void) { REQUIRE(pthread_detach(pthread_self()) == 0); }
static void thread_exit(void)
{
#ifdef NCE_PROCESS
    extern void nce_process_thread_exit(void);
    nce_process_thread_exit();
#endif
    pthread_exit(NULL);
}
static int thread_join(lkl_thread_t t) { return pthread_join((pthread_t)t, NULL) ? -1 : 0; }
static lkl_thread_t thread_self(void) { return (unsigned long)pthread_self(); }
static int thread_equal(lkl_thread_t a, lkl_thread_t b) { return pthread_equal((pthread_t)a, (pthread_t)b); }
static void *thread_stack(unsigned long *size)
{
    *size = pthread_get_stacksize_np(pthread_self());
    return (char *)pthread_get_stackaddr_np(pthread_self()) - *size;
}
static struct lkl_tls_key *tls_alloc(void (*destructor)(void *))
{
    struct lkl_tls_key *key = malloc(sizeof(*key));
    if (!key) return NULL;
    if (pthread_key_create(&key->key, destructor)) { free(key); return NULL; }
    return key;
}
static void tls_free(struct lkl_tls_key *key) { REQUIRE(pthread_key_delete(key->key) == 0); free(key); }
static int tls_set(struct lkl_tls_key *key, void *value) { return pthread_setspecific(key->key, value); }
static void *tls_get(struct lkl_tls_key *key) { return pthread_getspecific(key->key); }

static void *page_alloc(unsigned long size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static void page_free(void *p, unsigned long size) { REQUIRE(munmap(p, size) == 0); }
static unsigned long long host_time(void)
{
    struct timespec t;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (unsigned long long)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void timer_fire(void *context) { ((struct timer *)context)->fn(); }
static void *timer_alloc(void (*fn)(void))
{
    struct timer *t = calloc(1, sizeof(*t));
    REQUIRE(t);
    t->fn = fn;
    t->queue = dispatch_queue_create("husk.lkl.timer", DISPATCH_QUEUE_SERIAL);
    t->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, t->queue);
    REQUIRE(t->source);
    dispatch_set_context(t->source, t);
    dispatch_source_set_event_handler_f(t->source, timer_fire);
    dispatch_resume(t->source);
    return t;
}
static int timer_oneshot(void *timer, unsigned long delta)
{
    struct timer *t = timer;
    dispatch_source_set_timer(t->source, dispatch_time(DISPATCH_TIME_NOW, delta), DISPATCH_TIME_FOREVER, 0);
    return 0;
}
static void timer_barrier(void *unused) { (void)unused; }
static void timer_free(void *timer)
{
    struct timer *t = timer;
    dispatch_source_cancel(t->source);
    dispatch_sync_f(t->queue, NULL, timer_barrier);
    dispatch_release(t->source);
    dispatch_release(t->queue);
    free(t);
}
static void jump_set(struct lkl_jmp_buf *b, void (*fn)(void))
{
    _Static_assert(sizeof(jmp_buf) <= sizeof(struct lkl_jmp_buf), "jump buffer size");
    if (setjmp(*(jmp_buf *)b) == 0) fn();
}
static void jump_back(struct lkl_jmp_buf *b, int value) { longjmp(*(jmp_buf *)b, value); }

struct lkl_host_operations nce_lkl_host = {
    .print = host_print, .panic = host_panic,
    .sem_alloc = sem_alloc, .sem_free = sem_free, .sem_up = sem_up, .sem_down = sem_down,
    .mutex_alloc = mutex_alloc, .mutex_free = mutex_free, .mutex_lock = mutex_lock, .mutex_unlock = mutex_unlock,
    .thread_create = thread_create, .thread_detach = thread_detach, .thread_exit = thread_exit,
    .thread_join = thread_join, .thread_self = thread_self, .thread_equal = thread_equal, .thread_stack = thread_stack,
    .tls_alloc = tls_alloc, .tls_free = tls_free, .tls_set = tls_set, .tls_get = tls_get,
    .mem_alloc = malloc, .mem_free = free, .page_alloc = page_alloc, .page_free = page_free,
    .time = host_time, .timer_alloc = timer_alloc, .timer_set_oneshot = timer_oneshot, .timer_free = timer_free,
    .jmp_buf_set = jump_set, .jmp_buf_longjmp = jump_back,
    .memcpy = memcpy, .memset = memset, .memmove = memmove,
};
