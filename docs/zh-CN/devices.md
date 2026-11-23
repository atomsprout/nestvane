# 设备与宿主集成

客体是为 Android 模拟器的虚拟硬件构建的 Android 系统（产品为 `sdk_phone16k_arm64`，它的虚拟主板叫 “ranchu”）。
这样选的好处是，Android 里已经带着一批容易模仿的硬件的驱动：通向宿主渲染器的管道、简单的网络接口、上报绝对坐标的触摸屏。
这篇文档说明其中每一样是怎样接到手机上的。

## 图形

**一帧画面的路径。**

1. Android 应用用 OpenGL ES 或 Android 自己的渲染器绘制，和在任何设备上一样。
2. 客体里模拟器的图形库并不真正绘制。它们把每个图形调用编码后写进管道设备 `/dev/goldfish_pipe_dprctd`；缓冲区通过 `/dev/goldfish_address_space` 共享。
3. 内核层把管道里的字节交给 **gfxstream**。它是 Android 模拟器在桌面上使用的渲染器，这里为 iOS 构建，作为 `NestvaneGfxstream.framework` 装进 App（[`ios/GfxstreamSession.hpp`](../../ios/GfxstreamSession.hpp)、[`runtime/GfxstreamPipe.hpp`](../../runtime/GfxstreamPipe.hpp)）。
4. gfxstream 解码这些调用，交给 **ANGLE**。ANGLE 是在其他图形接口之上实现 OpenGL ES 的库，这里只带 **Metal** 后端构建（[`tools/angle-iphoneos.gn`](../../tools/angle-iphoneos.gn)）。
5. Android 的合成器 SurfaceFlinger 把各个窗口合成为屏幕的一帧，走的也是同一条管道。
6. App 从 gfxstream 把合成好的帧以 RGBA 像素读回（`gfxstream_guest_display_read`），包成一张图像，显示在视图里（[`ios/App.mm`](../../ios/App.mm) 的 `StartDisplayReader`）。

gfxstream 配合一组补丁使用，补丁在 [`tools/patches/`](../../tools/patches)：iOS 的窗口实现、内核层与之通信的管道接口、主显示的读回，以及在没有自己窗口的情况下运行所需的几处修正。
gfxstream 和 ANGLE 的源码本身不在这个仓库里，需要什么见 [building.md](building.md)。

**帧率。**
有两个设置必须一致：客体多久合成一次，App 多久去取一次新帧。
两者都来自 `NESTVANE_DISPLAY_HZ`，App 把它设为 60；屏幕能显示 120 并且在 App 的 Settings 里打开了“120 Hz”时设为 120；屏幕的最高刷新率更低时取屏幕的。客体通过启动配置拿到这个值，作为自己的 vsync 频率。
早先 App 每 33 毫秒取一次帧，画面被限制在每秒 30 帧。

在 Settings 里滑动列表时的实测：iPhone 15 Pro 在 60 Hz 下最高约每秒 55 帧。
120 Hz 下，手指在同一个列表上持续移动时，Android 每秒合成 120 帧，App 显示出其中的 86 到 120 帧（iPhone 15 Pro）。

此前 120 Hz 不起作用有两个原因，都和速度无关。
框架默认把绘制限制在每秒 60 帧，不管显示模式是多少；[`fixtures/overlay-framework`](../../fixtures/overlay-framework) 里的覆盖包把这个默认值提到 120。
而针对框架自身资源的覆盖包此前从未生效：zygote 最先为它们运行 `/system/bin/idmap2`，客体根目录里没有这个程序，每次开机 zygote 都记下 `'idmap2 create-multiple' failed`。
早先“120 Hz 下约 50 帧”的数字，其实是 Android 在按 60 帧绘制。

**着色器编译。**
SurfaceFlinger 平常在开机时预编译一批着色器。在这里这一步要 3.9 秒，所以关掉了（`service.sf.prime_shader_cache=0`），着色器在第一次用到时再编译。

## 存储

Android 把所有会变的状态写在 `/data` 下。它由三种机制承载。

