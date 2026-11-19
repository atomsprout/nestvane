# Android 17 属性初始化

2026-11-18。原版 Android 17 `init second_stage` 在 iPhone 11 的签名 Release App 中读取 Android 镜像的属性上下文和 build.prop，生成 768 个客体属性文件。第二个客体随后通过原版 Bionic 的 `__system_property_get` 读取这批文件，输出两行并退出 0。

| 属性 | iPhone 11 和 Android SDK 参考模拟器的输出 |
| --- | --- |
| `ro.build.version.sdk` | `37` |
| `ro.product.cpu.abilist` | `arm64-v8a` |

- [iPhone 11 验证摘要](p2-properties-iphone11.txt)
- [Android SDK 模拟器对照](p2-properties-reference.txt)
- [AOSP 属性初始化](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/property_service.cpp)
- [Bionic 属性区映射](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/system_properties/prop_area.cpp)

客体 `/dev` 的可写文件和 `MAP_SHARED` 映射在内存中共享字节。客体进程退出或解除映射后，文件内容仍可供同一运行会话里的读取器验证。SELinux 标签作为客体文件元数据保存，当前内核运行时没有 SELinux 策略执行。引导文件与生成的属性区都位于 App 内。

参考 SDK 镜像为 ARM64 API 37 构建 `CE2A.260420.051.A3`，采集的 43 个文件涵盖 ELF 闭包和五个分区的属性输入。读取器 SHA-256 为 `77496c09ef218bec405ff4b73acb236631fac30bd9e88058b0d904aae51d4c5c`。Mac 上的地址和未定义行为检测通过。移除该镜像的 build.prop 输入后，读取器退出 1，验证结果为失败。P1 与动态 ELF 回归仍通过。

init 在尝试向 `/apex` 挂载 `tmpfs` 时停止（syscall 40）。当前已验证早期属性区的生成和读取，后续还需要属性服务线程及 socket、APEX 挂载和启动脚本。
