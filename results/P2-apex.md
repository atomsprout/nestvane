# P2 Runtime APEX 启动探针

2026-11-18。iPhone 11（iOS 26.0.1）的开发签名 Release App 在无宿主 JIT 的模式下启动原版 Android 17 `apexd --bootstrap`。客体进入 `u:r:apexd:s0`，识别并扫描到同一 SDK 镜像的 `/system/apex/com.android.runtime.apex`。独立 Bionic 进程仍读出 API 37 和 `arm64-v8a`。未装入 APEX 包的对照构建也在真机上完成了空目录扫描。含包探针本次用时 25.001 秒，计时区间覆盖约 1.37 亿条客体指令、调度及 loop 重试。

[两次真机报告摘录](p2-apex-iphone11.txt)

- 采集脚本从 API 37 ARM64 SDK 镜像 `CE2A.260420.051.A3` 采集 166 个 Android 文件。从参考 SELinux 内核查询一个 exec 域转换和 1,528 条属性访问决策，客体对覆盖范围内的查询返回对应的允许或拒绝结果。此处只复现启动所需的查询接口。完整 SELinux 策略加载和内核权限执行待实现。
- Runtime APEX SHA-256 为 `9d57884494326e12a5afffce1e0841a40bfcb5610f65766759a8f5829b0d9cd5`。包内的 `apex_payload.img` 是从文件偏移 4,096 开始的 8,744,960 字节未压缩 ZIP 条目，具有 ext4 超级块及 AVB footer。参考 loop 设备的偏移和长度与该条目一致，逻辑块为 4,096 字节。参考模拟器用同一只读 ext4 loop 设备挂载了 `/apex/com.android.runtime@1` 和 `/apex/com.android.runtime`。[AOSP APEX 格式](https://source.android.com/docs/core/ota/apex)
- 当前真机在打开 `/dev/loop-control` 时收到 `ENOENT`，也报告 `/dev/device-mapper` 缺失。激活数量为 0。下一项验收是让客体挂载真实只读载荷，确认 `apexd` 报告激活 1 个包，并让独立客体读取挂载后的文件。
- Mac 上复跑采集器，属性查询表和 Runtime APEX 与首次采集一致。P1 五项正确性测试、两项性能门槛及客体 ABI 回归通过。ABI 回归也通过了 ASan 和 UBSan。

后续验证已完成真实载荷的挂载和读取。见 [Runtime APEX 激活结果](P2-apex-activation.md)。
