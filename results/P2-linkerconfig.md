# P2 原版 linkerconfig 生成链接器配置

2026-11-18。iPhone 11（iOS 26.0.1）运行开发签名 Release App。原版 Android 17 init 在客体伪终端上完成日志转发，并从挂载的 Runtime APEX 执行原版 `linkerconfig`。ELF 的 `/system/bin/linker64` 解释器链接和 Bionic 库链接来自同一个 SDK 镜像。生成了 74,333 字节的系统链接器配置和 11,947 字节的 Runtime APEX 配置。探针读取 PID 1 启动命名空间里的实际文件，验证了系统默认搜索路径和 Runtime APEX 命名空间的搜索路径。

[真机记录](p2-linkerconfig-iphone11.txt)

- 客体 PTY 覆盖主从端分配、锁定、记录子进程输出、poll 和关闭事件。目录模式和所有者通过同一个目录节点保存，支持 Android 通过 `/proc/self/fd` 更新目录模式的路径。完整终端交互所需的行规程待实现。
- Mac 上的 ABI 回归和 ASan/UBSan 通过。来自同一 SDK 的解释器和库符号链接、三份分区配置及三份系统库清单可重新采集。Android 17 Runtime APEX 与独立 Bionic 属性读取仍通过。
- 原版 init 已完成 `perform_apex_config`，进入 `wait_for_coldboot_done`。本次运行中，ueventd 打开客体 `AF_NETLINK` 的内核设备事件通道时失败，所以完整启动仍在等待设备冷启动。[AOSP ueventd 的 coldboot 流程](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/ueventd.cpp)

后续真机已通过该等待。见 [客体设备冷启动](P2-coldboot.md)。
