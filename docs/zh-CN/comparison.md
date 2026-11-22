# Nestvane、两仪和 Andarium

在 Nestvane 之前，已经有两个同类项目把 Android 跑进了 iPhone 的 App 里，Nestvane 受益于它们。
本页说明它们各是什么、Nestvane 和它们是什么关系、三者有什么不同。

关于这两个同类项目的说法，来自它们自己的页面（2026 年 11 月 22 日读到的内容），以及我们自己的一次测量（已标明）。
它们的作者比本页更了解自己的项目，欢迎指正。

## 两个同类项目

**[两仪（Twoyi）](https://twoyi.app/)** 在 iPhone、iPad 或 Android 手机上运行一个独立的 Android 系统。
它的网站说 iOS 版不需要越狱、不用 JIT，Android 的代码“在构建两仪时被提前翻译成原生 ARM64 指令”，运行 Android 13，
约 3 秒到桌面。它以 IPA 的形式分发，由用户自己签名。就我们所知，iOS 版没有公开源码。

**[Kernarium](https://github.com/imxieyi/Kernarium)** 在 iOS 上运行真正的 Linux 内核：把 User-mode Linux 编译成原生
arm64，客体程序由线程化解释器执行。**Andarium** 是它的 Android 应用，曾经上架 App Store，被 Apple 下架后改为侧载 IPA。
它是开源的，许可证是 GPL-2.0-or-later。

## Nestvane 和它们的关系

- Nestvane 是从研究两仪开始的。两仪证明了不用 JIT、只靠提前翻译，就足以让 Android 在 iPhone 上可用；Nestvane 走的是这个思路。
- Nestvane 没有使用这两个项目的任何代码。它是从零写的：自己的内核层、自己的改写器、自己的解释器。
- 规划 Nestvane 时，Kernarium 是可以读源码的那个系统，它公开的优化记录解释了解释器的时间花在哪里。
  Nestvane 做了相反的取舍：不要真内核，要翻译后的代码。

## 对比

| | Nestvane | 两仪（iOS） | Andarium |
| --- | --- | --- | --- |
| 源码 | 开源，Apache-2.0 | 就我们所知没有公开 | 开源，GPL-2.0-or-later |
| Android | 17，由 AOSP 构建 | 13 | 使用自己的镜像；README 没有写版本 |
| 客体看到的内核 | 为 App 写的，在用户空间 | 未说明 | 真正的 Linux 6.18（User-mode Linux） |
| Android 的代码怎样执行 | 提前改写成原生代码；其余由解释器执行 | 提前翻译 | 线程化解释器；能开启时可用 JIT |
| 应用自带的原生库 | 解释执行，慢 | 未说明 | 和其他代码一样解释执行 |
| 启动到桌面 | 约 10 秒；首次约 17 秒（iPhone 17 Pro） | 网站称“约 3 秒” | 2026 年 11 月我们在 iPhone 17 Pro 上测过一次约 25 秒，构建和数据都不同；它的 README 没有写时间 |
| iOS | 26 及以上 | 15 及以上 | 26 及以上 |
| 项目自己测过的设备 | iPhone 15 Pro、iPhone 17 Pro | 未说明 | 未说明 |
| 保留了多少 Android | 带小组件的 Launcher3、Settings、Files、安装器、媒体服务 | 未说明 | 完整的系统镜像 |
| 分发 | IPA，自己签名 | IPA，自己签名 | IPA，侧载 |

## Nestvane 的选择

- **两边都用最新的。** iOS 26 及以上系统上的 Android 17。只在 iPhone 15 Pro 和 iPhone 17 Pro 上测过，别的手机不知道能不能用。
- **裁剪得少。** 桌面保留了小组件，Settings、Files 和安装器都是原版的。需要再重编一次 Android 才能做的裁剪，没有做。
- **不追求启动速度。** 停在了约 10 秒，项目不打算为了更快而继续裁系统。
- **内核层而不是内核。** 启动快、容易嵌进 App，但不如真内核完整：Android 没有用到的东西就没有。

## 它是怎么做出来的

Nestvane 是一个人指挥编码智能体写出来的，用了 2026 年 11 月的五天。前两天用的是 Codex（GPT-6.2 Sol），结果是 Android 能到桌面，
但靠解释器要约十分钟。之后两天用的是 Claude Fable 5.2：复盘原来的设计、保留寄存器重编 AOSP、写改写器，以及
[`results/G0.md`](../../results/G0.md) 里记录的那些把开机降到约 10 秒的工作。
