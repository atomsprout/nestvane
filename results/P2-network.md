# P2 原版网络模块启动

- 前一轮签名完成后，原版 `init` 因 `bpfloader` 的 Tethering APEX 服务缺失，请求重启。收集同版原版 APEX 后，iPhone 11 激活 7 个原版模块并进入 `netbpfload`，随后因客体缺少 `unprivileged_bpf_disabled` sysctl 退出。[真机](p2-network-iphone11.txt)
- 实现客体 bpffs 挂载和 `unprivileged_bpf_disabled` 后，Mac 上原版 `netbpfload` 继续检查 `bpf_jit_enable`，这是标准启动链当前的失败点。启动配置保留原版网络和自检门槛。[Mac](p2-network-host.txt)
- 自有 API 37 APK 夹具可用 `tools/build-smoke-apk.sh` 构建。在 ARM64 Android 17 参考模拟器中已安装并启动，点击增量，强制停止后重新启动读回 `Count: 1`。Nestvane 真机客体中的 APK 安装待验收。

- iPhone 11 再次验证了原版 `odsign` 成功，并激活 8 个原版 APEX。原版 `netbpfload` 越过了首项 sysctl 检查，最终也停在 `bpf_jit_enable`。[真机复测](p2-network-abi-iphone11.txt)
- Mac 隔离诊断采用一份临时启动脚本，暂不执行网络启动链，未写入 `bpf.progs_loaded`。原版 `init` 创建两个 Zygote 套接字，Bionic 链接器越过旧描述符限制后进入原版 `app_process64`，原版 Conscrypt 自检退出 0。这次记录只覆盖 Zygote 原生入口，进程仍在早期初始化异常退出。[Mac 框架诊断](p2-zygote-entry-host.txt)

- 补入原版 `/init.environ.rc`，修正 ARM64 参数栈中 `argv` 与环境字符串的顺序，以及 Bionic 使用的匿名内存扩展和默认 FPCR 读取。Mac 上的 AndroidRuntime 已开始引导 `com.android.internal.os.ZygoteInit` 并进入 ART 的堆初始化。虚拟运行时与参考开机镜像的读屏障配置不同，ART 拒绝该镜像，随后在 `memfd_create` 处受阻。标准真机启动仍受网络 ABI 限制。[Mac ART 初始化](p2-zygote-runtime-host.txt)

- 实现了客体 `memfd_create` 的启动路径。原版 ART 创建三个描述符后进入真实的回退编译，随后编译进程遇到当前客体内存预算。新增 `tools/prepare-boot-image.py`，从校验通过的同版参考环境离线生成与客体读屏障匹配的 `boot.art`、`boot.oat`、`boot.vdex`。修复低地址文件映射后，Mac 隔离诊断中的 Zygote 打开并映射这组产物，越过先前的现场重编译点。[Mac 预编译验证](p2-zygote-preopt-host.txt)

- 同一组预编译产物在 iPhone 11 的独立框架诊断版上也成功映射。原版 `app_process64` 进入 Zygote 原生引导，随后达到 360 秒诊断限时。该诊断暂不执行网络启动脚本，标准启动仍停在网络 ABI。[真机预编译验证](p2-zygote-preopt-iphone11.txt)

- 原有匿名堆预算在 Mac 反复拒绝低地址 96 MiB 映射。按需提供匿名页后，iPhone 11 和 Mac 均成功映射约 192 MiB 和 96 MiB 两段堆。两者随后复现同一个主线程栈访问错误。统一实际栈和 `RLIMIT_STACK` 后，Mac 上原版 ART 已完成这段栈初始化并开始初始化运行时指标。[真机堆验证](p2-zygote-heap-iphone11.txt) · [Mac 堆与栈诊断](p2-zygote-heap-host.txt)

- 采集同版 ADB APEX 和系统公共库后，Mac 隔离诊断激活 9 个 APEX，原版 Zygote 完成公共库预加载，并加载 `libicu_jni.so`、`libjavacore.so` 和 `libopenjdk.so`。运行随后遇到 ARM64 的 `TRN2`，已补齐 `TRN1/2` 并加入回归验证。完整参考镜像采集命令已成功收集 658 个 Android 文件。[Mac 原生库预加载](p2-zygote-preload-host.txt)

