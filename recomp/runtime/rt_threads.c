/* rt_threads.c - tiny worker pool for parallel loops (used by the renderer). */
#include "rt.h"
#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK lock_t; typedef CONDITION_VARIABLE cond_t;
#define LOCK(l) AcquireSRWLockExclusive(l)
#define UNLOCK(l) ReleaseSRWLockExclusive(l)
#define WAIT(c, l) SleepConditionVariableSRW(c, l, INFINITE, 0)
#define BCAST(c) WakeAllConditionVariable(c)
static int ncpu(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t lock_t; typedef pthread_cond_t cond_t;
#define LOCK(l) pthread_mutex_lock(l)
#define UNLOCK(l) pthread_mutex_unlock(l)
#define WAIT(c, l) pthread_cond_wait(c, l)
#define BCAST(c) pthread_cond_broadcast(c)
static int ncpu(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif

static lock_t lk; static cond_t cv_job, cv_done;
static int nworkers, started;
static volatile unsigned gen;
static void (*job_fn)(int, void *); static void *job_arg;
static int job_n, job_next, job_done;

static void run_tasks(void) {
    for (;;) {
        LOCK(&lk);
        int i = job_next < job_n ? job_next++ : -1;
        UNLOCK(&lk);
        if (i < 0) return;
        job_fn(i, job_arg);
        LOCK(&lk);
        if (++job_done == job_n) BCAST(&cv_done);
        UNLOCK(&lk);
    }
}
#ifdef _WIN32
static DWORD WINAPI worker(void *p) {
#else
static void *worker(void *p) {
#endif
    (void)p; unsigned seen = 0;
    for (;;) {
        LOCK(&lk);
        while (gen == seen) WAIT(&cv_job, &lk);
        seen = gen;
        UNLOCK(&lk);
        run_tasks();
    }
    return 0;
}
static void start(void) {
    started = 1;
#ifdef _WIN32
    InitializeSRWLock(&lk); InitializeConditionVariable(&cv_job); InitializeConditionVariable(&cv_done);
#else
    pthread_mutex_init(&lk, NULL); pthread_cond_init(&cv_job, NULL); pthread_cond_init(&cv_done, NULL);
#endif
    nworkers = ncpu() - 1; if (nworkers > 15) nworkers = 15;
    const char *e = getenv("RT_THREADS"); if (e) nworkers = atoi(e) - 1;
    if (nworkers < 0) nworkers = 0;
    for (int i = 0; i < nworkers; i++) {
#ifdef _WIN32
        CreateThread(NULL, 1 << 20, worker, NULL, 0, NULL);
#else
        pthread_t t; pthread_create(&t, NULL, worker, NULL); pthread_detach(t);
#endif
    }
    rt_log("threads", "renderer using %d threads", nworkers + 1);
}
void plat_parallel_for(int n, void (*fn)(int, void *), void *arg) {
    if (!started) start();
    if (!nworkers) { for (int i = 0; i < n; i++) fn(i, arg); return; }
    LOCK(&lk);
    job_fn = fn; job_arg = arg; job_n = n; job_next = 0; job_done = 0; gen++;
    BCAST(&cv_job);
    UNLOCK(&lk);
    run_tasks();
    LOCK(&lk);
    while (job_done < job_n) WAIT(&cv_done, &lk);
    UNLOCK(&lk);
}
