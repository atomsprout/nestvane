# nestvane 实施计划

## 目标与首个验收

在普通 ARM64 iPhone 上，用开发签名的 nestvane App 本地运行 Android 17（API 37）。宿主无需越狱、调试器或 JIT。当前真机测试使用 iPhone CX（iOS 26.2），在该设备上进入可交互的 Android 桌面，安装并运行选定的 APK，并让 Android 应用使用 iPhone 的实体摄像头。

首版交付和真机通过条件见 P3。P1 的技术验证用同一个客体 ELF 对比解释执行和预编译执行，以验证无 JIT 原生入口、回退正确性和成本。P1 的关键可行性已完成真机验证，见 [P1 结果](results/P1.md)。P2 的 Android 17 动态链接测试程序已在 iPhone 11 上成功运行，见 [真机结果](results/P2-linker.md)。原版 Android 17 `init` 已在 iPhone 11 输出第二阶段启动日志，见 [启动记录](results/P2-init.md)。原版 init 已生成属性区，独立 Bionic 客体从中读到了 API 与 ABI，见 [属性验证](results/P2-properties.md)。原版 init 已挂载 APEX 和 linkerconfig 使用的 tmpfs，并监听属性 socket，见 [真机日志](results/P2-mounts.md)。原版属性服务线程已运行并响应请求，见 [线程验证](results/P2-threads.md)。原版 init 已完成挂载设置，读取真实启动脚本并开始执行 early-init，见 [真机启动记录](results/P2-early-init.md)。原版 vendor 子上下文与 ueventd 已经越过 exec 并开始执行，见 [服务启动记录](results/P2-services.md)。原版 apexd-bootstrap 已以参考域启动，并在 iPhone 11 扫描到 Runtime APEX，见 [真机记录](results/P2-apex.md)。只读 Runtime APEX 已在 iPhone 11 激活，独立 Bionic 客体已读取挂载后的 linker64，见 [激活记录](results/P2-apex-activation.md)。原版 PID 1 已在真机收到并回收 bootstrap 服务的正常退出。原版 linkerconfig 已在 iPhone 11 生成了系统和 Runtime APEX 命名空间配置，见 [记录](results/P2-linkerconfig.md)。原版 ueventd 已在真机处理客体 loop 设备事件并完成 coldboot，见 [记录](results/P2-coldboot.md)。真机通过了原版 Android 17 系统 BoringSSL 自检并进入 early-fs，见 [记录](results/P2-early-fs.md)。原版 vold 已进入程序并创建设备目录，见 [真机记录](results/P2-vold.md)。原版 servicemanager 已在真机登记为 Binder 上下文管理器，vold 打开了 Binder 设备并完成握手，见 [Binder 验证](results/P2-binder.md)。原版 vold 已注册，原版 vdc 在真机按名称查找并完成两次 RPC，见 [记录](results/P2-vold-rpc.md)。客体 /data 已在真机验证跨 App 进程和覆盖安装持久化，见 [存储验证](results/P2-data.md)。原版 ART 已在 iPhone 11 上执行测试 DEX 并正常退出，见 [ART 真机结果](results/P2-art.md)。原版 Keystore2、软件 KeyMint 和常规 apexd 已在真机推进，见 [Keystore 启动记录](results/P2-keystore.md)。原版 Keystore 和 casefolding 已通过真机启动条件，init 已进入用户 0 初始化。原版用户 0 目录初始化和模块类路径生成已在真机完成，见 [用户 0 真机记录](results/P2-user0.md)。系统 ART APEX 已激活，原版 art_boot、odsign、odrefresh 和 dex2oat64 已在 iPhone 11 进入执行，见 [系统 ART 记录](results/P2-system-art.md)。原版 ART 签名成功属性已在真机读回。标准启动链已进入 Tethering APEX 的原版 netbpfload，见 [网络模块记录](results/P2-network.md)。P2 继续验证客体网络 ABI 和 Zygote。

## 主路线

采用 **AArch64 用户空间执行器 + Linux ABI + 自建 Android 17 客体**。宿主内核不承担客体 Binder、PID 或地址空间语义。客体 ELF 当作数据加载，解释器提供通用执行路径。构建时针对已知 ELF 生成宿主 ARM64 入口，由 Xcode 链接并随 App 签名。详细边界见 [AOT.md](AOT.md)。

整机软件解释用于必要时复现同一客体上的问题或验证性能差距。无 JIT 的 QEMU TCI 会解释内核和用户态指令，可能明显慢于硬件辅助虚拟化。真实差距以同一台 iPhone 上的任务数据为准。Linux 上用带真实内核的参考设备来核对语义，这项工作可独立于 iOS 整机模拟进行。

## Apache-2.0 发布路线

