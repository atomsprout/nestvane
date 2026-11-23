# 试过的应用

从 APK 安装的开源应用，每个打开一次。这里说的是每个应用走到了哪一步，不是它好不好用：
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
| Wikipedia | r/50616 | 4 个 | 能打开；搜索和首页信息流可用。文章打不开：需要 WebView |
| DuckDuckGo | 5.301.1 | 10 个 | **让 Android 停住。** 画面不再更新，直到重启 Android |
| Feeder | F-Droid，构建 4191 | 2 个 | **让 Android 停住**，在它的第一个界面画出来之后一瞬间 |

22 个里 20 个能打开。让 Android 停住的两个都带着一个用 Go 写的库（`libwg-go.so`、`libgojni.so`），列表里别的应用都没有。
关于原因目前只知道这一点。在找到原因之前，带 Go 库的应用都应当预期会这样。
用 More 菜单里的 *Stop Android* 再 *Start Android* 可以恢复；引起问题的应用仍然装着。

## 1.3 改了什么

**按 4 KiB 页排布的库。** 这里的 Android 用的是 iPhone 的 16 KiB 页，它的链接器拒绝加载按 4 KiB 页排布的库，而很多应用带的还是这种库。
AntennaPod 就是一个：一启动就退出，报 `dlopen failed: … program alignment (4096) cannot be smaller than system page size`。
Android 有一种兼容的加载方式，现在对所有应用打开（`bionic.linker.16kb.app_compat.enabled`）；
这种方式需要让内存获得可执行权限，内核层不再拒绝。

## 还有什么挡着

- **没有 WebView。** 在自己界面里显示网页的应用会提示 Android System WebView 正在更新。
- **原生库是解释执行的**，约每秒四千万条指令。在自己的库里干重活的应用会很慢。
- **Go 库会让 Android 停住**，如上。
- **没有 Google Play 服务。** 需要它的应用没有试。

没有列入的：VLC，手头的 APK 只有 x86 版，不能在手机上运行。