**快照。**
[`runtime/ProbeData.cpp`](../../runtime/ProbeData.cpp) 把 `/data` 保存在一个 SQLite 数据库 `guest-data.sqlite3` 里，位于 App 的 Application Support 目录（`Nestvane/` 下）。
Android 运行期间，文件在内核层的内存里。
客体调用 `fsync` 或 `sync` 时，内核层在一个事务里把有变化的条目写进数据库；两次提交至少间隔两秒，因为 Android 同步得很频繁，而每次提交都有成本。
数据库以完全同步写入的方式打开，所以已提交的快照在 App 被杀之后仍然完好。
每个条目保存的内容：路径、类型、内容、属主、权限、时间戳、扩展属性和访问控制列表。

**大文件。**
应用装多了以后，把每个文件都放在内存和数据库里就行不通了：装上十四个应用后，开机时间从 6.5 秒升到 10.8 秒，内存从 2.05 GB 升到 2.78 GB。
所以 1 MiB 及以上的普通文件，内容各存进一个宿主文件 `guest-data.sqlite3.files/<inode>`，有人读它时才映射进内存（[`runtime/Runtime.hpp`](../../runtime/Runtime.hpp) 里的 `SharedBuffer`）。
数据库里只为它留一条很小的记录。文件长过 1 MiB 的那一刻就从数据库里搬出去。

**崩溃语义。**
两种机制的时机不同，由此得出下面几条。

- 数据库是客体上一次同步时的快照。文件的名字、长度和存在与否，以快照为准。
- 大文件的内容是最后写进去的内容。App 被杀之后，大文件可能比快照新，不会比快照旧。
- 被删除的大文件，它的宿主文件要等到某次同步之后、再没有记录提到它时才删除。没有任何记录提到的宿主文件在挂载时清掉。
- 宿主文件不做 `msync` 或 `F_FULLFSYNC`。断电之后，大文件的内容可能比快照旧。这是已知的缺口。

上限：单个文件 256 MiB，总量 2 GiB。

**共享文件夹。**
Android 的共享存储，也就是应用看到的 `/storage/emulated/0`，是客体目录 `/data/media/0`。
内核层把这个目录对应到一个真实的文件夹：App 容器里的 `Documents/Android`。
那里的每个文件和目录都是宿主上同名的一个文件或目录，长度完全一致；客体里的新建、删除和改名直接作用在宿主文件夹上。数据库里只保存每一项的属主、权限和标签。
App 打开了文件共享，所以 iOS 的“文件”App 在“我的 iPhone → Nestvane → Android”下显示这个文件夹。

挂载时以宿主文件夹为准：从外面放进来的文件会出现在 Android 里，从外面删掉的文件随之消失。
Android 运行期间从 iOS 一侧做的改动，要等下一次启动 Android 才看得到。

**简化的 FUSE。**
在真实设备上，应用通过媒体库提供的 FUSE 文件系统访问共享存储，媒体库对每个应用、每个文件检查权限。
在这里，`fuse` 挂载只是 `/data/media` 的一个普通视图，媒体库的守护线程只收到打开会话的那一个请求（见 [kernel-layer.md](kernel-layer.md)）。
对用户来说，共享存储能用；对安全来说，共享存储按应用的访问控制没有执行。

## 网络

**客体看到的样子。**
一块以太网接口 `eth0`，配置方式与 Android 模拟器的用户态网络相同：客体自己是 `10.0.2.15/24`，网关是 `10.0.2.2`，域名服务器是 `10.0.2.3`。
内核层应答 netlink 查询、接口 `ioctl` 和 `/sys/class/net/eth0` 下的文件，让这块接口在 Android 看来是真的（[`runtime/GuestRoute.hpp`](../../runtime/GuestRoute.hpp)）。
在 Android 一侧，客体根目录声明了以太网特性，以太网服务才会启动；资源覆盖包（[`fixtures/overlay-connectivity`](../../fixtures/overlay-connectivity)）给 `eth0` 固定的地址配置。
之后 Android 的连接服务像对待任何网络那样把它连上并完成验证。

**宿主套接字。**
这里不转发数据包。客体的每个 TCP 或 UDP 套接字，只要目的地不是客体自己，就与一个宿主套接字配对（[`runtime/HostSocket.hpp`](../../runtime/HostSocket.hpp)）。
`connect`、`send`、`recv`、`poll` 和 `epoll` 的就绪状态、`SO_ERROR`、`shutdown` 都转到宿主套接字上，宿主的错误号换算成 Linux 的。
宿主套接字都是非阻塞的；等在上面的客体线程隔一小段时间再被检查一次，因为宿主套接字自己没法唤醒客体的调度器。
只支持 IPv4。

