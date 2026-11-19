# P2 Android 17 early-fs

2026-11-18。iPhone 11（iOS 26.0.1）的签名 Release App 在无宿主 JIT 的客体中执行了 Android 17 原版 `/system/bin/boringssl_self_test64`。参考内核查询的域转换使它以 `u:r:boringssl_self_test:s0` 运行。原版 PID 1 回收该进程，报告退出状态 0，执行 `late-init` 并进入 `early-fs`。

[真机报告摘录](p2-early-fs-iphone11.txt)

- 客体的 mmap 分配基址在进程创建和 exec 时从 24 位页号空间随机化。`mmap_rnd_bits` 会控制后续地址空间，超出客体支持范围的写入返回 EINVAL。Android 通过真实 sysctl 读回进入下一阶段。当前 ELF 和栈的初始映射仍按固定布局加载。[AOSP init 的 mmap 熵检查](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/security.cpp)
- 复跑启动输入采集器，从同一 ARM64 API 37 SDK 镜像采集 177 个 Android 文件。原版系统自检通过。相同镜像的 vendor 自检服务及依赖已采集，当前 vendor 触发条件在属性访问检查处被拒绝，因此本次日志只证明系统自检通过。
- 该构建在 `early-fs` 启动 `vold` 时请求重启。客体输入还未包含该服务。参考镜像的 `vold` 使用 `/system/bin/linker64`，有 28 个直接动态库依赖，包含 Binder 和存储相关库，参考 fstab 为 `/vendor/etc/fstab.ranchu`。完整 userdata、Zygote 和 APK 启动仍待验证。

后续真机已进入原版 vold。见 [vold 启动验证](P2-vold.md)。
