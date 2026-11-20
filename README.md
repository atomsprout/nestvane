# nestvane

目标是在普通 iPhone 上无宿主 JIT、本地运行 Android 17。首版在 iPhone 11 验收 Android 桌面、APK 安装与运行，以及 Android 应用访问 iPhone 摄像头。

- [实施计划](PLAN.md) 包含首个真机原型、阶段验收和性能继续条件。
- [AOT 设计记录](AOT.md) 包含出发点、nestvane 第一版的执行路径和验证方法。

## P1 可行性原型

[真机验证结果](results/P1.md) 包含 iPhone 11、模拟器和 ARM64 Linux 的实测记录。

在 macOS 上安装 Xcode、XcodeGen 和 LLD，然后运行：

```sh
tools/validate-host.sh
tools/prepare-ios.sh
open nestvane.xcodeproj
```

用生成的 `nestvane` scheme 在 iPhone 或模拟器上运行。真机需要通过 Xcode 选择自己的开发签名团队。App 启动后会执行测试，并将报告写入 App 容器的 `Documents/validation.txt`。

独立 Linux 对照需要本机 `qemu-system-aarch64` 和可供 QEMU virt 启动的 ARM64 Linux 内核。把内核绝对路径交给 `tools/validate-linux.sh`。

## Android 17 动态 ELF 探针

[P2 真机结果](results/P2-linker.md) 记录了 iPhone 11 上的 Android 17 动态链接验证。测试程序使用原版链接器加载 Bionic，调用 `strlen` 和 `write` 后正常退出。

在单独目录备好同一 ARM64 Android 17 构建的 `linker64`、`libc.so` 和 `libdl.so`。运行 `tools/probe-dynamic.sh /absolute/path/to/android17-runtime` 在 Mac 上验证。再运行 `tools/prepare-ios.sh`，即可用 Xcode 构建包含该探针的 iOS 版本。

## Android 17 第二阶段 init 和属性探针

[属性验证](results/P2-properties.md) 记录了原版 `init` 生成属性区和独立 Bionic 客体读取属性。[线程验证](results/P2-threads.md) 记录了真机上的属性服务线程和 Unix socket 请求往返。[启动脚本验证](results/P2-early-init.md) 记录了原版 init 执行到 early-init 的真机进度。[服务启动记录](results/P2-services.md) 包含 vendor 子上下文和 ueventd 的真机结果。[Runtime APEX 探针](results/P2-apex.md) 记录了原版 apexd 在 iPhone 11 上启动、识别原版包和补齐 loop 接口前的位置。[Runtime APEX 激活结果](results/P2-apex-activation.md) 记录了同一只读载荷在 iPhone 11 上挂载，以及独立 Bionic 进程读取其中的 linker64。[链接器配置验证](results/P2-linkerconfig.md) 记录了原版 linkerconfig 在真机生成系统和 Runtime APEX 命名空间配置。[设备冷启动验证](results/P2-coldboot.md) 记录了原版 ueventd 在真机接收客体设备事件并完成 coldboot。[early-fs 验证](results/P2-early-fs.md) 记录了原版系统 BoringSSL 自检通过后进入 early-fs。[vold 启动验证](results/P2-vold.md) 记录了原版存储服务的启动。[Binder 验证](results/P2-binder.md) 记录了真机上的 binderfs 挂载、原版 servicemanager 的上下文管理器注册和 vold 的握手，以及客体间的传输测试。[vold RPC 验证](results/P2-vold-rpc.md) 记录了原版 vdc 在真机查找 vold 并完成两个 checkpoint RPC。[持久化 /data](results/P2-data.md) 记录了 iPhone App 终止重启及覆盖安装后的客体文件读回。

在相同 ARM64 API 37 SDK 模拟器上启用 `adb root`，把对应的模拟器序列号交给采集工具。用 `--readelf` 可以指定 LLVM 工具的路径。

```sh
tools/prepare-init-root.py emulator-5580
tools/probe-init.sh
tools/prepare-ios.sh
```

采集目录包含原版 init、依赖库、属性上下文、build.prop、各分区的 init 启动脚本、ueventd 配置、apexd 的链接依赖、Runtime APEX、解释器与 Bionic 的链接、链接器命名空间输入，以及从参考内核查询的启动策略数据。已有早期版本的目录可先移到别处，再重新采集。生成的 iOS 项目从 `generated/android17-init-root` 打包参考文件和自制属性读取器。

系统 ART、`odrefresh` 和 `dex2oat64` 的真机进度见 [系统 ART 验证](results/P2-system-art.md)。可选的 `--small-art-heap` 将采集镜像的编译器堆范围设为 16–96 MiB，便于验证受限设备的启动路径。

原版 Zygote 的离线诊断可用同版参考模拟器生成独立客体目录。此配置使用预编译 ART 启动镜像、关闭客体 ART JIT，并跳过网络启动，适合检查 ART 和框架引导。实测见 [启动记录](results/P2-network.md)。

```sh
tools/prepare-boot-image.py emulator-5580 --root generated/android17-init-root \
  --output generated/android17-framework-diagnostic --framework-diagnostic
tools/probe-init.sh generated/android17-framework-diagnostic generated/framework-diagnostic.sqlite3
```

如需验收 APK 安装和启动，先生成项目自有的计数器 APK，再构建包含框架应用和 APK 的客体目录。客体在完成启动后使用原版 `cmd` 安装并启动计数器。报告分别记录安装进程、启动命令和 Activity 创建结果。

```sh
tools/build-smoke-apk.sh
tools/prepare-boot-image.py emulator-5580 --root generated/android17-init-root \
  --output generated/android17-framework-smoke --framework-diagnostic --framework-apps \
  --smoke-apk generated/android17-smoke.apk
tools/prepare-property-reader.sh generated/android17-framework-smoke
```

可在上述镜像构建命令中加入 `--compiler-filter speed --precompile-framework-code`，在参考模拟器上同时预编译系统服务和 SystemUI。系统服务和 SystemUI 的产物合计约 146 MB。

使用带图形后端的 Mac 探针运行该目录，或把它作为 `generated/android17-init-root` 构建 iPhone App。[本地图形后端验证](results/P2-network.md)

## Android 17 原版 ART 探针

`tools/prepare-art-root.py` 从 ARM64 API 37 SDK 模拟器生成本地资源目录，`--d8` 指定 Android Build Tools 的 D8 路径。`tools/probe-dynamic.sh --art` 运行 Mac 探针。有该资源目录时，iOS 验证构建也会运行它。进度见 [ART 验证](results/P2-art.md)。
