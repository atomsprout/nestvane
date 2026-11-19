# Android 17 早期挂载与属性 socket

2026-11-18。iPhone 11（iOS 26.0.1）上的原版 Android 17 `init` 在客体内完成 `tmpfs` 到 `/apex`、`/linkerconfig` 的挂载。它创建并注册了 `epoll`、`signalfd` 和通知用的 `eventfd`，随后建立 Unix `SOCK_SEQPACKET` pair，绑定并开始监听 `/dev/socket/property_service_for_system`。属性读取回归继续通过。

- [iPhone 11 启动记录](p2-mounts-iphone11.txt)
- [Android init 第二阶段挂载实现](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/init.cpp)
- [Linux ARM64 文件标志](https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/include/uapi/asm/fcntl.h)
- [Linux epoll 数据布局](https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/eventpoll.h)

实现维护 App 内的挂载、描述符和本地 socket 状态。Mac ABI 检查覆盖挂载前的只读限制、挂载属性、`eventfd` 计数与 `epoll` 可读事件，以及 Unix seqpacket 的传输和关闭通知。P1 和 Bionic 属性读取仍通过。

当前在 syscall 220（`clone`）处停止，参数为 `0x3d0f00`，发生在属性服务创建工作线程时。当前挂载为后续 APEX 留出位置，APEX 包激活、属性服务线程响应及 Android 服务启动仍待验证。
