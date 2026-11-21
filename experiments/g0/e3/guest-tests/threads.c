// Exercises what a threaded Bionic program needs from the runtime: clone with CLONE_THREAD, futex
// through mutexes and condition variables, thread-local storage and join.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { threads = 8, rounds = 200000 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t turn_changed = PTHREAD_COND_INITIALIZER;
static long counter;
static int turn;
static __thread long local;

static void* count(void* raw) {
    const long id = (long)raw;
    for (int round = 0; round < rounds; ++round) {
        pthread_mutex_lock(&lock);
        ++counter;
        pthread_mutex_unlock(&lock);
        local += id;
    }
    return (void*)local;
}

// Threads take turns in order, so every hand-over is a condition variable wake-up.
static void* relay(void* raw) {
    const long id = (long)raw;
    for (int round = 0; round < 2000; ++round) {
        pthread_mutex_lock(&lock);
        while (turn != id) pthread_cond_wait(&turn_changed, &lock);
        turn = (turn + 1) % threads;
        pthread_cond_broadcast(&turn_changed);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main(void) {
    pthread_t handles[threads];
    long sum = 0;
    for (long id = 0; id < threads; ++id) pthread_create(&handles[id], NULL, count, (void*)id);
    for (long id = 0; id < threads; ++id) {
        void* result;
        pthread_join(handles[id], &result);
        sum += (long)result;
    }
    printf("counter=%ld expected=%d thread_local_sum=%ld expected=%ld\n", counter, threads * rounds, sum,
           (long)rounds * (threads * (threads - 1) / 2));
    for (long id = 0; id < threads; ++id) pthread_create(&handles[id], NULL, relay, (void*)id);
    for (long id = 0; id < threads; ++id) pthread_join(handles[id], NULL);
    printf("relay turn=%d\n", turn);
    return counter == threads * rounds ? 0 : 1;
}
