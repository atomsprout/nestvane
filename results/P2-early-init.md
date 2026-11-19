# Android 17 启动脚本与早期动作

2026-11-18。iPhone 11（iOS 26.0.1）上的签名 Release App 在无宿主 JIT 的情况下运行原版 Android 17 第二阶段 init。init 完成 /mnt 的递归绑定挂载并输出 `SetupMountNamespaces done`，随后打开原版 `/system/etc/init/hw/init.rc` 和多个分区的启动脚本，进入 `early-init`。它生成了 bootstrap 与 default 的链接器配置，并成功将 `/linkerconfig/bootstrap` 绑定到 `/linkerconfig`。

- [iPhone 11 运行摘要](p2-early-init-iphone11.txt)
- [AOSP 挂载命名空间](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/mount_namespace.cpp)
- [AOSP init 子上下文](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/subcontext.cpp)

参考镜像仍为 ARM64 API 37 `CE2A.260420.051.A3`。采集器现提取 155 个公开参考镜像文件，其中 112 个为原版 .rc 文件。运行时开始解析脚本时会输出日志，进口文件不存在时也会记录，日志条数因此与成功打开的文件数不同。采集产物仍位于 Git 忽略的 generated/。

Mac ABI 测试验证了通过绑定目录修改源文件、向祖先目录绑定子目录、通过 unshare 及 setns 在隔离的挂载视图之间切换，以及 fork 后的私有内存、描述符表和 waitid 状态。两个挂载视图仍可引用同一文件存储。跨挂载命名空间的传播语义仍有缺口。

主进程当前停止在 `pipe2`（syscall 59）。其 vendor 子进程在设置 SELinux 执行上下文失败后被探针暂停，主进程继续执行。子进程 exec、APEX 激活、常规系统服务和 APK 仍待验证。P1 与 Bionic 属性读取回归继续通过。
