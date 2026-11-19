#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/loop.h>
#include <linux/netlink.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static int bpf(enum bpf_cmd command, union bpf_attr *attr) {
    return syscall(__NR_bpf, command, attr, sizeof(*attr));
}

static int receiver(void) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    const struct sockaddr_nl addr = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (fd < 0) return -1;
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
    close(fd);
    return -1;
}

static ssize_t receive(int fd, char *buffer, size_t size) {
    struct iovec entry = {.iov_base = buffer, .iov_len = size};
    struct msghdr message = {.msg_iov = &entry, .msg_iovlen = 1};
    return recvmsg(fd, &message, MSG_DONTWAIT);
}

int main(void) {
    const int source = open("/system/etc/nestvane/health-filter.bin", O_RDONLY);
    if (source < 0) { perror("open Health filter"); return 1; }
    struct bpf_insn instructions[4096];
    const ssize_t bytes = read(source, instructions, sizeof(instructions));
    close(source);
    if (bytes <= 0 || bytes % sizeof(instructions[0])) { perror("read Health filter"); return 1; }
    union bpf_attr program = {.prog_type = BPF_PROG_TYPE_SOCKET_FILTER, .insn_cnt = bytes / sizeof(instructions[0]),
        .insns = (uintptr_t)instructions, .license = (uintptr_t)"Apache 2.0"};
    int fd = bpf(BPF_PROG_LOAD, &program);
    if (fd < 0) { perror("load Health filter"); return 1; }
    const char *path = "/sys/fs/bpf/nestvane_probe_health_filter";
    union bpf_attr object = {.pathname = (uintptr_t)path, .bpf_fd = fd};
    if (bpf(BPF_OBJ_PIN, &object) < 0) { perror("pin Health filter"); return 1; }
    close(fd);
    object.bpf_fd = 0;
    object.file_flags = BPF_F_RDONLY;
    fd = bpf(BPF_OBJ_GET, &object);
    if (unlink(path) != 0 || fd < 0) { perror("reopen Health filter"); return 1; }
    const int filtered = receiver(), control = receiver();
    if (filtered < 0 || control < 0) { perror("uevent sockets"); return 1; }
    if (setsockopt(filtered, SOL_SOCKET, SO_ATTACH_BPF, &fd, sizeof(fd)) != 0) {
        perror("attach Health filter");
        return 1;
    }
    close(fd);
    const int loop = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    const int number = 63;
    if (loop < 0 || ioctl(loop, LOOP_CTL_ADD, number) < 0) { perror("create probe loop"); return 1; }
    char packet[256];
    const ssize_t dropped = receive(filtered, packet, sizeof(packet));
    const int dropError = errno;
    const ssize_t delivered = receive(control, packet, sizeof(packet));
    if (ioctl(loop, LOOP_CTL_REMOVE, number) != 0 && errno != ENOTTY) { perror("remove probe loop"); return 1; }
    if (dropped != -1 || dropError != EAGAIN || delivered <= 0 ||
        !memmem(packet, delivered, "SUBSYSTEM=block", sizeof("SUBSYSTEM=block"))) {
        fprintf(stderr, "Health uevent result filtered=%zd errno=%d control=%zd\n", dropped, dropError, delivered);
        return 1;
    }
    puts("Nestvane Health BPF: SDK filter reopened and rejected an actual block uevent");
    return 0;
}