手机从 Wi-Fi 换到蜂窝或换回来时，App 会结束客体所有打开的 TCP 连接。
在旧网络上建立的连接不会搬到新网络上，也不会报错：放着不管它只是不再有数据，复用连接的应用每个请求都要等到超时。
真手机上的 Android 在网络消失时会自己结束这些连接；客体看到的是一个永远不变的假网络，所以由宿主来做，客体读到流结束后重新连接。

在自身没有 IPv4 的网络上（一些蜂窝网络是这样），如果系统报告有网关，客体的 IPv4 目的地址会改写成网关应答的 IPv6 地址（NAT64）。
这一部分没有在这样的网络上试过。

**DNS。**
地址 `10.0.2.3` 是 App 里的一个域名服务器（`runtime/DnsProxy.hpp`）。它只在宿主的回环地址上监听，每个查询都交给系统的解析服务（`DNSServiceQueryRecord`）回答，所以域名的解析结果和手机上其他 App 一样，也跟随系统的 VPN 和 DNS 设置。只回答 UDP 查询，`.local` 下的名字直接拒绝。
App 从不直接连网络自己的域名服务器。那通常是路由器，App 一往它发包，iOS 就会询问是否允许访问本地网络，以前这发生在第一次开机时。客体发出的组播和广播包、发往客体自己那个虚构子网的包，也都不带出去。某个 Android 应用真的去连本地网络里的设备时，这个询问仍会在那一刻出现。

**iOS 的联网许可。**
在需要逐个 App 放行联网的手机上，只有 App 使用系统自己的网络接口时 iOS 才会弹出询问；普通套接字不会触发询问，只会被拒绝。
所以 App 启动时先通过 `NSURLSession` 向 Apple 的连通性检查地址发一个 `HEAD` 请求，并把结果写进 `network-check.txt`。询问由此弹出。
App 的 `Info.plist` 里也写了本地网络的用途说明。

已验证：Wikipedia 能载入首页。没有测过：下载大文件和长连接。

## 声音

内核层提供一块声卡，就是 Android 音频服务用来播放的那种：一个没有任何控件的控制设备 `/dev/snd/controlC0`，和一个播放设备 `/dev/snd/pcmC0D0p`。声卡没有可打开的控制设备时，音频服务不会使用它的播放设备。
播放设备对流参数的回答是它最终定下的值；它接收采样，可以被轮询是否有空位，并报告播放位置。
它的缓冲区和它告诉客体的一样长（四个周期），一次写入有多少空位就收多少。这一点很要紧：音频服务按自己的时间往声卡里填，只要声卡有空位而它手里没有数据，就补一段静音。声卡如果吞下的比它声称的多，或者让一次长写入一直等，每秒就会被补五次静音，真实的声音则被丢掉来腾位置。

客体写入的数据进入一个环形缓冲区（[`runtime/GuestAudio.hpp`](../../runtime/GuestAudio.hpp)）。
在宿主一侧，App 按客体的采样格式运行一个音频输出单元。它等缓冲区里有了约 43 毫秒的采样才开始取，并一直留着这么多，客体偶尔写晚了也听不出来。
Android 最后一次播放之后三秒，App 把手机的音频输出交还，别的 App 的声音可以继续；Android 再播放时重新取得。
如果没有人把采样取走，声卡就按时钟的节奏播放，声音无处可去。

镜像里 Android 的媒体音量在最高一档，响度由手机自己的音量键决定。

**现状。** 在 iPhone 15 Pro 上用带 AAC 声音的 1280×720 H.264 视频测量：Android 每秒往声卡写 48000 帧，手机的输出以同样的速率取走，最响的采样与文件里的一致。一条流开头几秒之后，音频服务不再补静音。
这是对采样的测量；声音没有用耳朵评判过。
没有声音输入。

### 视频

