# 试过的应用

从 APK 安装的应用，每个打开一次：先是开源应用，1.5 起也有从各家官网下载的常用应用。这里说的是每个应用走到了哪一步，不是它好不好用：
“能打开”指它的第一个界面画出来了，除非备注里另有说明，再往后的都没有试。

2026 年 11 月 23 日在 iPhone 17 Pro（iOS 27.0.1）上用后来成为 1.3 版的构建测试。APK 来自 F-Droid 或各项目自己的发布页。
每个应用通过 App 的测试通道按包名打开，15 到 18 秒后保存一帧画面。

| 应用 | 版本 | 自带的原生库 | 结果 |
| --- | --- | --- | --- |
| 2048（Privacy Friendly） | 1.4.2 | 无 | 能打开 |
| Aegis Authenticator | F-Droid，构建 82 | 2 个 | 能打开，停在介绍页 |
| Amaze File Manager | F-Droid，构建 126 | 1 个 | 能打开，随后询问权限 |
| AntennaPod | 3.12.3 | 1 个 | 能打开。1.3 之前一启动就退出，见下文 |
| Fossify Calendar | 1.12.2 | 1 个 | 能打开 |
| Fossify Clock | 1.6.0 | 1 个 | 能打开 |
| Fossify Contacts | F-Droid，构建 13 | 1 个 | 能打开，随后询问权限 |
| Fossify File Manager | F-Droid，构建 16 | 1 个 | 能打开 |
| Fossify Gallery | 1.14.2 | 12 个 | 能打开，随后询问权限 |
| Fossify Notes | 1.7.0 | 1 个 | 能打开 |
| F-Droid | 2.0.3 | 1 个 | 能打开，随后询问权限 |
| K-9 Mail | 25.0 | 4 个 | 能打开，停在欢迎页 |
| KeePassDX | 4.5.8 | 2 个 | 能打开 |
| Loop Habit Tracker | F-Droid，构建 20302 | 无 | 能打开，停在介绍页 |
| Markor | 2026.10.0 | 无 | 能打开，停在介绍页 |
| NewPipe | 0.29.3 | 2 个 | 能打开 |
| ScriptWidget | 1.1.1 | 1 个 | 能打开。第一个小组件预览约两分钟才出来 |
| Tasks | 15.14 | 4 个 | 能打开 |
| Termux | F-Droid，构建 1002 | 2 个 | 能打开，随后询问权限 |
| Wikipedia | r/50616 | 4 个 | 能打开；搜索和首页信息流可用。文章需要 WebView，1.6 起有了；应用本身没有重试 |
| DuckDuckGo | 5.301.1 | 10 个 | 打不开过：它是浏览器，需要 WebView，1.6 起有了；没有重试。1.3 里它会让 Android 停住，见下文 |
| Feeder | F-Droid，构建 4191 | 2 个 | 能打开。1.3 里它会让 Android 停住，见下文 |

22 个里 21 个能打开。

## 为 1.5 又试的 32 个

2026 年 11 月 23 日在 iPad Air 11 英寸（M3，iPadOS 27.0.1）上试，用的是 1.5 开发过程中的构建，每个应用在它引出的修复之后又试了一次；Telegram 和 Element 最后一次是在 iPhone 14（iOS 17.3.1）上。除 Telegram、Signal、今日头条和抖音用各家官网提供的安装包外，都来自 F-Droid。打开后 16 到 30 秒存一帧，慢的等到两分钟。