- iPhone 11 复现了相同的库预加载和后续指令缺口。使用 `-Xusejit:false` 后，原版 ART 仍初始化 Zygote 共享缓存。Mac 复测确认 `MFD_ALLOW_SEALING`、`F_SEAL_FUTURE_WRITE` 和 `MADV_DONTFORK` 让原版 ART 完成缓存封印，随后推进到 `FABD` 指令。这批指令和文件封印、映射继承均已加入宿主回归。可通过 `prepare-boot-image.py --framework-diagnostic` 从干净采集目录生成该离线诊断客体。[真机预加载](p2-zygote-preload-iphone11.txt) · [Mac 缓存封印](p2-zygote-seals-host.txt)

- iPhone 11 在关闭客体 JIT 的诊断中同样成功执行了 ART 的两次封印操作，并到达原版框架的 `FABD`。使用工具重新生成的同版客体在 Mac 验证了 `FABD` 之后的路径，并定位到 `FCVTAS`。两条指令均已有实现和回归测试。[真机缓存封印](p2-zygote-seals-iphone11.txt)

- 使用重新采集并生成的独立客体，iPhone 11 上原版 ART 后台验证了 587 个启动类，接着在 `SO_SNDBUF` 停下。补齐 Unix socket 缓冲区选项后，iPhone 11 和 Mac 均进入 Java 层的 `Zygote: begin preload` 和 `ZygoteHooks.beginPreload()`，随后遇到 `SHLL`。已实现并回归测试 `SHLL/SHLL2`。[Java 预加载证据](p2-zygote-java-preload.txt)

- Mac 在 `SHLL/SHLL2` 修复后推进到 `Zygote: Preloading classes...`，随后在临时变更客体 GID 时请求 `setregid`。已补齐客体 `setregid` 和 `setreuid`，并验证临时切换、恢复与无权限请求被拒绝。[类预加载证据](p2-zygote-java-preload.txt)

- 缩减版在 Mac 和 iPhone 11 上均因缺少类而无法初始化 `SystemServiceRegistry`。采集工具改用同版参考环境的四条 Java 类路径选择原版平台 JAR 和 APEX，并收集 `installd` 与 `libandroid_servers.so`。将只读 APEX 目录从可写目录配额中分开后，Mac 和 iPhone 11 均激活 32 个 APEX，原版 `derive_classpath` 输出与 SDK 参考逐字节一致。两端通过原先的缺类位置并执行到 `FMINNM`。已补齐该指令以及原版字体、DRM JNI 和 Aconfig 资源。[完整类路径证据](p2-zygote-full-classpath.txt)

- Ext4 直接引用连续文件范围，将分散区段推迟到首次读取时合并。独立检查解析了全部 32 个真实 APEX 载荷，完成目录索引时约 88 MiB 文件内容使用底层视图，分散的文件尚无合并副本。另通过了合成 ext4 的连续区段、稀疏区段和内联软链接验证。[Ext4 记录](p2-zygote-full-classpath.txt)

- iPhone 11 已加载原版 DRM JNI、Roboto 和字体配置，随后请求 `FIONREAD`。补齐该接口后 Mac 继续执行到向量 `FCVTZS`，之后又到达向量 `FADD`。已补齐两条指令及对应边界回归。[字体预加载与 fork 记录](p2-zygote-system-server-fork.txt)

- Mac 在原版延迟预加载选项下验证 `system_server` 启动路径。补齐 Unix socket 地址查询、客体 MDWE、逐线程 proc stat 和实际描述符枚举后，原版 Zygote 已 fork 出客体子进程。补入 `/storage` 根挂载点后，子进程成功 bind mount `/mnt/user/0` 到 `/storage`。下一处实际失败是创建 `/sys/fs/cgroup/system/uid_1000`。可用 `prepare-boot-image.py --framework-diagnostic --defer-zygote-preload` 生成该诊断镜像。[fork 记录](p2-zygote-system-server-fork.txt)

