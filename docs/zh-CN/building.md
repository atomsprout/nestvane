# 构建

这篇文档列出构建需要什么，以及各步骤的顺序。
命令取自脚本自己的用法说明；运行之前请先读脚本，其中有几个假定了特定的机器布局。

完整的构建并不能只靠这个仓库完成。
它需要一份 AOSP 源码和一台用来编译它的 Linux 机器、一个运行编译结果的参考模拟器，以及 gfxstream 和 ANGLE 的源码。
下文把这些逐一标为**不在仓库里**。
没有一条命令能完成整个构建，也没有持续集成；各步骤都是手工运行的，结果记在 [`results/G0.md`](../../results/G0.md) 里。

## 需要什么

| 条件 | 用途 |
| --- | --- |
| 装有 Xcode 的 macOS | 构建 App，以及用 Apple 的 `clang` 汇编翻译后的代码。 |
| [XcodeGen](https://github.com/yonaskolb/XcodeGen) | 由 [`project.yml`](../../project.yml) 生成 `nestvane.xcodeproj`。 |
| LLD（`PATH` 里有 `ld.lld`） | 链接 [`fixtures/`](../../fixtures) 下的小型客体测试程序。 |
| Python 3 | 构建脚本。 |
| Android SDK 的构建工具（`aapt2`、`zipalign`、`apksigner`、`d8`） | 构建资源覆盖包和测试应用。 |
| Android SDK 的模拟器和 `adb` | 参考模拟器。 |
| 一台 Linux 构建机和 `android-17.0.0_r1` 的 AOSP 源码——**不在仓库里** | 带保留寄存器构建 Android。记录里用的是一台 32 核、运行 Ubuntu 24.04 的云主机。 |
| 与工具链版本对应的 LLVM 源码、`toolchain/llvm_android` 和一份 NDK——**不在仓库里** | 重编工具链自带的预编译运行时库。 |
| gfxstream 和 ANGLE 的源码——**不在仓库里** | 渲染器。 |
| 具备 Extended Virtual Addressing 和 Increased Memory Limit 能力的 Apple 开发团队 | 给 App 签名（[`ios/Native.entitlements`](../../ios/Native.entitlements)）。 |
| iOS 26 或更高版本的设备 | 运行。 |

## 第 1 步：带保留寄存器构建 Android

在 Linux 机器上的 AOSP 源码树里进行。脚本在 [`experiments/aosp/`](../../experiments/aosp)。

对源码树应用修改（21 个文件上的 26 处修改）：

```
usage: patch.py <aosp root>      (safe to run again; every edit checks whether it is already applied)
```

把 AOSP 当作预编译件使用的运行时库（compiler-rt builtins、libunwind、libc++、libc++abi、最小版 UBSan 运行时）按同样的保留寄存器重新编译，并覆盖安装到预编译件的位置：

```
build-runtimes.sh <aosp root> <llvm-project at the toolchain's base revision> <llvm_android> <ndk> <clang version>
```

构建。脚本选择的产品是 `sdk_phone16k_arm64-cp2a-userdebug`，即 16 KiB 页的模拟器镜像；它假定源码在 `~/aosp`，日志写到 `~/logs`：

```
build.sh <log name> [out directory inside the tree] -- <m targets...>
```

检查结果里是否还有点名使用保留寄存器的代码：

```
usage: audit.py <tree> [--top N]
```

补丁还改了构建配置，让启动镜像、system_server 和应用全部事先完整编译（`speed`）；因为没有 JIT，没编译到的方法就只能一直解释执行。

## 第 2 步：在参考模拟器里启动镜像

**不在仓库里：** 在 Mac 上的 Android 模拟器里运行构建出的镜像，并且能用 `adb root`。
仓库里没有用自定义镜像启动模拟器的脚本。
同一份镜像在那里能正常开机，因为在真实内核上，打过补丁的动态链接器找不到地址表，行为与原版一致。

模拟器有两个用途：后面两个脚本从它那里采集文件和策略决定，它也是判断行为是否正确的参照。

## 第 3 步：采集启动输入

[`tools/prepare-init-root.py`](../../tools/prepare-init-root.py) 从运行中的模拟器里取出客体开机所需的东西：`init`、各个库、属性上下文、各分区的 init 脚本、APEX 模块，以及向参考内核查询得到的 SELinux 决定（写在 `nestvane-metadata/` 下）。

这个脚本没有自己的用法说明；它的参数解析器接受一个位置参数 `serial`，以及选项 `--readelf`（默认 `llvm-readelf`）、`--small-art-heap` 和 `--output`。早期原型的说明（[`results/PROTOTYPE.md`](../../results/PROTOTYPE.md)）里是这样运行它的：

```
tools/prepare-init-root.py emulator-5580
```

`serial` 是模拟器的 `adb` 序列号，例如 `emulator-5580`。默认输出到 `generated/android17-init-root`；输出路径必须尚不存在。

## 第 4 步：组装客体根目录

[`tools/prepare-boot-image.py`](../../tools/prepare-boot-image.py) 生成客体启动所用的目录：加入框架应用（桌面、SystemUI、Settings、包安装器等），取用编译好的代码，补上客体需要的服务，去掉客体没有的硬件的声明。

与这里有关的参数，附脚本参数解析器里的说明文字：

```
serial
--root ROOT                  Unpacked boot input used to check the reference build
--output OUTPUT
--framework-diagnostic       Produce a complete offline diagnostic guest root using the compiled image
--framework-apps             Include launcher, SystemUI and package-management apps from the matching reference
--prebuilt-code              The image carries compiled code for its own collector (a build patched by
                             experiments/aosp/patch.py): keep its boot image and take its compiled code
--tethering-loader PATH      Use the output of tools/build-netbpfload.py and run the standard BPF init actions
```

`--prebuilt-code` 是针对第 1 步所得镜像的选项；脚本要求它与 `--framework-diagnostic` 同用，`--framework-apps` 同样要求 `--framework-diagnostic`。
`prepare-ios-native.sh` 的文件头把这个顺序写作“`tools/prepare-init-root.py`, then `tools/prepare-boot-image.py --prebuilt-code`”。
发布的构建所用的确切命令行，没有写在任何脚本里。

[`tools/build-netbpfload.py`](../../tools/build-netbpfload.py) 构建 Android 网络 eBPF 程序的加载器，供 `--tethering-loader` 使用。它从 AOSP 仓库拉取源码，需要 `--serial`、`--toolchain`（NDK 的工具链目录）和 `--output`。

## 第 5 步：构建覆盖包和桌面应用

```
usage: build-overlays.sh <root>...   (needs the Android SDK build tools; signs with the debug key)
```

[`tools/build-overlays.sh`](../../tools/build-overlays.sh) 编译 `fixtures/overlay-*` 下的资源覆盖包，作为 product 分区的静态覆盖包放进客体根目录；它们不必重新构建框架就能改变框架的设置（例如固定的网络配置）。
它还构建 [`fixtures/home`](../../fixtures/home)，一个没有代码的应用，为桌面提供时钟和默认布局。
SDK 的位置取自 `ANDROID_HOME`，没有设置时是 `~/Library/Android/sdk`。

## 第 6 步：翻译代码并生成 Xcode 工程

```
usage: prepare-ios-native.sh <guest root> [<tree with unpacked APEXes>...]
```

[`tools/prepare-ios-native.sh`](../../tools/prepare-ios-native.sh) 依次做这些事：

1. 以 16 KiB 页构建在客体里运行的几个小工具（[`tools/prepare-property-reader.sh`](../../tools/prepare-property-reader.sh)）；
2. 构建测试夹具（[`tools/build-fixtures.sh`](../../tools/build-fixtures.sh)）；
3. 为 iOS 设备翻译客体根目录里的每个 ELF 文件；
4. 把客体根目录复制到 `generated/native-app/android17-init-root`，App 工程从那里把它作为资源收进去；
5. 运行 `xcodegen generate --spec project.yml`。

第 3 项的翻译就是 [`tools/build-native-code.py`](../../tools/build-native-code.py)：

```
usage: build-native-code.py <output directory> <tree>... [--only <substring>]... [--ios] [--used <list>]
```

它写出 `<output>/modules.c`、`<output>/addresses.txt`，在 iOS 目标下还有 framework `<output>/ios/lib/NestvaneCode<n>.framework`。
第二个及以后的 `<tree>` 参数是构建产物里解开的 APEX 模块目录：其中的文件只需与客体启动所用镜像里的字节相同，因为模块按内容匹配。
只要文件、它的地址和改写器都没变，翻译就在多次运行之间保留。脚本自己的注释说，全部翻译一遍要一刻钟。

`--used <清单>` 让一部分翻译不进 framework。清单由运行时在 `NESTVANE_NATIVE_USED=1` 下打印的 `native module used: <key> <base> <name>` 行组成。`prepare-ios-native.sh` 不传这个选项；`results/G0.md` 里记录的那个更小的构建，是带着它直接运行 `build-native-code.py` 得到的。

## 第 7 步：构建渲染器

**不在仓库里：** gfxstream 和 ANGLE 的源码及其构建。
仓库里有的是：

- [`tools/patches/`](../../tools/patches) 里给 gfxstream 的补丁，记录里说按加入的先后顺序应用；
- 面向 iOS 设备、只带 Metal 后端的 ANGLE 构建所用的 GN 参数 [`tools/angle-iphoneos.gn`](../../tools/angle-iphoneos.gn)；
- 当时使用的版本和命令，记在 [`results/P2-network.md`](../../results/P2-network.md) 里（中文）。

## 第 8 步：构建 App 并嵌入渲染器

打开 `nestvane.xcodeproj`，为真机构建 `nestvane-native` scheme，在 Xcode 里选择开发团队。
这个目标链接 12 个 `NestvaneCode` framework，并把客体根目录作为资源带上。
（另一个目标 `nestvane` 是更早的验证 App，全部靠解释执行。）

然后把渲染器的 framework 加进构建好的 App 并重新签名：

```
Usage: embed-ios-graphics.sh input.app gfxstream.dylib angle-build-directory output.app
```

[`tools/embed-ios-graphics.sh`](../../tools/embed-ios-graphics.sh) 检查各 framework 的最低 iOS 版本不高于 App 的，把 gfxstream 库包成 `NestvaneGfxstream.framework`，复制 ANGLE 的 `libEGL` 和 `libGLESv2` 两个 framework，并用输入 App 签名时所用的 Apple Development 身份给全部内容签名。输出路径必须以 `.app` 结尾，且尚不存在。

得到的 App 怎样装到设备上、TestFlight 构建怎样产生，仓库里没有脚本。

## 在 Mac 上检查运行时

```
tools/validate-host.sh
```

[`tools/validate-host.sh`](../../tools/validate-host.sh) 不带参数。它先构建夹具，再在 Mac 上编译并运行三个测试程序：解释器的验证、内核层的接口测试（[`tools/validate-boot-abi.cpp`](../../tools/validate-boot-abi.cpp)），以及客体地址空间的测试（[`tools/validate-guest-space.cpp`](../../tools/validate-guest-space.cpp)）。
它只需要 Xcode 的命令行工具和 `ld.lld`，是检查运行时改动最快的办法。

## 有用的环境变量

这些变量由运行时或 App 读取，记录里都提到过。

| 变量 | 作用 |
| --- | --- |
| `NESTVANE_NATIVE_USED=1` | 列出一次会话映射过的翻译，供 `build-native-code.py --used` 使用。 |
| `NESTVANE_NATIVE_WORKERS=<n>` | 在主循环之外运行原生代码的宿主线程数；0 表示不用。默认 3。 |
| `NESTVANE_DISPLAY_HZ=<n>` | 客体合成和 App 取帧的频率。 |
| `NESTVANE_DISPLAY_RATE=1` | 把每秒合成和显示的帧数写进 `display-rate.txt`。 |
| `NESTVANE_TRACE_DENIED=1` | 记录以“权限被拒绝”结束的系统调用，以及路径沿途的去向。 |
| `NESTVANE_NO_SOUND=1` | 不带声音设备运行。 |