| 应用 | 版本 | 自带原生库 | 结果 |
| --- | --- | --- | --- |
| App Manager | 4.1.1 | 4 个 | 能打开，询问权限 |
| Aurora Store | 4.8.4 | 1 个 | 能打开，停在欢迎页 |
| Binary Eye | 1.75.7 | 3 个 | 能打开，停在欢迎页 |
| Breezy Weather | 6.2.2 | 3 个 | 能打开 |
| Fennec（F-Droid 的 Firefox） | 160.0.0 | 15 个 | 能打开，一分半到两分钟后到欢迎页 |
| FairEmail | 1.2341 | 1 个 | 能打开，停在欢迎页 |
| Fossify Calculator | 1.4.0 | 1 个 | 能打开 |
| Fossify Camera | 1.5.0 | 3 个 | 能打开，询问权限 |
| Fossify Messages | 1.10.2 | 1 个 | 能打开，询问是否设为短信应用 |
| Fossify Music Player | 1.8.5 | 1 个 | 能打开，询问权限 |
| Fossify Phone | 1.11.1 | 1 个 | 能打开，询问是否设为电话应用 |
| Fossify Voice Recorder | 1.7.4 | 2 个 | 能打开并录音；在 iOS 里允许麦克风后录到的是设备的麦克风 |
| Joplin | 3.7.14 | 26 个 | 能打开；两分钟内显示笔记列表 |
| LibreTube | 32.1 | 无 | 能打开，显示热门视频 |
| Luanti | 5.18.0 | 2 个 | 能打开，横屏 |
| Mastodon | 2.13.7 | 无 | 能打开，停在欢迎页 |
| Mindustry | 8（构建 160.8） | 2 个 | 能打开，停在加载画面，横屏 |
| MuPDF viewer | 1.28.5a | 1 个 | 能打开，要求选文件 |
| Nextcloud | 35.1.0 | 6 个 | 能打开，停在欢迎页 |
| Organic Maps | 2026.11.11 | 1 个 | 能打开，询问权限 |
| OsmAnd~ | 5.4.11 | 7 个 | 能打开，停在欢迎页 |
| PPSSPP | 1.19.3 | 5 个 | 能打开，横屏 |
| Shattered Pixel Dungeon | 4.0.2 | 2 个 | 能打开，停在标题画面 |
| Signal | 8.36.3 | 12 个 | 能打开，停在欢迎页。1.5 之前它生成不了密钥，见下文 |
| Telegram | 13.3.1 | 4 个 | 能打开，停在欢迎页。1.5 之前它一启动就停止，见下文 |
| Telegram X | 0.28.11 | 8 个 | 能打开，停在欢迎页 |
| Element Classic | 1.6.62 | 21 个 | 能打开，约一分钟后到第一个画面。1.5 之前它一启动就停止，见下文 |
| 今日头条 | 19.2.0 | 161 个 | 能打开，频道栏和底栏都在。1.6 有了 WebView 后信息流能加载，两分钟内出现新闻标题 |
| 抖音 | 32.2.6 | 204 个 | 能打开，栏目和底栏都在。1.6 有了 WebView 后两分钟内视频仍加载不出来：它自带的原生库是解释执行的。要占约 5 GB 内存，只能在 iPad 上 |
| AnkiDroid | 2.25.3 | 2 个 | 能打开到引导页（1.6，有了 WebView；之前一分钟内没有画面） |
| Jellyfin | 2.7.3 | 2 个 | 能打开到 Connect to Server（1.6，有了 WebView 和 SO_BROADCAST；之前只提示需要 WebView） |
| KDE Connect | 1.37.3 | 1 个 | 一启动就停止：它要用 Wi-Fi 服务，这里的 Android 没有 |

32 个里 29 个能打开；54 个合计 50 个；1.6 后 51 个（AnkiDroid）。

没有试：TikTok 和 YouTube。两家都不提供安装包下载，YouTube 还依赖 Google Play 服务。

## 为 1.13 又试的 50 个

2026 年 11 月 25 日和 26 日在 iPhone 14（iOS 17.3.1）上试，用的是 1.13 开发过程中的构建，每个应用在它引出的修复之后又试了一次。安装包来自各家官网、项目自己在 GitHub 的发布页或 F-Droid，没有用镜像站。应用装上后按包名打开，等 25 到 90 秒存一帧，再卸载；一次启动 Android 连着试六个或四个。

