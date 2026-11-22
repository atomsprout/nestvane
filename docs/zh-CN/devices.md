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
两者都来自 `NESTVANE_DISPLAY_HZ`，App 把它设为 60；屏幕的最高刷新率更低时取屏幕的。客体通过启动配置拿到这个值，作为自己的 vsync 频率。
早先 App 每 33 毫秒取一次帧，画面被限制在每秒 30 帧。

在 Settings 里滑动列表时的实测：iPhone 15 Pro 在 60 Hz 下最高约每秒 55 帧。
在 iPhone 17 Pro 上要求 120 Hz，得到的帧率和 60 Hz 一样，约每秒 50 帧，所以瓶颈不在屏幕。
到底卡在哪一段（应用绘制、SurfaceFlinger、渲染器，还是把每一帧读回处理器再做成图像这条显示路径），还没有分析。

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

**DNS。**
地址 `10.0.2.3` 代表宿主正在使用的域名服务器。发给它的查询被发往宿主的服务器，应答在客体看来来自 `10.0.2.3`。
宿主的域名服务器通常是本地网络上的路由器，而 iOS 要等用户允许访问本地网络之后才让 App 连它。在那之前，发给它会以“没有到主机的路由”失败，于是改用一串公共服务器里的下一个。

**iOS 的联网许可。**
在需要逐个 App 放行联网的手机上，只有 App 使用系统自己的网络接口时 iOS 才会弹出询问；普通套接字不会触发询问，只会被拒绝。
所以 App 启动时先通过 `NSURLSession` 向 Apple 的连通性检查地址发一个 `HEAD` 请求，并把结果写进 `network-check.txt`。询问由此弹出。
App 的 `Info.plist` 里也写了本地网络的用途说明。

已验证：Wikipedia 能载入首页。没有测过：下载大文件和长连接。

## 声音

内核层提供一个播放设备 `/dev/snd/pcmC0D0p`，就是 Android 音频服务用来播放的那种。它实现了设置流参数、写入采样和报告播放位置。
客体写入的内容进入一个五分之一秒长的环形缓冲区（[`runtime/GuestAudio.hpp`](../../runtime/GuestAudio.hpp)）。
在宿主一侧，App 按客体的采样格式运行一个音频输出单元，从缓冲区取采样，没有采样时播放静音。
如果没有人把采样取走，设备就让客体按时钟的节奏播放，声音无处可去。

**现状：还没有出过声。**
测试中，Android 的音频服务打开了设备、设置了参数，之后什么都没有写入。
混音器控制设备（`controlC0`）没有实现。
软件编解码器已经装上并能启动，但播放不前进，有没有声音设备都一样，原因不明。见 [status.md](status.md)。

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
