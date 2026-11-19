# Android 17 动态链接探针

2026-11-18。iPhone 11（iOS 26.0.1）上的 nestvane Release App 在无宿主 JIT 的客体解释器中运行了 Android 17 原版 `linker64`。链接器自行打开并映射 `libc.so` 与 `libdl.so`，完成重定位和 Bionic 初始化，把控制交给客体 ELF 的 `_start`。程序经由 Bionic 的 `strlen` 和 `write` 输出 `nestvane bionic ready!`，进程退出码为 0。

| 环境 | 结果 |
| --- | --- |
| 官方 Android 17 ARM64 模拟器，API 37 | 相同测试程序输出同一行并退出 0。 |
| iPhone 11，签名 Release 构建 | 首次安装后一次启动和随后三次进程重启均通过。记录到 464,767–464,970 条客体指令和 214–217 次客体系统调用。 |
| Mac，AddressSanitizer 和 UndefinedBehaviorSanitizer | 12 次均通过，无 sanitizer 报告。 |
| P1 回归 | iPhone 和 Mac 的五项原有检查及两项吞吐门槛均通过。 |

- [iPhone 11 App 完整报告](p2-iphone11.txt)
- [iPhone 11 四次运行摘要](p2-iphone11-repeats.txt)
- [Android 17 对照结果](p2-android37-reference.txt)

探针采用只读的客体文件映射、单任务 Linux ABI 和 Android 链接器的默认搜索路径。客体没有挂载属性区、生成的 linkerconfig 或 `libnetd_client.so`，控制台中的 linkerconfig 警告来自链接器。当前验证范围是动态链接的原生 ELF。下一步是准备客体根文件系统并推进第二阶段 `init`。

## 输入和复现

使用 SDK 的 `system-images;android-37.0;default;arm64-v8a` revision 6，构建号 `CE2A.260420.051.A3`，SDK XML 公布的 zip SHA-1 为 `f94cb780748169b5944b7c2e5c0693e5077ec8c4`。[Android 17 模拟器说明](https://developer.android.com/about/versions/17/get)

| 文件 | SHA-256 |
| --- | --- |
| `/apex/com.android.runtime/bin/linker64` | `15190ce5c2094d06e82db97cb98162f970e956ffb15a725e336a2bcc91576605` |
| `/apex/com.android.runtime/lib64/bionic/libc.so` | `7df642b39c34c6e67e5a729239be59bb378ea06a91f398d37cfe7495afc4d6a8` |
| `/apex/com.android.runtime/lib64/bionic/libdl.so` | `0481ad1349734ab79535f84d416a022b48403aa0088aaa3dfb584bc7cd783b63` |
| `android17-dynamic.elf` | `66fd076f883a5770b132500867fe22543f5a69b59500bb3f0049fb25375d7e1f` |

从相同构建取出三份运行库，放在同一目录下，保留原文件名。macOS 需要 Xcode 和 LLD，iOS 项目生成还需要 XcodeGen。

```sh
tools/probe-dynamic.sh /absolute/path/to/android17-runtime
tools/prepare-ios.sh
```

用 Xcode 的 `nestvane` scheme 在设备上运行。应用把结果写到 `Documents/validation.txt`。SDK 文件和生成的测试资源位于 Git 忽略的 `generated/`。