| 应用 | 来源 | 结果 |
| --- | --- | --- |
| 微信 | weixin.qq.com | 约 90 秒后打开到启动画面：它用自己的加载器映射库，其中一部分是解释执行的 |
| QQ | im.qq.com | 打开到启动画面 |
| 支付宝 | alipay.com | 打开到登录页 |
| 哔哩哔哩 | app.bilibili.com | 能安装（1.13 之前被拒绝，见下面）；90 秒内没有画面 |
| 知乎 | zhihu.com | 能安装（1.13 之前被拒绝）；一启动就停，停在它自己的原生代码里 |
| 网易云音乐 | music.163.com | 一启动就停：它的安全库在没设成可执行的内存里运行代码 |
| 酷狗音乐 | kugou.com | 打开到隐私提示 |
| 百度地图 | map.baidu.com | 打开到用户协议 |
| 百度网盘 | pan.baidu.com | 不支持：提供的安装包只有 32 位，iPhone 的处理器不运行 32 位代码 |
| 夸克 | quark.cn | 60 秒内没有画面 |
| UC 浏览器 | ucweb.com | 60 秒内没有画面 |
| 钉钉 | dingtalk.com | 打开到隐私政策 |
| 飞书 | feishu.cn | 18 秒后打开到登录页 |
| 腾讯会议 | meeting.tencent.com | 能打开 |
| WPS Office | wps.com | 一启动就停 |
| 腾讯视频 | v.qq.com | 打开到启动画面（1.13，见下面） |
| 爱奇艺 | iqiyi.com | 60 秒内没有画面：它加载的一个库让整个应用变成解释执行 |
| 优酷 | youku.com | 打开到欢迎页 |
| 铁路12306 | 12306.cn | 一启动就停：它被加固（梆梆）包着，加固在这里起不来 |
| WhatsApp | whatsapp.com | 打开到服务条款 |
| Zoom | zoom.us | 两分钟里窗口一直是空白。1.13 之前它在网络接口的请求上一直等 |
| Firefox | Mozilla | 能打开（1.13，见下面）；它的辅助进程会停，浏览器本身继续 |
| Firefox Focus | Mozilla | 能打开（1.13） |
| Brave | GitHub | 打开到欢迎页（nightly 构建） |
| Opera | opera.com | 60 秒内没有画面：解释执行 |
| Tor Browser | torproject.org | 能打开 |
| Bitwarden | GitHub | 能打开（1.13） |
| Proton VPN | GitHub | 打开到登录页（1.13） |
| Mullvad VPN | mullvad.net | 能打开 |
| Tailscale | tailscale.com | 能打开；请求 VPN 权限时停住，这个 Android 没有那个对话框 |
| WireGuard | wireguard.com | 能打开（1.13） |
| v2rayNG | GitHub | 能打开，询问权限 |
| Session | getsession.org | 一启动就停：它要 Wi-Fi 服务，这个 Android 没有 |
| Element X | GitHub | 能打开（1.13） |
| Wire | wire.com | 能打开 |
| Briar | briarproject.org | 打开到设置向导 |
| Jitsi Meet | F-Droid | 能打开 |
| Thunderbird | GitHub | 打开到欢迎页 |
| Tusky | GitHub | 打开到登录页 |
| Tuta Mail | F-Droid | 能打开 |
| VLC | videolan.org | 打开到介绍页（1.13） |
| Kodi | kodi.tv | 能打开 |
| RetroArch | libretro.com | 能打开 |
| Moonlight | moonlight-stream.org | 能打开 |
| Kiwix | kiwix.org | 能打开 |
| Syncthing-Fork | GitHub | 打开到介绍页 |
| Obtainium | GitHub | 能打开，询问权限 |
| Mihon | GitHub | 一启动就停，停在它自己的原生代码里 |
| LocalSend | GitHub | 60 秒内没有画面：它的 Flutter 引擎是解释执行的 |
| AppFlowy | GitHub | 46 秒后能打开（Flutter） |

50 个里 36 个能打开，另有一个（百度网盘）在 iPhone 上根本不能运行；试过的 104 个应用合计 87 个能打开。

## 1.13 改了什么

一次启动里接连打开很多应用，比每次启动只开一个应用发现的问题多。

**开过几个应用后，Android 不再停住，Nestvane 也不再被结束。**

- Nestvane 用的内存超过 iOS 允许的（iPhone 14 上约 4.3 GB）时，iOS 不打招呼就结束它。现在剩得不多时，先结束 Android 排得最低的应用，和 Android 的低内存查杀一样：先是缓存的应用，再是上一个应用，然后是服务；屏幕上的从不结束。
- 宿主内存分配失败会结束整个 Android。现在系统调用拿不到内存时先腾地方再试一次，给不了内存的进程单独结束。
- 一个进程出了内存错误而它没有处理函数时，整个 Android 都结束。现在和 Linux 一样只结束这个进程。映射了但暂时拿不到宿主内存的地方出错时，先腾地方再跑。
- 第一个 WebView 渲染进程结束后，任何应用都启动不了：WebView 的 zygote 用一个没实现的调用报告这件事，然后一直等，后面每个应用的启动都排在它后面。
- 400 MB 的安装包（QQ、飞书）在为安装复制时内存不够，Nestvane 被结束。现在安装包直接从文件交给 Android，不复制。

**用 JNA 或 Rust 的应用原生运行。** Bitwarden、Element X、Firefox Focus 和 Proton VPN 用 libffi 生成小段跳板代码，放在它们自己选地址、映射成可执行的文件里。这让整个应用变成解释执行，到第一个画面要几分钟。现在这类代码放在只有它被解释执行的地方。应用用自己的加载器加载的库（微信）也一样。

**缺的系统调用和请求**，每一个都会让一个进程一直等下去：`pselect6`（VLC）；向在另一个核上运行的线程发 `tgkill`，以及 `madvise` 的几种提示（Go 程序：WireGuard、Tailscale）；TCP 和 UDP 套接字上的边沿触发 `epoll`（Telegram、Tailscale）；`ptrace`（现在像对无关进程那样拒绝）；带新栈、不发信号的 `clone`，以及带 `__WALL` 的 `wait4`（Firefox、夸克、UC）；套接字上的 `SIOCGIFCONF`、回环接口的地址和 `FIONREAD`（Zoom、腾讯视频、AppFlowy）。

