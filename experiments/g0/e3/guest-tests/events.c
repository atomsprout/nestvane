// eventfd and epoll, as Android's Looper uses them: one thread waits, another wakes it.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int wake;

static void* waker(void* raw) {
    (void)raw;
    usleep(20000);
    uint64_t one = 1;
    write(wake, &one, 8);
    return NULL;
}

int main(void) {
    wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int pipes[2];
    pipe(pipes);
    const int epoll = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 0x1111222233334444ull};
    printf("add eventfd: %d\n", epoll_ctl(epoll, EPOLL_CTL_ADD, wake, &event));
    event.data.u64 = 7;
    printf("add pipe: %d\n", epoll_ctl(epoll, EPOLL_CTL_ADD, pipes[0], &event));
    printf("add twice: %d errno=%d\n", epoll_ctl(epoll, EPOLL_CTL_ADD, pipes[0], &event), errno == EEXIST);

    struct epoll_event ready[4];
    printf("nothing ready: %d\n", epoll_wait(epoll, ready, 4, 10));
    uint64_t value = 0;
    printf("empty read: %zd errno=%d\n", read(wake, &value, 8), errno == EAGAIN);

    pthread_t thread;
    pthread_create(&thread, NULL, waker, NULL);
    int count = epoll_wait(epoll, ready, 4, 2000);
    printf("woken: %d data=%llx events=%x\n", count, (unsigned long long)ready[0].data.u64, ready[0].events);
    pthread_join(thread, NULL);
    value = 5;
    write(wake, &value, 8);
    printf("read: %zd value=%llu\n", read(wake, &value, 8), (unsigned long long)value);
    printf("drained: %d\n", epoll_wait(epoll, ready, 4, 0));

    write(pipes[1], "x", 1);
    count = epoll_wait(epoll, ready, 4, 0);
    printf("pipe ready: %d data=%llu\n", count, (unsigned long long)ready[0].data.u64);
    printf("delete: %d\n", epoll_ctl(epoll, EPOLL_CTL_DEL, pipes[0], NULL));
    printf("after delete: %d\n", epoll_wait(epoll, ready, 4, 0));

    const int semaphore = eventfd(3, EFD_SEMAPHORE);
    read(semaphore, &value, 8);
    printf("semaphore read: %llu\n", (unsigned long long)value);
    read(semaphore, &value, 8);
    read(semaphore, &value, 8);
    fcntl(semaphore, F_SETFL, O_NONBLOCK);
    printf("semaphore empty: %zd\n", read(semaphore, &value, 8));
    return 0;
}