- 最新的 iPhone 11 真机日志已越过 `FIONREAD` 和前两条向量指令，在字体预加载中推进至寄存器版 `FCMGT`。相关向量比较已实现并通过宿主回归。Aconfig 原版服务已进入执行，默认标志存储初始化仍未成功。[真机与 Aconfig 日志](p2-zygote-system-server-fork.txt)

- iPhone 11 的延迟预加载诊断依次创建了 `uid_1000` 和进程组，把 Zygote 派生的 `system_server` 子进程写入客体 `cgroup.procs`。当前 cgroup2 实现覆盖进程分组和继承，尚未提供内存控制器。真机随后在 `PR_SET_SECCOMP` 停下。[运行记录](p2-zygote-cgroup-seccomp.txt)

- 实现了 Android 引导策略使用的 classic BPF 指令、系统调用过滤和子任务继承。宿主回归验证了权限检查、无效跳转、过滤器快照和 fork 继承。Mac 上原版子进程跨过 seccomp，继续执行到 SELinux 应用域选取。采集器现在保留参考镜像的 `seapp_contexts`。[运行记录](p2-zygote-cgroup-seccomp.txt)

- 第二轮 iPhone 11 真机诊断越过 seccomp，进入原版 SELinux 上下文初始化。随后补入参考镜像的上下文文件、按参考策略校验域名和允许的进程域切换，并补齐实际遇到的 `CMTST`。Mac 已越过原版 libselinux 上下文初始化失败点，下一处停在 `libart.so` 的零地址读取。当前诊断只覆盖采集到的启动域转换。[运行记录](p2-zygote-cgroup-seccomp.txt)

- iPhone 11 同样通过了 SELinux 上下文初始化，并在 `libart.so` 的相同指令和相邻指令序列读到零地址。采集器从干净目录重新采集了 1009 个参考文件，所得两条进程域转换和设备诊断所用的转换一致。宿主回归和 ASan/UBSan 均通过。尚未观察到 Java `SystemServer` 服务就绪。[运行记录](p2-zygote-cgroup-seccomp.txt)

- 已实现客体 ARM64 的同步访存信号、备用信号栈和 `rt_sigreturn`。同一个 Bionic ELF 在 SDK 参考系统和 Nestvane 上均能从零地址和 `PROT_NONE` 访存恢复，信号码和地址一致。Mac 原版 ART 接收信号后改写返回 PC，继续执行到 Java `SystemServer`。初次运行所用旧策略快照没有该域的属性权限记录。[信号及框架日志](p2-zygote-signal.txt)

- iPhone 11 也观察到 ART 处理访存信号并恢复。Mac 换用新采集的策略后，原版 Java `SystemServer` 成功写入启动属性，输出 `Entered the Android system server!`，并完成对当前镜像所含权限配置的读取。下一处实际请求是查找客体 eBPF 对象，已开始在同一诊断镜像上验证。[运行日志](p2-zygote-signal.txt)

- iPhone 11 上原版 Java `SystemServer` 已成功进入并完成当前权限配置读取。在离线配置中，客体 eBPF 查找按文件系统状态返回 `ENOENT`，原版内存事件监听器继续走缺对象路径。程序随后在 `ApplicationSharedMemory` 停下。实现了 `/dev/ashmem` 共享映射和权限收窄，同一测试 ELF 在 SDK Android 和 Nestvane 上验证两条映射共享状态，关闭描述符后仍能访问。采集器也已保留参考分区中的 `permissions` 和 `sysconfig` XML。[运行及对照日志](p2-zygote-signal.txt)

- iPhone 11 读取了同版 `platform.xml`，原版 `libcutils` 成功打开动态生成的 `/dev/ashmem<boot-id>`。Mac 将匿名共享对象预算按启动配置放宽后，完成 `ApplicationSharedMemory` 并进入 `SystemServer.createSystemContext()`。延迟预加载诊断在 Perfetto JNI 注册处停下，已切回常规 Zygote 预加载复测。[运行日志](p2-zygote-signal.txt)

