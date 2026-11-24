# nestvane 实施计划

## 目标与首个验收

在普通 ARM64 iPhone 上，用开发签名的 nestvane App 本地运行 Android 17（API 37）。宿主无需越狱、调试器或 JIT。当前真机测试使用 iPhone CX（iOS 26.2），在该设备上进入可交互的 Android 桌面，安装并运行选定的 APK，并让 Android 应用使用 iPhone 的实体摄像头。

首版交付和真机通过条件见 P3。P1 的技术验证用同一个客体 ELF 对比解释执行和预编译执行，以验证无 JIT 原生入口、回退正确性和成本。P1 的关键可行性已完成真机验证，见 [P1 结果](results/P1.md)。P2 的 Android 17 动态链接测试程序已在 iPhone 11 上成功运行，见 [真机结果](results/P2-linker.md)。原版 Android 17 `init` 已在 iPhone 11 输出第二阶段启动日志，见 [启动记录](results/P2-init.md)。原版 init 已生成属性区，独立 Bionic 客体从中读到了 API 与 ABI，见 [属性验证](results/P2-properties.md)。原版 init 已挂载 APEX 和 linkerconfig 使用的 tmpfs，并监听属性 socket，见 [真机日志](results/P2-mounts.md)。原版属性服务线程已运行并响应请求，见 [线程验证](results/P2-threads.md)。原版 init 已完成挂载设置，读取真实启动脚本并开始执行 early-init，见 [真机启动记录](results/P2-early-init.md)。原版 vendor 子上下文与 ueventd 已经越过 exec 并开始执行，见 [服务启动记录](results/P2-services.md)。原版 apexd-bootstrap 已以参考域启动，并在 iPhone 11 扫描到 Runtime APEX，见 [真机记录](results/P2-apex.md)。只读 Runtime APEX 已在 iPhone 11 激活，独立 Bionic 客体已读取挂载后的 linker64，见 [激活记录](results/P2-apex-activation.md)。原版 PID 1 已在真机收到并回收 bootstrap 服务的正常退出。原版 linkerconfig 已在 iPhone 11 生成了系统和 Runtime APEX 命名空间配置，见 [记录](results/P2-linkerconfig.md)。原版 ueventd 已在真机处理客体 loop 设备事件并完成 coldboot，见 [记录](results/P2-coldboot.md)。真机通过了原版 Android 17 系统 BoringSSL 自检并进入 early-fs，见 [记录](results/P2-early-fs.md)。原版 vold 已进入程序并创建设备目录，见 [真机记录](results/P2-vold.md)。原版 servicemanager 已在真机登记为 Binder 上下文管理器，vold 打开了 Binder 设备并完成握手，见 [Binder 验证](results/P2-binder.md)。原版 vold 已注册，原版 vdc 在真机按名称查找并完成两次 RPC，见 [记录](results/P2-vold-rpc.md)。客体 /data 已在真机验证跨 App 进程和覆盖安装持久化，见 [存储验证](results/P2-data.md)。原版 ART 已在 iPhone 11 上执行测试 DEX 并正常退出，见 [ART 真机结果](results/P2-art.md)。原版 Keystore2、软件 KeyMint 和常规 apexd 已在真机推进，见 [Keystore 启动记录](results/P2-keystore.md)。原版 Keystore 和 casefolding 已通过真机启动条件，init 已进入用户 0 初始化。原版用户 0 目录初始化和模块类路径生成已在真机完成，见 [用户 0 真机记录](results/P2-user0.md)。系统 ART APEX 已激活，原版 art_boot、odsign、odrefresh 和 dex2oat64 已在 iPhone 11 进入执行，见 [系统 ART 记录](results/P2-system-art.md)。原版 ART 签名成功属性已在真机读回。标准启动链已进入 Tethering APEX 的原版 netbpfload，见 [网络模块记录](results/P2-network.md)。P2 继续验证客体网络 ABI 和 Zygote。

## 主路线

采用 **AArch64 用户空间执行器 + Linux ABI + 自建 Android 17 客体**。宿主内核不承担客体 Binder、PID 或地址空间语义。客体 ELF 当作数据加载，解释器提供通用执行路径。构建时针对已知 ELF 生成宿主 ARM64 入口，由 Xcode 链接并随 App 签名。详细边界见 [AOT.md](AOT.md)。

整机软件解释用于必要时复现同一客体上的问题或验证性能差距。无 JIT 的 QEMU TCI 会解释内核和用户态指令，可能明显慢于硬件辅助虚拟化。真实差距以同一台 iPhone 上的任务数据为准。Linux 上用带真实内核的参考设备来核对语义，这项工作可独立于 iOS 整机模拟进行。

## Apache-2.0 发布路线

首版选择 **Nestvane 自有代码采用 Apache-2.0，随包分发的第三方组件保留各自许可**。MIT 和 Apache-2.0 都可用于自有代码。选择 Apache-2.0 是因为它明确授予贡献者可授予的相关专利权，并便于沿用 AOSP 中采用同一许可的代码。根目录的 `LICENSE`（Apache-2.0）和 `NOTICE` 已于 2026-11-22 加入。按发布构建的实际内容列出第三方版本、来源、许可和适用的通知。保留上游版权和 SPDX 标识，包括补丁携带的上游代码。Apache-2.0 标签只用于有权按该许可发布的文件。[Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0)

