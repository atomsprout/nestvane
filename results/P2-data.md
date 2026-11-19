# P2 持久化客体 /data

2026-11-18。iPhone 11（iOS 26.0.1）上的开发签名 Release App。客体的 `/data` 通过 SQLite 事务保存到 iOS App 的 Application Support。客体看到普通文件路径并使用 Linux 文件系统调用。

- 首次运行的原生 AArch64 客体写入 24 字节，经 `renameat` 改名并通过 `fsync` 提交。终止 iOS App 后启动的新进程读回并逐字节验证。覆盖安装后第三个进程仍读到同一份内容。
- 原版 Android 17 init 在同一客体挂载上创建了启动目录，新进程的原版 init 再次读到了这些目录。Binder 服务就绪和两个原版 vdc checkpoint RPC 在三次运行中都通过。[真机日志摘录](p2-data-iphone11.txt)
- Mac 回归覆盖非 root 文件所有权、重启读取、重命名和删除、旧写入者冲突保护。SQLite 同步目前会提交文件系统快照，单文件上限 64 MiB。Android 的文件级加密、目录重命名和 APK 安装持久化仍待验证。
