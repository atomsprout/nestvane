# P2 原版 vold 启动

2026-11-18。iPhone 11（iOS 26.0.1）的开发签名 Release App 在无宿主 JIT 的客体中运行原版 Android 17 vold。服务进入参考域 `u:r:vold:s0`，动态链接成功并创建 `/dev/block/vold`。原版 init 同次运行进入了 `post-fs-data`。探针在该次真机运行执行了 350,000,000 条客体指令，耗时约 80.7 秒。

[真机日志](p2-vold-iphone11.txt)

- 使用 ARM64 API 37 SDK 镜像采集 204 个启动文件，新增 vold 系统链接闭包、分区文件上下文、参考 fstab 和参考策略的 vold 域转换。SELinux 用户空间加载了文件标签配置。完整内核 SELinux 执行仍待实现。
- 客体调度记录进程请求的 I/O 优先级，文件数据仍由内存后端提供。Mac 上的 AOT 和客体 ABI 回归通过。
- 本次真机运行在 vold 打开 `/dev/hwbinder` 时遇到 ENOENT。进到 `post-fs-data` 只证明 init 执行到该动作段。Binder 服务注册、userdata 持久化和 APK 仍待验证。[AOSP vold 启动流程](https://android.googlesource.com/platform/system/vold/+/refs/heads/main/main.cpp)
