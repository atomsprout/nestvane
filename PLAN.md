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

## 1.1 之后的版本规划（2026-11-22 定）

1.1 发布后剩下的四个大问题，按用户受影响的程度排：

1. 普通用户装不了：扩展地址权限只给付费开发者账号。
2. 应用自带的原生库只能解释执行，约每秒四千万条指令。
3. 显示路径浪费：每帧读回 CPU 再转成图片，读取用自己的定时器。120 Hz 下合成 113–121 帧，交给 iOS 的只有 85–120 帧。
4. 媒体不完整：视频软解，只测到 720p，没有麦克风和录制。

版本按这个顺序排。估计的天数按 1.0 和 1.1 的节奏给，不是承诺。

| 版本 | 主题 | 内容 | 验收 | 估计 |
| --- | --- | --- | --- | --- |
| 1.2 | 显示与日常体验 | 画面直接交给 GPU 图层，不再读回 CPU；由屏幕刷新信号驱动；发热时自动降帧率；切换网络后恢复；短信补上系统广播，让等验证码的应用能收到；网站和 README 的数字、截图更新到 1.1；More 菜单加“Submit a bug”，跳到 GitHub 的新建 issue 页，自动带上 Nestvane 版本、iOS 版本和机型 | 120 Hz 下交给 iOS 的帧稳定在 115 以上；同一段滑动的 CPU 占用比 1.1 低，前后都量；Wi-Fi 切到蜂窝后应用仍能联网 | 2–3 天 |
| 1.3 | 媒体 | H.264 和 HEVC 硬件解码；1080p 播放；麦克风输入 | 1080p 30 帧不掉帧，声音同步；录音应用能录到声音 | 3–4 天 |
| 1.4 | 应用原生代码速度 | 解释器提速（上面“用户安装的应用”一节的三条路）；把装不上或起不来的应用分类 | ScriptWidget 第一个预览 30 秒以内；一张实测过的应用兼容表 | 4–5 天，效果不确定 |
| 2.0 | 不需要付费账号 | 下一节的软件 MMU，连同固定地址占用一起压；顺带降内存，争取 6 GB 的手机 | 免费账号签名的包 30 秒内到桌面 | 一周以上，风险最高 |

几条说明：

- **2.0 先做实验，不先做实现。** 和 1.2 同时花 1–2 天，把下一节“动手前先量”的数量出来，再用一个小程序量软件 MMU 的访问序列比现在慢多少。开机慢一倍以上就换方向。
- **1.3 和 1.4 可以对调。** 看反馈里说视频卡的多，还是说应用慢、打不开的多。
- **没有排进版本的。** 用快捷指令启动应用、内置 root、无障碍。它们不解决上面四个问题，1.2 之后按反馈再定。
- **1.1 留下没验证的。** 屏幕本身是否每秒刷新 120 次，由 1.2 的显示改动一并回答；用手指操作 120 Hz 开关、短信输入框和更新提示那一行，声音用耳朵听，iPhone 17 Pro 上的全新安装，都还没有做。

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