- **宿主运行时**沿用自有 ARM64 执行器、Linux ABI 和 iOS 桥接。参考 Kernarium 的架构及对照数据，避免把 GPL 运行时或原生 Linux 内核链接进首版 App。Kernarium 的 Linux 内核为 GPL-2.0-only，并与宿主代码组成同一二进制文件。更换链接方式本身无法解决由此产生的许可问题。[Kernarium 许可说明](https://github.com/imxieyi/Kernarium/blob/main/LICENSES/README.md) · [ASF 兼容性说明](https://www.apache.org/licenses/GPL-compatibility.html)
- **Android 17 发布镜像**由固定的 AOSP 源码版本自建，记录 Repo manifest 和产物校验值。保留实际打包模块的许可与适用通知，包含 APEX、原生库和生成的预编译产物。现有 SDK `default` 镜像用于内部验证。不能仅凭 `default` 名称推定整包的再分发权限。第三方验收 APK 单独导入。图形框架及其传递依赖按实际发布二进制核对许可和通知。[AOSP 许可说明](https://source.android.com/docs/setup/contribute/licenses) · [Android SDK 条款](https://developer.android.com/studio/terms)
- **性能先验证执行器，再决定投入。** 用同一镜像和数据卷在 iPhone CX 上采样，区分用户态指令、ABI 调度和等待开销。先做可开关的镜像绑定宿主 AOT，覆盖启动采样命中的系统 ELF 和 `boot*.oat`，保留对后装 APK、未知映射和异常的解释路径。范围从真实启动关键路径扩大，净收益用端到端时间和峰值内存判断。
- **并行按正确性和实测收益推进。** 若采样显示不同地址空间有足够的并行工作，先验证两个宿主执行线程，并回归共享页可见性、原子操作、futex、Binder 唤醒和退出。继续条件为真机启动阶段确有收益。最终仍按 P3 完成 APK、持久化和实体相机验收，并在无宿主 JIT 的 iPhone CX 上连续三次低于 10 秒进入可触控桌面。若性能差距仍大，再评估是否接受独立分发、保有自身 GPL 义务的内核组件及真实设备边界。

## P0 固定基线和测试条件

- **客体**：首轮锁定 AOSP `android-17.0.0_r1`、ARM64 64-bit-only、开发用 `userdebug`。记录 Repo manifest revision、构建变量、工具链、产物 SHA-256 和客体页大小。先固定一种页大小，选择和目标库及 Android 构建都匹配的配置。宿主页粒度与客体页语义分别处理。
- **开发环境**：macOS/Xcode 构建 iOS App，Linux 构建 AOSP。ARM64 Linux + KVM 上的 Cuttlefish 可以作为同指令集参考设备。已有可匹配的参考镜像时，先验证实验，再做完整源码构建。Cuttlefish 的整机镜像依赖其虚拟硬件，nestvane 需要独立的客体产品配置。
- **真机**：先固定一台可用于开发签名的物理 iPhone，记录型号、iOS 版本和 Xcode 版本。验收用正常点按启动，收集 App 自己的结果和计数。模拟器用于开发调试。
- **交付物**：一条可重复的 iOS 构建路径、一份测试环境记录和一套自有测试 ELF。首轮可先用无动态依赖的自制 ELF，AOSP 全量构建不是 P1 的前置条件。

## P1 在 iPhone 上验证无 JIT AOT

1. 建一个最薄的 iOS 测试 App，内嵌一个自制 AArch64 ELF。先实现加载 `PT_LOAD`、初始栈、客体寄存器与内存、初始 PC，以及最小 `write/exit` 服务。最初的用例覆盖算术、分支、读写内存和系统调用退出。
2. 在同一运行时里保留单步解释与解码块执行。用构建期工具翻译一小组基本块，先支持测试程序覆盖的有限指令子集。生成宿主源文件或目标文件，静态链接到 iOS App。未知指令、无法匹配的映射以及外部目标返回解释器。
3. 在同一真机、同一版本、同一优化级别测三种路径。对照 ARM64 Linux 上的输出和退出码，再比较三条客体路径在检查点的 PC、寄存器、标志位、修改内存和异常结果。测试必须包含地址变化、未翻译分支、指令内容变化、跨页访问，以及执行到一半发生客体异常的恢复。
4. 输出基准记录。端到端时间、热点时间、解释和 AOT 进入次数、回退原因、额外 Mach-O 字节、峰值内存各有一列。Release 构建中每种模式至少重复 20 次，记录中位数、P95、输入规模和设备热状态。P95 只用于首轮观察，临界结果增加样本复核。

**继续条件**：测试状态一致，AOT 在两类 CPU 热点中至少各达到块解释模式的 2 倍吞吐，并报告入口、内存和回退开销。这是立项阈值。若只有纯算术循环通过，先测受控内存访问和控制流，再决定是否扩大翻译器。若回退或内存检查吞掉收益，先改测到的瓶颈。

## P2 让真实 Android 用户空间启动

- 首先运行固定 Android 17 构建的 `linker64` 和一个动态链接的 Bionic 测试程序。补齐 ELF interpreter、auxv、TLS、映射权限、重定位所需行为、信号及动态加载，并用 Android 17 参考客体做差分验证。
- 再运行定制的第二阶段 `init`。实现或适配 Android 启动实际依赖的文件描述符、虚拟文件系统、属性、Unix socket、时钟、进程/线程、futex 和信号。用启动日志逐条定位失败点。系统文件与可写数据分开打包，构建时记录所需 APEX 内容及布局。
- 打通内建 Binder 路径。验收两客体进程的事务、对象引用、文件描述符传递和服务重启。随后启动 Zygote、`system_server`、包管理和 `installd`。以同一版本的 Android 17 ART Service 行为为准。
- **继续条件**：能稳定启动到包管理可用，安装和启动自有测试 APK。优先支持参考构建和选定 APK 的依赖。用未实现调用的日志保持缺口可见，并用独立测试固定已实现的行为。

## P3 首版真机验收

当前验证镜像使用 SDK 的 AOSP `default` 变体，首版发布镜像按 Apache-2.0 发布路线从 AOSP 源码构建。两者都保留桌面、设置、文件选择器、输入法和运行所需的系统组件。首版不预装 Play 商店、Google Play 服务、Google 服务框架或 Google 应用。验收 APK 通过 iOS「文件」导入，并在这一环境中完成各自的验收操作。

- **Android 桌面**。在当前验收设备上正常打开开发签名的 Nestvane，客体完成启动、可触摸操作 Launcher 和切换应用。连续三次冷启动均进入桌面，记录首个可交互帧的耗时和峰值内存。
- **安装和运行 APK**。从 iOS「文件」选择 APK，经 Android 包管理器安装，在桌面启动并完成每款应用约定的基本操作。验收覆盖自有冒烟 APK 和至少两个在同版 ARM64 Android 参考环境通过的第三方 APK。重启 Nestvane 后，验证已安装应用仍可启动且应用数据保留。
- **实体摄像头**。iOS 侧使用 AVFoundation 采集帧，由客体相机服务通过 AIDL Camera HAL 暴露给 Android 应用。先接通一颗后置摄像头。在使用标准 Camera2 API 的 APK 中验证实时预览、拍照和照片读回。分别验证 iOS 和客体应用的相机授权，退出相机或宿主进入后台时释放采集。前置摄像头和其他镜头可沿用同一接口逐步接入。
- AOT 优先优化追踪确认的系统热点，显示或摄像头帧传输分别测量耗时与内存，按瓶颈优化。

第三方验收使用两个固定的离线应用。以下两项已在 ARM64 Android 17 SDK 参考系统（CE2A.260420.051.A3）通过真实安装、启动和基本操作。iPhone 17 Pro 已通过「文件」安装两款 APK，并将客体已安装文件与参考输入逐字节核对。OpenCalc 已从桌面打开。两款应用的完整操作与重启后数据恢复仍待验收。APK 从项目发布页下载。同一参考系统还验证两款 APK 都可通过 `cmd package install -r -S <字节数>` 从标准输入安装。

| 应用 | 验收操作 | APK SHA-256 |
| --- | --- | --- |
| OpenCalc 3.2.0（53） | 输入 `7 × 8` 并确认界面显示 `56`，重启后检查计算历史。[F-Droid 发布页](https://f-droid.org/packages/com.darkempire78.opencalculator/) | `2642613868a8a80f0ce963a4863bc153e7281b97b3bbdf36ca92a99dded7134a` |
| Editor 1.98（198） | 输入 `Nestvane17`，通过 Android 文件选择器存为 `nestvane-reference.txt`，重启后重新打开确认文本。[F-Droid 发布页](https://f-droid.org/packages/org.billthefarmer.editor/) | `343d17e11d0914e1a7708df2781580a0413e52501c0a41d91f7ef725d9615fe9` |

iOS「文件」导入与客体安装助手已经实现。用户选择的 APK 以只读描述符交给客体 `cmd package install`，由包管理器检查签名和安装资格。当前存储上限为单个 APK 64 MiB。Android 17 参考系统以同一助手验证两款 APK 安装及损坏包拒绝。Mac 上用诊断接收器验证了整文件读取、EOF、结果回传与停止时保存。iPhone 14 已通过 iOS「文件」安装 Smoke、OpenCalc 和 Editor，正式安装 APK 的 SHA-256 与输入一致。Smoke 已从 Launcher 打开并通过触控把计数从 0 改为 1。第三方应用操作、重启后的应用和数据恢复仍待端到端验收。


## 10 秒冷启动性能目标

从 iPhone CX（iOS 26.2）点击已安装的 Release App，到真实 Android 17 Launcher 接受触摸并打开应用抽屉，连续三次均须 **低于 10 秒**。使用完成首次初始化且已安装验收 APK 的数据卷。每轮开始前退出宿主 App，Android 从 init 重新启动。首次初始化、进程重建后从内存快照恢复、同进程回前台分别计时。记录启动阶段、触摸结果、热状态和峰值物理内存。30 秒可用于评估原型进展。

已有比较中，CX 基线在 App 内部计时开始后 605.820 秒呈现 Launcher 帧，触摸未验收。同一台 iPhone 17 Pro 上，Nestvane 曾在 497.027 秒报告 Launcher 绘制，Andarium 两次约 25 秒到桌面、28–29 秒点进 Settings，构建与数据卷不同。两仪公开的约 3 秒是首次准备后的 iOS Android 13 桌面时间，尚无同口径测量。用 CX 基线估算，10 秒需要约 60 倍端到端改善。[真机记录](results/P2-network.md) · [两仪口径](https://twoyi.app/)

1. **固定可重现的首版客体。** 从固定版本 AOSP 源码构建 ARM64、单用户产品。保留 ART、Binder、应用权限、包管理、桌面、设置、输入法、GPU 合成、持久化和后置 Camera2。把账号同步、引导动画及经启动依赖核对可后置的任务移出首屏路径。首版延后电话、多用户、额外镜头及大型游戏兼容。保留安装验收所需的文件选择器和运行依赖。每次改变系统组件，复测启动、APK 使用和相机，防止把工作转移到点按时。[AOSP 产品配置](https://source.android.com/docs/setup/create/new-device)
2. **重做启动执行路径。** 发布入口直接启动客体，诊断探针独立运行。先测宿主诊断、根目录枚举和渲染器初始化的成本，并验证渲染准备和客体早期启动能否重叠。用同版本镜像开启和关闭宿主 AOT 对照，先覆盖从 linker/Bionic 到 Zygote 和 system_server 的实际热点，并纳入相应的 `boot*.oat`。跨已验证代码块保留宿主寄存器状态，在内存映射、系统调用及异常边界准确恢复客体状态。由 Clang/LLVM 在构建机生成签名链接所需的目标文件，后装 APK 仍由解释器兜底。Apache-2.0 的 Remill 可做 AArch64 指令提升的离线对照，原型先比较同一段 Bionic 热路径的吞吐、回退和产物大小。[Android ART](https://source.android.com/docs/core/runtime/configure) · [Remill](https://github.com/lifting-bits/remill) · [LLVM 许可](https://llvm.org/docs/DeveloperPolicy.html)
3. **用整机收益决定是否扩大。** 每种镜像先内部配对 AOT 开关，再比较精简镜像与原参考镜像。记录真实启动关键路径上的宿主 CPU、ABI 服务、等待和 I/O 时间。按地址空间可运行情况决定是否投入双执行线程。Kernarium 用作独立性能基准，gVisor 的 KVM 与 Linux syscall 拦截路径需要额外移植，Apple 公开的 Hypervisor 文档面向 macOS。若精简镜像加整模块 AOT 仍无法接近 10 秒，单独评估从设备本地检查点恢复，要求 GPU、输入和相机重新连接后通过同一触摸动作，镜像与数据版本不匹配时正常冷启动。[Kernarium 优化记录](https://github.com/imxieyi/Kernarium/blob/main/docs/optimization-history.md) · [gVisor 平台](https://gvisor.dev/docs/architecture_guide/platforms/) · [Apple Hypervisor](https://developer.apple.com/documentation/hypervisor)

## 精简客体的裁剪方案

新路线（保留寄存器重编 AOSP、静态改写、原生执行，见 [G0 记录](results/G0.md)）已经在 App 里启动到 Launcher。再次启动的时间：iPad Air (M3) 12.2 秒，iPhone 17 Pro 13.8 秒，iPhone 11 30.7 秒；全新安装后的首次启动是 36–38 秒。到桌面时物理内存约 2.2 GB。现在的开机基本是算力，所有客体线程轮流使用一个宿主线程。iPhone 11 上 33 秒时客体 CPU 最大的几项是 system_server 6.5 秒、两个应用进程各约 3 秒、zygote 2 秒、Launcher 1.3 秒。

裁剪减少的是总工作量，对再次启动、首次启动和内存都有效，先于多核调度进行。每一步记录三个数：再次启动到 Launcher 的时间、首次启动的时间、到桌面时的物理内存。每一步都要保持 Launcher 显示、触摸可用、APK 可安装，先在参考模拟器上确认开机，再上真机。

0. **算清账。** 不改镜像。列出开机时启动的全部守护进程和 Java 进程各自的 CPU 和内存，system_server 各服务的启动耗时，以及首次启动比再次启动多出的时间花在哪。
1. **不重编的裁剪。** 只改开机目录：去掉用不上的守护进程的启动脚本，用特性开关关掉 system_server 里不需要的服务，不打包开机时被拉起而用不上的系统应用。
2. **重编的裁剪。** 在构建机上做精简的产品配置：去掉不需要的模块和应用；换成不带最近任务和手势服务的 Launcher3，默认桌面不放搜索框等 Widget；逐个评估 mainline 模块的去留，每去掉一个都会改变启动类路径，一次只动一个；按裁剪后的系统重新生成预加载类表。SystemUI 先保留，只关掉用不上的部分。
3. **多核调度。** 裁剪之后进行：客体线程的原生执行段放到辅助宿主线程上并行，系统调用仍由主循环串行处理。

不裁的部分：包管理、Activity 和窗口管理、输入、存储、权限、设置存储、输入法、WebView。第三方应用依赖它们。

### 第 1 步的清单（2026-11-21，按 iPhone 15 Pro 上的开机日志和 Settings 的实测）

开机后被广播或绑定拉起、在这个场景里用不上的应用进程：`com.android.rkpdapp`（远程密钥配置）、`.adservices`、`com.android.federatedcompute.services`、`com.android.ondevicepersonalization.services`、`com.android.healthconnect.controller`、`com.android.devicelockcontroller`、`com.android.photopicker`、`com.android.shell` 的堆转储接收器、`com.android.keychain`。九个进程，每个 0.2–0.4 GiB 常驻。

用不上的守护进程和启动脚本：`adbd`（每隔几秒重试启动 `mdnsd`）、`update_engine`、`gsid`、`snapuserd`、`uncrypt`、`recovery-*`、`tradeinmode`、`profcollectd`、`simpleperf`、`heapprofd`、`traced_perf`、`perfetto`、`incidentd`、`dumpstate`、`llkd`、`storaged`、`bootstat`、`logcatd`、`dmesgd`、`wifi`、`usbd`、`virtual_camera`。另有一个客户端每秒等一次不存在的 `android.hardware.media.c2` 软件编解码服务，要么补上服务，要么去掉等待方。

Settings 实测：首页、Network & internet、Apps、Display 都能打开，子页面 0.15–0.35 秒显示。Storage 崩溃（`Volume external_primary not found`）：客体没有共享存储卷，Files 的“Can't load content”是同一个原因。

Settings 里去掉的入口（用特性声明和资源覆盖关掉，不改代码）：SIM 卡和移动网络、热点和共享、蓝牙和已连接设备、电池、锁屏和生物识别、安全与紧急情况、系统更新、多用户、备份。保留：应用（卸载、权限、默认应用）、通知、显示（深色、字号）、声音、存储、无障碍、语言和输入法、日期和时间、关于。

要补而不是裁的核心功能：共享存储卷（`/storage/emulated/0`，Files、Storage 页和应用存文件都靠它）。

### 用户安装的应用（同一天实测）

装 APK 的三个入口：App 里的“Install APK…”按钮，主屏幕长按图标的“Install APK…”快捷操作，以及在“文件”、隔空投送或分享菜单里用本 App 打开 `.apk`。

iOS 不允许运行设备上生成的代码，用户装的应用因此有两个固定的限制：DEX 由 ART 的解释器执行（解释器本身是原生的）；应用自带的原生库没有翻译，由运行时的解释器执行，约每秒四千万条指令，而且和界面线程共用一个宿主线程。ScriptWidget 1.1.1 的界面流畅可用，它的小组件预览靠自带库里的 QuickJS 解析 2.8 MB 的脚本，第一个预览约 2 分钟出来。

按收益排的三条路：多核调度（第 3 步，自带库的线程不再和界面抢一个核）；解释器提速（访存直接走地址空间的偏移表、按块预解码）；对随 App 打包的已知应用，让改写器支持占用 x25–x28 的代码，把它们的库也翻译成原生。

### 第二个里程碑（2026-11-21 定）

目标：用户装的应用用起来不卡，常用系统功能齐全，别人能拿到并装上。顺序和验收：

1. 源码 Release：基于 `milestone-1`，不带 IPA。
2. 多核调度：原生执行段和自带库的解释放到辅助宿主线程上，系统调用仍串行。ScriptWidget 第一个预览 30 秒以内，开机不变差，连续开机 10 次稳定。
3. 共享存储卷：Settings 的 Storage 页不崩溃，Files 能列目录，应用能存取文件。
4. 不重编的裁剪：按上面的清单，记录内存和开机时间的前后对比。
5. 解释器提速：自带库的解释速度至少翻倍。
6. 重编的裁剪：**暂不做（2026-11-22 定）。** 开机 8–10 秒、安装包 1.14 GB 已经达到目标，不重编的裁剪已做完清单里的大部分。它剩下的收益只在内存上；以后有应用因内存被 iOS 终止时再回头做，届时按 [构建文档](docs/zh-CN/building.md) 重建 AOSP 源码树。
7. 给别人安装的 IPA：裁剪后定体积；许可证和分发方式由用户定。

## 里程碑（2026-11-22 定，1.2 发布后）

1.1 和 1.2 已发布。1.2 做完的：画面不经处理器直接到屏幕、发热时少画、换网络后恢复、应用能收到短信广播、Submit a Bug。

现在的四个大问题，按用户受影响的程度排：

1. 普通用户装不了：扩展地址权限只给付费开发者账号。
2. 内嵌网页的应用打不开：镜像里没有 WebView。
3. 应用自带的原生库只能解释执行，约每秒四千万条指令。
4. 媒体：视频软解（1.5 起 1080p 30 帧够用），没有硬件解码和相机录像；麦克风 1.5 加上了。

估计的天数按 1.0 到 1.2 的节奏给，不是承诺。

| 版本 | 主题 | 内容 | 验收 | 估计 |
| --- | --- | --- | --- | --- |
| 1.3 | 稳定与兼容 | 最低系统降到 iOS 17（已在 iPhone 14 上试过，6 GB 内存够用）；修已知的小毛病：每条短信让电话进程崩溃一次、adbd 每 5 秒重启一次、命令行工具连不上网；测一批常见开源应用，列兼容表；回归测试一条命令跑完 | 一张实测的兼容表；一条命令跑完回归；日志里没有周期性的崩溃和重启 | 2–3 天 |
| 1.4（已发布） | 修遗留问题 | 解释器没有的指令只结束遇到它的应用，并补上 Go 和常见向量指令（带 Go 库的应用不再让 Android 停住）；短信写入不再报错（给电话 UID 补上权限）；命令行工具能联网（`setitimer` 和半关闭后的挂断） | 见 `results/G0.md` | 已完成 |
| 1.5（已发布） | 兼容、横屏和媒体 | 兼容表扩到 54 个应用（50 个能打开），修掉其中发现的问题：应用生成不了密钥、命名管道、约 70 条指令、大应用碰到的几个上限、域名应答；横屏；1080p 的 H.264 和 HEVC（软件解码，HEVC 之前会让编解码器崩溃）；麦克风。**没做成的两样**：iPad 的平板布局（平板密度下桌面和应用窗口在这个构建里不能用，改不了系统应用）和硬件解码（要重新构建 AOSP；软件解码已经够 1080p 30 帧）。麦克风录到的内容还没人验证 | 见 `results/G0.md` 和 `docs/compatibility.md` | 已完成 |
| 1.6（已发布） | 开机收尾，然后 WebView | 详细步骤见下面“1.6 详细计划”一节。开机变慢 2026-11-23 已查清是冷启动，加了预读（装完第一次启动约 12 秒到约 9 秒），还差放置之后的实测 | Wikipedia 的文章 10 秒内显示 | 一周以上，风险高 |
| 1.7（已发布） | 用户应用的原生库提速，内置 root | 详细步骤见下面“1.7 详细计划”一节。解释执行放到工作线程、解释器提速（上面“用户安装的应用”一节）；内置 root：在 Nestvane 的设置里打开后，Android 里的应用能申请到 root 权限 | ScriptWidget 第一个预览 30 秒以内；一个要 root 的应用申请后拿到 uid 0 | 4–5 天，效果不确定；root 另加 2–3 天 |
| 1.8 起 | 见下一节“1.7 之后的版本” | | | |

几条说明：

- **1.6 是关键的一步。** WebView 的原生库是现成编译好的，没有让出保留寄存器，现在只能解释执行。改写器能处理这种代码，WebView 才有可能。
- **1.6 和 1.8 都先做 1–2 天的实验，再决定做不做。** 1.6 量一段用满寄存器的代码改写后慢多少；1.8 把下一节“动手前先量”的数量出来，再量软件 MMU 的访问序列慢多少。不行就换方向。
- **顺序是 2026-11-23 定的。** WebView 放到 1.6，内置 root 放到 1.7，软件 MMU 放到 1.8。1.5 的三样（兼容表、iPad、媒体）也是当天确认的；兼容表里要有今日头条、抖音、TikTok、YouTube 这一类应用。今日头条和抖音试了，能打开但内容要等 WebView；TikTok 和 YouTube 没有官方安装包，没试。
- **天花板。** 用户自己装的应用的原生库不能在手机上翻译（iOS 不让运行设备上生成的代码），1.7 只靠解释器提速（预先打包常见库 2026-11-24 定为不做）。
- **1.5 留下的待办（2026-11-23）。** 都不大，随后面的版本做：
  - 麦克风：在设备上允许权限后，验证录到的是真实声音（现在只验证到 Android 开始录音）。
  - 最终版在 iPad 上跑一遍回归（发布时 iPad 锁着，最后几项改动只在 iPhone 14 上测过）；用手转动设备验证横屏。
  - 开机变慢：排到 1.6 的第一件事（见上表）。
  - 桌面不跟着设备转；iPad 的平板布局；硬件解码。后两样要重新构建 AOSP。
  - 没打开的应用：KDE Connect（要 Wi-Fi 服务）；AnkiDroid、Jellyfin、今日头条和抖音的内容等 1.6 的 WebView。
  - 给密钥权限和命名管道补主机测试（现在只有真机验证）。
- **没有排进版本的。** 用快捷指令启动应用。
- **不做的。** 无障碍（VoiceOver 与 TalkBack，2026-11-23 定）。iOS 16（2026-11-23 定）：最低系统留在 iOS 17。试验版做过，装到 iPhone XR 时传到一半断连，没有跑起来；XR 只有 3 GB 内存，4 GB 的 iPhone 11 已经放不下 Android。
- **一直没验证的。** 屏幕本身是否每秒刷新 120 次（数的是 iOS 的刷新回调）；真的把手机用热；只有 IPv6 的蜂窝网络；声音用耳朵听；卸载后的全新安装（1.2 没有做）。

## 1.7 之后的版本（2026-11-24 定）

给读者看的版本见 [docs/zh-CN/roadmap.md](docs/zh-CN/roadmap.md)，这里是做法和验收的细节。

1.7 发布时的状况：开机 iPhone 17 Pro 7.3 秒、iPad Air 7.6 秒、iPhone 14 10.9 秒（`results/boot-baseline.txt`）；WebView、内置 root、存储跟随设备都有了。剩下的大问题，按用户受影响的程度排：

1. **普通用户装不了。** 要付费开发者账号，因为要扩展地址空间的权限（issue #3）。
2. **应用自带的原生库仍然慢。** 解释执行约每秒 7,000–8,000 万条，ScriptWidget 的预览会超时，抖音没有视频。
3. **欠着的验证。** 1.5–1.7 留下的几项一直没做。
4. **要重新构建 AOSP 的功能。** iPad 的平板布局、硬件视频解码；构建机已删，只有快照。

| 版本 | 主题 | 内容 | 验收 | 估计 |
| --- | --- | --- | --- | --- |
| 1.8 | 存储总量（小修，已做） | 1.7 把整个卷都报成 `/data`，Android 又把 `/system` 加进总量再向上取 2 的幂，256 GB 的手机显示共 512 GB。已改成 `/data` = 卷减去 `/system`（7d3f68b） | 256 GB 显示共 256 GB、已用与 iOS 一致（iPad 128 GB 已验证） | 已完成 |
| 1.8 | 长按清理后台 | 长按 Nestvane 底栏的 Home 键，弹出“清理后台”：结束 Android 里在后台的应用进程（系统进程和当前前台应用不动），清空最近任务。走现有的 App 到 Android 的请求通道（`fixtures/android17-text.sh` 加一种请求，用 `am kill-all` 和清空最近任务的命令） | 清理后最近任务为空、后台应用进程不在了、当前应用不受影响；记下清理前后的内存 | 约 1 天 |
| 1.8 | 交互的 root shell | root 应用开一个一直能敲命令的 `su`（伪终端，如终端类应用的 root 模式）；查清内核层的伪终端在 `su` 之后是否照常工作，不行就补 | 用测试程序或终端应用开 `su`，连敲几条命令都有输出 | 半天到 1 天 |
| 1.8 | 补欠着的验证 | 麦克风录音用耳朵听；iPad 用手转一次；密钥权限和命名管道的主机测试；放置一夜后的冷启动。查出问题小就一起修，大就记下排后面 | 每项写进 `results/G0.md` | 各半天 |
| 1.8 | 先查后定的两项 | 桌面不跟着设备转；KDE Connect 一直弹“keeps stopping”（它要 Wi-Fi 服务）。各查半天：一天内能修好就放进 1.8，否则移到“之后” | 查明原因，写进 `results/G0.md` | 各半天查 |
| 1.8 | 两个小修 | 放置一段时间后的冷启动在 iPhone 17 Pro 上复测，看开机预读的效果；Nestvane 设置里的 root 授权列表显示应用名和图标，不只是包名 | 冷启动时间写进 `results/G0.md`；列表里认得出是哪个应用 | 各半天 |
| 1.9 | 应用自带的库再快一截（只做通用的） | 浮点和 SIMD 运算进快速路径（工作线程最常见的停止原因）；按块缓存、直接跳转的解释；把仍在故障的相邻块并到同一段宿主内存。不针对某个应用或某个版本调：应用一升级，库就变了，按它挑的热点和预先翻译都会失效，只有解释器本身的提速对所有版本都有效 | Mac 上的解释器基准（`tools/bench-interpreter.sh`）再快一倍；一批不同类型的应用（Flutter、React Native、带原生 SDK 的）抽查开机后的操作是否变快。ScriptWidget 和抖音只作参考样本，不作为过关条件 | 1 周，效果不确定 |
| 2.0 | 不需要付费账号 | 先用 1–2 天量清楚：到桌面时宿主地址空间被哪些部分占着（代码跳转表约 3.3 GiB、翻译后的代码约 2.6 GB、系统镜像约 1.5 GB、客体内存），以及软件 MMU 的访问序列慢多少。可行再做：所有客体进程共用一个内存池按页表映射，固定占用改成按需映射。详见下面“待办：软件 MMU”一节 | 免费账号签名的包 30 秒内到桌面；有付费账号时开机不变慢 | 1–2 周，风险最高；量完不可行就停在“量清楚” |
| 2.1 | KernelSU 兼容 | 见下面“KernelSU 兼容”一节：先让 KernelSU Manager 能授权，再决定做不做模块 | Manager 显示已工作、能给应用授权和撤销 | Manager 几天；模块另算 |
| 2.2 | 体验 | 桌面跟着设备转（多半是 Launcher3 在手机尺寸下锁竖屏，改设置或资源覆盖；要改系统应用就得重新构建 AOSP）；共享文件夹实时同步（监听 iOS 那边的文件变化，反映到共享存储并通知媒体库）；IPv6（v6 的连接、路由、DNS 的 AAAA 记录，Android 报告 v6 地址）；Wi-Fi 服务先只做“不崩”：一个报告 Wi-Fi 关闭或不可用的服务，让调用 WifiManager 的应用（KDE Connect）不再崩。“假装连着 Wi-Fi”（SSID、信号）要 1–2 周、可能要重新构建 AOSP，遇到需要的应用再做 | 桌面横竖都能用；从“文件”放进去的文件不重启就出现；只有 IPv6 的网络上能联网；KDE Connect 能打开 | 1–2 周（旋转半天–2 天、同步 2–3 天、IPv6 2–4 天、Wi-Fi 1–2 天） |
| 2.3 | 要重新构建 AOSP | iPad 的平板布局、硬件视频解码。先要从快照恢复构建机，按 [构建文档](docs/zh-CN/building.md) 重建 | | 恢复构建机 1 天，各项另算 |

几条约定：

- **1.8 只收小修**：每项一天以内、改动范围小，不碰执行引擎。整个版本约 3–4 天。
- **每个版本发布前过开机门槛**：`tools/boot-check.sh` 在 iPhone 14 上不慢于基线；iPhone 17 Pro 和 iPad Air 现在也有基线，手边有哪台就测哪台。启动波动大（同一构建相邻几次差 1–3 秒），没通过时先和上一版轮流测（`BOOT_CHECK_ENV`），查清是不是真的变慢。
- **1.9 和 2.0 都先量、再做。** 量出来不可行就换方向，不硬做。1.9（提速）在前：一步一步加、每步都能测能停，风险小；2.0（软件 MMU）要重做地址空间并重新翻译全部代码，工作量和风险都最大。

## 1.6 详细计划（2026-11-23 定）

目标：内嵌网页的应用能用。顺序是先让 WebView 能跑（哪怕慢），再让它快；每一步都能单独验收，做不下去时停在上一步也有用。

**现状。**

- 镜像里没有 WebView 包，只有加载它用的 `libwebviewchromium_loader.so` 和 `libwebviewchromium_plat_support.so`。日志报 `Default WebView package (com.android.webview) not found`。
- 今日头条、抖音的网络库每个请求都调 `CookieManager.getInstance()`。每次等满 1 秒，而且在锁里排队，所以内容出不来。
- 改写器认定 x18 和 x25–x28 是运行时自己的。现成编译的代码只处理了手写汇编里最简单的三种写法（`Rewriter.reserved`：`mov`、以 sp 为基址的 `stp/ldp` 的前后变址），带偏移的 `stp/ldp`（函数序言里最常见的那种）还不认。
- 预先翻译的模块按内容识别，放在固定的客体地址上（`0x8000_0000`–`0xFFF0_0000`）。现在用到 `0xE428_0000`，剩约 440 MB。

### 第 0 步：1.5 的尾巴（半天到 1 天）

| 事 | 做法 | 验收 |
| --- | --- | --- |
| 放置之后的冷启动 | iPhone 17 Pro 放一夜不开 Nestvane，第二天第一次启动计时；再用 `NESTVANE_NO_PREFETCH=1` 的包对比一次 | 记进 `results/G0.md`；预读带来的差别有数 |
| 麦克风 | 设备上点“允许”，用 Android 的录音应用录 10 秒再放出来 | 能听到录进去的声音 |
| iPad 回归 | `tools/regress-device.sh` 在 iPad 上跑最终版；用手转一次设备 | 回归全过，应用跟着转 |
| 主机测试 | 给密钥权限（app 域能 `rebind`、隔离域不能）和命名管道（`mkfifo`、阻塞打开、边沿触发的 epoll）补进 `tools/validate-host.sh` | 主机测试全过 |
| 存储容量跟随设备（2026-11-23 加） | Android 现在看到的是写死的 8 GiB 上限（`maxWritableBytes`），设置里显示“共 16 GB”（Android 把总量向上取到 2 的幂）。改成：总量取 App 所在卷的容量，剩余取 iOS 给的“重要用途可用空间”减去余量，写入上限也用它 | Android 的“存储空间”显示的总量和剩余与 iPhone 设置里的一致 |
| 自启推迟（可选） | 第三方应用的 `BOOT_COMPLETED` 推到桌面显示之后再发 | 装 30 个应用时到桌面的时间不变长 |

### 第 1 步：拿到 WebView 并装进镜像（1 天）——已完成（2026-11-23）

- **来源只用 AOSP。** `platform/external/chromium-webview` 里的预编译包（`com.android.webview`，AOSP 自己用的那个），从 AOSP 的 git 取；构建机的磁盘快照里也有同一份。不用第三方 WebView。
- 先看清楚：版本号、包大小、`libwebviewchromium.so` 的大小，是不是只要 arm64，有没有用 x18（Android 的 arm64 代码约定不用 x18，要扫一遍确认）。
- 装成系统应用，确认 `config_webview_packages.xml` 指向它，开机后 `WebViewUpdateService` 选中它。
- 开机不能变慢：系统在开机时会为 WebView 的库准备共享的重定位数据（RELRO）。这一步要么推迟，要么确认不花时间。验收是到桌面的时间变化不超过 0.5 秒。

验收：`dumpsys webviewupdate` 显示可用；今日头条不再每个请求等 1 秒（这一步就可能让它的内容出来，因为 `CookieManager` 只需要加载 WebView，不需要画网页）。

### 第 2 步：解释执行下先跑通（1–2 天）——已完成（2026-11-23）：Wikipedia 的文章能显示；WebView 的库在原地解释、进程其余部分本地执行，文章约 25 秒开始加载。详见 `results/G0.md`

WebView 的原生库现在只能交给解释器，约每秒四千万条指令，会很慢，但先证明对不对。

- **单进程。** 关掉 WebView 的多进程渲染，不起隔离的渲染进程（少一个进程、少一套沙盒和 seccomp）。
- **不用 JIT。** 给 V8 加 `--jitless`（只用解释器 Ignition，没有 WebAssembly）。否则 V8 生成的机器码也只能交给解释器，而且要可写可执行的内存。命令行参数通过 WebView 的调试命令行文件给。
- **画面。** WebView 通过 hwui 的绘制回调用 GL 画，走现有的 gfxstream。先看能不能画出来。
- 解释器缺的指令按 1.5 的办法补，每条都和 Mac 处理器对比。

验收：Wikipedia 的一篇文章能显示出来，不限时间；记下用了多久、多少条指令、内存多少。**这里是第一个决定点**：如果解释执行下已经在 30 秒内，可以先发一个“能用但慢”的版本。

### 第 3 步：改写器实验（1–2 天，决定做不做第 4 步）——已完成：约 3% 的指令用到这几个寄存器，估计慢 1.1–1.2 倍，做第 4 步。库的位置固定在 0x1_4000_0000（zygote 的预留在 Android 17 是 1 GiB，放不进原先的固定地址区）

问题：现成编译的代码把 x25–x28 当普通的被调用者保存寄存器用，改写后的代码里这四个是运行时的（上下文、代码表、内存表、临时）。

- **做法。** 客体的 x25–x28 放在上下文里它们的槽位上（现在 `Rewriter.reserved` 已经这么放）。用到它们的指令改写成：先把要用的值取到空闲的寄存器，执行，再写回。没有空闲寄存器时，临时把一个普通寄存器存到上下文里腾出来。
- **量什么。** 拿一个不保留寄存器编译的库（WebView 的库本身，或 libhevc 那样的解码器），统计用到 x25–x28 的指令占多少；改写后跑一段基准，和保留寄存器编译的同一份代码比慢多少。
- **还要查的。** WebView 的库由它自己的加载器放到预留好的地址上（为了共享 RELRO），而预先翻译的模块要求放在固定地址。要确认能让它落在翻译时用的地址上，或者让运行时把它挪过去。剩下约 440 MB 的固定地址放不放得下。

决定：改写后比保留寄存器的代码慢不到 1.5 倍，就做第 4 步；慢得多，就看能不能按函数找出空闲寄存器换掉（更复杂），或者退回到“解释执行能用但慢”。

### 第 4 步：通用的寄存器改写，翻译 WebView 的库（3–4 天）——已完成（2026-11-23）：Wikipedia 的文章 4.4 秒加载完成，见 `results/G0.md`

- 所有读写 x25–x28 的指令：数据处理、比较、条件选择，以它们为数据的读写，以它们为基址的读写，带偏移和前后变址的 `stp/ldp`，`br/blr`。每一类写一组测试，和解释器的结果对比（同 `tools/vector-instructions.S` 的做法）。
- 用新改写器重新翻译全部系统模块（约 8 分钟），回归必须全过，开机时间不变。这一步改到了所有代码，最容易出问题。
- 把 WebView 的库加进翻译，随 App 打包。记下 IPA 增加多少（库约百 MB 量级，翻译后约 2.5 倍）。

验收：Wikipedia 的文章 10 秒内显示。

### 第 5 步：应用验证和发布（1–2 天）——已完成（2026-11-24）：1.6 发布

- 逐个试：Wikipedia 文章、Jellyfin、AnkiDroid、DuckDuckGo、今日头条、抖音、Firefox（它自带 Gecko，不用 WebView，用来对比）。
- 量：开机时间、到桌面的内存、打开一篇文章的时间和内存、滚动帧率。
- 更新兼容表、`docs/status.md`、README 的已知问题、网站的 Limits，写发布说明，发 1.6。

### 风险

- WebView 的库很大，翻译后的代码和 IPA 都会明显变大；固定地址只剩约 440 MB。
- 全部重新翻译会碰到所有系统代码，一处改错会让开机失败。改写器的每类指令先有测试再用。
- Chromium 对内核的要求多（共享内存、futex、信号、线程数、`memfd`、`/proc` 的内容），会冒出一串内核层的缺口，第 2 步就会遇到。
- 内存：一个打开网页的应用可能多用几百 MB，iPhone 14（6 GB）可能放不下。

## GitHub issues 的安排（2026-11-24）

| Issue | 问的事 | 排到哪 | 说明 |
| --- | --- | --- | --- |
| [atomsprout/nestvane#2](https://github.com/atomsprout/nestvane/issues/2) | 存储能不能跟随设备的本地存储 | 1.7 第 0 步 | Android 现在看到写死的 8 GiB 上限，显示成“共 16 GB”。改成总量取 App 所在卷、剩余取 iOS 的“重要用途可用空间”减去余量。做完在 issue 里附截图后关闭 |
| [atomsprout/nestvane#3](https://github.com/atomsprout/nestvane/issues/3) | 还需要付费开发者账号吗 | 1.8 | 需要，原因是扩展地址权限只给付费账号。1.8 做软件 MMU，连同代码表、翻译产物、镜像映射这些固定占用一起压到约 7 GiB 以内，验收是免费账号签名的包 30 秒内到桌面。动手前先量到桌面时宿主地址各块的占用。风险最高，可能做不成，issue 保持打开，进展更新在里面 |
| [atomsprout/nestvane#4](https://github.com/atomsprout/nestvane/issues/4) | 怎么在 iOS 16 上跑 | 不做 | 最低系统 iOS 17（2026-11-23 定）。只能停在 iOS 16 的设备是 iPhone 8、8 Plus、X，内存 2–3 GB，而 Android 到桌面就要 2.2–2.6 GB；能装 iOS 17 的设备升级系统即可。试验版在 iPhone XR 上也没跑起来。回复说明原因后关闭 |

## 1.7 详细计划（2026-11-24 定）

目标：用户自己装的应用里，自带原生库的那部分不再慢得不能用；可以在设置里打开内置 root。每一步单独验收，做不下去时停在上一步也能发。

**现状。**

- 应用自带的原生库在原地解释执行（`runtime/AArch64.cpp`），每条指令重新译码，没有块缓存，约每秒四千万条指令。原生代码已经能交给工作线程（`NESTVANE_NATIVE_WORKERS`），**解释执行仍在主循环上**，和系统调用、其他线程的调度共用一个宿主线程。ScriptWidget 主循环一半多的时间在解释它的库。
- 量过的慢：ScriptWidget 的第一个小组件预览约 2 分钟；抖音开机两分钟内黑屏，它的库也是解释执行的（10 个 TCP 连接）。
- 1.6 已经有“按内容识别、放在固定地址、预先翻译”的路（WebView 放在 `0x1_4000_0000`，改写器能处理用满 x18、x25–x28 的现成代码），应用的库理论上也能走这条路，但 1.7 不做（见第 3 步后的说明）。
- 内核层没有 setuid 程序的语义；`su` 不存在。
- 发版门槛：`tools/boot-check.sh` 在 iPhone 14 上中位数不能慢于 11075 ms（`results/boot-baseline.txt`）。

### 第 0 步：1.6 的尾巴（1 天）——部分完成（2026-11-24）：存储跟随设备、开机时第三方应用先不运行（比推迟广播更直接）、渲染进程的 seccomp 都做了，见 `results/G0.md`；麦克风、iPad 横屏、两项主机测试、放置后的冷启动没做

| 事 | 做法 | 验收 |
| --- | --- | --- |
| 存储容量跟随设备 | 见 1.6 第 0 步的同名一行：总量取 App 所在卷的容量，剩余和写入上限取 iOS 的“重要用途可用空间”减去余量 | Android“存储空间”的总量和剩余与 iPhone 设置里一致 |
| 自启推迟 | 第三方应用的 `BOOT_COMPLETED` 推到桌面显示之后发。1.6 量到 Element 自启在开机前 25 秒里占 13 秒 CPU | 装着 Element 时 `boot-check.sh` 的中位数比基线快；推迟后应用照样收到广播 |
| 渲染进程的 seccomp | 1.6 为了先跑通关掉了（`--disable-seccomp-filter-sandbox`）。内核层已有 seccomp 和 SIGSYS，补齐 Chromium 过滤器要的部分后去掉这个参数 | Wikipedia 文章照常加载，时间不变差；渲染进程里有过滤器 |
| 麦克风、iPad 横屏、主机测试、放置后的冷启动 | 同 1.6 第 0 步的表 | 同上 |

### 第 1 步：先量，定位慢在哪（半天）——已完成：Mac 上解释器的时间约三分之一在译码、三分之一在取指和访存；iPad 上 ScriptWidget 解释的线程只分到主循环约 29% 的时间，另有每秒几万次块边界上的故障

- 选三个基准：ScriptWidget 第一个预览（QuickJS）、抖音到出视频、一个小的计算基准（同一个库在 Mac 上原生跑一遍，作为“翻译后能到多快”的上限）。
- 用现有的计数，分出：每条指令的译码、访存查表、分支、系统调用往返、等主循环的时间各占多少。抖音另外列出它的库里哪些函数最热（`NESTVANE_TRAP_DUMP` 和断点的调用者已经有）。

验收：一张表，写进 `results/G0.md`；后面两步按表里的大头排顺序。

### 第 2 步：解释执行放到工作线程上（1–2 天）——已完成，验收没达成：工作线程上解释，调到有翻译的代码时在同一线程本地执行；ScriptWidget 的预览仍然出不来（它给脚本设了截止时间，见 `results/G0.md`）

- 现在原生执行段可以交给工作线程，解释执行不行。改成同一套“交出、运行、取回”：解释的线程连续跑满一个时间片就交给工作线程，遇到系统调用、故障、信号再回主循环。
- 解释器的状态都在线程自己的上下文里，要查清共享的部分（地址空间表的修改、代码表、日志）都有锁或只在主循环改。
- 先只对应用进程开，系统进程不动，开关用环境变量，量过再默认打开。

验收：ScriptWidget 预览时间至少减半；`boot-check.sh` 不慢于基线；`tools/regress-device.sh` 全过。

### 第 3 步：解释器本身提速（2–3 天）——已完成，验收一半：常见指令预先解码、代码窗口和内存窗口，Mac 上快约 2.2 倍，iPad 上每秒 7,000–8,000 万条；ScriptWidget 的预览没有在 30 秒内出来

- **按块预解码。** 一段直线代码第一次执行时译码成操作的数组，以后按数组执行；块按客体地址缓存，页被写或 `munmap` 时作废（`IC IVAU` 已经能识别）。
- **访存直接走地址表。** 解释器读写内存现在每次走通用路径，改成和翻译后代码一样按 1 MiB 一块查 `GuestSpace` 的表，只在缺页时走慢路。
- **热点指令先做。** 按第 1 步的表挑：通常是加载/存储、比较分支、加减和逻辑运算。
- 每类改动用 `tools/validate-rewriter.py` 的办法和 Mac 处理器对比，主机测试加进 `tools/validate-host.sh`。

验收：计算基准的解释速度至少翻倍（每秒八千万条以上）；ScriptWidget 第一个预览 **30 秒以内**（原定的 1.7 验收）。

**不做：常见开源库预先翻译（2026-11-24 定）。** 一份翻译只对得上库的一个版本，应用一升级就失效，而且只能做开源许可的库。应用自带的库全部靠第 2、3 步提速。

### 第 4 步：内置 root（2–3 天）——已完成：用自己的测试程序 `fixtures/root` 在 iPad 上验证四种情况：允许时 `su -c id` 是 uid 0、能读只有 root 能读的文件；没回答时 iOS 弹窗询问（名字是包名），su 等着；拒绝时 Permission denied；开关关着时没有 su。没有用 Termux（不再下载应用）；交互的 shell 没有验证伪终端

- **开关在 Nestvane 的设置里，默认关**，打开时说明风险（Android 里任何得到允许的应用都能读写所有应用的数据）。
- **`su` 由内核层实现。** 镜像里放一个 `/system/xbin/su`；内核层在 `execve` 到这个文件时，如果开关打开、发起的应用已被允许，就把进程的凭证换成 uid 0、所有权能，SELinux 域换成一个专门的 `su` 域。其他文件照旧没有 setuid 语义。
- **每个应用要问一次。** 第一次申请时由 App 弹出 iOS 的提示（“<应用名> 申请 root 权限”，允许/拒绝），结果按包名记住，设置里能看到和撤销。
- 支持常见用法：`su`、`su -c <命令>`、`su <uid>`、交互的 shell（要有伪终端）。

验收：Termux 里 `su -c id` 显示 `uid=0(root)`；拒绝的应用拿不到；开关关掉后 `su` 不存在或失败；一个查 root 的应用显示已 root。

### 第 5 步：应用验证和发布（1 天）——已完成（2026-11-24）：1.7 发布，开机门槛 iPhone 14 中位数 10.92 秒（基线 11.08 秒），见 `results/G0.md`

- 逐个试：ScriptWidget、抖音、今日头条、兼容表里带原生库的其他应用，以及 Termux 的 root。
- **`tools/boot-check.sh` 必须通过**（不慢于 11075 ms），通过后 `--record`，数字写进发布说明。
- 更新兼容表、`docs/status.md`、README、网站，发 1.7。

### 风险

- 解释执行上工作线程后，原来只在主循环里串行发生的事会并发：最容易出的是偶发的错，回归要多跑几遍。
- 预解码的块缓存要正确作废，应用自己生成代码（极少数不用 JIT 的引擎也会改代码页）时容易出错。
- root 是安全上的口子，默认关、逐个应用问、能撤销；不做成“给所有应用 root”。

## KernelSU 兼容（2026-11-24 记，排在 2.1）

1.7 的 root 由内核层实现：执行 `/system/bin/su` 时把进程换成 root，谁能拿由 iOS 弹窗决定。这和 KernelSU 的思路一样（root 在内核里决定，管理器通过一个特殊的系统调用跟内核说话），所以兼容它主要是在内核层实现它的接口。选 KernelSU 不选 Magisk：Magisk 要修补启动镜像、开机先跑 `magiskinit`、常驻 `magiskd`、用 Zygisk 注入 zygote，我们的启动流程是自己的，注入还会碰到预先翻译的代码，成本高得多。

1. **先量清楚接口。** 读 KernelSU 管理器和 `ksud` 跟内核的约定：管理器用的系统调用和命令号、版本号、授权列表（允许和拒绝的 uid）、App Profile、`su` 的路径和行为，以及管理器怎样判断“已工作”。列成表写进 `results/G0.md`。
2. **管理器能用。** 在内核层按表实现这些命令：版本、是否为管理器（按包名和签名认定，用户自己装官方的 Manager）、读写授权列表。授权的来源改成两个都认：iOS 弹窗和管理器里的开关，答案存在同一处，Nestvane 设置里的列表与管理器一致。验收：Manager 显示已工作，能看到应用列表、给一个应用授权后它的 `su -c id` 是 uid 0，撤销后拿不到。
3. **模块（可选，之后再定）。** KernelSU 的模块靠 `ksud` 在开机各阶段运行脚本，再用 overlay 挂载改系统文件。要在开机流程里起 `ksud`，并确认内核层的 overlay 挂载能用到什么程度。验收：一个只改 `system.prop` 的简单模块生效。

**许可。** KernelSU 是 GPL。只按接口自己实现、不拷它的代码，Nestvane 的 Apache 许可不受影响；Manager 和 `ksud` 由用户从官方下载，不随 App 打包。

**不做。** Magisk 本身、Zygisk、Xposed 这类往 zygote 里注入代码的框架。

## 待办：软件 MMU，去掉对扩展地址权限的依赖（2026-11-22 记，暂不做）

这个思路来自两仪（Twoyi）一位开发者的提示：客体内存不要直接映射到宿主地址上，用软件 MMU。感谢。

- **现状。** 翻译后的代码已经按 1 MiB 一块查表访问内存（`runtime/GuestSpace.hpp`），但每个客体进程各占一份宿主地址。从 zygote fork 出来的几十个进程各带一份 zygote 的地址，物理内存靠 `vm_remap` 的写时复制共享，地址不共享。iOS 不带扩展地址权限只给约 7 GiB，带上约 53 GiB，所以现在必须有这项权限，也就必须付费开发者账号签名。
- **要做的。** 所有客体进程共用一个内存池，只在宿主里映射一次；每个进程一张页表，记录客体地址对应池里的哪一页、能不能写；fork 和共享内存由两张页表指向同一页实现，写时由运行时自己复制。宿主地址的占用就只和实际用到的物理内存成正比。
- **代价。** 要重做地址空间管理、fork、mmap、共享内存、文件映射和缺页处理，翻译器生成的访问序列要改，全部代码重新翻译。每次访问比现在的三条指令查平表更慢（现在约为原生的 1.31 倍），慢多少要实验。按页打散后相邻两页在宿主里不相邻，跨页的读写要单独检测。写保护要在查表时区分读和写。
- **只做它不够。** 翻译后的代码约 2.6 GB、代码跳转表约 3.3 GiB（只占地址）、系统镜像的映射约 1.5 GB，这三块固定占用加起来已经超过 7 GiB，要一起压：跳转表缩小或只覆盖用到的模块，代码和镜像按需映射。
- **另一项权限是另一回事。** “提高内存上限”管的是物理内存，到桌面要 2.2–2.7 GB；不带它时上限是多少、够不够，没有测过。
- **动手前先量。** 到桌面时宿主地址实际用了多少、上面每一块各占多少，还没有量过。

## 首个开发批次

按依赖切成三个可独立验收的改动。

| 顺序 | 改动 | 验收 |
| --- | --- | --- |
| 1 | iOS 空壳、客体状态与 ELF 加载、单步解释和自有夹具 | 无调试器启动 App，页面显示客体输出及退出码 |
| 2 | 解码块执行、状态比对和计时 | 同一 ELF 两条路径结果一致，生成可导出的基准记录 |
| 3 | 离线编译器、显式 AOT 入口、回退测试 | 同一签名 App 完成三模式测试，给出 P1 继续条件的判断 |

代码实际开始前，目录按功能需要落地。可先用 `ios/`、`runtime/`、`tools/`、`fixtures/`，待形成客体构建时再加 `guest/`。在完成 P1 前，投入集中在正确性与可测性。

## 资料入口

- Android 17 AOSP 标签 : https://android.googlesource.com/platform/manifest/+/refs/tags/android-17.0.0_r1
- AOSP 构建环境 : https://source.android.com/docs/setup/start/requirements
- Cuttlefish : https://source.android.com/docs/devices/cuttlefish/get-started
- ART Service : https://source.android.com/docs/core/runtime/configure/art-service
- AOSP 页大小 : https://source.android.com/docs/core/architecture/16kb-page-size/16kb
- UTM iOS 与 SE : https://docs.getutm.app/installation/ios/
- gfxstream : https://github.com/google/gfxstream
- MoltenVK : https://github.com/KhronosGroup/MoltenVK
- Android AIDL Camera HAL : https://source.android.com/docs/core/camera/camera3
- iOS 摄像头帧采集 : https://developer.apple.com/documentation/avfoundation/avcapturevideodataoutput
