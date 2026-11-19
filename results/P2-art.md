# P2 原版 ART 启动探针

2026-11-18。在不依赖宿主 JIT 的 iPhone 11 开发签名 Release App 上启动 ARM64 Android 17 SDK 镜像的原版 `dalvikvm64`。探针用 `-Xint` 从原版核心 JAR 初始化 ART。Java 验收程序在参考 SDK 模拟器上计算并打印 42。

- iPhone 11 上的进程 6440 完成了原版核心 JAR 的打开、低地址堆和主线程栈初始化，执行约 2.14 亿条客体指令，耗时 95.469 秒。这次运行停在客体 `process_vm_readv`。同一轮的原版 init、持久化客体 `/data` 和 vdc checkpoint RPC 也通过了。[真机日志摘录](p2-art-iphone11.txt)
- 后续真机进程 6445 已通过 ART 对自身客体地址空间的 `process_vm_readv`，继续到公共库预加载。该次真机停在缺少图形 HIDL 原版库，耗时 99.743 秒。Mac 已采齐带版本名的 HIDL 库并推进到后续 ELF 映射。采集器现在按 SDK 的原始公共库清单拉取依赖闭包，保留原版链接器命名空间。当时仍待验证 Java 输出。[AOSP 公共库预加载](https://android.googlesource.com/platform/art/+/refs/heads/main/libnativeloader/library_namespaces.cpp)
- Mac 随后用当前 541 文件资源集越过了该图形依赖，把 ART 推进到 3.5 亿条客体指令的实验上限。该轮耗时 85.944 秒，停止时的客体私有映射约 350 MiB。该轮扩大了客体内存配额。[Mac 诊断摘录](p2-art-host.txt)

## 首个 Java 程序在 iPhone 11 正常退出

- iPhone 11 开发签名 Release 构建以 `-Xint` 启用原版 ART 的解释执行，加载原版 Java 核心库和时区资源，执行自有 DEX，输出 `nestvane ART computed 42` 并退出为状态 0。进程 6469 执行了 510,957,685 条客体指令，探针耗时 19.002 秒。该时间包含原版 ART 探针的客体执行和跟踪开销。进程 6470 重启复测同样正常退出，耗时 18.403 秒。两次设备热状态均为 nominal，独立原版 init 探针均完成了两个 vdc checkpoint RPC，且读回并提交了持久化客体数据。[真机日志摘录](p2-art-success-iphone11.txt)
- Mac 对完整 Java 探针进行了两次复测，输出和退出状态均一致，耗时 7.281 和 7.199 秒。切换为有序客体映射查找后，Mac 在同一轮 ARM64 指令覆盖下，从约 96 秒降到约 5.3 秒到达约 3.61 亿条指令处的同一停止点。Mac 的 ABI 回归也通过了线程优先级、futex 重排与同步信号等待测试。[Mac 日志摘录](p2-art-success-host.txt)
- ART 与 init 目前各自运行在验证 App 中的独立客体实例。下一项验收是在同一 Android 启动链路中启动原版 Zygote 和 `system_server`，继而验证 APK 安装及启动。
- 按 A64 主编码分组调度原有指令执行器。对照优化前的解释器，264,351 个指令字的支持状态、寄存器、客体内存和故障结果一致。Mac 上交替运行原版 ART 探针各 6 次，均输出 42 且退出 0，中位耗时从 7.855 秒降至 6.833 秒。Mac 整套启动中同一组 18,596 个 Zygote 预加载类从 78.838 秒降至 68.715 秒。宿主 ABI 回归通过。整机真机耗时还需用新包复测。
