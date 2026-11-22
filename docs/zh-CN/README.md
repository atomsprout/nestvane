# Nestvane 文档

Nestvane 在一个普通的 iOS App 里运行完整的 Android 17 系统，系统由 Android 开源项目（AOSP）的源码构建。
它不用即时编译器，不需要越狱，也不用虚拟化接口。
iOS 不允许 App 创建可执行内存，所以凡是原生执行的客体指令，都是在构建 App 之前翻译好、作为已签名的代码随 App 一起发布的。

这组文档说明它如何工作、各部分怎样构建，以及项目目前的状态。
读者设定为第一次接触这个项目的开发者。

English version: [../README.md](../README.md).

## 文档列表

| 文档 | 内容 |
| --- | --- |
| [architecture.md](architecture.md) | 一次读完整个系统：约束、四个组成部分、客体进程如何运行、一次系统调用怎样走完。 |
| [static-rewriting.md](static-rewriting.md) | 客体的 ARM64 ELF 文件怎样变成宿主代码：保留寄存器、改写规则、固定的装载地址、打包方式，以及没有翻译的代码如何处理。 |
| [kernel-layer.md](kernel-layer.md) | 客体看到的 Linux 内核，在用户态实现：任务、内存、文件与挂载、Binder、信号、套接字、SELinux、seccomp、设备节点。 |
| [devices.md](devices.md) | 图形、存储、网络、声音、电量、时区和触摸：客体与手机相接的地方。 |
| [building.md](building.md) | 构建 App 需要什么、各步骤的顺序，命令取自脚本本身。 |
| [design-notes.md](design-notes.md) | 设计取舍与踩过的坑：挑选出的一些问题，每个按“问题、原因、处理”写出。 |
| [status.md](status.md) | 哪些能用、哪些还不能用、实测数字和已知限制。 |

第一次阅读建议按架构、静态改写、内核层、设备、现状的顺序。
构建文档和设计笔记可以单独读。

## 通用术语

| 术语 | 在这里的含义 |
| --- | --- |
| 宿主 | iOS App 以及运行它的手机。宿主代码指 iOS 装载器接受、带签名的代码。 |
| 客体 | Android 系统及其中的一切：进程、内存、文件和地址。 |
| 客体地址 | Android 进程自己看到的地址。它不是宿主地址，两者之间靠一张表换算。 |
| 模块 | 一个客体 ELF 文件（可执行文件、共享库，或一份编译好的 Java 代码）连同它的翻译。 |
| 翻译 | 事先由模块的指令生成的宿主代码。 |
| 改写器 | 生成翻译的程序：[`experiments/g0/e3/rewrite.py`](../../experiments/g0/e3/rewrite.py)。 |
| 内核层 | Linux 内核接口的用户态实现：[`runtime/ProbeLinux.cpp`](../../runtime/ProbeLinux.cpp) 及其周围的文件。 |
| 解释器 | 执行没有翻译的客体代码的 ARM64 解释器：[`runtime/AArch64.cpp`](../../runtime/AArch64.cpp)。 |
| 客体根目录 | 客体启动所用的目录树，在构建一侧组装，作为 App 的资源发布。 |
| 参考模拟器 | 标准的 Android 模拟器，在真实的 Linux 内核上运行同一份 Android 构建。用来采集输入和对照行为。 |
| APEX | Android 可更新系统模块的打包格式。每个模块是一个只读的 ext4 镜像，挂载在 `/apex` 下。 |
| ART | Android 运行时，执行 Java 和 Kotlin 代码。它的编译器 `dex2oat` 生成 ARM64 代码，存放在 `.oat` 和 `.odex` 文件里。 |
| Zygote | Android 里所有应用进程的父进程，应用进程都由它 fork 出来。 |
| Binder | Android 的进程间调用机制，在真实设备上是一个内核驱动。 |

## 仓库里的位置

| 路径 | 内容 |
| --- | --- |
| [`runtime/`](../../runtime) | 链接进 App 的运行时：内核层、解释器、原生代码入口、客体地址空间、设备模型。 |
| [`ios/`](../../ios) | iOS App：显示 Android 画面的视图、触摸、APK 交接、设置、声音输出。 |
| [`tools/`](../../tools) | 构建和验证脚本，以及给 gfxstream 打的补丁。 |
| [`experiments/aosp/`](../../experiments/aosp) | 对 AOSP 源码树的修改和构建脚本。 |
| [`experiments/g0/`](../../experiments/g0) | 可行性实验，改写器也在这里。 |
| [`fixtures/`](../../fixtures) | 小型客体程序、资源覆盖包、桌面应用和测试应用。 |
| [`results/`](../../results) | 测量记录和设备报告。 |
| `generated/` | 构建产物：客体根目录、翻译后的代码、Xcode 工程的输入。不入库。 |

## 文档背后的记录

文档是概括，细节在记录里。

- [`results/G0.md`](../../results/G0.md) 是持续更新的实验记录，中文。“G0”是项目给五个前提实验（地址空间、写时复制重映射、保留寄存器与改写、故障恢复、装载已签名代码）起的名字，这些实验通过之后才采用了现在的设计。记录在实验之后继续往下写，包含此后的每一次测量。其中的子实验叫 E1 到 E5；改写器放在 `experiments/g0/e3` 下，是因为它起源于第三个实验。
- [`PLAN.md`](../../PLAN.md) 是项目计划，中文。
- [`AOT.md`](../../AOT.md) 和 [`REVIEW.md`](../../REVIEW.md) 是更早的设计笔记，中文。
- `results/P1.md` 和 `results/P2-*.md` 记录的是更早的阶段，那时全部客体代码都靠解释执行。内核层是在那个阶段写成的，现在用的仍是它。

## 范围与准确性

文档里的每个数字都来自 `results/G0.md`，并注明测量所用的设备。
没有验证过的内容，文中会直接说明。
项目尚未选定许可证，这组文档也不声明任何许可证。

## 联系方式

- 网站：<https://atomsprout.com/nestvane/>
- 仓库：<https://github.com/atomsprout/nestvane>
- Discord：<https://discord.gg/eGzEaP6TzR>
- Telegram：<https://t.me/atomsprout>
- 邮箱：<atomsprout@gmail.com>
