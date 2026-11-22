# 内核层

Android 的程序是 Linux 程序。它们的一切都向内核要：内存、文件、线程、定时器、套接字，还有 Android 进程之间相互调用所用的 Binder 驱动。
iOS App 里没有 Linux 内核可问，iOS 内核的接口也不一样，而且大部分不对 App 开放。
所以 Nestvane 自带一份 Linux 系统调用接口的实现，在用户态，用普通的 C++ 写成。
这组文档把它叫作内核层。

它不是 Linux 的移植，而是照着接口的可观察行为写出来的：每个调用返回什么、事情按什么顺序发生、`/proc` 和 `/sys` 下的文件里有什么内容。
Android 依赖确切行为的地方，内核层紧跟 Linux；Android 只需要一个答复的地方，内核层给一个固定的答复。
这篇文档会说明哪些属于前者，哪些属于后者。

## 源文件

| 文件 | 内容 |
| --- | --- |
| [`runtime/ProbeLinux.cpp`](../../runtime/ProbeLinux.cpp)、[`ProbeLinux.hpp`](../../runtime/ProbeLinux.hpp) | 系统调用、任务、调度器、挂载、文件、套接字、信号。约 12,000 行。 |
| [`runtime/ProbeBinder.cpp`](../../runtime/ProbeBinder.cpp) | Binder 驱动。 |
| [`runtime/ProbeData.cpp`](../../runtime/ProbeData.cpp) | `/data` 背后的持久化存储。 |
| [`runtime/GuestSpace.cpp`](../../runtime/GuestSpace.cpp)、[`GuestSpace.hpp`](../../runtime/GuestSpace.hpp) | 以宿主内存实现的客体进程地址空间。 |
| [`runtime/Ext4.cpp`](../../runtime/Ext4.cpp) | 只读的 ext4 镜像读取器。 |
| [`runtime/GuestRoute.hpp`](../../runtime/GuestRoute.hpp)、[`GuestNetlink.hpp`](../../runtime/GuestNetlink.hpp) | 网络接口和路由 netlink。 |
| [`runtime/BpfProgram.hpp`](../../runtime/BpfProgram.hpp)、[`GuestFirewall.hpp`](../../runtime/GuestFirewall.hpp) | Android 的网络守护进程装入的 eBPF 程序和包过滤规则。 |
| [`runtime/DynamicProbe.cpp`](../../runtime/DynamicProbe.cpp) | 运行客体线程并调用内核层的主循环。 |

名字里的 “Probe”（探针）是历史遗留：内核层最初是一个测试工具，一次运行一个 Android 程序，并报告第一个它答不上来的系统调用。

## 任务与调度

一个客体线程是一条 `Thread` 记录：保存的寄存器、所属进程、挂载命名空间、seccomp 过滤器、信号状态，以及它正在等什么。
进程是由它的线程共享的 `Process` 记录，里面有内存映射（`Segment`）、地址空间、文件描述符和身份凭据。
`clone`、`fork` 和 `execve` 像 Linux 那样创建和替换这些记录；`fork` 以写时复制的方式复制地址空间。

调度是一个宿主线程上的单个循环。
`switchThread` 按线程号的顺序取下一个没有在等待的线程，恢复它的寄存器，让它运行一轮：一个时间片的原生代码，或固定数量的解释指令。
定时器和等待的截止时间每 200 微秒检查一次，而不是每次切换都检查，因为一次开机要切换线程几十万次。
没有线程可运行时，循环睡到最近的截止时间，或睡到有宿主事件到来。

所有系统调用都在这一个线程上执行，所以内核层自己的状态不需要加锁。
代价是系统调用永远不会并行。
线程的原生代码如果用满一整轮都没有离开，可以交给另一个核上的工作线程（见 [static-rewriting.md](static-rewriting.md)）；它下一次系统调用时又回到主循环。

简化之处：无论客体设置什么调度策略或优先级，所有线程都按 `SCHED_OTHER`、同等权重调度。控制组是一棵没有任何控制器的 `cgroup2` 树：进程可以在各组之间移动，Android 给进程分组只需要这一点，资源限制一概不执行。

## 内存

每个进程有一个按地址排序的段列表，每个段有范围、保护属性和后备：匿名页、文件的私有副本，或进程间共享的存储（内存文件、ashmem、Binder 缓冲区）。
`mmap`、`munmap`、`mprotect`、`mremap` 和 `madvise` 都作用在这个列表上。

代码要原生执行，客体内存就必须是位置可预知的真实宿主内存。
[`GuestSpace`](../../runtime/GuestSpace.hpp) 提供的正是这个：客体地址空间切成 1 MiB 的块，每块要么以某个偏移对应着宿主内存，要么根本没有后备。
有后备的块连成一段一段，每段两端各有保护页，所以越出基址寄存器所在那一段的访问会出错，而不会碰到无关的内存。
不可访问的保留（`PROT_NONE`）——Android 的分配器一次保留几个 GiB——在变成可访问或被碰到之前不占宿主地址空间。

