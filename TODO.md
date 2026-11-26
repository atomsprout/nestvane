# TODO

2026-11-24 定，1.9 发布后，按 [#5](https://github.com/atomsprout/nestvane/issues/5) 的反馈排。版本号往下加 1.10、1.11……，2.0 留给真正的大突破。每项的做法和验收见 [PLAN.md](PLAN.md)，总览见[路线图](docs/zh-CN/roadmap.md)。做完一项就勾掉，发布时在 #5 里标出来。

每个版本发布前：`tools/boot-check.sh` 开机只能更快、不能更慢（iPhone 14 基线见 `results/boot-baseline.txt`），过了用 `--record` 记下，数字写进发布说明。

## 1.10 装应用和文件（1–2 周）

- [x] 在 Android 的文件管理器里点 APK 能装（#5-2）。原因是内核层缺 OFD 记录锁，MediaProvider 打开文件时卡住；Google Files 走的是同一条路，没有实测（不再下载新应用）
- [x] iOS 的分享菜单和“用其他应用打开”能把 APK 交给 Nestvane 安装（#5-3）：交过来的文件按内容认 APK，不看扩展名；用 devicectl 交文件验证过，没有在分享菜单里手点
- [x] 共享文件夹实时同步，放进去不用重启，并通知媒体库（#5-1）
- [x] 图片和视频能显示、能播放（#5-9）：系统里没有能打开图片、视频、声音的应用，加了一个很小的 Viewer
- [x] Nestvane 栏里加“最近任务”键（#5-7）
- [ ] ~~桌面跟着设备转~~ 移到 1.15：Android 17 的 Launcher3 在手机上没有这个设置，只在大屏上转，要改 Launcher3

## 1.11 网络、录制和卡死（1–2 周）

- [x] 没网（#5-4）：对方没补充网络环境，没能复现。按最可能的原因改了：网络对应用报告成 Wi-Fi（只认 Wi-Fi 和蜂窝的应用以前当成没网）。老的 `NetworkInfo.getType()` 仍是以太网，要改连接模块（1.15 重建时）
- [ ] Wi-Fi 服务：做过，能运行、报告 Wi-Fi 关着，但开机慢 0.3 秒（iPhone 14 同一构建开关对比 9451 对 9141 毫秒），1.11 不带。移到 1.15：改 system_server，开机完成后再启动它（2026-11-25 定：1.11 已让应用看到 Wi-Fi，只有直接找 WifiManager 的应用如 KDE Connect 还缺它，不值得为它单独花时间；有用户报常用应用因此用不了再提前）
- [x] IPv6：手机网络有 IPv6 时，IPv6 目的地经主机出去，查得到 AAAA，仍先试 IPv4。只有 IPv6 的网络（NAT64）仍没实测
- [x] 录屏导出 0 KB（#5-8）：没能复现。系统录屏在 iPhone 14 上录出 668 KB、存进媒体库；多半是 1.10 修好的媒体库等锁。请对方复测
- [x] 录音声音不对（#5-13）：采样率、声道都对（48 kHz 单声道、44.1 kHz 双声道，每秒帧数对得上）。补了 iOS 输入格式中途变化时重建录音，以前会停住或按旧采样率转。没能用耳朵判断
- [x] 菜单里一键导出诊断信息（#5-10）
- [x] 开机：close 和读套接字不再叫醒所有等待的线程，查找表页不再逐页 pwrite，stat 和挂载点查找不再线性；iPhone 14 中位数 8388 毫秒（1.10 是 8747）
- [ ] 下一步可做：`mountFor` 也按前缀查（改好了但没测开机，没带进 1.11）；mmap 和 fork 的宿主 VM 调用仍是开机里最大的一块
- [x] 卡死的一个原因：地址空间满了新进程起不来。lmkd 以前一直停在 `mlockall`，现在桌面出来后运行（和 Android 一起启动要多几百毫秒）；满了时结束最不重要的缓存应用

## 1.12 KernelSU 和备份（已发布）

- [x] KernelSU Manager 显示已工作，能给应用授权和撤销 root（#5-5）：内核层按 KernelSU 3.3.1 的接口（UAPI 5）回答它的驱动，官方 Manager 按签名证书认；授权就是 iOS 里的同一份回答。iPhone 14 上用官方 Manager 3.3.1 测过：已工作 32680，开关打开应用拿到 root，关掉被拒。顺带修了 `/data` 里的程序不能运行、kill 发 SIGTERM 给自己处理它的进程会卡住
- [ ] KernelSU 模块：要在开机各阶段运行 `ksud`、做 overlay 挂载，之后再定
- [x] 备份先做精简版，排在 KernelSU 前面（#5-12）：Android 停下后把数据导出成一个文件存进“文件”，导入时整个替换，原来的数据先留一份；不带共享文件夹，不做跨版本兼容，约 1–2 天。整台 iPhone 的备份本来就包含 Android 的数据，这个只为回滚（装 KernelSU 前留存档）和只挪 Android（2026-11-25 定）。做完：More 菜单里 Back Up / Restore Android，Apple Archive（LZ4）；原来的数据留到 Android 从备份启动到桌面，没到桌面就停了放回去。iPhone 14 上 507 MB 备份成 331 MB，恢复 0.8 秒，卸载的应用和改过的文件都回来了。文件选择器那一步没有在手机上手点过

## 1.13 相机和稳定性（已发布）

- [x] 前置相机出现在相机应用里（#5-14）：内核层的 qemud 相机服务多了 `nestvane-front`（640×480，朝前，方向 270），iOS 这边用前置摄像头、不镜像。iPhone 14 上 Camera2 列出两个相机：id 10 朝后、id 11 朝前
- [x] 拍照和录像能存下来（#5-14）：iPhone 14 上测试应用 `fixtures/media` 的 `CameraActivity` 用后置、前置各拍一张 640×480 的 JPEG，后置录 5 秒视频（H.264 145 帧、AAC 声音），都存进 DCIM/Camera、在媒体库里，iOS“文件”的共享文件夹里也有（2026-11-26，1.13 发布后）。之前没拍成是 iOS 设置里 Nestvane 的相机开关是关的，用 UI 测试打开。当时手机放在暗处，画面很暗（平均亮度 15）。没录成的视频留下 0 KB 的 `.pending-*` 文件。Google 相机没复测
- [x] 用旧相机接口（Camera API 1）的应用打不开相机（Open Camera 显示 “Failed to open camera”）：镜像没有 media profiles，内置的视频尺寸最大 352×288，API 1 的 `Parameters` 只列视频尺寸范围内的预览尺寸，640×480 不在里面。改成运行时挂一份 `/vendor/etc/media_profiles_V1_0.xml`（640×480 的 H.264 档）。iPhone 14 上 Open Camera 拍照、录 6.2 秒带声音的视频都存进 DCIM/OpenCamera。在 1.13 发布之后，跟下一版发
- [x] 小修：Allow root 改成每次启动 Android 时读；设置页脚改成放进 Android 文件夹的文件几秒内出现
- [x] 再测 50 个主流应用，共 104 个，87 个能打开（[清单](docs/zh-CN/compatibility.md)）。修了一次启动里接连开很多应用时的问题：iOS 因内存结束 Nestvane（现在先结束 Android 排得最低的应用）、宿主内存不够或进程出错时整个 Android 停住（现在只结束那个进程）、WebView 的 zygote 卡住后所有应用都起不来、400 MB 的 APK 复制时内存不够；JNA 和 Rust 的应用原生运行；补了 pselect6、边沿触发 epoll、clone/wait4 的几种用法、网络接口的 ioctl、缺的七个共享库
- [ ] 还没打开的：网易云音乐（安全库在不可执行的内存里跑代码）、12306（梆梆加固）、知乎和 Mihon（自己的原生代码里崩）、WPS、Session（要 Wi-Fi 服务，1.15）、哔哩哔哩、夸克、UC、爱奇艺、Opera、LocalSend（太慢）、Zoom（窗口空白）；百度网盘只有 32 位，不支持
- [x] 稳定性：一次启动里把装着的 8 个应用各开三轮（24 次，每次 12 秒再回桌面，1.13 发布的构建，iPhone 14）。Android 没停、Nestvane 没被结束、没有 ANR；内存紧时 Nestvane 先结束缓存应用，按设计
- [ ] WebView 的渲染进程崩溃（SIGSEGV，`<anonymous:140000000>` 里 pc 0x1243b48，只有一帧），带 WebView 的应用跟着被结束（“Render process kill wasn't handled … killing application”）。测试应用开着十几秒或从后台回来时出现，三轮里每轮一次。1.12 一样（同一地址），不是 1.13 新出的。崩在 `libwebviewchromium.so` 偏移 0x1243b48，是链表摘节点（`ldp x9, x8, [x0]; str x8, [x9, #8]`，像 `base::LinkNode::RemoveFromList`）；读到的两个指针 0x8097110000000000 和 0x7f68eeffffffffff 互为按位取反，是 PartitionAlloc 空闲链表条目的样子，说明这个节点所在的内存已经释放过（用了已释放的内存）。查过一项不是：MADV_DONTNEED 对共享匿名映射不清零（照 Linux）后照样崩，每次打开 11 秒左右就崩，已撤回。下一步看开着十几秒时渲染进程做什么（定时的清理任务，比如 PartitionAlloc 的 purge），再查相关的系统调用

## 1.14 稳定和开机（2026-11-26 定）

1.13 发布后发现的问题都放在 1.14 修，原来 1.14 的重建 AOSP 顺延到 1.15。

- [x] 用旧相机接口的应用（Open Camera）能开相机（上面 1.13 一节有细节，204e630）
- [x] WebView 的渲染进程崩溃：内核层的 ashmem 没实现 PIN、UNPIN、GET_PIN_STATUS（回 ENOTTY）。Chromium 的 discardable memory 用它们给内存块加锁、解锁，失败后它的堆把还在用的块当成丢了，之后在 `DiscardableSharedMemoryHeap` 合并空闲块时摘一个已释放的节点（链表摘节点处 SIGSEGV）。补上后：从不回收，PIN 总是 ASHMEM_NOT_PURGED，记下哪些范围解锁着。iPhone 14 上 WebView 测试应用连开三轮（每次 25 秒）不崩，8 个应用三轮也没有一次崩溃
- [ ] 开机比基线慢：同一台 iPhone 14，1.11 自己当天也慢 0.5 秒，查清楚原因，正式检查要过
- [ ] 开机门槛、文档、发布

## 1.15 要重新构建 AOSP

- [ ] 从快照恢复构建机，按[构建文档](docs/zh-CN/building.md)重建
- [ ] iPad 的平板布局
- [ ] 桌面跟着设备转（改 Launcher3 的 RotationHelper）
- [ ] 硬件视频解码
- [ ] Wi-Fi 服务开机完成后再启动（改 SystemServer），不占开机时间；KDE Connect 能打开
- [ ] 老接口 `NetworkInfo.getType()` 也报 Wi-Fi（改连接模块）
- [ ] 考虑：签名伪装，让 microG 能用（不承诺，#5-6）

## 2.0

留给真正的大突破，比如不再需要付费开发者账号，或者应用自带的原生库不再解释执行。

## 不做

- 随 App 打包 Google Play 服务（#5-6）：闭源，不能分发
- Android 9 的选项（#5-11）：另一整套系统，慢也不在 Android 版本
- Magisk、Kitsune、Zygisk、Xposed：往 zygote 里注入代码，和我们的启动方式冲突
- 软件 MMU（去掉付费账号）：量过不可行，以后再说
- 把某个应用的库预先翻译后随 App 打包；无障碍