视频由 Android 的软件编解码器（`media.swcodec`）解码到图形分配器的缓冲区里，这些缓冲区是宿主 GPU 上的 YUV 缓冲区。
为此宿主的渲染器要有两样东西：客体在解码器写入之前会把这种缓冲区读回来，宿主做不到时就中止进程（`YuvCache` 特性）；客体对 YUV 缓冲区的写入要和其他缓冲区走同一条路到达宿主（[`tools/patches/gfxstream-yuv-buffers.patch`](../../tools/patches/gfxstream-yuv-buffers.patch)）。

## 电量、时区与触摸

**电量。**
App 通过 UIKit 监听手机的电量和充电状态，写进 [`runtime/GuestBattery.hpp`](../../runtime/GuestBattery.hpp)。
内核层把它们呈现为 `/sys/class/power_supply/battery` 和 `ac`，Android 的健康服务大约每分钟读一次，所以 Android 显示的数字最多会滞后这么久。

**时间。**
客体有自己的时钟，从宿主当前的时间起步。
App 在启动配置里传入手机的时区名，`init` 据此设置 `persist.sys.timezone`。

**触摸。**
[`runtime/GuestTouch.hpp`](../../runtime/GuestTouch.hpp) 是一个 Linux 输入事件的队列。
App 的视图把触摸换算成 0 到 32767 刻度上的绝对坐标并放进队列；内核层通过 `/dev/input/event0` 把它们交出去，Android 的输入系统像读任何触摸屏一样读它。
App 底栏的返回和主页按钮，作为按键事件经同一个设备送入。

**短信。**
客体里没有电话进程，也没有射频服务，短信不可能像在手机上那样到达。
App 的 *Send Text Message…* 把发件人和文字排进队列（[`runtime/GuestApkInbox.hpp`](../../runtime/GuestApkInbox.hpp)）；开机后启动的一个客体服务（[`fixtures/android17-text.sh`](../../fixtures/android17-text.sh)）从 `/dev/nestvane-text` 读取，没有消息时这个设备会等待；它用 Android 的 `content` 工具往短信数据库写一行，再发一条通知。
为此镜像带上了短信数据库（TelephonyProvider）和 Messaging 应用；数据库运行在名为 `com.android.phone` 的进程里，那不是电话应用。
另一个服务为同一条短信发出 `SMS_RECEIVED` 广播（[`fixtures/android17-sms.sh`](../../fixtures/android17-sms.sh)），等短信的应用因此能收到；它以 root 运行，因为只有系统能发这条广播。
短信在广播里以网络本该送来的 PDU 形式携带，由 [`tools/sms-broadcast/SmsBroadcast.java`](../../tools/sms-broadcast/SmsBroadcast.java) 生成：UCS-2 编码，超过 70 个字符时按每段 67 个字符拆开。
写成号码的发件人到达时只剩数字，不带空格和连字符。

**安装应用。**
用 App 打开的 `.apk` 文件（来自“文件”、隔空投送或分享菜单），或者用 App 菜单里、主屏幕图标上的 Install APK… 选中的文件，会被复制到一个收件箱（[`runtime/GuestApkInbox.hpp`](../../runtime/GuestApkInbox.hpp)）。
Android 开机完成后，一个小的客体程序（[`fixtures/android17-apk-install.c`](../../fixtures/android17-apk-install.c)）用原版的 `cmd package install` 命令把它交给 Android 自己的包管理器。
收件箱接受不超过 256 MiB 的文件。

**设备标识。**
型号、品牌和制造商属性设成了 Nestvane，Settings 里显示的版本号是构建 ID。

## 客体没有的硬件

[`tools/prepare-boot-image.py`](../../tools/prepare-boot-image.py) 去掉了不存在的硬件的声明，免得 Android 去等永远不会启动的服务：Wi-Fi 和指纹的硬件服务被拿掉，蓝牙、定位、Wi-Fi、超宽带和指纹等特性被标为不可用。
运动传感器服务存在，但传感器列表是空的。
把手机后置摄像头共享给 Android 的代码是有的（[`runtime/GuestCamera.hpp`](../../runtime/GuestCamera.hpp)、[`ios/IOSCamera.mm`](../../ios/IOSCamera.mm)）；它在当前构建里的状态，在这组文档所依据的记录中没有验证。