**共享库。** Android 声明的七个库镜像里没有（USB 配件库、测试库等），需要其中任何一个的应用 Android 都拒绝安装：哔哩哔哩和知乎。

**图形。** 应用绘制时手机的图形被重置，Nestvane 就结束（Firefox）。现在那次绘制失败，Android 继续。

## 1.5 里改了什么

**密钥。** 应用在 Android 的密钥库里生成不了密钥：密钥库去问这个应用有没有权限，对所有应用的回答都是没有，因为那张回答表当初只为系统自己的进程取过。现在应用得到的是 Android 的策略给每个应用的权限。Signal 和 Element 一启动就停在这里，凡是把设置加密保存的应用都会这样。

**命名管道。** `mkfifo` 被拒绝。Element 的数据库库要在应用自己的目录里建一个，还要监视它的变化，而管道原来也不能这样监视。

**指令。** Telegram 停在一条解释器没有的指令上。把这些应用的原生库全部扫了一遍（`tools/scan-interpreter-coverage.py`），补上了其中普通代码用到的：约 70 条向量和浮点指令、半精度的互相转换，以及程序自己生成代码后要用的缓存指令。每条向量指令都用相同的操作数在 Mac 的处理器和解释器上各跑一遍比较结果（557 例）。

**大应用。** 抖音的安装包 271 MB，会起几百个线程。它碰到的几个上限都放大了：安装包大小（到 1 GiB）、Android 的线程总数（到 4096）、应用的 Java 代码能用的内存（到 256 MB），以及一个系统服务线程前面能排多少个请求。

**域名。** 解析结果经过几层别名时，应答被截断，应用被告知主机不存在。现在应答按协议允许的方式压缩。

**横屏。** 要求横屏的应用显示方向正确；转动设备，Android 跟着转。

## 1.3 之后改了什么

**让 Android 停住的应用。** DuckDuckGo 和 Feeder 各带一个用 Go 写的库，在 1.3 里打开其中任何一个都会让 Android 停住：画面不再更新。
原因是运行应用自带库的解释器遇到了一条它没有的指令，而这会结束整个运行。改了两处。
解释器没有的指令现在只结束遇到它的那个应用，发的是没有这条指令的处理器会发的那个信号。
指令本身也补上了：Go 的库用到的那些，以及这张表里的应用的库中出现的另外约四十条向量指令。
每一条都在 Mac 的处理器上和解释器里用同样的操作数各跑一遍，再比较结果（`tools/validate-vector-instructions.cpp`）。
在 iPhone 14（iOS 17.3.1）上试过：Feeder 能打开并保持运行；DuckDuckGo 因为没有 WebView 自己退出，Android 继续运行。

## 1.3 改了什么

**按 4 KiB 页排布的库。** 这里的 Android 用的是 iPhone 的 16 KiB 页，它的链接器拒绝加载按 4 KiB 页排布的库，而很多应用带的还是这种库。
AntennaPod 就是一个：一启动就退出，报 `dlopen failed: … program alignment (4096) cannot be smaller than system page size`。
Android 有一种兼容的加载方式，现在对所有应用打开（`bionic.linker.16kb.app_compat.enabled`）；
这种方式需要让内存获得可执行权限，内核层不再拒绝。

## 还有什么挡着

- **WebView 在 1.6 加上了。** AOSP 的 Android System WebView 128，和系统其他部分一样预先翻译。JavaScript 不用 V8 的 JIT。WebView 里打开一篇 Wikipedia 文章 4 到 8 秒。
- **Android 数据里能放的文件数有上限**，装到三十来个应用时到了上限，再装的应用就启动不了。原来是 4096 个文件、2048 个目录，现在是它的十六倍。
- **原生库是解释执行的**，约每秒四千万条指令。在自己的库里干重活的应用会很慢。
- **没有 Google Play 服务。** 需要它的应用没有试。
- **没有 Wi-Fi 服务。** 这里的 Android 通过有线接口上网。应用去取 Wi-Fi 服务又没料到取不到，就会停止（KDE Connect、Session）。计划在 1.14 加上。
- **没有 VPN 对话框。** VPN 应用请求建立 VPN 的权限时会停住（Tailscale）。
- **加固**把应用的代码包起来的（12306 的梆梆），在这里起不来。
- **32 位应用**不能运行：iPhone 的处理器没有 32 位模式（百度网盘的安装包）。
- **大应用启动慢。** 它们的 Java 代码在这里没有预先编译。Firefox 到第一个画面最长要两分钟。

前两张表里没有 VLC，因为当时手头的 APK 只有 x86 版；1.13 试了它的 ARM 版。
