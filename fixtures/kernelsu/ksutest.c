// What KernelSU's ksud does to get root (KernelSU 3.3.1): ask for the driver with reboot and two
// magic numbers, read what it is, and ask it for root; then show who it is. One line for each step.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = -1;
    long result = syscall(SYS_reboot, 0xDEADBEEF, 0xCAFEBABE, 0, &fd);
    printf("reboot %ld errno %d fd %d\n", result, result < 0 ? errno : 0, fd);
    if (fd < 0) return 1;
    char path[64], target[128] = {0};
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    readlink(path, target, sizeof target - 1);
    printf("link %s\n", target);
    uint32_t info[4] = {0};
    result = ioctl(fd, 0x80104b02, info);
    printf("info %ld version %u flags %u features %u uapi %u\n", result, info[0], info[1], info[2], info[3]);
    struct { uint16_t count, total; uint32_t uids[8]; } list = {8, 0, {0}};
    result = ioctl(fd, 0xc0044b06, &list);
    printf("allow list %ld errno %d\n", result, result < 0 ? errno : 0);
    printf("before uid %d\n", getuid());
    result = ioctl(fd, 0x00004b01, 0);
    printf("grant %ld errno %d\n", result, result < 0 ? errno : 0);
    char context[64] = {0};
    FILE* attr = fopen("/proc/self/attr/current", "r");
    if (attr) { fread(context, 1, sizeof context - 1, attr); fclose(attr); }
    printf("after uid %d euid %d context %s\n", getuid(), geteuid(), context);
    FILE* packages = fopen("/data/system/packages.list", "r");
    printf("packages.list %s\n", packages ? "readable" : strerror(errno));
    if (packages) fclose(packages);
    return 0;
}
