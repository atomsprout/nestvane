# P2 Runtime APEX 激活

2026-11-18。iPhone 11（iOS 26.0.1）运行开发签名 Release App，宿主未启用 JIT。原版 Android 17 `apexd --bootstrap` 配置 loop 设备，挂载原版 Runtime APEX 的只读 ext4 载荷，并 bind 到 `/apex/com.android.runtime`。原版进程报告 `Activated 1 packages`。独立 Bionic 进程从活动挂载视图打开 `/apex/com.android.runtime/bin/linker64`，验证 ELF64 和 AArch64 标识，同时读取到 API 37 和 `arm64-v8a`。

[真机报告摘录和 Mac 对照](p2-apex-activation-iphone11.txt)

- 含包启动探针本次在 iPhone 11 用时约 25.8 秒，从进入解释执行到启动探针停下，约 1.43 亿条客体指令。Mac 的同一探针和已有 AOT 与客体 ABI 回归通过，客体 ABI 回归也通过 ASan 和 UBSan。
- Runtime APEX 来自同一 Android 17 SDK 镜像。loop 设备从原版 apexd 给定的偏移和长度读取 APEX 内的 ext4 载荷。Mac 上改动副本的 AVB 认证区后，原版 apexd 报告 `HASH_MISMATCH`，激活验证失败，Bionic 属性读取仍通过。[AOSP APEX 格式](https://source.android.com/docs/core/ota/apex)
- 当前实现覆盖只读、extent 格式 ext4 和启动所需的 loop 接口。可写 ext4、客体 dm-verity、更新 APEX 和完整 Android 启动待实现。当前仍有 ueventd 设备事件缺口。

同日后续构建把子进程退出通知传入 PID 1 的 signalfd。原版 init 在 iPhone 11 上报告 apexd-bootstrap 以状态 0 退出，随后用 wait4 回收 PID 6，并开始执行后续 early-init 动作。启动 linkerconfig 之前，日志包装器打开 `/dev/ptmx` 时收到 ENOENT。随后进程在 `/dev/sys` 的目录所有权设置处停止。[AOSP linkerconfig 启动路径](https://raw.githubusercontent.com/aosp-mirror/platform_system_core/main/init/builtins.cpp)
