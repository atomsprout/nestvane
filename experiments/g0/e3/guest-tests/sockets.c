// Unix stream sockets between two threads, as the zygote and its clients use them.
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char* path = "/data/local/tmp/test.socket";

static void* client(void* raw) {
    (void)raw;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, path);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (connect(fd, (struct sockaddr*)&address, sizeof address) != 0) { perror("connect"); return NULL; }
    write(fd, "ping", 4);
    char reply[8] = {0};
    struct iovec part = {reply, sizeof reply - 1};
    struct msghdr message = {.msg_iov = &part, .msg_iovlen = 1};
    const ssize_t got = recvmsg(fd, &message, 0);
    printf("client got %zd: %s\n", got, reply);
    close(fd);
    return NULL;
}

int main(void) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, path);
    unlink(path);
    const int server = socket(AF_UNIX, SOCK_STREAM, 0);
    printf("bind: %d\n", bind(server, (struct sockaddr*)&address, sizeof address));
    printf("listen: %d\n", listen(server, 4));
    struct sockaddr_un name = {0};
    socklen_t length = sizeof name;
    getsockname(server, (struct sockaddr*)&name, &length);
    printf("name: %s family=%d\n", name.sun_path, name.sun_family);

    pthread_t thread;
    pthread_create(&thread, NULL, client, NULL);
    const int peer = accept4(server, NULL, NULL, SOCK_CLOEXEC);
    char request[8] = {0};
    printf("server got %zd: %s\n", read(peer, request, 7), request);
    struct ucred credentials = {-1, -1, -1};
    length = sizeof credentials;
    printf("peer credentials: %d uid=%d\n", getsockopt(peer, SOL_SOCKET, SO_PEERCRED, &credentials, &length), credentials.uid);
    struct iovec parts[2] = {{"po", 2}, {"ng", 2}};
    struct msghdr message = {.msg_iov = parts, .msg_iovlen = 2};
    printf("sendmsg: %zd\n", sendmsg(peer, &message, 0));
    pthread_join(thread, NULL);
    close(peer);

    int pair[2];
    printf("socketpair: %d\n", socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
    write(pair[0], "abc", 3);
    printf("pair read: %zd\n", read(pair[1], request, 7));
    unlink(path);
    return 0;
}
