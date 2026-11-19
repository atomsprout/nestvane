# P2 客体 Binder 启动

2026-11-18。iPhone 11（iOS 26.0.1）的开发签名 Release App 在无宿主 JIT 的客体中运行 ARM64 API 37 SDK 镜像中的原版 Android 17 组件。

- 原版 init 挂载客体 `binderfs`，创建三条设备符号链接。原版 `servicemanager`、`hwservicemanager` 和 `vold` 都进入各自程序，并带有参考镜像中的服务进程域。
- 原版 `vold` 打开 `/dev/hwbinder` 和 `/dev/binder`，两次 `BINDER_VERSION` 均成功，并分别建立两个只读的接收缓冲区。该轮执行 350,000,000 条客体指令，真机耗时 64.743 秒。[真机日志](p2-binder-iphone11.txt)
- 原版 `servicemanager` 的 `BINDER_SET_CONTEXT_MGR_EXT` 和首次 `BINDER_WRITE_READ` 均返回成功。它随后报告服务名自注册失败，并停在未实现的 `timerfd_create`。
- Mac 的客体 ABI 回归用两个客体进程验证了 Binder 句柄 0 的同步请求及回复、只读接收缓冲区和驱动填写的调用者身份。回归也通过了 AddressSanitizer 和 UndefinedBehaviorSanitizer。Binder 数据结构和 ioctl 对照 [Android common kernel Binder UAPI](https://android.googlesource.com/kernel/common/+/refs/heads/android-mainline/include/uapi/linux/android/binder.h)。
- Mac 后续启动验证补通了 `/proc/<pid>/attr/current`。原版服务管理器可以读取自身上下文，进一步发现自注册还缺参考镜像的 `service_manager` 访问类。
- 该轮 Binder 实现只支持纯数据 Parcel 的传输。后续的对象传递和真机 RPC 验证见 [vold 注册和调用](P2-vold-rpc.md)。完整对象引用和文件描述符传递、`vold` 注册、持久化 `/data`、Zygote 和 APK 启动仍待验证。
