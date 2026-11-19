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

- iPhone 11 第 7389 号进程使用 [签名打包工具](../tools/embed-ios-graphics.sh) 安装 gfxstream、EGL 和 GLES 三个框架。正常 App 启动自动初始化本地渲染器，71 ms 后进入原版 Android，随后 SurfaceFlinger 启动。渲染器由完整启动过程持有，客体描述符关闭后再释放。尚待共享显存和显示帧验证。

- Android 17 SDK 参考模拟器中，两个独立 Goldfish 描述符映射同一共享显存块，读回了彼此的写入。客体现在支持 Goldfish 分配、声明、释放和共享映射。共享分配按参考值 16 KiB 对齐。常规块使用同一边界并返回实际分配尺寸。宿主 ABI 回归、ASan/UBSan 与 iOS 签名构建通过。Mac 接入真实 Gfxstream 后，原版 Ranchu HWC 初始化 640×960 主显示配置并启动 VSync 线程，之后在 `cmeq d0, d1, d0` 停止。待验证 Android 帧的合成和真机呈现。[Goldfish 设备 ABI](https://android.googlesource.com/kernel/goldfish-modules/+/f1eb0fa28a745663513be2f426e8c8106f5630fc/uapi/goldfish_address_space.h)

- iPhone 11 第 7389 号进程的最终报告确认原版 SurfaceFlinger 完成 EGL 1.4 初始化，获得 OpenGL ES 3.0，渲染器为 ANGLE Metal Apple A13 GPU，最大纹理尺寸 16,384。原版 Zygote 完成预加载，`system_server` 在约 420 秒进入，LightsService 启动后进入 DisplayManager。此轮构建尚未包含共享显存实现，Ranchu Mapper 打开设备失败，运行到达 720 秒诊断时限。随后共享显存代码和标量 `CMEQ` 修复分别通过宿主回归与 iOS 签名构建。

- [主显示读取补丁](../tools/patches/gfxstream-primary-display.patch) 在通道补丁之后应用，用同一 Gfxstream 后端读取真正提交到主显示的 RGBA8 帧。Mac 诊断用例对比 64×64 帧的全部 16,384 字节，覆盖首帧前读取、缓冲区不足、重复读取、同一缓冲区再次提交。iPhoneOS 后端和带 UIKit 呈现的签名 App 均已构建。
- Mac 用原版 Android 17 越过标量 `CMEQ` 后，Ranchu HWC 到达 640×960 主显示交换链分配。原版 Allocator 打开 `pipe:refcount` 返回 `ENOENT`，随后以 `NO_RESOURCES` 结束申请。SurfaceFlinger 因无法创建 Composer 客户端退出，DisplayManager 在查询合成色彩空间时阻塞。主显示真实客体帧仍待验证。

- [缓冲区生命周期补丁](../tools/patches/gfxstream-color-buffer-lifetime.patch) 在主显示补丁之后应用。Nestvane 启动器启用 Gfxstream 的 `RefCountPipe` 模式，原版客体以小端方式写入缓冲区编号。同一客体文件的最后一个描述符关闭后通知宿主。宿主回归验证分段写入、重复写入、只关闭副本和不完整编号关闭。三个图形补丁均可从固定版本依次应用。Mac 和 iPhoneOS 后端，以及带该通道的 iOS 签名构建均已成功。[Android Emulator RefcountPipe](https://android.googlesource.com/platform/hardware/google/aemu/+/refs/heads/emu-36-1-release/host-common/RefcountPipe.cpp)

- iPhone 11 第 7520 号进程已越过 `CMEQ`，复现原版 Ranchu 的交换链分配失败和 `refcount` 缺失。Mac 接上该通道后，交换链分配原有的 `NO_RESOURCES` 消失，同步 Binder 回复随后报 `FAILED_TRANSACTION`。iPhone 11 第 7565 号进程已安装启动带缓冲区生命周期通道和首帧记录的版本。若 UIKit 接收客体帧，`Documents/display-first-frame.txt` 会记录进程号、耗时和尺寸。

- Binder 现在在接收端安装普通 `BINDER_TYPE_FD`，验证节点或调用的文件接收标志，并将接收端描述符设为 close-on-exec。回归使用两个客体进程验证拒绝时唤醒调用方、接收端描述符号重定位、发送端提前关闭和释放 Binder 缓冲区后的文件可用性。宿主回归、ASan/UBSan 和 iOS 签名构建通过。Mac 上原版 Ranchu 越过交换链分配的 Binder 回复，在 `frintm v1.2s, v1.2s` 这一新解释器停点结束，尚未提交可读帧。[Linux Binder 传输实现](https://android.googlesource.com/kernel/common/+/refs/heads/android-mainline/drivers/android/binder.c)

- Mac 复测加入向量 `FRINTM` 和 `FRINTA` 后，原版 SurfaceFlinger 确认已加载 AIDL Composer3 HAL，连接主显示 HAL ID 0，创建 Surface 消费者并初始化刷新预测。解释器随后在向量 `FNEG` 停止。该指令修复的宿主 ABI 回归与 iOS 签名构建通过，首帧待下一次启动确认。

- iPhone 11 第 7565 号进程验证缓冲区生命周期通道，原版 Ranchu 在约 67 秒进入 640×960 主显示初始化，消除前一轮的资源分配错误。此版仍复现 Binder 描述符回复失败并达到 720 秒诊断时限。Mac 启动在补齐 `FNEG` 后进入 SurfaceFlinger 刷新调度，随后触发 `LDAXP`。成对独占访存、SurfaceFlinger 控制管道的 epoll 支持，以及继续推进后触发的 `FCVTL` 已分别通过宿主回归。新版签名构建记录 Composer3 加载和主显示连接。

- iPhone 11 第 7590 号进程在 68,561 ms 加载原版 Composer3 HAL，在 68,582 ms 连接原版 Android 主显示。最终报告确认在 `fcvtn v2.2s, v2.2d` 停止，与 Mac 停点一致。Mac 加入转换后继续到同一基本块的向量 `FMINNM`，并确认随后使用 `FMAXNM`。这两条指令与既有标量路径共用数值选择逻辑，宿主回归和 iOS 签名构建已通过。尚待原版 Android 首帧呈现。

- Mac 复测越过向量最值操作后，SurfaceFlinger 将主显示设为 60 Hz、完成开启显示的 power mode 2，随后触发 `cmgt v3.2d, v2.2d, #0`。此前控制管道的 epoll 错误已消失。向量有符号比较修复通过宿主 ABI 和 iOS 签名构建。第 7592 号真机验证进程已经运行包含前一批向量修复的版本。

- iPhone 11 第 7592 号进程确认原版主显示已完成 power mode 2，随后触发和 Mac 相同的 `CMGT` 停点。Mac 修复后推进到 `CMLE`，并在相同基本块发现 `UMINV`。两项指令实现的宿主回归和 iOS 签名构建均已通过。

- 补齐 `SMOV` 和向量 `FRINTP` 后，Mac 上原版 Android 17 完成 DisplayManager 启动（740 ms），在 WaitForDisplay 等到主显示（704 ms），随后进入 PackageManagerService。iPhone 11 第 7596 号进程曾停在相同 `SMOV`，包含两项修复的版本已作为第 7604 号进程启动。
- 启动镜像采集加入原版 `bootanimation`，同版 API 37 ARM64 参考采集共 1,196 个文件，确认 `libbootanimation_preapex.so` 及其额外 ELF 依赖，并读回参考 SELinux 的 `init → bootanim` 转换。既有 `framework-res.apk` 包含内建开机动画使用的两张标志图片。待实测客体提交动画帧。[AOSP 开机动画实现](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/cmds/bootanimation/BootAnimation.cpp)

- 带原版开机动画的客体启动到 `bootanim`，创建了动画 Surface，并进入内建动画绘制。Mac 上首先触发向量 `URSHR`，修复后推进到 `RADDHN`。首帧尚待验证。

- iPhone 11 第 7604 号进程完成 DisplayManager（2,458 ms）和 WaitForDisplay（4,291 ms），随后进入 PackageManagerService。系统在包管理初始化期间触发 Watchdog，APK 安装尚待验证。
- Mac 上原版开机动画触发真实的窗口缓冲区刷新。此前 Gfxstream 启动默认声明 Virtio GPU 同步能力，客体 EGL 随后因无法打开 render node 在同步路径触发空指针异常。[Goldfish 同步补丁](../tools/patches/gfxstream-goldfish-sync.patch) 在缓冲区生命周期补丁之后应用，只对 Nestvane 的旧版 Goldfish 启动器关闭这项不支持的能力。Mac 复测越过这处异常并在 SurfaceFlinger 的向量 `FSQRT` 处停止。iPhoneOS 后端目标已构建。

- 离线诊断镜像支持 `--hardware-timeout-multiplier`。本轮真机签名镜像设置为 4，使用 Android 为软件模拟器定义的 `ro.hw_timeout_multiplier` 观察包管理在较长的 Watchdog 时间内能否启动。启动耗时继续按宿主墙钟记录。此项只用于诊断选项，正常配置保持默认值。[AOSP 硬件超时设置](https://android.googlesource.com/platform/frameworks/base/+/HEAD/core/java/android/os/Build.java)

- iPhone 11 第 7616 号进程在 89,606 ms 记录到原版开机动画开始绘制，随后完成 Zygote 的 18,596 个类预加载。iOS 尚未读到主显示帧。带短时渲染跟踪的 Mac 构建捕获到窗口缓冲区刷新，并在原版 Ranchu Mapper 的两处元数据读取记录到相同的 AArch64 `ldr x8, [x22]` 访存异常。Goldfish 客体请求 `PROT_WRITE` 的共享映射并使用同一映射读取元数据。客体现在按 ARM64 Linux 语义允许读取可写页。宿主回归使用原版 ioctl 形状验证两个 `PROT_WRITE` 映射共享同一数据页，并验证 `mprotect` 可撤销和恢复访问。宿主回归和 iOS 签名构建已通过。[ARM64 Linux 页保护实现](https://android.googlesource.com/kernel/common/+/5c2fe8d11ae05411566f9d69321375ea686603d4/arch/arm64/mm/mmap.c) · [Goldfish 客体共享内存映射](https://android.googlesource.com/device/generic/goldfish-opengl/+/12cb957b594973b061f168b4f51dc9059d0af847/shared/GoldfishAddressSpace/include/goldfish_address_space_android.impl)

- Mac 复测在修复映射权限后越过两处 Ranchu 元数据读取异常，宿主收到两次开机动画窗口缓冲区刷新。随后原版 `libui.so` 在变换显示区域时使用 `fcvtms w20, s1`，此前解释器未实现。该指令现在支持从单精度和双精度向负无穷取整后转换为有符号整数。宿主 ABI 回归和 iOS 签名构建通过，主显示继续验证。

- 原版 Composer3 在共享映射和 `FCVTMS` 修复后向宿主发送 `rcCompose`。独立 Gfxstream 二进制没有定义 `CONFIG_AEMU`，此前走完同步合成后会跳过提交主屏的步骤。[无窗口合成提交补丁](../tools/patches/gfxstream-headless-compose.patch) 在 Goldfish 同步补丁之后应用，让独立构建也遵守合成请求中的提交语义。补丁栈从固定上游版本依次应用成功，Mac RGBA8 读回回归通过，iPhoneOS 后端和带此后端的签名构建成功。第 7616 号 iPhone 11 进程确认四倍 Watchdog 限时下再次进入 PackageManagerService（563,664 ms），该次启动早于此渲染补丁。

- Mac 调用栈确认独立 Gfxstream 的 `rcCompose` 等待完成回调时，Apple 默认的主线程合成路径把任务交给了未配置的窗口回调。[离屏合成线程补丁](../tools/patches/gfxstream-headless-worker.patch) 在无窗口合成提交补丁之后应用。显式启用 `Surfaceless` 的 Apple 构建改用合成工作线程，有窗口的调用保持原线程行为。Mac 真实 Android 17 启动读回了 640×960 主显示帧（序号 1），RGBA8 逐字节回归也通过。iPhoneOS 后端已签名并以第 7797 号进程安装启动。第 7616 号诊断在第 900 秒结束，已进入系统包管理初始化和尚在运行的客体 `installd`，仍待验证 APK 安装。

- iPhone 11 第 7797 号进程在 104,642 ms 通过 UIKit 呈现第一张 640×960 Android 主显示帧。`display-first-frame.png` 的 614,400 个 RGB 像素均为黑色，SHA-256 为 `bba1490bddb66d492aac0fc33350fd3f6904245eb7fc9c0a692bfb6a5c5f3295`。这是上游客体提交帧的真机读回结果。既有诊断开关现在也会按进程号保存首张有 RGB 空间变化的帧。Mac 逐字节回归覆盖真实非均匀像素与仅 alpha 不同的情况，iOS 签名构建通过。可识别的 Android 画面、桌面和触控仍待验证。

- Mac 完整验证报告显示，黑色首帧提交后，原版 SurfaceFlinger 的主线程在客体 `sendmsg` 停止。开机动画线程正在等待 Unix 套接字的输入。运行时现可通过已建立连接的 Unix 套接字发送多个 iovec，将它们按一次消息入队，并把发送唤醒交给原有 epoll 路径。回归覆盖坏指针不产生半条消息、单条消息保留边界、接收端收到唤醒及收包后队列为空。宿主回归和 iOS 签名构建通过。客体线程报告同时显示了 epoll 等待的描述符、事件掩码和套接字队列长度，方便确认实际等待对象。

- 补齐发送端后，Mac 原版 Android 17 读回第 3 帧时出现非均匀 RGB。原先的 `sendmsg` 停点经 ELF 符号定位到 Android `BufferReleaseChannel::ProducerEndpoint::writeReleaseFence`。随后开机动画线程在读取释放消息时触发同一通道的 `recvmsg`。已支持 Unix 包套接字无附带描述符的分段接收。回归验证跨 iovec 拷贝、截断标志、peek 保留消息和拷贝故障后的重试。宿主回归和 iOS 签名构建通过。带发送端修复的长时间 Mac 验证已完成 WaitForDisplay 并启动 PackageManagerService。iPhone 11 第 7951 号进程正在验证带发送端和画面采集的构建。

- iPhone 11 第 7951 号进程在 103,451 ms 呈现 640×960 首帧，并成功导出带原版 Android 开机字标的 PNG。原始采集的 SHA-256 为 `79a4b9bc883d62a39303ff689ea21f13eb0ee258b2b6b18d10fa103211947576`。字标上下颠倒，图像检查确认 UIKit 重复调整了客体合成的行序。签名构建已改为直接使用读回行序并单独记录首张有内容画面的呈现时间。修复后的已签名构建已作为真机第 7956 号进程启动。

- iPhone 11 第 7956 号进程已呈现正向的原版 Android 开机字标。首帧时间为 113,027 ms，首张有内容的帧为 113,072 ms，尺寸均为 640×960。真机 PNG 的 SHA-256 为 `ce492b1ee068dbc3e20bdad0025ac046ad4643981dbd71907e5b7a94df1462e1`。Mac 使用新接收路径后继续运行开机动画，终端报告中该线程已越过 `recvmsg` 原停点。
- Mac 上包管理进入创建组件时探测 `FS_IOC_ENABLE_VERITY`，此前不支持的 ioctl 会挂起调用方。虚拟文件系统现在明确返回 Linux 的 `ENOTTY`，并一致处理测量、读取元数据以及不允许 ioctl 的 `O_PATH` 描述符。客体未启用文件 verity。启动观察器现在也记录包管理创建和扫描各步骤。宿主 ABI 回归和 iOS 签名构建通过。[Linux fs-verity ioctl 语义](https://docs.kernel.org/filesystems/fsverity.html)

- 新的 `--framework-diagnostic --framework-apps` 打包选项从已通过版本与 boot 输入哈希校验的 ARM64 SDK 参考环境收集桌面所需的系统应用。样本包含 Launcher3、SystemUI、Settings、PackageInstaller、DocumentsUI、存储提供方和关联的三个 APEX。构建 CE2A.260420.051.A3 的完整命令已成功，13 个 APK 均保留 DEX。镜像约 956 MiB，已在 Mac 开始启动验证。APK 安装和桌面启动仍待确认。
- 处理 fs-verity 探测后的 Mac 启动日志确认包管理完成 `createSubComponents`，继续执行 `get system config` 和 `addSharedUsers`。当前不含额外系统 APK 的诊断镜像尚未证明包管理可用。

- 带 13 个系统 APK 的镜像已成功打成约 971 MiB 的 iPhone 开发签名包，确认 Launcher3 和 PackageInstaller 在 App 包内。第 7962 号进程在 iPhone 11 初始化本地渲染器并进入真实 Android 17 启动。
- Mac 终端堆栈显示，原版 PackageManager 因 `/system/etc/selinux/plat_mac_permissions.xml` 返回 ENOENT 而终止。采集工具补齐了参考镜像的五个分区策略，要求平台策略存在。五份原版 XML 均已取回并通过结构解析，平台文件含六条签名策略，vendor 文件含一条。带这些文件的镜像开始下一轮启动验证。[Android 应用标签策略加载](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/services/core/java/com/android/server/pm/SELinuxMMAC.java)

- UIKit 的 Android 显示视图已将单指按下、移动、释放和取消送入客体 `/dev/input/event0`。坐标以实际画面区域为准。客体设备实现了单指 evdev 能力、轴范围、事件读取与 epoll 唤醒，支持 EventHub 使用的 `EPOLLWAKEUP` 标志。宿主回归验证设备枚举、触点状态、按下与释放、读出故障后的重试。iPhoneOS 签名构建通过。Android 原版 InputReader 接收、应用响应与输入延迟尚待真机验收。
- 带原版应用标签策略的 Mac 启动已越过原停点，完成包管理的 `loadFallbacks`、读取用户设置和 `setFirstBoot`。后续初始化出现新的 system_server 异常，正在收集完整堆栈。

- 触控队列按客体输入设备打开状态启用。开机画面上的触碰会在 Android 尚无读端时丢弃，设备关闭时清理排队事件。宿主 ABI 馈送测试已验证阻塞的 guest epoll 在宿主提交触点后恢复运行。Mac 启动探针也接入同一输入设备，以便验证原版 EventHub 的枚举。iPhoneOS 签名构建通过。

- Mac 上原版包管理已扫描系统 APK。核心 `framework-res.apk` 的 `resources.arsc` 请求只读共享映射，长度 `0x128b544`、偏移 `0xef6000`，客体返回 `ENOMEM`。核心包未登记，随后扫描系统应用时触发空引用。对只读根文件中的完整页面，新实现直接引用已经映射的原始图像字节，复用数据并保留客体写保护。对文件尾部的非完整页面继续零填充。宿主回归覆盖关闭 FD 后的可读性、拆分与恢复保护、禁止写入和尾页零填充。开发签名构建已通过，Mac 复测正在运行。

- 根文件直接映射后，Mac 原版包管理已越过核心 `framework-res.apk` 的映射错误，随后在 DocumentsUI 文件尾附近和 APEX 内的 APK 继续遇到 `ENOMEM`。只读共享映射现复用 APEX 及系统 APK 的完整页面，仅复制并零填充末尾不足一页的数据；文件末尾以外的整页仍会触发客体访存异常。回归覆盖同一映射中的完整页、尾页和越界页，开发签名构建通过。Mac 已启动带尾页修复的完整复测。
- 带首批 APK 映射和触控桥的 iPhone 11 第 7999 号进程在 100,392 ms 呈现正向 Android 开机字标，PNG 为 640×960，与第 7956 号进程的内容 SHA-256 一致。该次真机仍在运行，桌面和触控尚待验收。

- Mac 上第一版只读共享映射验证确认核心 `framework-res.apk` 已解析并进入证书校验。系统 APK 扫描随后仍有 ZIP 中央目录映射返回 `ENOMEM`，Scudo 最终报告分配失败。只读私有映射现在也复用不可变镜像页面；如果来宾后来用 `mprotect` 请求写权限，只复制受影响的私有范围并计入原有预算。宿主回归验证私有写入不会改变源文件和共享映射，未改保护的相邻页仍共用原始数据。开发签名构建通过，完整扫描复测已启动。

- Mac 的 APK 尾页修复版完成系统包扫描并写出包设置。原版 PackageManager 随后报告缺少必需的 `android.ext.shared`。该轮已越过此前的扫描期间内存错误，仍待补齐系统共享库。
- 匿名私有地址保留区在大范围解除 `PROT_NONE` 时延迟分配实际页面。宿主回归验证未写入页保持零填充、页面在写入后才计入预算、解除映射后归还预算，原区域两侧的保护页继续生效。iPhoneOS 开发签名构建通过。

- 在 Android 17 ARM64 SDK 参考镜像上，`pm path android.ext.shared` 返回工厂 APK `/system/app/ExtShared/ExtShared.apk`，包信息确认其声明同名动态共享库。诊断镜像脚本已将其收录。完整重建采集到 14 个 APK 及 3 个相关 APEX，ExtShared 保留清单和 DEX。完整镜像探针新增独立 Bionic 进程读回 `sys.boot_completed` 的检查，启动验收仍在运行。
- 第 7999 号 iPhone 11 进程在约 14 分 42 秒进入 PackageManager 首次启动设置及系统包扫描。该轮使用此前的镜像，已观察到热状态为 serious，桌面启动仍待验收。
- 第 7999 号真机测试达到 1,200 秒诊断时限，执行约 228.1 亿条客体指令，最后仍处于系统包扫描阶段。该轮报告已导出。包含 ExtShared、APK 尾页修复、只读私有映射和匿名内存修复的新包通过开发签名与嵌入文件校验，已覆盖安装。第 8152 号进程在 155 ms 进入 Android 17 init，本轮开始时热状态为 serious。

- 带 ExtShared 的 Mac 启动记录显示系统包扫描用时 141.722 秒，PackageManager 写出设置并完成创建。持久化的 `/data/system/packages.list` 有 41 行，按包名确认包含 `android.ext.shared`、Launcher3、SystemUI 和 PackageInstaller。此时尚未确认桌面显示或客体完成启动。
- 新增可选的 `--smoke-apk` 镜像输入。客体 init 在 `sys.boot_completed=1` 后以 shell 身份调用原版 Android `cmd` 安装并冷启动项目自有 APK，报告分别记录安装、启动命令和 Activity 实际创建。参考 SDK 上命令返回 `Success` 和 `LaunchState: COLD`，应用读回此前保存的计数 1。Mac Nestvane 镜像复测已开始，包含测试 APK 的 iPhoneOS 开发签名包通过校验。
- 4 倍硬件超时的 Mac 镜像完成包管理后，Android Watchdog 报告主线程阻塞 246 秒。4 秒宿主 CPU 采样中，Binder 死亡轮询占该执行线程 671 个样本中的 104 个，热点为每次检查扫描所有客体线程。活着的进程首线程现在用索引查找；首线程退出后保留扫描后备路径。回归覆盖 Binder 对端首线程退出时无通知、最后一个线程退出后收到通知。宿主验证和包含验收 APK 的 iPhoneOS 签名构建通过。新增完整的 SystemServerTiming 日志，并保留离线镜像 12 倍超时的对照运行。
- Watchdog 的主线程堆栈定位到原版 `BatteryService.registerHealthCallback` 阻塞在 `android.hardware.health.IHealth/default`。服务管理器反复查找，镜像中的 VINTF 和 init 文件声明了该服务，但之前缺少可执行文件。采集脚本现收录 SDK 原版 Health HAL、ELF 依赖和参考策略，确认 init 转到 `u:r:hal_health_default:s0`，且参考策略允许注册该实例。重新生成的 Mac 镜像已启动，带 12 倍诊断超时和自有 APK 的 iPhoneOS 构建通过签名及文件校验。
- iPhone 11 第 8152 号进程捕获到 640×960 Android 画面，耗时 114,038 ms，Zygote 预加载 18,596 个类，进入 `system_server` 耗时 756,725 ms。这次运行仍使用早期镜像，未包含 Health HAL。
- Mac 的短程探针确认原版 Health HAL 在 `hal_health_default` 域启动，服务管理器找到对应 VINTF 声明。HAL 的下一条日志为 `wakealarm_init: timerfd_create failed`，原版 ELF 对该调用传入 `CLOCK_BOOTTIME_ALARM`。现由客体的计时器和调度器兑现该时钟，创建及设定时保留 `CAP_WAKE_ALARM` 检查。宿主回归验证权限、计时器到期和 epoll 可读性，iPhoneOS 签名构建通过。客体计时仅在宿主 App 正常运行时服务。[Linux timerfd 实现](https://android.googlesource.com/kernel/common/+/2d74e4b8afbf07c148e08b4524ccfc9987d34373/fs/timerfd.c)
- iPhone 11 第 8152 号进程到达 1,200 秒诊断时限。真机 Watchdog 堆栈定位到 DisplayManagerService 等待 SyncRoot，发生在包管理前，和之前 Mac 的 Health 等待位置不同。已备份报告。包含原版 Health HAL 和自有 APK 的第 8205 号进程已启动，运行上限设为 1,800 秒，已确认 SurfaceFlinger 连接显示。该轮启动早于闹钟 ABI 签名包完成。
- 在 Android Watchdog 写出终止原因时，探针现在记录发出日志的 Android 进程线程状态和当前的图形工作线程，避免正常停机报告缺少已经退出的 `system_server`。宿主回归从客体 liblog 写入路径验证被观察进程进入快照，iPhoneOS 签名构建通过。
- Mac 短程探针启用闹钟 ABI 后，原版 Health HAL 的日志由计时器创建错误推进到等待 `bpf.progs_loaded`。离线镜像跳过原版的 BPF 启动动作。另建了恢复 `trigger load-bpf-programs` 的对照镜像，用来确认真实加载路径和下一处失败，不改写该属性。
- 恢复原版 `load-bpf-programs` 的 Mac 对照镜像再次确认 `netbpfload` 在 `/proc/sys/net/core/bpf_jit_enable` 返回 ENOENT。继续实现来宾 BPF ABI，首批支持数组映射创建、读写、迭代和 FD 生命周期，并保留权限与内存上限。同一个 ARM64 Bionic 夹具在 Android 17 SDK 原版内核与 Nestvane 的客体 ABI 上均正常退出，验证零初始化、写回、迭代、CLOEXEC 和标准错误码。宿主回归额外验证复制 FD、写入失败后的值保留及只读权限。iPhoneOS 构建和签名校验通过，实机当前启动仍使用之前的构建。来宾程序加载、附着和 `bpf.progs_loaded` 尚未通过。[Android BPF 数组实现](https://android.googlesource.com/kernel/common/+/e095493091e850d5292ad01d8fbf5cde1d89ac53/kernel/bpf/arraymap.c)
- 用同版本 SDK 的 `bpftool` 盘点参考内核，共有 98 个 BPF map 和 87 个程序，其中数组 32 个、普通哈希 33 个、ring buffer 20 个。现在支持普通哈希 map 的创建、插入、读写、删除、迭代和容量检查。相同 ARM64 Bionic 夹具在原版内核和 Nestvane 均退出 0，原生回归和 iPhoneOS 开发签名通过。原版加载器初始化仍未完成。
- BPF map 现可固定到客体 bpffs。在关闭原 FD 后可按路径重新打开，支持重命名、删除路径以及重新打开时的只读权限。原版内核与 Nestvane 运行同一 ARM64 Bionic 夹具，均观察到 mode 0600、普通 open 返回 EIO、重新打开获得 CLOEXEC，删除名称后已有只读 FD 仍可读出原值。宿主回归和 iPhoneOS 签名构建通过。
- 客体支持装载受限的 `BPF_PROG_TYPE_SOCKET_FILTER` 并通过 `SO_ATTACH_BPF` 应用于客体 uevent。执行器解释字节码，使用有界栈、包访问校验和指令上限。对照测试使用 SDK 参考镜像中由公开 AOSP Health 过滤器编译出的字节码。其 400 字节程序在参考 Android 内核和 Nestvane 解释器都保留 `power_supply` 与未分类数据包，并丢弃 `block` 包。宿主回归验证非法指令和跳转、装载、附着及 FD 关闭后的截断或丢弃，iPhoneOS 开发签名通过。原版全量加载仍待验证。[AOSP Health 过滤器源码](https://android.googlesource.com/platform/hardware/interfaces/+/eb39ea1855023c268b12c226699a0351e101c8b2/health/utils/libhealthloop/filterPowerSupplyEvents.c)
- iPhone 11 的第 8205 号进程越过了前一轮的显示等待，约 13 分 20 秒进入 PackageManager，14 分 58 秒已记录系统框架扫描进展。该轮测试继续运行，尚未确认客体完整启动。
- bpffs 固定操作扩展到 socket filter 程序。参考 Android 内核将原版 Health 程序固定后，以只读请求重新打开，移除文件名后附着，过滤结果不变。Nestvane 宿主回归执行同样的程序 FD 生命周期，并验证客体 uevent 的丢弃和截断。包含该改动的 iPhoneOS 开发签名通过。
- 采集器补齐原版加载器从分区读取的 BPF ELF，保留子目录与文件名并校验 ELF 架构。相同 SDK 的完整采集获得 1,217 个 Android 文件，其中 11 个 `/system/etc/bpf` 对象和一个 vendor Health 对象是此前缺少的输入。采集的 vendor 文件与差分测试所用字节逐一一致。
- 可用 `tools/prepare-bpf-uevent-probe.sh` 将独立 Bionic 夹具装入诊断根目录。Mac 启动时，原版 init 启动夹具，后者从 SDK Health 字节码装载程序、固定到 bpffs、关闭创建 FD、重新打开并附着到一个 uevent 套接字。创建探针 loop 设备时，普通监听器收到 `SUBSYSTEM=block`，带过滤器的监听器没有收到；init 记录夹具退出 0。包含同一夹具的 iPhoneOS 签名包已通过文件校验。
- iPhone 11 第 8205 号进程报告首次系统包扫描耗时 534.122 秒，时间戳为宿主启动后 1,352,481 ms。
- 第 8205 号 iPhone 11 进程到达 1,800 秒时限。最终报告确认原版 PackageManager 扫描发现 74 个系统应用条目，系统应用扫描用时 531,864 ms，并重复记录原版 HealthLoop 等待 `bpf.progs_loaded`。仍未通过完整启动属性。已备份上一轮报告并安装原版 BPF 过滤器验收构建。第 8210 号进程在 141 ms 进入原版 Android 17 init，短程真机验收正在运行。
- 客体支持 `BPF_MAP_TYPE_PERCPU_HASH` 和 `BPF_MAP_TYPE_PERCPU_ARRAY`。来宾 CPU 拓扑和调度器均暴露一个可能 CPU，值按内核 ABI 以 8 字节对齐，并保留调用者写入的填充字节。对同一个 ARM64 Bionic 程序，Android 17 参考内核报告 4 个 CPU，Nestvane 报告 1 个；两者分别完成两种 map 的查找、写入和步幅验证。
- iPhone 11 第 8210 号进程的最终报告确认，独立 Bionic 夹具在原版 Android init 下装载 SDK Health 过滤器、重新打开并附着，成功丢弃实际的 block uevent；init 记录退出 0。已归档该真机报告，完整系统启动仍待验收。
- 客体支持按 ID 枚举和重新打开 BPF map 与程序，只有仍有 FD、固定名称或附着引用的对象会出现。相同 ARM64 Bionic 夹具在 Android 17 参考内核和 Nestvane 中通过，覆盖 CLOEXEC 及新 FD 的只读限制。包含 ID 和 per-CPU map 探针的 iPhoneOS 签名包已验证。
- 两份隔离 Mac 诊断仅用临时 JIT sysctl 值观测原版加载器下一步。首轮定位到 `BPF_PROG_GET_NEXT_ID`，补齐 ID 后原版开始处理 Tethering APEX 的 `offload.o`，下一项请求为 `BPF_BTF_LOAD`。原版的完成判定保持有效。[Android BPF 内核接口](https://android.googlesource.com/kernel/common/+/16c71c51132847fb260e6b7e741bf6d241a8b9df/kernel/bpf/syscall.c)
- 客体可以加载有界的 BTF 标准类型记录，并让已支持的 BPF map 引用类型信息、检查 key/value 宽度。相同 Bionic 夹具在 SDK 原版内核和 Nestvane 中验证了有效类型、截断类型、尺寸错误及关闭 BTF FD 后保留 map 引用。iPhoneOS 签名构建通过。独立 Mac 对照中，原版加载器加载 `offload.o` 的 BTF 后开始创建 `tether_error_map`，随后请求 `BPF_OBJ_GET_INFO_BY_FD`。[Android BTF UAPI](https://android.googlesource.com/kernel/common/+/9c39d5ab450f7181775957000f4aff33bfef9f7b/include/uapi/linux/bpf.h)
- iPhone 11 第 8240 号进程的三个独立探针均由原版 init 回收，退出状态为 0。按 ID 重新打开对象和只读限制通过；per-CPU 哈希、数组各报告一个 CPU、8 字节步幅并保留填充值 `0x7a`；原版 Health 过滤器客体 uevent 复验通过。完整启动仍待验收。
- `BPF_OBJ_GET_INFO_BY_FD` 可回读客体 map 的实际尺寸、ID、名称和 BTF 引用，也可读回 BTF 对象的原始数据。相同 Bionic 夹具在 Android 17 参考内核和 Nestvane 都通过完整及短结构回读，宿主回归和 iPhoneOS 签名通过。隔离 Mac 对照中，原版加载器已创建并枚举 `offload.o` 的八个映射，下一项失败为 `tether_dev_map`（`DEVMAP_HASH`）。[Android BPF 对象信息 ABI](https://android.googlesource.com/kernel/common/+/16c71c51132847fb260e6b7e741bf6d241a8b9df/kernel/bpf/syscall.c)
- 新增 BPF 网络设备哈希 map，关联当前客体的 loopback 接口并检查虚拟接口编号。相同 Bionic 夹具在 Android 17 参考内核与 Nestvane 均验证了 loopback 写入、无效接口返回 EINVAL、无效更新保留原值及实际更新标志行为。宿主回归与 iPhoneOS 签名通过。隔离 Mac 诊断中，原版加载器已创建并固定 `offload.o` 的全部九个 map。下一处失败在装载首个 `BPF_PROG_TYPE_SCHED_CLS`（`schedcls_tether_downstream6_ether`）。[Android BPF 设备 map](https://android.googlesource.com/kernel/common/+/35aa7a801190f6d004ce42904cbd1a003ff9c97e/kernel/bpf/devmap.c)
- 第 8250 号 iPhone 11 进程完成有界验收，原版 init 回收 BTF/map 回读夹具并记录退出 0。结果确认在真机可以读回原始 BTF、保留已关闭 BTF FD 的类型引用，并从 map 读回名称、尺寸和类型 ID。完整启动仍未通过。已备份第 8250 号报告。

- 客体新增一批 `SCHED_CLS` 指令与包 helper，支持映射 FD 重定位、直接包读写和 `BPF_PROG_TEST_RUN`。同一个 ARM64 Bionic 探针在 Android 17 参考内核和 Nestvane Mac 客体运行原版 `schedcls_tether_downstream6_ether$5_4` 字节码，均验证短帧旁路、非法 IPv6 版本计数、无规则旁路，以及命中规则时的返回动作、MAC 改写、跳数递减与流量记账。宿主 ABI 回归和 iPhoneOS 签名通过。在临时 sysctl 配置下，Mac 隔离诊断里的原版 `netbpfload` 已成功装载首个分类程序，随后因查询程序 ID 返回 EINVAL 停止。真机分类验证和客体网络挂载仍待完成。[Linux BPF 指令集](https://docs.kernel.org/bpf/standardization/instruction-set.html) · [Linux skb 测试运行](https://codebrowser.dev/linux/linux/net/bpf/test_run.c.html)

- `BPF_OBJ_GET_INFO_BY_FD` 现在返回客体程序的类型、ID、名称、标签、解释字节码长度和引用的映射 ID。相同原版 IPv6 程序在 Android 17 参考内核和 Nestvane 的标签均为 `9172b392a6f83a26`，均返回四个映射 ID。Nestvane 如实报告 2,560 字节解释代码和 0 字节原生 JIT 代码。Mac 隔离诊断确认原版加载器成功读取并打印该 ID、随后命中新版 `netbpfload` 的非零 JIT 长度检查。采用解释执行的客体需要显式适配 AOSP 加载器的 JIT 要求，并继续依照实际装载结果完成启动门槛。[AOSP 加载器检查](https://android.googlesource.com/platform/packages/modules/Connectivity/+/a0fa137d8524f41932bde5449a0db14254db4a7b/bpf/loader/NetBpfLoad.cpp) · [Linux 程序元数据 ABI](https://codebrowser.dev/linux/linux/include/uapi/linux/bpf.h.html#bpf_prog_info)

- iPhone 11 进程 8264 在原版 `init` 的 boot 动作执行原版 IPv6 分类探针，验证 MAC 改写、跳数递减和映射计数，服务在 0.240 秒后退出 0。这次 360 秒诊断仍未达到 `sys.boot_completed=1`。
- [AOSP 加载器补丁](../tools/patches/aosp-netbpfload-interpreter.patch)最初在 Connectivity 提交 `2519a78731526d2eb20ae8812acdcab6ef7a09b6` 验证可应用，随后移到 Android 17 正式发布标签。在客体 AOSP 树的 `packages/modules/Connectivity` 下应用后，`netbpfload` 先用真实的 `BPF_PROG_LOAD` 和 `BPF_PROG_TEST_RUN` 验证一段读取 IPv6 帧的字节码，然后允许具有非零解释字节码长度的程序。原有 ELF 加载、固定及完成属性检查继续执行。自检函数以同一 Bionic 二进制在 Android 17 参考内核和 Nestvane Mac 客体退出 0，完整 Tethering APEX 尚未使用该补丁重建。[固定 AOSP 源码](https://android.googlesource.com/platform/packages/modules/Connectivity/+/2519a78731526d2eb20ae8812acdcab6ef7a09b6/bpf/loader/NetBpfLoad.cpp)
- 程序元数据构建在 iPhone 11 进程 8268 复测成功。原版 `init` 记录探针退出 0，耗时 0.254 秒。标签为 `9172b392a6f83a26`，解释字节码 2,560 字节，JIT 代码 0 字节，引用四个映射。原版程序的 IPv6 帧改写和计数复测也通过；本次 135 秒诊断仍未观察到完整启动。
- iPhone 11 进程 8275 执行相同原版 IPv4 探针，确认 NAT、校验和、计数和时间戳均通过。原版 `init` 记录服务耗时 0.236 秒，正常退出。
- 上一版补丁所用的公开主分支源码只接受 Android 16 的启动脚本元数据。补丁已移到 `android-17.0.0_r1`。新的 [加载器构建工具](../tools/build-netbpfload.py) 用同标签源码与 API 37 参考设备中的共享库生成诊断 ARM64 ELF。Mac 隔离诊断把源码构建的二进制提供给客体中的加载器路径，保留原厂 APEX 文件，并完成解释器自检、`offload.o` 的八个分类程序和 `test.o` 的加载。下一处是 `clatd.o`，其 libbpf 装载请求带 BTF 函数及源码行表。完整 Tethering APEX 仍待在 AOSP 构建环境重建。[正式发布标签](https://android.googlesource.com/platform/manifest/+/refs/tags/android-17.0.0_r1)

- iPhone 11 进程 8290 通过源码版加载器的解释器自检，随后创建 `/sys/fs/bpf/netd_readonly` 时遇到客体目录上限。目录上限已从 512 提升至 2,048，保留 ext4 索引的独立计数。宿主回归先保存并恢复 540 个 `/data` 目录，再成功挂载 bpffs 并创建对应目录。
- libbpf 提交的 BTF 程序函数与行表现在随程序保留，可通过 `BPF_OBJ_GET_INFO_BY_FD` 读回。Android 17 参考内核和 Nestvane Mac 使用同一 Bionic 探针，均拒绝重复指令偏移，并在初始 BTF 描述符关闭后正常回读函数与行表。宿主 ABI 回归通过。更新运行时后，Mac 中 `clatd.o` 的首条加载失败从 EOPNOTSUPP 推进至 EINVAL。原版节还使用尚未实现的 `bpf_skb_change_proto` 和 `bpf_skb_adjust_room`。
- 保留原客体数据库的 iPhone 11 进程 8292 越过目录创建失败，源码版加载器自检通过，并加载及固定 `offload.o` 的全部八个分类程序，再完成 `test.o`。真机随后与 Mac 一样停在 `clatd.o`。Mac 诊断确认首个 CLAT 程序的一个 BTF 函数和 88 条行记录已通过校验，下一处是第 183 个指令槽调用的 helper 31（`bpf_skb_change_proto`）。本轮验证使用隔离客体文件视图提供源码构建的加载器，完整 Tethering APEX 重建与 Android 完整启动仍待验证。

- 实现线性客体包的 IPv4/IPv6 协议切换和包头空间调整。Android 17 参考内核和 Nestvane Mac 执行同一 ARM64 探针中的原版 `schedcls/ingress6_clat_ether$4_14`，确认无规则时保持原帧，命中时正确转换 IPv6 UDP 以及带 IPv6 分片头的 UDP 帧，且输出 IPv4 和 UDP 校验和、映射计数均正确。宿主 ABI 和 iPhoneOS 编译签名通过。源码版加载器在 Mac 完成 `clatd.o` 加载，随后停在 `dscpPolicy@funcs.o` 的首个程序。[Linux 协议转换和包头调整](https://codebrowser.dev/linux/linux/net/core/filter.c.html)
- iPhone 11 进程 8295 的原版 CLAT 探针耗时 0.228 秒，正常退出。源码版加载器随后成功加载 `clatd.o`，推进到 DSCP 策略。
- 客体支持带 BTF 函数表的 eBPF 回调与 `bpf_for_each_map_elem`。在 Android 17 参考内核和 Nestvane Mac 中执行同一 ARM64 探针里的原版 DSCP 分类器。两端均选中覆盖更窄目标端口范围的规则，保留 ECN 并验证重写后的 IP 校验和。宿主回归验证回调读取映射键、写入调用方栈和按返回值提前停止。源码版加载器在 Mac 完成 `dscpPolicy@funcs.o` 加载，随后创建 `netd@funcs.o` 的 `blocked_ports_map` 时遇到可映射数组标志。[Linux 映射回调 ABI](https://codebrowser.dev/linux/linux/include/uapi/linux/bpf.h.html)
- iPhone 11 进程 8304 的原版 DSCP 探针正常退出，耗时 0.221 秒。源码版加载器也完整加载 `dscpPolicy@funcs.o`，接下来是 `netd@funcs.o` 的 `blocked_ports_map`。
- 客体新增 `BPF_F_MMAPABLE` 数组。Android 17 参考内核与 Nestvane Mac 中，同一 Bionic 探针均验证八字节元素对齐、数组系统调用与共享映射互相可见、只读映射无法再加写权限、关闭 FD 后映射仍保持对象存活。宿主 ABI 回归通过。源码版加载器在 Mac 越过 `blocked_ports_map`，随后在 `sk_storage` 遇到尚未支持的套接字局部存储映射。[Linux BPF 数组映射](https://codebrowser.dev/linux/linux/kernel/bpf/arraymap.c.html)
- iPhone 11 进程 8305 同一轮复验了原版 CLAT、DSCP 和 BPF 共享数组。共享数组探针耗时 0.226 秒并退出 0，确认八字节元素对齐、共享写入、只读映射保护和关闭 FD 后的生命周期。源码版加载器成功加载 CLAT 和 DSCP，并越过 `blocked_ports_map`，该轮下一处失败为 `sk_storage`，未达到 `sys.boot_completed=1`。
- 客体新增带 BTF 的 BPF 套接字局部存储映射。映射值跟随客体套接字，重复 FD 可访问同一份值，同一 socketpair 的另一个套接字有独立存储。Android 17 参考内核和 Nestvane Mac 的独立 Bionic 探针均通过。Mac 源码版加载器提交的 `sk_storage` 使用有修饰的 BTF 整数键，现可解析到 32 位整型。宿主 ABI 回归覆盖带有 const 修饰的键、关闭原 FD 后从别名读取、套接字间隔离及删除。套接字局部存储尚未在 iPhone 11 复验。
- Mac 源码版加载器已越过 `sk_storage`。下一处失败为创建 `packet_trace_ringbuf`，客体返回 EOPNOTSUPP。该轮未验证完整启动。
- 客体支持内核生产型 BPF 环形缓冲区，含独立的读写消费页、只读生产页与双映射数据视图、预留/提交/丢弃/复制写入和 `epoll` 可读状态。Android 17 参考内核和 Nestvane Mac 上的同一个 ARM64 Bionic 探针验证了满缓冲区保留未读记录、回绕记录在两个位置可读、丢弃标志及消费后的通知状态。宿主 ABI 回归和 iPhoneOS 开发签名通过。Mac 源码版加载器越过 `packet_trace_ringbuf`，下一项失败为 `local_net_access_map`（最长前缀匹配映射）。iPhone 11 进程 8321 已启动包含环形缓冲区与套接字局部存储探针的构建。完整启动仍待验收。[Linux BPF 环形缓冲区 ABI](https://docs.kernel.org/bpf/ringbuf.html)
- iPhone 11 进程 8321 的环形缓冲区和套接字局部存储探针均由原版 init 执行，各耗时 0.224 秒并退出 0。源码版加载器也成功越过这两种映射，停在创建 `local_net_access_map`。该轮仍未达到 `sys.boot_completed=1`。
- 补齐 BPF 最长前缀匹配映射。用户空间查找和客体 BPF 查找共享前缀选择逻辑，更新、删除和枚举按各自的精确前缀语义执行。Android 17 参考内核和 Nestvane Mac 的同一 ARM64 探针均验证了非整字节的最具体前缀、超过容量时拒绝写入、替换时保留原键字节、子前缀优先枚举及删除后的回退。宿主 ABI 回归通过。Mac 源码版加载器越过 `local_net_access_map`，随后在 `local_net_note_op_cache_map` 创建时返回 EOPNOTSUPP。最长前缀匹配改动仍待 iPhone 11 复验。[Linux BPF 最长前缀匹配 ABI](https://codebrowser.dev/linux/linux/kernel/bpf/lpm_trie.c.html)
- 补齐 `local_net_note_op_cache_map` 所需的 BPF LRU 哈希映射。参考 Android 17 的实际 map 元数据显示类型 9、4 字节键和值、100 个条目。Nestvane 在单客体 CPU 上按最近使用顺序淘汰，有界容量内的客体 BPF 查找会刷新顺序，用户空间查询不会。相同 ARM64 探针在 Android 17 参考内核和 Nestvane Mac 验证了达到容量后的淘汰、客体程序查找及删除。宿主回归通过。Mac 源码版加载器创建了 `netd@funcs.o` 的所需映射，下一处失败为加载 `sockops_accecn_option_6_1`，完整启动仍待验收。[Linux BPF LRU 哈希 ABI](https://docs.kernel.org/bpf/map_hash.html)
- iPhone 11 进程 8324 的最长前缀匹配与 LRU 探针均由原版 init 执行，各耗时 0.224 秒并退出 0。源码版加载器也越过 `local_net_access_map` 和 `local_net_note_op_cache_map`，停在 `sockops_accecn_option_6_1`，返回 EOPNOTSUPP。该轮未达到 `sys.boot_completed=1`。真机验证使用开发签名和隔离的客体加载器文件视图，尚未重建完整 Tethering APEX。
- 补充对照中，同一 ARM64 探针通过 Linux `BPF_PROG_TEST_RUN` 运行分类程序，Android 17 参考内核和 Nestvane Mac 都从客体 BPF helper 返回最具体的 /13 前缀、回退到 /8，并在无规则时返回空值。下一处失败的原版 AccECN 程序需要 TCP 套接字回调上下文、套接字局部存储以及 TCP 选项的预留和写入。[Android 17 AccECN 源码](https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/tags/android-17.0.0_r1/bpf/progs/tcpAccECN.h)
- 客体新增受限的 `BPF_PROG_TYPE_SOCK_OPS` 执行上下文，支持读取当前套接字的局部存储、设置回调标志以及预留和写入有界 TCP 选项。Mac 的原版 640 字节 AccECN 探针覆盖全局开关、套接字隔离、SYN 旁路、11 字节选项预留以及三组 24 位计数的网络字节序编码。宿主 ABI 回归与 iPhoneOS 开发签名构建通过。源码版加载器越过 `sockops_accecn_option_6_1`，随后在 `schedcls_egress_accecn_eth_6_1` 返回 EINVAL。TCP 传输路径的 cgroup 附着与事件派发、iPhone 11 复验和完整启动仍待验证。[Linux sockops ABI](https://codebrowser.dev/linux/linux/include/uapi/linux/bpf.h.html)
- iPhone 11 进程 8325 的隔离原版 AccECN 探针验证了回调标志、SYN 旁路、选项长度和计数编码。Android 源码版加载器也越过 `sockops_accecn_option_6_1`，停在以太网出站分类器。该轮尚未达到 `sys.boot_completed=1`。
- 客体新增旧版 BPF 绝对与间接包读取指令，按网络字节序读包并在越界时终止该程序。分类器可用实际关联的客体套接字访问同一份局部存储。Mac 直接运行参考 Android 17 ELF 中的两个原版 AccECN 出站分类器，验证 Ethernet 和 raw IP 下的 IPv4/IPv6 SYN 与 ACK、ACE 标志、ECN 和独立计算的 TCP/IPv4 校验和。源码版加载器越过两个出站分类器，下一项为 cgroup `ingress_stats_6_1_26q2`，返回 EOPNOTSUPP。宿主 ABI 回归通过，出站分类器仍待真机复验。[Linux BPF 旧版包访问](https://docs.kernel.org/bpf/standardization/instruction-set.html#legacy-bpf-packet-access-instructions)
- iPhone 11 进程 8328 的原版以太网和 raw IP AccECN 出站分类器探针通过，覆盖 IPv4/IPv6 的 TCP 标志、ECN 和校验和。真机源码版加载器推进到 `ingress_stats_6_1_26q2`。
- 客体可加载 cgroup 包程序，并支持有界 BPF 本地函数、L3 数据视图、相对包头读取、运行期间的映射更新与旧值生命周期，以及由客体传输层提供的套接字查询接口。相同 ARM64 Bionic 探针在 Android 17 参考内核和 Nestvane Mac 中验证了带 BTF 的本地调用、IPv4 与其他值的分支以及 L3 读取。宿主回归额外验证内层函数更新映射后仍可读取调用期间保留的旧值。源码版加载器已越过原版 cgroup 统计程序，下一项失败为 `skfilter_egress_xtbpf_4_9`。该轮未完成启动，cgroup 附着和实际 TCP/UDP 传输派发仍待实现。[Linux BPF test-run 包上下文](https://codebrowser.dev/linux/linux/net/bpf/test_run.c.html)
- iPhone 11 进程 8329 的 cgroup 包探针在原版 init 下耗时 0.225 秒并退出 0。源码版加载器越过 cgroup 入站和出站统计程序，停在 `skfilter_egress_xtbpf_4_9`。该轮未达到 `sys.boot_completed=1`。
- 客体 socket filter 支持映射查找与更新、套接字身份和原子计数，流量控制动作复用分类器的包上下文。相同 ARM64 Bionic 夹具用原版 netd 入站、出站与流量控制动作的字节码在 Android 17 参考系统和 Nestvane Mac 上执行，均验证每接口的收发包数、字节数与返回值。宿主 ABI 回归和 iPhoneOS 构建通过。源码版加载器越过这批过滤器，下一项失败为 `cgroupsock_inet_create_5_10_t`，需要在 INET 套接字创建路径执行相应 BPF，上游程序用该回调创建套接字局部存储。当前批次的原版过滤器真机测量尚待完成。
- iPhone 11 进程 8330 的原版 netd 流量统计探针以退出状态 0 完成，验证了接口收发包数和字节数。源码版加载器停止在 INET 创建回调，未达到 `sys.boot_completed=1`。
- Nestvane 支持创建 IPv4/IPv6 的 TCP 与 UDP 描述符，并把 INET 创建回调接到真实的客体 `socket()` 路径。cgroup 附着支持继承和解除，局部存储由客体原有内存预算管理。Mac 的 ARM64 探针在临时客体 cgroup 附着原版 Android 17 INET 创建程序，通过校验 cookie、UID、GID、TCP/UDP 标志、权限拒绝和解除后的行为。参考 Android 拒绝在原有程序下附着额外探针，使用两个未连接的套接字只读核对了已附着原版程序产生的相同元数据。宿主回归和 iPhoneOS 构建通过。Mac 源码版加载器越过 INET 创建，下一处为 `cgroupsockrelease_inet_release_5_10_t`。本批次尚未实现 INET 传输，iPhone 11 进程 8334 正在复验。[Linux cgroup BPF 继承](https://codebrowser.dev/linux/linux/kernel/bpf/cgroup.c.html)
- iPhone 11 进程 8334 的原版 INET 创建探针正常退出，耗时 0.222 秒。真机验证 TCP/UDP 元数据、局部存储、权限拒绝和解除附着，并由源码版加载器推进到 `cgroupsockrelease_inet_release_5_10_t`。
- 客体支持 INET 释放回调，使用创建时的 cgroup 层级，并按照关闭时仍生效的附着执行。Mac 上的原版回调在关闭最后一个重复描述符时删除 cookie 标签，关闭第一个描述符时保留标签，解除附着后保留原映射条目。宿主回归和 iPhoneOS 签名构建通过。源码版加载器越过释放回调，下一处为 `bind4_inet4_bind_5_15_t`。真机进程 8337 已开始复验，完整启动仍待验证。
- iPhone 11 进程 8337 的原版 INET 释放探针正常退出，耗时 0.224 秒，确认关闭最后一个 FD 时才删除标签。源码版加载器推进到 `bind4_inet4_bind_5_15_t`，该轮仍未完成启动。
- 客体支持 IPv4/IPv6 回环与通配地址绑定、端口分配、占用检测和 `getsockname` 回读。INET 地址程序调用接入 `bind()`。Mac 上的原版 Android 17 IPv4/IPv6 回调验证了端口位图拒绝、关闭后的端口重新绑定、IPv6 临时端口，以及原版回调授予的低端口例外。宿主回归和 iPhoneOS 编译通过。源码版加载器越过 INET 地址程序，下一处为 `getsockopt_prog_6_1_26q2`。真机绑定验证与 TCP/UDP 传输仍待验收。[Linux BPF 套接字地址 ABI](https://codebrowser.dev/linux/linux/include/uapi/linux/bpf.h.html)
- iPhone 11 进程 8342 的原版 IPv4/IPv6 绑定探针正常退出，耗时 0.226 秒。实际验证端口策略、关闭后重新绑定、IPv6 临时端口及低端口授权。参考 Android 上的系统原版回调也允许在移除探针的 `CAP_NET_BIND_SERVICE` 后绑定 80，拒绝绑定 102。真机源码版加载器越过 INET 地址程序，停在 `getsockopt_prog_6_1_26q2`。本轮尚未达到 `sys.boot_completed=1`。
- 客体支持 cgroup 套接字选项回调。原版 Android 17 选项程序经客体的 `getsockopt` 和 `setsockopt` 进入 BPF，在 Mac 验证了授权 PID、L4S 写入和读回、短缓冲区错误、读取并清空丢包原因以及常规套接字选项回退。宿主回归和 iPhoneOS 编译通过。Mac 的源码版加载器已成功装载并固定完整的 `netd@funcs.o`，随后进入 UprobeStats 加载器，后者跳过未实现的可选对象后，在交接到缺失的 `/system/bin/bpfloader` 时失败。iPhone 11 进程 8346 已启动复验。[Linux cgroup 套接字选项](https://codebrowser.dev/linux/linux/kernel/bpf/cgroup.c.html)
- iPhone 11 进程 8346 确认源码版加载器成功装载并固定完整的 `netd@funcs.o`，其中包含原版两条套接字选项程序。原版 UprobeStats 加载器随后因缺少平台 `bpfloader` 退出。本次真机日志覆盖程序加载，独立套接字选项语义的真机复验仍待完成。
- 客体文件收集器现提取 Android 17 原版平台 `bpfloader` 及其递归 ELF 依赖，并从参考系统收集两次 BPF 加载器交接使用的 SELinux 进程标签。重新从参考系统收集了 1222 个文件，原版二进制与四个新增库的哈希均与独立诊断镜像一致。Mac 冷启动确认源码版 `netbpfload` 依次进入原版 UprobeStats 和平台加载器，后者开始读取 `timeInState.bpf`。本轮限时诊断尚未达到 `sys.boot_completed=1`。
- iPhone 11 进程 8350 完成三段加载器交接，原版平台 `bpfloader` 以 `u:r:bpfloader:s0` 运行，开始处理 `timeInState.bpf`，随后首次发出 `BPF_TOKEN_CREATE`。该轮在此调用暂停。客体已补齐初始用户命名空间的返回语义。Android 17 参考内核中，普通 bpffs 根目录 FD 返回 EOPNOTSUPP，子目录及非 bpffs FD 返回 EINVAL，关闭或仅路径 FD 返回 EBADF。宿主 ABI 回归确认相同的根目录错误、无效标志和 FD，并验证卸载后已打开的根目录 FD 仍保留身份。该调用未在客体中创建令牌或授予权限。[Linux BPF token 实现](https://codebrowser.dev/linux/linux/kernel/bpf/token.c.html)
- 补齐 Android 平台自检使用的 socket filter 环形缓冲区 helper，复用已有的预留、提交、丢弃和查询语义。参考 Android 17 内核执行原版 `skfilter_ringbuf_test` 指令并读回值 12345，宿主 ABI 回归覆盖 socket filter 产生事件和原有分类器的回绕、丢弃与通知行为。Mac 使用源码版 Tethering 加载器和原版平台加载器冷启动，越过了强制环形缓冲区自检。原版 init 记录 `bpfloader` 退出 0，并读到 `bpf.progs_loaded=1`。若干可选跟踪对象仍因缺少内核 BTF 或 tracepoint 支持而跳过，尚未达到 `sys.boot_completed=1`。[Linux BPF 基础 helper](https://codebrowser.dev/linux/linux/kernel/bpf/helpers.c.html)
- iPhone 11 进程 8352 复验原版平台环形缓冲区自检。原版 `bpfloader` 退出 0，原版 init 读到 `bpf.progs_loaded=1`，随后因镜像缺少 `/system/bin/netd` 而停止。
- 从 Android 17 参考系统重新采集 1231 个文件，含原版 `netd`、递归 ELF 依赖和 `u:r:netd:s0` 转换。与独立诊断镜像对照，原版程序和八个新增库字节一致。Mac 原版 init 执行了 `netd1shot`，原版 netd 进入其 SELinux 域并加载 Tethering APEX 中的 `libnetd_updatable.so`。下一处是 `prog_netd_egress_stats` 的 cgroup 出站程序附着，目前返回 EOPNOTSUPP。带 netd 的真机构建已启动复验，完整启动仍待验证。
- iPhone 11 进程 8353 复现 Mac 的启动位置。原版 init 启动 `netd1shot`，进程进入 `u:r:netd:s0`，随后原版 `libnetd_updatable` 附着 `prog_netd_egress_stats` 时返回 EOPNOTSUPP。另以同一个 ARM64 Bionic 探针在参考系统和 Mac 执行了原版环形缓冲区自检程序，两端均读回实际提交的 12345。真机尚未读到 `sys.boot_completed=1`。
- 补齐 cgroup 入站与出站包程序附着，并接入客体 IPv4/IPv6 回环 UDP。Mac 上的 ARM64 Bionic 探针验证出站拒绝、入站丢弃、读写、连接和报文截短，宿主 ABI 回归验证两个地址族、附着期间切换、报文来源和无连接语义。Android 17 参考系统通过普通 UDP 回环对照，已安装的参考策略拒绝在临时 cgroup 额外附着探针。Mac 上原版 `netd1shot` 越过出站和入站统计程序附着，下一项是 `prog_netd_connect4_inet4_connect`。iPhone 11 已进入本批构建复验，TCP 和外网数据通路仍待实现。[Linux cgroup 包过滤语义](https://codebrowser.dev/linux/linux/kernel/bpf/cgroup.c.html)
- iPhone 11 进程 8359 复验原版 netd 越过入站与出站包程序附着，随后停在 `prog_netd_connect4_inet4_connect`。独立 UDP 探针进入程序，日志未保留其退出结果。
- 接入 IPv4/IPv6 cgroup 连接与 UDP 发送、接收地址回调。宿主 ABI 验证两种地址族下拒绝连接、改写连接和发送目标端口、改写接收报告的来源端口。Mac 冷启动中的原版 netd 已越过连接和 UDP 地址程序，推进至 `prog_netd_sockops_accecn_option` 附着。真实 TCP 传输与完整启动仍待完成。[Linux UDP 发送与接收地址回调](https://codebrowser.dev/linux/linux/net/ipv4/udp.c.html)
- iPhone 11 进程 8360 中，原版 init 启动的 ARM64 UDP 探针正常退出，覆盖 IPv4/IPv6 入站与出站策略、连接拒绝和端口改写。原版 netd 越过连接与 UDP 地址程序附着，停在 `prog_netd_sockops_accecn_option`。
- 客体支持 IPv4/IPv6 TCP 回环监听、连接、队列和双向字节流，连接与数据段经过 cgroup 包过滤，并可执行 sockops 的连接、建立和 TCP 头选项回调。宿主回归验证主动和被动端都生成选项，出站程序在两向实际数据段中读到选项，且半关闭后对端读到 EOF。参考 Android 17 验证普通双栈 TCP/UDP 回环。Mac 原版 netd 越过 AccECN 附着，下一处挂起调用是 `BPF_PROG_QUERY`。真机复验进行中，TCP 仍是客体内回环。
- iPhone 11 进程 8380 的 TCP/UDP 双栈测试以状态 0 退出。原版 netd 越过 AccECN 附着，和 Mac 一样推进至 `BPF_PROG_QUERY`。
- 支持 cgroup BPF 的直接查询和继承查询，返回真实程序 ID、数量与附着标志。参考 Android 17 读到根 cgroup 出站程序 ID 24，子 cgroup 的继承结果一致。宿主回归额外覆盖容量不足、旧版查询结构、附着顺序和解除后结果。Mac 中原版 netd 的出站查询越过原停点，下一处调用为 `SIOCGIFFLAGS`（0x8913）。最新真机复验已启动，完整启动仍待验证。[Linux cgroup BPF 查询](https://codebrowser.dev/linux/linux/kernel/bpf/cgroup.c.html)
- iPhone 11 进程 8382 已越过原版 netd 的 BPF 附着查询，停在真实的 `SIOCGIFFLAGS` 调用。双栈 TCP/UDP 探针再次退出 0。
- 为客体回环补齐接口标志、开关状态、索引与 MTU 查询。参考 Android 17 和 Mac 的同一 ARM64 探针均读到标志 0x49、索引 1、MTU 65536，未知接口返回 ENODEV。宿主 ABI 回归验证关闭接口后的路由失败与重新打开。Mac 原版 netd 越过接口标志查询，接下来在一次 `connect` 调用收到 ECONNREFUSED。最新真机构建已安装复验，`sys.boot_completed=1` 尚未通过。[Linux 回环接口实现](https://codebrowser.dev/linux/linux/drivers/net/loopback.c.html)
- iPhone 11 进程 8385 中，读取回环接口标志、索引和 MTU 的独立 ARM64 探针退出 0。原版 netd 与 Mac 一样到达本地连接自检。Mac 跟踪确认它先在 IPv6 通配地址监听，再从 IPv4 连接该端口。
- 客体现在支持默认 IPv6 双栈监听接收 IPv4 连接和报文，向 IPv6 端报告映射后的 IPv4 地址。宿主回归覆盖 TCP 双向发送、UDP 来源、双栈端口冲突和 `IPV6_V6ONLY`。相同 ARM64 Bionic 探针在 Android 17 参考内核和 Mac 均完成 IPv4 到 IPv6 监听的双向读写。Mac 标准启动链中的原版 `netd1shot` 以 0 退出，常驻 netd 日志报告 `libnetd_updatable_init success`。原版 init 启动 Zygote 并进入类预加载。常驻 netd 随后报告镜像缺少 `iptables-restore` 和 `ip6tables-restore`，完整启动尚未验证。
- iPhone 11 进程 8399 的双栈探针正常退出。原版 `netd1shot` 以 0 退出，常驻 netd 报告 `libnetd_updatable_init success`。真机 Zygote 输出 `begin preload`，尚未满足 `sys.boot_completed=1`。
- 补齐参考镜像的原版 `iptables` 多调用程序和 `iptables-restore`、`ip6tables-restore` 两个名称。完整重采集得到 1232 个文件，程序字节、链接与 SELinux 域均已核对。Mac 冷启动日志确认两个原版命令在 `u:r:netd:s0` 中执行，后续工具子进程退出，常驻 netd 重试。防火墙内核接口与完整启动仍待验证。
- 参考镜像预装了零字节的只读分区文件 `/system/etc/xtables.lock`，重新采集确认客体打包中保留。加入客体 `flock` 后，宿主回归验证独立打开间竞争、重复 FD 共享锁、升级竞争和末次关闭释放；Android 17 参考内核的独立 Bionic 夹具通过相同竞争序列。Mac 上原版防火墙工具越过锁文件，开始初始化 `filter`、`raw` 与 `mangle` 规则表，该内核 ABI 仍待补齐。[AOSP 锁文件变更](https://android.googlesource.com/platform/external/iptables.git/+/2bf769bb24c2ecf2ffac37773c1656cc15b654dd%5E%21/)
- iPhone 11 进程 8406 的独立文件锁测试退出 0，原版 IPv4 和 IPv6 防火墙程序都越过文件锁，并进入规则表查询。
- 客体实现 IPv4/IPv6 原版 xtables 规则表的查询、基本规则替换、计数器和回环数据路径执行。参考 Android 17 与 Mac 的同一 Bionic 夹具验证标准表、挂钩和结构尺寸，参考镜像无 IPv6 NAT 表。宿主回归用两个地址族的真实 UDP 路径确认 OUTPUT 丢弃与放行、无效替换保留原规则、旧计数器可读。Mac 的 Android 启动已越过规则表初始化，原版工具开始请求 `tcp`、`TCPMSS`、`connmark` 与 `CONNMARK` 扩展。完整启动尚未通过。[Linux xtables 规则表 ABI](https://codebrowser.dev/linux/linux/net/ipv4/netfilter/ip_tables.c.html)
- iPhone 11 进程 8410 的 IPv4/IPv6 规则表 Bionic 查询探针退出 0。原版防火墙工具随后请求 TCP、TCPMSS 和连接标记扩展。
- 客体实现 TCP/UDP 端口匹配、TCP 标志和选项匹配，以及在 SYN 上设置或限制 MSS。宿主用两个地址族验证只有匹配端口的 SYN 发生变化，现有较小 MSS 保留，IPv4 头和 TCP 校验和正确。参考 Android 17 的两个原版 restore 工具在测试模式接受同一条规则。Mac 冷启动中原版工具越过 TCP/TCPMSS，常驻 netd 仍因 `connmark` 与 `CONNMARK` 未实现而未能完成规则初始化。[Linux TCPMSS 实现](https://codebrowser.dev/linux/linux/net/netfilter/xt_TCPMSS.c.html)
- 实现来宾 `SO_MARK` 和 IPv4/IPv6 的连接标记。TCP 两端共用一份连接标记，回环 UDP 按套接字对跟踪并过期。宿主回归确认保存、掩码、移位恢复及后续报文读取同一连接状态。Android 17 参考内核确认匹配修订 1、目标修订 1/2 返回 0，未知修订返回 EPROTONOSUPPORT。Mac 冷启动的原版工具越过连接标记，随后请求 `NFLOG`、`REJECT` 与 `u32`。常驻 netd 仍未完成规则初始化。[Linux 连接标记实现](https://codebrowser.dev/linux/linux/net/netfilter/xt_connmark.c.html)
- 实现 IPv4/IPv6 xtables `u32` 修订 0。客体规则可组合网络字节序读取、掩码、移位、按报文内容跳转、区间与反选。宿主回归对两个地址族验证动态 TCP 首部、两个独立的载荷条件、截断、反选及拒绝无效移位后保留旧规则。参考 Android 17 的原版 restore 工具在测试模式接受同样的 netd 表达式。Mac 冷启动的原版工具已越过 `u32`，仍报告 `NFLOG`、`REJECT` 和 `state` 未支持，尚未读到 `sys.boot_completed=1`。[Linux u32 匹配实现](https://codebrowser.dev/linux/linux/net/netfilter/xt_u32.c.html)
- iPhone 11 第 8437 号进程运行同一 Bionic 诊断程序，IPv4/IPv6 的 TCP、TCPMSS、连接标记和 u32 修订号查询均通过，规则表探针也退出 0。早期第 8431 号诊断在查询后打印日志，再读取可能已被写日志改变的 errno。更正探针先保存 errno 后复测通过。原版工具仍报告其他扩展缺失，`sys.boot_completed=1` 尚未通过。
- 客体支持 IPv4/IPv6 xtables 的连接状态匹配。TCP 区分发起的 SYN 和握手响应后的报文，UDP 按回环端点跟踪首包与应答。宿主回归以真实客体 UDP 套接字验证首包与同向报文放行，回复及之后两方向均匹配 ESTABLISHED 规则。Android 17 参考系统的原版恢复工具接受 ESTABLISHED 规则。Mac 启动的原版 netd 越过 `state`，在当前规则初始化仍报告 `NFLOG` 和 `REJECT` 缺失。[Linux 连接状态匹配](https://codebrowser.dev/linux/linux/net/netfilter/xt_state.c.html)
- iPhone 11 第 8454 号进程复验 IPv4/IPv6 规则表和修订号查询，两项 Bionic 探针均退出 0，覆盖新增的 `state` 修订 0。同一轮隔离诊断由原版 init 启动参考镜像中的 AIDL Ranchu Camera Provider，客体进程进入 `u:r:hal_camera_default:s0`。尚未确认 Provider 完成注册，也未接入 iPhone 相机。原版工具仍报告 `NFLOG` 与 `REJECT` 缺失，未达到 `sys.boot_completed=1`。
- 参考 Android 17 的隔离网络命名空间实测默认 `REJECT` 和 TCP reset。IPv4/IPv6 的已连接 UDP 在 INPUT 拒绝时本次 send 成功、随后 `SO_ERROR` 为 ECONNREFUSED。在 OUTPUT 拒绝时 send 返回 EPERM，随后 `SO_ERROR` 同样为 ECONNREFUSED。两种挂钩的 TCP reset 均使非阻塞 connect 返回 EINPROGRESS，poll 后读到 ECONNREFUSED。客体待复现这些结果。[IPv4 REJECT](https://codebrowser.dev/linux/linux/net/ipv4/netfilter/ipt_REJECT.c.html) · [IPv6 REJECT](https://codebrowser.dev/linux/linux/net/ipv6/netfilter/ip6t_REJECT.c.html)
- 客体支持 IPv4/IPv6 规则的 REJECT 修订 0，并把本地拒绝结果送到 TCP/UDP 套接字。宿主回归覆盖 INPUT/OUTPUT、无效规则、UDP 的可读错误和非阻塞 TCP 的 poll 与 SO_ERROR。测试值与 Android 17 参考内核一致。Mac 冷启动的原版工具已越过 REJECT，仍停在 NFLOG。
- iPhone 11 进程 8461 的 IPv4/IPv6 规则表和 REJECT 修订号查询探针均退出 0。原版工具越过 REJECT，仍缺 NFLOG，尚未完整启动。
- 支持 xtables NFLOG 与客体 NETLINK_NETFILTER 日志组。按组绑定、设置复制模式和长度、限制其他套接字访问同组，并通过真实 UDP 数据路径交付客体日志。当前记录逐条交付。宿主 ABI 回归与参考 Android 17 隔离网络测试均验证 IPv4/IPv6 报文前缀、挂钩、限定后的载荷及组所有权。Mac 原版 netd 进入子链创建，出现 mark/MARK 与规则列表读取问题，尚未达到 sys.boot_completed=1。[Linux NFLOG](https://codebrowser.dev/linux/linux/net/netfilter/nfnetlink_log.c.html)
- 支持 IPv4/IPv6 的报文标记匹配修订 1 与 MARK 目标修订 2。宿主回归确认仅修改匹配报文的掩码位，并保留其余位。Android 17 参考镜像的原版恢复工具接受同一条掩码规则。[Linux 报文标记](https://codebrowser.dev/linux/linux/net/netfilter/xt_mark.c.html)
- iPhone 11 进程 8464 通过 NFLOG 端到端探针。客体的原版 IPv4/IPv6 restore 工具各自安装测试规则，Bionic 探针实际发送和接收 UDP，并核验 netlink 日志中的组、挂钩、前缀和截取后的报文内容，随后撤销规则。该诊断仍未达到 sys.boot_completed=1。
- 加入报文标记后的 Mac 冷启动越过 mark/MARK 参数解析，原版 netd 进一步停在 BandwidthController。原版 restore 请求 bpf 匹配的 --object-pinned，客体尚未支持该 xtables ABI。规则列表中的 state 扩展警告仍在。
- 实现 IPv4/IPv6 xtables bpf 修订 1 的固定路径和程序 FD 模式，原子替换规则时解析程序并持有引用，在客体报文路径调用原有 BPF 执行器。宿主回归验证真实 UDP 丢弃与放行、删除固定名称和关闭 FD 后继续执行、无效替换保留旧规则。参考 Android 17 的原版双栈工具接受实际 netd denylist 路径。Mac 冷启动越过 --object-pinned 解析，BandwidthController 下一处提示是 policy 匹配扩展。经典内联 BPF 字节码模式尚未实现。[Linux xt_bpf](https://codebrowser.dev/linux/linux/net/netfilter/xt_bpf.c.html)
- iPhone 11 进程 8473 通过固定 BPF 修订号探针，原版工具越过 --object-pinned 并在 BandwidthController 报告缺少 policy。NFLOG 端到端探针在同轮再次退出 0。尚未达到 sys.boot_completed=1。
- 客体支持 IPv4/IPv6 xtables policy 修订 0 对无 XFRM 变换的回环报文做匹配。入站和出站按各自挂钩计算，普通回环报文匹配 `--pol none`，不匹配 `--pol ipsec`。宿主回归验证两种结果及无效替换，参考 Android 17 的原版工具接受同样的 IPv4/IPv6 策略。Mac 冷启动越过 BandwidthController，RouteController 因地址族尚未支持而退出，完整启动待验证。[Linux 策略匹配](https://codebrowser.dev/linux/linux/net/netfilter/xt_policy.c.html)
- iPhone 11 进程 8479 的相机目录探针以状态 0 退出，来自 AVFoundation 的后置摄像头已通过 Goldfish 相机目录呈现给客体探针。同轮帧探针发起 iOS 相机授权，未在等待窗口获得回应，65 秒后以状态 13 退出。真实帧、Camera2 预览和拍照仍待真机验证。
- 实现客体 NETLINK_ROUTE。支持双栈规则的查询、添加、删除和内核 ACK，提供实际回环接口、地址和本地路由快照，并将规则结果用于客体回环连接。`writev` 的多个片段按一条 netlink 报文发送。宿主回归验证规则确实改变连接结果，以及无待收消息的已连接 netlink 套接字不会报告 EOF。参考 Android 17 的 Bionic 探针在隔离网络命名空间验证双栈规则读写和回环拒绝。Mac 冷启动越过 RouteController，原版 netd 接着因尚无 NETLINK_XFRM 而退出。尚未达到 sys.boot_completed=1。[AOSP RouteController](https://android.googlesource.com/platform/system/netd/+/refs/heads/main/server/RouteController.cpp)
- iPhone 11 进程 8503 通过同一 Bionic 双栈路由探针，验证原版 netlink 规则 ACK、查询、重复规则拒绝、实际回环连接拒绝及恢复。相机目录探针继续通过，真实帧仍在等待 iOS 授权。
- 支持原版 netd 初始化时对空 XFRM SA 和策略表的清空、ACK 和查询。宿主和 Android 17 参考系统的 Bionic 对照通过。当前客体传输未实现 IPsec 加密或隧道，建立变换仍返回 EOPNOTSUPP。Mac 冷启动确认原版 XfrmController 完成初始化，下一条阻塞为 netlink recvfrom 请求来源地址。客体现支持返回内核来源、短地址缓冲区和 MSG_PEEK/MSG_TRUNC，宿主回归通过。[AOSP XfrmController](https://android.googlesource.com/platform/system/netd/+/refs/heads/main/server/XfrmController.cpp)
- 加入 netlink recvfrom 来源地址后，Mac 的 Android 17 原版 netd 越过此前的收包停点。常驻 netd 下一次挂起出现在 epoll_ctl 的事件掩码检查，Zygote 已开始类预加载，完整启动仍未通过。
- 已用同版 SDK 核对 10 项启动输入，生成包含 14 个工厂系统 APK、Launcher3、Camera2/计数器验收 APK 和原版相机服务的镜像。镜像保留实际 BPF 加载动作，诊断版使用源码构建的 Tethering 加载器。iPhone 11 进程 8510 已启动 1,800 秒诊断，4.4 秒进入 Android init，102.2 秒到达原版 SurfaceFlinger。桌面、APK 安装和 Camera2 实体帧尚未取得本轮验收证据。
- 原版 netd 在 Mac 首次调用 `EPOLL_CTL_ADD` 时对 eventfd 使用掩码 `0x80002001`。客体现在按每个订阅分别记录 eventfd 的读写通知，只在重新收到相关通知或修改订阅后再次报告准备状态。宿主 ABI 回归和 Android 17 Bionic 对照验证重复写入、零值写入、多个观察者、故障重试和读后恢复可写，iPhoneOS Release 编译通过。Mac 再次启动已越过 eventfd，追踪确认 netd 接下来以同一掩码注册 Unix socket。[Linux eventfd 实现](https://github.com/torvalds/linux/blob/master/fs/eventfd.c)
- iPhone 11 进程 8510 到达 90 亿条客体指令上限。报告确认该诊断构建缺少仓库已有的 `state` 和 `policy` xtables 扩展支持，netd 因此反复退出，原版 init 重启 Zygote。`sys.boot_completed`、APK 安装与实际可交互帧均未取得本轮证据。
- 加入 Unix socket 的读事件边沿触发。连接排队、收到数据和对端关闭可以再次唤醒订阅，待读数据未变化时不会重复报告。宿主 ABI 与 Android 17 Bionic 对照通过，包括 Unix 分组套接字 EOF 的 `EPOLLIN | EPOLLHUP`。Mac 启动确认原版 netd 注册服务，Zygote 预加载 18,596 个类，原版 `system_server` 开始启动 ActivityManager。使用同一份完整仓库代码和源码版 Tethering 加载器重新编译 iPhoneOS 验证包；iPhone 11 进程 8539 以 400 亿条指令、1,800 秒上限启动，77.5 秒连接原版 SurfaceFlinger 的显示设备。[Linux Unix socket 实现](https://github.com/torvalds/linux/blob/master/net/unix/af_unix.c)
- iPhone 11 进程 8539 在 103.9 秒报告原版 BootAnimation 已显示。宿主分别于 106.252 秒显示首帧、106.298 秒保存首个非纯色帧。检查该进程独立记录的 640 × 960 PNG，内容为原版 Android 开机标志。215.939 秒进入 Zygote 类预加载。桌面和系统启动完成仍待真机验收。
- iPhone 11 进程 8539 运行原版 `system_server`，约 12 分 49 秒完成 DisplayManager 首屏等待，约 12 分 50 秒开始初始化 PackageManager。已完成系统配置读取和共享用户注册，Launcher 和 APK 安装还待本轮日志验证。
- `prepare-boot-image.py --tethering-loader` 将源码构建的 ARM64 加载器随客体镜像打包，保留原版 `trigger load-bpf-programs`，并核对 SDK 工厂 Tethering APEX 的哈希。运行时在客体原 APEX 路径使用这个明确选定的 ELF，保持原模块的链接命名空间和文件元数据。无需临时修改 Mac 或 iPhone 的 C++ 源码。重建核对 11 个启动输入，收集 14 个工厂 APK 和 3 个依赖 APEX。在 Mac 上，原版 init 记录 bpfloader 正常退出，netd 完成服务注册，Zygote 预加载 18,596 个类。iPhoneOS Release 构建与开发签名通过。新包待安装，iPhone 11 进程 8539 继续本轮系统应用扫描。
- iPhone 11 进程 8539 在 1,316.572 秒记录原版 PackageManager 完成系统应用扫描，扫描耗时 525.756 秒。CoreDevice 截取的实体屏幕确认 iOS 相机授权弹窗已出现。客体启动仍在运行，Launcher、安装与实体相机首帧尚待核对。
- 真机进程 8539 达到 30 分钟诊断上限。原版 PackageManager 扫描了 74 个系统包，走到保存包设置。报告中 `installd` 在递归恢复应用数据标签时多次遇到 ENODATA。客体补齐新建持久文件和目录的初始标签，并让旧数据中缺失标签的 inode 通过文件系统的 unlabeled 上下文参与 restorecon。应用数据标签的类型来自镜像的 seapp_contexts。Android 17 参考内核和 Mac 客体均用同一个原版 libselinux 探针成功递归恢复包根、缓存目录和文件的标签。持久化、无效上下文和目录权限回归通过。iPhoneOS 编译与签名通过，下一轮将复验完整应用初始化。
- 进程 8539 的完整日志显示原版 Ranchu 相机服务未启动，vendor_init 被拒绝从 vendor/build.prop 写入相机协议的 ro.boot 属性。参考 Android 17 的属性由 bootconfig 提供。镜像构建现提供同样的 bootconfig 项，并按 Ranchu 的 gralloc 属性启动原版相机服务。Mac 客体中独立 Bionic 读回协议版本 1，原版 init 以 hal_camera_default 进入 Ranchu 相机服务。同轮原版 libselinux 标签探针通过。iPhoneOS 编译与签名通过，真机相机帧和 Camera2 预览待验证。
- 完整镜像从 Android 17 参考内核采集了 38 个允许 Zygote 动态切换的应用域，以及对应的 Binder 和 HWBinder 服务决策。宿主回归覆盖权限范围、应用 UID 对应的类别、继承描述符和禁止二次切换。Mac 客体中的原版 libselinux 将 Zygote 子进程切到与参考 Editor 进程一致的 `u:r:untrusted_app_34:s0:c160,c256,c512,c768`，并验证可以查询 Activity 服务、会被拒绝查询 vold 服务。Android 桌面和第三方 APK 仍待 iPhone 11 端到端验证。
- Mac 的完整镜像完成默认显示初始化、工厂包扫描及系统 ContentProvider 安装。Zygote 随后报告 Android 17 的 HintManager 在读取 Power HAL 的 SupportInfo 时得到 null。参考系统通过 `/vendor/apex/com.android.hardware.power.apex` 提供对应的原版 Power HAL，该 APEX 仍待加入镜像。
- 对原版 Ranchu 相机进程的系统调用跟踪显示，它通过 AF_VSOCK 连接相机通道。客体实现 Android SDK qemupipe 使用的主机 CID 2 和数据端口 5000，仅允许相机 HAL 的两个服务名。宿主回归验证未连接错误、Ping 端口拒绝、描述符复制和接收设备目录，原版 HAL 在 Mac 客体中已请求测试摄像头目录。iPhoneOS Release 编译通过，实体帧待真机验证。[AOSP qemupipe](https://android.googlesource.com/platform/hardware/google/gfxstream/+/refs/heads/main/guest/qemupipe/qemu_pipe_guest.cpp)
- iPhone 11 进程 8575 完成 18,596 个 Zygote 类预加载，启动约 647 秒进入 DisplayManager 的默认显示阶段。约 1,314 秒时 Watchdog 报告主线程阻塞并终止 Android 系统进程。持久化的 ANR 文件记录同一原因，客体 debuggerd 没有导出 Java 线程栈。该轮尚无 Launcher、APK 安装及实体相机帧证据。
- 从 Android 17 参考系统收集原版 Power APEX 及两个 HAL 的 SELinux 域。客体允许只读的厂商 APEX 通过原版 apexd 挂载。完整基础镜像采集得到 1,254 个 Android 文件。Mac 诊断验证原版 init 解析 APEX 内的启动脚本并执行两个服务，servicemanager 在 VINTF 中找到它们申请注册的 IPower 和 IPowerStats 接口。宿主 ABI 回归与 iPhoneOS Release 编译通过，完整镜像仍待复测 HintManager 启动。
