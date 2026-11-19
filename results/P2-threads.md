# Android 17 属性服务线程

2026-11-18。iPhone 11（iOS 26.0.1）运行原版 Android 17 第二阶段 `init`。两个由 Bionic 创建的属性服务线程使用各自的寄存器、栈、TLS 和线程 ID，在同一客体地址空间注册并等待读事件。观察到：

- 客体 TID 2 等待 `/dev/socket/property_service_for_system`。
- 客体 TID 3 等待 `/dev/socket/property_service`。
- 原版 `init` 自身以 Bionic 属性客户端发出请求。TID 3 接受连接、读取请求并向客户端返回 4 字节响应。当前客体未提供 SELinux 对端上下文，原版服务因此拒绝写入 `sys.usb.configfs` 和 `ro.kernel.version`。已有属性读回仍为 API 37 和 `arm64-v8a`。

主线程读到了运行时生成的 `/proc/mounts`，并设置了根挂载、`/apex` 和 `/linkerconfig` 的传播属性。当前停止于 `/mnt/user` 到 `/mnt/installer` 的递归绑定挂载。后续仍需绑定挂载、挂载命名空间及 APEX 激活。

- [iPhone 11 两次启动摘要](p2-threads-iphone11.txt)
- [AOSP 属性服务线程与请求处理](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/property_service.cpp)
- [AOSP 挂载命名空间](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/mount_namespace.cpp)

Mac 回归覆盖 `clone` 父子返回值、TID/TLS、futex 等待与唤醒、线程退出清除 TID，以及 Unix stream 监听、接受和双向传输。P1、动态 ELF、挂载及属性读取检查仍通过。