- 私有段直接放在 `GuestSpace` 里。
- 共享存储放在 `SharedBuffer` 里，即一批宿主页，映射进每个映射了这份存储的进程。
- 只读的文件内容从同一份宿主副本以写时复制的方式映射，免得许多进程各存一份。

客体的页大小是 16 KiB，与宿主相同。这是必需的：页大小一致时，客体的 `mprotect` 才能直接交给宿主。
出于同样的原因，Android 用的是 16 KiB 页的构建变体。

没有实现：`userfaultfd`（ART 因此退回停顿式的压缩回收）和 `io_uring`。没有交换分区。

## 文件、挂载与命名空间

文件系统层是按路径组织的虚拟文件系统，背后有几种存储。

| 对象 | 后备 |
| --- | --- |
| 系统分区（`/system`、`/vendor`、`/product` 等） | 随 App 发布的客体根目录，只读。 |
| `/apex` 下的 APEX 模块 | 只读的 ext4 镜像，由 `Ext4.cpp` 读取，由 Android 自己的 `apexd` 经模拟的 loop 设备挂载。 |
| `tmpfs` 挂载（`/dev`、`/mnt`、`/apex` 等） | 内存。 |
| `/data` | 持久化存储，见 [devices.md](devices.md)。 |
| `/proc`、`/sys` | 生成的文件：进程状态、映射表、命令行、网络接口、电源、内存压力。 |
| `binder`、`bpf`、`cgroup2`、`fuse` | 各类型单独处理。 |

`mount` 接受的类型是 `tmpfs`、`ext4`、`binder`、`bpf` 和 `fuse`，以及绑定挂载和重新挂载；其他类型返回“没有这种设备”。

**挂载命名空间与传播。**
Android 给每个应用进程一个独立的挂载命名空间，让它只看到自己的存储。
内核层把命名空间（`unshare(CLONE_NEWNS)`、`setns`）实现为各自独立的挂载表。
它还实现了挂载传播，因为 Android 依赖它：存储守护进程 `vold` 在初始命名空间里挂上共享存储，应用必须在自己的命名空间里看到这个挂载出现。
一个挂载被设为共享时属于某个对等组；被设为从属时，记住它原来所在的组。
新的挂载会被复制到每一个这样的命名空间：在那里，它所落的目录是通过同一对等组的挂载，或者通过该组的从属挂载看到的（[`bindMount`](../../runtime/ProbeLinux.cpp)）。
每个命名空间最多 128 个挂载。

**元数据。**
文件带有属主、属组、权限位、时间戳、扩展属性（包括 SELinux 标签）、POSIX 访问控制列表、记录锁和内存文件的封印（seal）。
权限检查依据这些信息，只有共享存储是例外，见下文。

**简化：FUSE。**
Android 唯一经 FUSE 挂载的文件系统是共享存储，由媒体库（media provider）里的一个守护线程提供。
内核层没有实现 FUSE 协议。
`fuse` 类型的挂载就是 `/data/media` 的一个直接视图。守护线程收到开头的 `FUSE_INIT` 请求并应答，此后它的读操作一直等到文件系统被卸载。
文件操作都不经过这个守护线程，所以媒体库本来要按应用做的访问控制没有生效：对 `/data/media` 下的路径，内核层不检查属主和权限位。

## Binder

Binder 是作为驱动模型实现的，没有转交给宿主的任何东西。
`ProbeBinder.cpp` 处理 `/dev/binder`、`/dev/hwbinder` 和 `/dev/vndbinder` 上的 `ioctl` 接口；这三个节点和真实设备上一样，来自一次 `binder` 文件系统的挂载。

已实现：`servicemanager` 所做的上下文管理器注册；带应答的同步事务和单向事务；分散-聚集（scatter-gather）事务（硬件接口层 HIDL 用它传缓冲区）；对象引用和句柄在进程间的转换及引用计数；传递文件描述符，包括单个传递和放在缓冲区里的描述符数组；死亡通知；looper 线程协议，包括要求进程新建线程；附在事务上的安全上下文；以及对 Binder 描述符的 `poll`。
事务数据送进接收方映射的缓冲区，每个映射上限 4 MiB，并按 Android 依赖的方式统计异步事务的额度。

Android 自己的 `servicemanager` 以及它之上的各个服务，都未经修改地运行在这上面。

## 信号、futex 与事件等待

**信号**遵循 Linux 的模型：线程级的屏蔽字、待处理集合、带 `siginfo` 的处理函数、备用栈、`rt_sigreturn`、打断等待的信号，以及结束进程的默认动作。
处理函数运行在内核层于客体内存里构造的信号帧上（`installSignalFrame`），`rt_sigreturn` 从这个帧恢复被打断的状态。
客体代码里的宿主故障，在客体地址确实无效时变成客体的 `SIGSEGV`；ART 的空指针检查就靠它。
处理函数返回到一个位于固定客体地址（`0xFFFF0000`）的跳板页，这一页在代码表里的表项通向离开原生代码的桩。

