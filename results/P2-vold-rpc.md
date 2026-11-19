# P2 原版 vold 注册和调用

2026-11-18。在 iPhone 11（iOS 26.0.1）的开发签名 Release App 上，使用同一份 ARM64 Android 17 API 37 SDK 镜像的原版二进制，无宿主 JIT。

- 独立 Bionic 属性读取器读到 `servicemanager.ready=true`。原版 `vold` 发布服务对象，两个原版 `vdc` 进程分别通过服务名取得 Binder 句柄，再用句柄直接向原版 `vold` 发起 RPC。
- 原版 init 的 `vdc checkpoint markBootAttempt` 和 `vdc checkpoint prepareCheckpoint` 都退出为状态 0。独立的原版 `service check vold` 在 shell 域收到参考 SELinux 策略的 `find` 拒绝。执行 350,000,000 条客体指令，真机耗时 61.451 秒。[真机日志](p2-vold-rpc-iphone11.txt)
- Mac 回归覆盖共享 futex 跨地址唤醒、timerfd、服务访问类、Binder 对象与句柄转换、对象偏移校验和死亡通知，并通过 AddressSanitizer 和 UndefinedBehaviorSanitizer。
- 当前 Binder 对象支持普通强引用和测试覆盖的生命周期。文件描述符、完整引用回收、完整内核 SELinux 和持久化 userdata 仍待实现。这些原版 checkpoint 命令验证了服务发现及 RPC 路径，当前镜像未挂载真实 userdata。