- iPhone 11 验证了新的共享内存预算，原版 `SystemServer` 推进到 `createSystemContext()` 内的 Perfetto JNI 注册调用。常规预加载在 Mac 暴露的 `SSHLL` 和标量 `FADDP` 已补齐并通过回归。本项目自有 APK 已在 API 37 参考模拟器安装并冷启动，Nestvane 客体尚未启动到包管理可用。[运行日志](p2-zygote-signal.txt)

- 常规 Zygote 预加载经过 `FADDP` 后开始加载媒体类，采集器补入原版媒体、音频和 RTP JNI 库及其 ELF 依赖。Mac 补齐后成功加载 RTP JNI，随后暴露 VINTF 和 RenderScript JNI 缺件，采集器也已纳入这两个库。另已采集原版 `cmd`，用于后续连接实际包管理服务。iPhone 11 的首轮常规预加载在诊断时间上限内进入 Java 类预加载，未完成该阶段。[运行日志](p2-zygote-eager.txt)

- 对 Mac 启动探针采样后，增加了按客体页索引的映射查找缓存。缓存命中仍检查当前映射和权限。还将 ELF 入口观察限制在实际命中待观察地址时。同一镜像两次各执行 90 亿条指令，单次耗时从 163.7 秒降到 141.0 秒。宿主 ABI 回归和 ASan/UBSan 通过，iPhone 11 已开始运行包含这项改动的诊断版。[测量记录](p2-zygote-eager.txt)

- iPhone 11 常规 Zygote 预加载记录了 18,596 个 Java 类和原版 RTP、VINTF、RenderScript JNI 加载成功。Mac 完成 Java 类预加载后进入资源初始化，已补齐向量 `UCVTF`、`SCVTF` 和立即数 `FMOV`，并采集 Zygote 缓存的参考 Java 库。原版 `cmd -l` 已在 Mac 上通过客体 Binder 读取 servicemanager 的服务列表并退出 0。诊断时间和指令上限现可通过宿主环境配置，默认上限保留。[运行记录](p2-zygote-eager.txt)

- Mac 常规 Zygote 已完成资源预加载，并从原版路径成功加载 `libperfetto_framework_jni.so`。下一步实际缺件为 `libcompiler_rt.so`，已从同版参考系统采集并加入可重建的镜像输入。当前镜像的原版 Java 和 JNI 输入通过了干净采集对照，向量指令通过宿主回归和 ASan/UBSan。[运行记录](p2-zygote-eager.txt)

- Mac 确认原版 `libcompiler_rt.so` 和 `libhttpengine.so` 均加载成功。下一处不支持的 `MOVI` 填一移位已补齐，并补采了参考分区的 `compatconfig`。采集测试验证了 `libcompiler_rt.so` 与本次诊断镜像逐字节一致。[运行记录](p2-zygote-eager.txt)

- iPhone 11 的常规 Zygote 已完成资源预加载，实测从原版路径加载了 Perfetto JNI。该构建停在缺少 `libcompiler_rt.so`，随后被 Zygote 重启流程带到诊断时限。Mac 确认补齐该库后可加载 HTTP 引擎并读取 333 条兼容规则。下一条 `SMAX` 已补齐，共享目录别名下的 `stat` 和 `access` 查找也有回归覆盖。[运行记录](p2-zygote-eager.txt)

- iPhone 11 复测确认常规预加载成功加载 `libcompiler_rt.so` 和 HTTP 引擎，读取 334 条兼容规则。真机运行约 372 秒后停在 `USUBL`。宿主回归和 Mac 启动测试已越过该指令，随后停在向量按元素乘法。新增 CLI 实时启动日志和 iOS `boot-progress.txt`，可以在完整报告写出前查看启动位置。iOS Release 签名构建已通过，真机复测待进行。[运行记录](p2-zygote-eager.txt)

- Mac 的常规预加载已越过按元素向量乘法，推进到 `SMLAL`。已实现有符号和无符号向量扩宽乘法及累加、累减，宿主回归和 ASan/UBSan 通过。iPhone 11 已从 `boot-progress.txt` 实时读到原版 Zygote 类预加载日志。[运行记录](p2-zygote-eager.txt)

