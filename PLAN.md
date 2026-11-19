# nestvane 实施计划

## 目标与首个验收

在普通 ARM64 iPhone 上，用开发签名的 nestvane App 本地运行 Android 17（API 37）。宿主无需越狱、调试器或 JIT。最终让用户和 Agent 在同一个客体里操作，支持 APK 安装、持久化和手动接管。

首个交付是**一份可装到真机的 iOS 构建，在同一个客体 ELF 上对比解释执行和预编译执行**。用它检验无 JIT 原生入口、回退正确性和成本，再决定是否投入 Android 启动。首个 Android 验收另见 P3。P1 的关键可行性已完成真机验证，见 [P1 结果](results/P1.md)。P2 的 Android 17 动态链接测试程序已在 iPhone 11 上成功运行，见 [真机结果](results/P2-linker.md)。原版 Android 17 `init` 已在 iPhone 11 输出第二阶段启动日志，见 [启动记录](results/P2-init.md)。原版 init 已生成属性区，独立 Bionic 客体从中读到了 API 与 ABI，见 [属性验证](results/P2-properties.md)。原版 init 已挂载 APEX 和 linkerconfig 使用的 tmpfs，并监听属性 socket，见 [真机日志](results/P2-mounts.md)。原版属性服务线程已运行并响应请求，见 [线程验证](results/P2-threads.md)。原版 init 已完成挂载设置，读取真实启动脚本并开始执行 early-init，见 [真机启动记录](results/P2-early-init.md)。原版 vendor 子上下文与 ueventd 已经越过 exec 并开始执行，见 [服务启动记录](results/P2-services.md)。原版 apexd-bootstrap 已以参考域启动，并在 iPhone 11 扫描到 Runtime APEX，见 [真机记录](results/P2-apex.md)。只读 Runtime APEX 已在 iPhone 11 激活，独立 Bionic 客体已读取挂载后的 linker64，见 [激活记录](results/P2-apex-activation.md)。原版 PID 1 已在真机收到并回收 bootstrap 服务的正常退出。原版 linkerconfig 已在 iPhone 11 生成了系统和 Runtime APEX 命名空间配置，见 [记录](results/P2-linkerconfig.md)。原版 ueventd 已在真机处理客体 loop 设备事件并完成 coldboot，见 [记录](results/P2-coldboot.md)。真机通过了原版 Android 17 系统 BoringSSL 自检并进入 early-fs，见 [记录](results/P2-early-fs.md)。原版 vold 已进入程序并创建设备目录，见 [真机记录](results/P2-vold.md)。原版 servicemanager 已在真机登记为 Binder 上下文管理器，vold 打开了 Binder 设备并完成握手，见 [Binder 验证](results/P2-binder.md)。原版 vold 已注册，原版 vdc 在真机按名称查找并完成两次 RPC，见 [记录](results/P2-vold-rpc.md)。客体 /data 已在真机验证跨 App 进程和覆盖安装持久化，见 [存储验证](results/P2-data.md)。原版 ART 已在 iPhone 11 上执行测试 DEX 并正常退出，见 [ART 真机结果](results/P2-art.md)。原版 Keystore2、软件 KeyMint 和常规 apexd 已在真机推进，见 [Keystore 启动记录](results/P2-keystore.md)。原版 Keystore 和 casefolding 已通过真机启动条件，init 已进入用户 0 初始化。原版用户 0 目录初始化和模块类路径生成已在真机完成，见 [用户 0 真机记录](results/P2-user0.md)。系统 ART APEX 已激活，原版 art_boot、odsign、odrefresh 和 dex2oat64 已在 iPhone 11 进入执行，见 [系统 ART 记录](results/P2-system-art.md)。原版 ART 签名成功属性已在真机读回。标准启动链已进入 Tethering APEX 的原版 netbpfload，见 [网络模块记录](results/P2-network.md)。P2 继续验证客体网络 ABI 和 Zygote。

## 主路线

采用 **AArch64 用户空间执行器 + Linux ABI + 自建 Android 17 客体**。宿主内核不承担客体 Binder、PID 或地址空间语义。客体 ELF 当作数据加载，解释器提供通用执行路径。构建时针对已知 ELF 生成宿主 ARM64 入口，由 Xcode 链接并随 App 签名。详细边界见 [AOT.md](AOT.md)。

整机软件解释用于必要时复现同一客体上的问题或验证性能差距。无 JIT 的 QEMU TCI 会解释内核和用户态指令，可能明显慢于硬件辅助虚拟化。真实差距以同一台 iPhone 上的任务数据为准。Linux 上用带真实内核的参考设备来核对语义，这项工作可独立于 iOS 整机模拟进行。

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

## P3 做出可用的真机原型

- 先做单显示器、单实例、可触摸的低复杂度显示路径。跑通 Android 显示、输入、前后台切换和数据持久化。选定一条跨 App 文件任务，两个测试 App 均可由项目控制，并增加至少一个有明确使用权限、在参考环境已跑通的 ARM64 第三方 APK。
- 定义客体控制服务的最小接口：启动应用、读取可获取的界面树、截图、点击、输入、返回和显式文件传递。控制通道限定在宿主和客体。用户接管时暂停动作队列，恢复时重新读取当前界面。测试不依赖模型才能跑通，先用确定的任务脚本。
- **真机验收**：显示 API 37 的桌面。从打开外层 App 起记录到首个可交互帧。连续运行 10 次跨 App 任务，至少 9 次完成，中途可接管，重启后能读回结果。另记录 20 次冷启动、输入到画面更新的 P95、峰值内存、后台恢复和 30 分钟持续运行表现。首轮交互目标为本地输入到可观察画面变化 P95 小于 250 毫秒，剔除模型、外网请求和无法提前结束的应用动画，逐项保留测量口径。
- AOT 先用在追踪确认占用明显的系统代码。若首要瓶颈是帧传输或渲染，先做显示链路和缓冲区优化。图形加速原型按需评估 gfxstream 与 MoltenVK，对缓冲导入、同步、回收和掉帧单独测量。

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