首版以 **Nestvane 自有代码采用 Apache-2.0，随包分发的第三方组件保留各自许可** 为目标。当前仓库尚未声明项目许可证。确认首方代码、文档和素材的授权来源后，再正式添加根目录 `LICENSE`。按发布构建的实际内容列出第三方版本、来源、许可和适用的通知。保留上游版权和 SPDX 标识，包括补丁携带的上游代码。Apache-2.0 标签只用于有权按该许可发布的文件。[Apache-2.0](https://www.apache.org/licenses/LICENSE-2.0)

- **宿主运行时**沿用自有 ARM64 执行器、Linux ABI 和 iOS 桥接。参考 Kernarium 的架构及对照数据，避免把 GPL 运行时或原生 Linux 内核链接进首版 App。Kernarium 的 Linux 内核为 GPL-2.0-only，并与宿主代码组成同一二进制文件。更换链接方式本身无法解决由此产生的许可问题。[Kernarium 许可说明](https://github.com/imxieyi/Kernarium/blob/main/LICENSES/README.md) · [ASF 兼容性说明](https://www.apache.org/licenses/GPL-compatibility.html)
- **Android 17 发布镜像**由固定的 AOSP 源码版本自建，记录 Repo manifest 和产物校验值。保留实际打包模块的许可与适用通知，包含 APEX、原生库和生成的预编译产物。现有 SDK `default` 镜像用于内部验证。不能仅凭 `default` 名称推定整包的再分发权限。第三方验收 APK 单独导入。图形框架及其传递依赖按实际发布二进制核对许可和通知。[AOSP 许可说明](https://source.android.com/docs/setup/contribute/licenses) · [Android SDK 条款](https://developer.android.com/studio/terms)
- **性能先验证执行器，再决定投入。** 用同一镜像和数据卷在 iPhone CX 上采样，区分用户态指令、ABI 调度和等待开销。先做可开关的镜像绑定宿主 AOT，覆盖启动采样命中的系统 ELF 和 `boot*.oat`，保留对后装 APK、未知映射和异常的解释路径。范围从真实启动关键路径扩大，净收益用端到端时间和峰值内存判断。
- **并行按正确性和实测收益推进。** 若采样显示不同地址空间有足够的并行工作，先验证两个宿主执行线程，并回归共享页可见性、原子操作、futex、Binder 唤醒和退出。继续条件为真机启动阶段确有收益。最终仍按 P3 完成 APK、持久化和实体相机验收，并在无宿主 JIT 的 iPhone CX 上连续三次 30 秒内进入可触控桌面。若性能差距仍大，再评估是否接受独立分发、保有自身 GPL 义务的内核组件及真实设备边界。

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


## 30 秒冷启动性能目标

从打开已安装的 Release App 到 Android 17 桌面接受第一次触摸，目标为 **30 秒内**。保留 P3 的连续三次冷启动验收，使用已装验收 APK 的同一数据卷，另测全新数据卷首次初始化。记录 iOS 启动、客体 init、Zygote、`sys.boot_completed`、Launcher 绘制、宿主呈现和实际触摸响应，同时记录热状态及峰值内存。进程内恢复单独计时。iPhone 17 Pro 上，原构建的全新数据卷在 1210.626 秒报告 Launcher 完成绘制，录屏确认触摸打开应用抽屉。预编译版的全新数据卷在 700.458 秒报告 Launcher 完成绘制。含触摸修复的版本从已有 APK 的数据卷启动，在 534.791 秒报告 Launcher，随后确认可用触摸打开应用抽屉。同一进程已从 Launcher 打开 OpenCalc，并通过触摸输入数字 7。进入 Android Settings 的对照仍待完成。随后套接字唤醒改动版在 iPhone 17 Pro 的已有数据卷上于 497.027 秒绘制 Launcher，热状态和数据卷与此前不同。iPhone CX 的全新数据卷首轮在 723.697 秒报告 Launcher 绘制。XCUITest 在启用 iOS 自动化时超时，同轮尚无触摸验收结果。

1. 保留现有无 JIT 路径，收集各启动阶段的客体 ELF、文件偏移、解释和 AOT 指令数，以及宿主执行、文件 I/O、Binder、等待和绘制成本。
2. 做可开关的镜像绑定 AOT 批次。先覆盖采样确认的 Bionic、ART 和系统库基本块，再扩大到同一镜像的 `boot*.oat`。生成代码随 Release App 签名，只接受匹配的不可变客体指令。差分比较跨页读写、异常、寄存器和可变映射回退。用真实启动对照评估耗时、包体积和内存，再扩大覆盖。
3. 按实测启动关键路径评估 ART 预编译产物复用和非关键工作的后移。保留 Binder、原版包管理、输入、图形和相机验收。若扩大 AOT 覆盖后仍有数量级差距，按 Apache-2.0 发布路线评估预编译指令分派和并行客体调度。原生 User-mode Linux 可作为独立架构基准。

参考 Kernarium 的 Android 17 版 Andarium。其 AOT 构建流程覆盖指定 ARM64 ELF 和 `boot*.oat`，以 Android 发布版本和不可变代码页校验控制绑定。其 Android 数据快照保存磁盘状态，运行内存休眠用于同一宿主进程内恢复。Kernarium 的宿主 CPU、内核和图形路径与 Nestvane 不同，此前在同一台 iPhone 17 Pro 测得 Andarium 两次冷启动到 Settings 分别为 27.602 和 28.941 秒，当前 iPhone CX 的目标以 Nestvane 真机实测为准。[AOT 实现](https://github.com/imxieyi/Kernarium/blob/main/tools/aot/README.md) · [架构](https://github.com/imxieyi/Kernarium/blob/main/docs/architecture.md) · [优化记录](https://github.com/imxieyi/Kernarium/blob/main/docs/optimization-history.md)

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