- iPhone 11 复测已越过 `USUBL`，随后和 Mac 一样到达按元素向量乘法。包含两批乘法修复的签名构建已在同一台真机启动。Mac 已越过扩宽乘法，随后在读取 `NZCV` 时停下。其读写已实现并通过宿主回归和 ASan/UBSan。[运行记录](p2-zygote-eager.txt)

- iPhone 11 已越过两次向量乘法停点，在 `NZCV` 复现 Mac 的旧停点。Mac 补齐 `NZCV` 后，常规 Zygote 完成全部预加载并进入原版 `SystemServer`，ActivityManager 启动时因找不到 `/proc/meminfo` 退出。新增内存信息快照，与客体 `sysinfo` 共用估算。已通过宿主回归、ASan/UBSan 和 iOS Release 构建，新一轮真机启动已开始。[运行记录](p2-zygote-eager.txt) · [Android SysMemInfo](https://android.googlesource.com/platform/system/memory/libmeminfo/+/refs/heads/main/sysmeminfo.cpp)

- Mac 复测已越过 `/proc/meminfo`。ActivityManager 启动期间，`SystemServer` 多次等待 `installd`，watchdog 随后在换算 CPU 时钟 tick 时除以零。已为客体 ELF 提供 `AT_CLKTCK=100`，同一个动态链接夹具在 Nestvane 和原版 API 37 均读到 100 并退出 0。离线诊断在宿主提供 `/data` 后恢复触发原版 `nonencrypted` 启动动作，以带起 `installd` 等服务。采集器同时补齐缺少的 WebView 引导库，干净镜像已重建。[运行记录](p2-zygote-eager.txt) · [Bionic sysconf](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/bionic/sysconf.cpp) · [Android init 事件](https://android.googlesource.com/platform/system/core/+/refs/heads/main/init/builtins.cpp)

- iPhone 11 在同一次离线启动中完成 18,596 个类和常规 Zygote 预加载，随后进入原版 `SystemServer` 并启动 ActivityManager。真机复现后续的 `installd` 等待和 watchdog 除零。Mac 在新版离线配置中启动了原版 `installd`，并正常加载原版 WebView 引导库。`installd` 首个挂起点是 `quotactl(Q_GETQUOTA)`。新增客体查询处理，已挂载的只读 Ext4 返回无配额操作的 Linux 错误，原版 `installd` 可据此选择目录统计路径。实际继续执行待复测。[运行记录](p2-zygote-eager.txt) · [Android 配额探测](https://android.googlesource.com/platform/frameworks/native/+/refs/heads/main/cmds/installd/QuotaUtils.cpp)

- Mac 复测中，原版 `installd` 越过配额查询，完成布局初始化。原版框架通过 Binder 找到它，并推进到 ActivityManager。随后 `SystemServer` 停在 `setsockopt(SOL_SOCKET, SO_REUSEADDR)`。已补齐该选项的读写，宿主回归、ASan/UBSan 和 iOS Release 构建通过。[运行记录](p2-zygote-eager.txt) · [Linux socket 选项](https://raw.githubusercontent.com/torvalds/linux/master/net/core/sock.c)

- iPhone 11 同样确认原版框架找到了 `installd`。它随后在 watchdog 查询 `HwBinder` 服务时抛出异常。Mac 越过 `SO_REUSEADDR` 后到达 ActivityManager 的调度线程创建，原先 64 个客体任务上限导致 `pthread_create` 失败。已提高到 256，并补齐客体 tmpfs 的目录访问、文件权限和同挂载点改名，使原版 Aconfig 能尝试在具有组写权限的目录里生成标志表。宿主回归、ASan/UBSan 和 iOS Release 构建通过。[运行记录](p2-zygote-eager.txt)

- 权限修复后，Mac 和 iPhone 11 都确认原版 Aconfig 能打开默认标志和 `/metadata` 目标文件，随后停在 `copy_file_range`。Mac 返回跨挂载点错误后观察到原版代码改用 `sendfile`，现已实现客体常规文件间复制。并实现 HIDL 使用的 Binder scatter/gather 请求和回复。宿主回归覆盖跨进程缓冲区复制、父指针修正和非法偏移，ASan/UBSan 通过。最新版已在 iPhone 11 开始复测。[运行记录](p2-zygote-eager.txt) · [HwBinder 传输](https://android.googlesource.com/platform/system/libhwbinder/+/refs/heads/main/IPCThreadState.cpp)

- Mac 复测确认原版 Aconfig 平台与主线初始化均退出 0，原版 Java 框架取得 HIDL 服务管理器。ActivityManager 下一步停在 `inotify_init1`。现已在客体文件系统实现文件监听，覆盖创建、写入、关闭、改名、删除、取消监听与 epoll 唤醒，宿主回归、ASan/UBSan 和 iOS Release 构建通过。[运行记录](p2-zygote-eager.txt) · [Linux inotify ABI](https://man7.org/linux/man-pages/man7/inotify.7.html)

- iPhone 11 确认原版平台 Aconfig 初始化退出 0，并取得 HIDL 服务管理器，随后复现同一个 `inotify_init1` 停点。真机保留的数据还触发了临时 Wi-Fi 标志文件的创建上限，宿主回归已验证持久文件和临时文件可分别使用各自的数量上限。真机数据保持原状，下一轮验证主线 Aconfig 和 inotify。[运行记录](p2-zygote-eager.txt)

- Mac 复测已完成原版 ActivityManager、DataLoaderManager 和 IncrementalService 启动，进入 PowerManager 后暂时没有可运行的客体线程。原调度器只会等待一秒内的定时事件，现按探针总时限等待客体定时事件。已通过宿主、ASan/UBSan 和 iOS Release 构建验证。iPhone 11 正在复测上一版的 inotify 和文件数上限修复。[运行记录](p2-zygote-eager.txt)

- PowerManager 等待唤醒锁时，init 日志显示原版 `system_suspend` 可执行文件缺失。已将原版服务、ELF 依赖和 SELinux 启动规则纳入采集，干净重建了 Android 17 离线镜像，iOS 签名构建通过。原版 suspend 服务在没有客体电源节点时支持 socketpair 回退，后续核对实际注册和 PowerManager 交互。[运行记录](p2-zygote-eager.txt) · [Android suspend 服务](https://android.googlesource.com/platform/system/hardware/interfaces/+/refs/heads/main/suspend/1.0/default/main.cpp)

- 新增 HAL 后，参考 `service_manager` 决策文件增长至 1,074,726 字节。调高有界输入上限后，Mac 成功载入完整决策并走完原版 PowerManager 启动，实测该阶段 327 ms。上一轮 iPhone 11 在 ActivityManager 中遇到 `SO_RCVTIMEO`；现已补齐 ARM64 两种选项号的读写、阻塞接收时限以及数据到达时的唤醒，宿主与 ASan/UBSan 验证通过。[运行记录](p2-zygote-eager.txt) · [Linux socket 接收超时](https://man7.org/linux/man-pages/man7/socket.7.html)

- iPhone 11 第 7129 号进程的实时日志确认原版 PowerManager 启动完成，耗时 1,018 ms。为后续显示启动验证，采集器已纳入原版 Lights HAL、SurfaceFlinger、ELF 依赖与 SELinux 进程转换。Android 17 参考采集共 1,155 个文件，重建的离线镜像为 799,348,572 字节，低于加载上限。显示服务与包管理待运行验证。

- Mac 离线复测进入 LightsService，并记录 `setsockopt(SOL_SOCKET, SO_SNDTIMEO_OLD)`。客体已支持两种 ARM64 发送超时选项，以及阻塞发送、写入和连接在队列不可用时的期限。宿主回归覆盖期限到达、队列重新可用及非阻塞调用，ASan/UBSan 通过。带发送超时和原版显示组件的签名构建已在 iPhone 11 启动，进程号 7150。[Linux Unix socket 实现](https://raw.githubusercontent.com/torvalds/linux/master/net/unix/af_unix.c)

- Mac 复测中，原版 Lights HAL 响应支持灯光查询，LightsService 完成启动（63 ms），随后进入 DisplayManager。框架在 `fnmul d17, d7, d7` 停止，SurfaceFlinger 同时报告缺少 EGL 实现。运行时已实现单精度与双精度 FNMUL，宿主回归和 ASan/UBSan 覆盖本次指令编码、负零和目标寄存器高位清零。[Arm 指令参考](https://documentation-service.arm.com/static/67e40f3398aa3c3b6eea6a85)

- Android 17 参考镜像采集现纳入 Ranchu Composer3、Allocator、EGL 和 Mapper 及各自依赖，干净采集共 1,192 个文件，离线客体镜像为 806,815,781 字节。参考模拟器的图形服务打开了 `goldfish_pipe_dprctd`、`goldfish_address_space` 和 `goldfish_sync`。诊断镜像保留匹配的 EGL、gralloc 和管道选择属性，原版 SurfaceFlinger 已在 Mac 进入进程初始化。只读镜像映射上限已独立于客体可写内存调至 1 GiB。iOS 签名构建通过，真机启动日志保留带相对时间的近期里程碑。[Goldfish 图形传输](https://android.googlesource.com/device/generic/goldfish-opengl/+/a24d1a13e7c6eca9b57a35cdd5fe5e3ac50ec103/system/OpenglSystemCommon/HostConnection.cpp)

- iPhone 11 的第 7150 号进程完成 LightsService（185 ms），进入 DisplayManager，并复现已修复的 `fnmul d17, d7, d7` 停点。Mac 使用后续镜像和 FNMUL 修复越过该指令，DisplayManager 随后等待 `SurfaceFlingerAIDL`。定向跟踪确认原版 Ranchu Composer3 和 Allocator 到达 ELF 入口，原版 EGL 驱动尝试打开 `/dev/goldfish_pipe_dprctd`，收到 `ENOENT` 后初始化失败。图形失败现写入实时里程碑。新版签名构建已在 iPhone 11 启动，进程号 7167。

- macOS 使用 SDK 自带的 `libgfxstream_backend.dylib`，经 Gfxstream 公共 C ABI 实测 64×64 RGBA8 缓冲区上传和读回，16,384 字节一致。复现使用 Gfxstream 头文件版本 `79c2174f9133fa004c1c9c70ecd50970f42fa53b` 和以下命令。该上游版本用 iPhoneOS SDK 构建静态后端时首先在 macOS 的 `libproc.h` 停止，源码还引用了 AppKit 和 macOS OpenGL。iPhone 本地渲染后端和 Goldfish 传输仍待接通。

  ```bash
  sdk_lib="$HOME/Library/Android/sdk/emulator/lib64"
  xcrun clang --target=aarch64-linux-gnu -c fixtures/android17-gfxstream.S -o /tmp/nestvane-gfxstream-guest.o
  ld.lld -m aarch64elf -static --no-relax -T fixtures/guest.ld \
    /tmp/nestvane-gfxstream-guest.o -o /tmp/nestvane-gfxstream-guest.elf
  xcrun clang++ -O2 -std=c++20 -Wall -Wextra -Werror -Iruntime -isystem "$GFXSTREAM_SOURCE/host/include" \
    runtime/{Runtime,AArch64,DynamicProbe,ProbeLinux,ProbeData,ProbeBinder,Ext4}.cpp \
    tools/{no-aot,probe-gfxstream-host}.cpp -lsqlite3 -o /tmp/nestvane-probe-gfxstream-host
  export ANDROID_GFXSTREAM_EGL=1 DYLD_LIBRARY_PATH="$sdk_lib:$sdk_lib/gles_angle:$sdk_lib/vulkan"
  /tmp/nestvane-probe-gfxstream-host "$sdk_lib/libgfxstream_backend.dylib"
  /tmp/nestvane-probe-gfxstream-host "$sdk_lib/libgfxstream_backend.dylib" /tmp/nestvane-gfxstream-guest.elf
  ```

- iPhone 11 第 7167 号进程的实时记录复现了 Mac 的图形停点。init 开始后 55,782 ms，原版图形模块打开 `/dev/goldfish_pipe_dprctd` 收到 `ENOENT`。SurfaceFlinger 启动后再次尝试，64,653 ms 时记录 `eglInitialize` 失败。此时真机温度为 nominal。

- 客体代码执行 `openat → write → read → close`，从 Mac 的真实 gfxstream 收到 `rcGetRendererVersion` 响应 `1` 并退出 0。宿主 ABI 测试覆盖描述符复制、分段握手、短写、坏地址后保留读取数据及等待可读。宿主常规与 ASan/UBSan 回归和 iOS 签名构建均通过。Mac 集成测试的适配器只处理该版本查询，iOS App 尚待接入完整渲染器。

- iPhone 11 第 7167 号进程完成 FNMUL 修复后的复测，`DisplayManagerService` 等待 `SurfaceControl.nativeGetCompositionDataspaces`，与 Mac 上的 `SurfaceFlingerAIDL` 等待一致。运行达到 720 秒诊断时限，仍待客体图形后端。

- Gfxstream `79c2174f9133fa004c1c9c70ecd50970f42fa53b` 现可用 [iPhoneOS 补丁](../tools/patches/gfxstream-iphoneos.patch) 构建。补丁把 iOS 窗口实现接到 UIKit/CAMetalLayer，保留原版 EGL 转发，macOS 的 CGL 视频路径仅在 macOS 使用。已从干净源码重新应用、编译和链接。Mach-O 平台为 iOS ARM64，最低版本 16.0，链接依赖中包含 UIKit、QuartzCore 和 IOSurface。ANGLE Metal 库与 App 集成待验证。

  ```sh
  git -C "$GFXSTREAM_SOURCE" apply "$PWD/tools/patches/gfxstream-iphoneos.patch"
  cmake -S "$GFXSTREAM_SOURCE" -B /tmp/nestvane-gfxstream-ios -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DDEPENDENCY_RESOLUTION=SYSTEM
  cmake --build /tmp/nestvane-gfxstream-ios --target gfxstream_backend
  ```

- iPhone 11（iOS 26.0.1，第 7368 号进程）从开发签名包成功载入 gfxstream framework，并解析三个实际渲染入口。此项验证到动态加载为止。加入 [Goldfish 通道补丁](../tools/patches/gfxstream-guest-pipe.patch) 后，iPhoneOS 构建成功导出五个通道操作。Mac 使用同一补丁的构建通过 ARM64 客体版本查询，由原版 RenderChannel 异步响应。该补丁可在 iPhoneOS 补丁之后应用，也可单独用于 Mac 构建。

- Mac 使用带非阻塞通道的本地 Gfxstream 构建运行原版 Android 17。原版 Composer3 和 Allocator 向服务管理器查询并登记各自的默认接口。SurfaceFlinger 完成 EGL 初始化，读到宿主 OpenGL ES 3.0 及最大纹理尺寸 16,384。随后 Ranchu Mapper 打开 Goldfish 共享显存设备失败，尚未验证默认显示。复现可在上述编译命令后，用带通道补丁的宿主库执行 `/tmp/nestvane-probe-gfxstream-host /path/to/patched-renderer.dylib --init guest-root-path guest.sqlite3`。宿主库应使用不同于 SDK 渲染库的文件名，确保 dyld 选择目标二进制。

- ANGLE `64a0b11458e24cc3afa7e04a2fb534109f30f7ab` 用 [iPhoneOS GN 参数](../tools/angle-iphoneos.gn) 和 Chromium Clang 24 构建出 ARM64 `libEGL.framework` 与 `libGLESv2.framework`。Gfxstream 从 App 的 Frameworks 目录加载两者。iPhone 11 第 7381 号进程从签名安装包运行原版 gfxstream 与 ANGLE Metal，`stream_renderer_init` 返回 0，非阻塞渲染通道返回真实协议版本 1。此验证运行在真机上，Android 客体接入与显示帧验证仍待完成。ANGLE 使用默认的运行时 Metal 着色器编译配置。
