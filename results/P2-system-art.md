# P2 系统 ART 与原版编译器

2026-11-18。Android 17 ARM64 SDK 镜像 CE2A.260420.051.A3，iPhone 11，iOS 26.0.1，Release 开发签名，无调试器。

- 原版 bootstrap 在同次启动激活 6 个 APEX，系统 ART 的 `art_boot` 正常退出。原版 `odsign` 通过 Keystore 和软件 KeyMint 初始化密钥。第一个真机构建的 `odrefresh` 因缺库退出，Bionic 读回 `odsign.key.done=1`、`odsign.verification.done=1`，校验成功属性未通过。[首次真机记录](p2-system-art-iphone11.txt)
- 补齐加载依赖后，真机上的 `odsign` 从上次启动保存的客体数据读回并验证密钥。原版 `odrefresh` 进入主程序，检查 boot image 并启动原版 `dex2oat64` 编译最小 boot classpath。独立 ART 测试程序仍能输出 `42`。本轮命中启动诊断限时，Zygote 尚未启动。[后续真机记录](p2-odrefresh-iphone11.txt)
- Mac 用同一基线复现。补齐了进程句柄和带 CLOEXEC 的文件描述符复制，修复了横跨相邻映射的客体读写。随后捕获最小 boot classpath 编译时 64 MiB 的映射申请被原有诊断配额拒绝。运行时限制启动 trace 的条目数，避免大量重复等待放大日志。[Mac 记录](p2-odrefresh-host.txt)

- 原版编译配置随后还请求了约 1 GiB 的客体映射。可选采集参数 `--small-art-heap` 只调整四项 ART 编译器堆属性，初始 16 MiB、最大 96 MiB，系统文件及二进制来自同一参考镜像。该配置下原版编译器请求的约 192 MiB 映射在 Mac 和 iPhone 11 上均成功，之后继续初始化 ART。已补齐这条路径暴露的向量 CLZ 和标量 MOV。此轮尚未产生可验收的 boot image，Zygote 和包管理都未验证。[Mac](p2-art-heap-host.txt) · [iPhone 11](p2-art-heap-iphone11.txt)
- 延长诊断时间后，同一小堆配置已在 iPhone 11 上实际编译 Java 方法，包括 `ConcurrentHashMap` 和 `BigDecimal`，日志中有 22 条方法计时。真机在 120 秒的诊断限时结束，Mac 抵达 50 亿条客体指令上限，并留下 5,216 字节的未完成 OAT 暂存文件。两个环境均未验证可用 boot image 或 Zygote 启动。[iPhone 11](p2-art-compile-iphone11.txt) · [Mac](p2-art-compile-host.txt)
- 使用更长的探针后，iPhone 11 和 Mac 的原版 `odsign` 均写入 `odsign.verification.done=1` 和 `odsign.verification.success=1`，由独立的 Bionic 进程读回。Mac 的客体快照保留了 1,452,208 字节的 `boot_minimal.art`、签名清单及其签名。当轮的主 boot classpath 产物仍不完整，原版签名路径可接受并签名部分产物。[iPhone 11](p2-art-signing-iphone11.txt) · [Mac](p2-art-signing-host.txt)
