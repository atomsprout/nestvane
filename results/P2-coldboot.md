# P2 客体设备冷启动

2026-11-18。iPhone 11（iOS 26.0.1）的开发签名 Release 构建在客体中启动原版 Android 17 ueventd。客体的 loop 驱动生成设备事件，原版 ueventd 通过 `NETLINK_KOBJECT_UEVENT` 接收，扫描客体 sysfs 并处理 `/dev/block/loop0`。原版 ueventd 报告完成 coldboot。原版 PID 1 读取到 `ro.cold_boot_done=true` 并继续启动。

[真机日志](p2-coldboot-iphone11.txt)

- 当前设备事件适配覆盖只读 loop 后端，不涉及 iPhone 的硬件设备。Mac ABI 回归验证了 netlink 消息、内核发送者信息和空队列行为。
- 同一真机运行继续通过 Runtime APEX 激活、原版 linkerconfig 生成和独立 Bionic 属性读取。此构建随后停在虚拟内核的 `mmap_rnd_bits` 接口缺失处。[AOSP ueventd 冷启动实现](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/ueventd.cpp)

后续真机已通过 mmap 熵检查和原版系统 BoringSSL 自检，进入 [early-fs](P2-early-fs.md)。
