# Android 17 第二阶段 init 启动检查

2026-11-18。iPhone 11（iOS 26.0.1）的签名 Release App 在 guest PID 1 运行 `/system/bin/init second_stage`。原版 bootstrap 链接器加载并重定位原版依赖库，约 2,000 万条客体指令后跳转到 init 的 ELF 入口。客体随后在虚拟 `/dev/kmsg` 写出 `init second stage started!`。探针连续三个进程均观察到启动日志，执行到停止点用时 1.8–3.2 秒。

- [一次完整 iPhone 11 报告](p2-init-iphone11.txt)
- [三次独立运行摘要](p2-init-repeats.txt)
- [AOSP 第二阶段入口](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/main.cpp)
- [AOSP 第二阶段实现](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/init.cpp)

当前客体根目录由采集工具按 ELF 依赖生成，共 33 个文件。它使用 ARM64 SDK API 37 镜像 `CE2A.260420.051.A3`，与动态 ELF 探针输入同源。客体的 `/dev/null` 与 `/dev/kmsg` 是 App 内的虚拟设备，日志从后者进入报告。

| 路径 | SHA-256 |
| --- | --- |
| `/system/bin/init` | `af80b8bc833f6b4a6e55ea486a0919983558bcda32fef18da9037ebbd48035ca` |
| `/system/bin/bootstrap/linker64` | `86333b5e9cb26e4dc4dd4339ed8d2033dd5932b30f89632266e88065dde65653` |

当前执行在 syscall 39（`umount2`）处停止。验证覆盖进入第二阶段函数和启动日志。属性服务、启动脚本和系统服务仍待验证。Mac 的地址及未定义行为检测器通过了动态 ELF 与 init 检查。iPhone 与 Mac 的 P1 回归仍通过。
