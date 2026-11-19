# Android 17 首批服务启动

2026-11-18。iPhone 11（iOS 26.0.1）的签名 Release App 使用无宿主 JIT 的客体运行时。原版 Android 17 init 通过 pipe2 协调子进程，通过 fork 和 execve 启动 vendor 子上下文及 ueventd。两者均到达新 ELF 的程序入口。原版 ueventd 输出 `ueventd started!` 并开始解析 `/system/etc/ueventd.rc`。

- [iPhone 11 两次运行日志](p2-services-iphone11.txt)
- [AOSP 服务启动实现](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/service.cpp)

客体 exec 保留 argv、环境、进程身份和允许继承的描述符。客体进程持有上下文标签，用于显式的 exec 标签和 Unix 对端标签读取。当前运行时没有加载 Android SELinux 策略。执行器在新 ELF 动态加载期间延长调度时间片，进入程序或阻塞后恢复常规调度。

采集的 165 个文件来自 ARM64 API 37 SDK 镜像 `CE2A.260420.051.A3`。原版 `ueventd` 在该镜像中链接到 `init`，运行时复现该别名，并使用原始 argv[0] 选择 ueventd 入口。采集器同时准备了原版 `apexd` 和链接依赖，apexd SHA-256 为 `bfeae4faa577ae3369ddb647b645ca17b74194c2e2053ff8fa41488674a9721f`。

本次记录中，`apexd-bootstrap` 在 init 启动前的安全上下文查询处失败，原版 init 随后请求客体重启。探针停在 guest syscall 142。APEX 激活、完整系统启动和 APK 运行尚待验证。Mac 回归覆盖管道传输、描述符复制与 EOF，以及 exec 失败保留原进程状态。P1 与 Bionic 属性读取仍通过。

后续真机运行已进入原版 apexd，并扫描到真实 Runtime APEX。见 [Runtime APEX 探针](P2-apex.md)。