**futex** 支持带位掩码的等待与唤醒、超时、重新排队（requeue），以及优先级继承的几种操作。共享内存里的 futex 按它的后备存储识别，所以两个把同一块内存映射到不同地址的进程，等的是同一个 futex。

**事件等待**包括 `epoll`、`poll`/`ppoll`、`eventfd`、`timerfd`、`signalfd` 和 `inotify`。
等待中的线程由事件本身唤醒，而不是靠轮询；例外是依赖宿主的等待（宿主套接字、渲染器），它们隔一小段时间再看一次。

## 套接字与 netlink

Unix 域套接字（流式、数据报和有序分组，支持传递描述符和凭据）在内核层里实现；Android 的属性服务、日志、zygote 的命令套接字等许多组件都用它。
发往回环地址的网络套接字留在内核层内部。发往其他地址的由宿主套接字承载，见 [devices.md](devices.md)。

netlink 实现了 Android 开机时用到的几个族：路由（链路、地址、路由、邻居）、内核对象事件（`uevent`，`ueventd` 读它来创建设备节点）、netfilter 和 XFRM。
链路和地址查询给出回环接口和一块以太网接口。路由表和邻居表的写入被接受但不保存，因为路由由宿主来做。
Android 的网络守护进程装入的 eBPF 程序和映射会被保存并解释执行（`BpfProgram.hpp`），包过滤规则也有相应的模型（`GuestFirewall.hpp`）；它们只能看到客体的回环流量。

## SELinux：采集下来的表

Android 以 SELinux 强制模式运行，它的用户态不断查询策略：`init` 要决定服务跑在哪个域里，`servicemanager` 要决定谁可以注册或查找服务，属性服务和 keystore 也是如此。
内核层里没有策略引擎。
答案是在准备客体根目录时从参考模拟器上采集的：[`tools/prepare-init-root.py`](../../tools/prepare-init-root.py) 和 [`tools/android_policy.py`](../../tools/android_policy.py) 向真实内核查询开机所需的决定，写成文本文件，放在客体根目录的 `nestvane-metadata/` 下（域转换、文件标签，以及属性、服务、硬件服务和 keystore 的访问表）。

运行时，内核层给出 `/sys/fs/selinux`，其中 `enforce` 读出来是 1，对 `access`、`create` 和 `context` 文件的查询按这些表作答。标签作为每个文件的 `security.selinux` 属性保存。

后果是：没有采集到的决定就不存在。
后来加进来的一些服务（媒体服务器、编解码器服务、媒体提取器）没有采集过策略行，它们的规则是参照相近的域手写的，见 [design-notes.md](design-notes.md)。
这里没有对每个系统调用做裁决的策略引擎；检查发生在 Android 用户态来问的地方，以及内核层自己查采集表的少数几处，比如给新建的文件打标签。

## seccomp

seccomp 过滤器是忠实实现的，因为 Android 会给 zygote 的子进程和各媒体服务装上过滤器，而行为不同的过滤器会改变进程被允许做的事。
`installSeccomp` 读入并校验经典 BPF 过滤程序。`enforceSeccomp` 在每次系统调用时运行线程上挂着的每一个过滤器，输入与 Linux 内核传入的数据相同（调用号、体系结构、指令指针、六个参数），并采用其中最严格的裁决：放行、返回错误、发 `SIGSYS`，或杀掉进程。
过滤器在 `fork` 和 `clone` 时继承。

这不只关乎忠实：动态链接器补丁早先用过一个私有系统调用，在还没有 seccomp 的内核层里一切正常，到了真实内核上却把 `system_server` 杀掉了。

## 设备节点

| 节点 | 用途 |
| --- | --- |
| `/dev/null`、`/dev/random`、`/dev/urandom`、`/dev/kmsg` | 与 Linux 相同。 |
| `/dev/ptmx`、`/dev/pts/*` | 伪终端。 |
| `/dev/binder`、`/dev/hwbinder`、`/dev/vndbinder` | Binder。 |
| `/dev/ashmem`、`/dev/dma_heap/system` | 可共享的内存；每次分配是一个内存文件。 |
| `/dev/loop-control`、`/dev/block/loop*` | loop 设备，用来挂载 APEX 镜像。 |
| `/dev/fuse` | 上文所述的简化 FUSE 会话。 |
| `/dev/goldfish_pipe_dprctd`、`/dev/goldfish_address_space` | 模拟器的图形管道和共享显存。 |
| `/dev/input/event0` | 触摸和按键。 |
| `/dev/snd/controlC0`、`/dev/snd/pcmC0D0p` | 一块声卡：一个没有控件的控制设备和一个播放设备。 |

设备节点由 Android 的 `ueventd` 根据内核层发出的内核对象事件创建，与真实设备上一样。

## 遇到没有实现的调用

内核层实现的是观察到 Android 会发出的那些调用。未知的系统调用号不会被悄悄忽略：主循环把它写进报告并挂起发出调用的进程；如果调用者是 `init`，就停掉客体。
这是有意为之。错误的答复往往要到很久以后才以一个不相干的故障冒出来，而停下来则直接点出了缺的是哪个调用。
