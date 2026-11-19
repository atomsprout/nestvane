# P2 Android 用户 0 与类路径

2026-11-18。原版 Android 17 SDK 镜像，iPhone 11 开发签名 Release 构建。

- 原版 `vold` 执行 `vold_prepare_subdirs`，随后原版 `vdc --wait cryptfs init_user0` 退出为 0。客体的目录创建标签由参考策略的 `setfscreate` 决策控制，并随客体数据保存。客体支持移动用户数据目录及更新已打开目录的路径。
- 原版 SDK Extensions APEX 的 `derive_sdk` 和 `derive_classpath` 进入执行。真机上的 `derive_classpath` 写入临时文件、重命名成功、以状态 0 退出，随后原版 `init` 读取最终文件 `/data/system/environ/classpath`。同次通过独立 ART、Keystore 启动属性、持久化客体数据和 checkpoint RPC。[真机日志](p2-user0-iphone11.txt)
- Mac 上从空数据及复用数据各启动了一次。两次用户 0 和类路径进程都正常退出。持久化快照包含真实用户目录和导出文件，当前文件包含 10 个 boot classpath 条目和 2 个 system server classpath 条目。宿主 ABI 回归及 ASan/UBSan 通过。[Mac 日志](p2-user0-host.txt)
- 现有启动镜像只包含所采集的部分框架片段。系统 ART APEX、`art_boot`、`odsign` 和 Zygote 尚待接入。
