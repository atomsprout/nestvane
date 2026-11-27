# 使用 Nestvane

这篇讲装好 Nestvane 之后怎么用（怎么装见[安装](installing.md)）：装应用、传文件、菜单、root 和 KernelSU Manager、备份，以及 Android 像是卡住时怎么办。哪些能用、哪些不能用，以及测过的数字，见[现状](status.md)。

## 屏幕和底栏

打开 Nestvane 就会启动 Android；装好后第一次启动比以后慢。Android 画面下面是 Nestvane 的底栏：

| 键 | 作用 |
| --- | --- |
| Back | Android 的返回。 |
| Home | Android 的主屏幕。长按出 **Clear Background Apps**，结束后台的应用、释放内存。 |
| Recent Apps | Android 的最近任务。 |
| More（…） | Nestvane 的菜单，下面说到的命令都在这里。 |

More 菜单里有 **Install APK…**、**Send Text Message…**、**Settings**、**Back Up Android…**、**Restore Android…**、**Submit a Bug…**、**Export Diagnostics…** 和 **Stop Android…**（停下以后是 Start Android）。

## 装应用

Android 应用是 APK 文件。下面任何一种都能装：

- **More → Install APK…**，再选文件。
- 在 iOS 的其他应用里（“文件”、Safari 的下载、隔空投送、聊天应用），分享这个 `.apk`，或者用“用其他应用打开”选 Nestvane。按内容认 APK，换了名字也能装。
- 在 Android 里用 Android 自己的“文件”打开 APK。

需要 Google Play 服务的应用不能运行：系统是 AOSP，没有 Google 的应用。

## iOS 和 Android 之间传文件

Android 的共享存储是 iOS“文件”App 里的一个文件夹：**我的 iPhone → Nestvane → Android**，里面就是 Android 的 `Download`、`Pictures`、`Movies` 等。从 iOS 放进去的文件几秒内在 Android 里出现；Android 应用存在这里的文件，iOS 也能打开。图片和视频在 Android 里用 Nestvane 带的小查看器打开。

## 相机

Android 有两个相机：iPhone 的后置和前置，都是 640×480。
相机应用能拍照、录带声音的视频，存在 `DCIM` 里，所以“文件” App 里也看得到。
应用第一次打开相机时，iOS 会问是否允许 Nestvane 使用相机。
iOS 设置 → 隐私与安全性 → 相机里 Nestvane 关着时，Android 的相机启动失败，到那里打开。
两种相机接口的应用都能用（Camera2，以及 1.14 起 Open Camera 用的旧接口 Camera API 1）。

## 设置

More → Settings 是 Nestvane 自己的设置：版本、共享文件在哪里、**120 Hz**（ProMotion 屏幕上才有，更流畅也更耗电，下次启动 Android 时生效）、**Allow root** 和请求过 root 的应用、**Check for updates**。

iPad 上还有 **Tablet layout (experimental)**：打开后 Android 从下次启动起按平板的方式排版，带任务栏。默认关着。它只在 iPhone 上按平板的尺寸试过，还没有在 iPad 上试；如果打开后 Android 显示不正常，在这里关掉再重新启动 Android。

Android 自己的设置里 Wi-Fi 和蓝牙显示为关着，蓝牙打不开：Nestvane 里没有这些硬件。Android 照样能上网，走的是 iPhone 的 Wi-Fi 或蜂窝。

## root

root 默认关。

1. More → Settings → 打开 **Allow root**。
2. 重新启动 Android：More → **Stop Android…**，再 Start Android（或者关掉 Nestvane 再打开）。这个开关在每次启动 Android 时读。
3. Android 应用运行 `su` 时，iOS 会问是否允许这个应用拿 root，回答会记住。

请求过的应用按名字列在 Settings 的 Allow root 下面，关掉某一个就是以后拒绝它。有 root 的应用能读写 Android 里所有应用的数据，只允许信得过的应用。

## KernelSU Manager

Nestvane 实现了 KernelSU 自己的接口，所以官方的 KernelSU Manager 能管理 root，和 iOS 弹窗一起用。不用刷机，也不用改内核：KernelSU 在内核里的那部分由 Nestvane 的内核层回答。

1. 按上面的方法打开 root。
2. 从 [KernelSU 在 GitHub 上的 Release](https://github.com/tiann/KernelSU/releases) 下载 Manager，文件名像 `KernelSU_v3.3.1_32680-release.apk`。只认 KernelSU 签名的 Manager，其他项目或分支编出来的不认。
3. 装上（上面任何一种方式）并打开。第一次打开要先准备它自己的工具，大约一分钟后才出首页；以后打开就正常了。
4. 首页显示 **Working（已工作）** 和版本号。它还会提示 GKI 模式只适合测试，那是对真手机内核说的，这里不用管。
5. **Superuser（超级用户）** 页列出应用。点开一个，打开 **Superuser** 开关就给它 root，关掉就收回。

回答是同一份：在 Manager 里允许的应用，Nestvane 设置里也显示允许；在 iOS 弹窗里允许过的应用，Manager 里也显示允许。拿到 root 的应用用 KernelSU 的方式（KernelSU 自己的工具就是这样）或者用 `su` 都行，按的是同一份回答。

不能用的：

- **模块。** 模块页是空的，装模块也没有实际作用：Android 启动时没有运行 KernelSU 的 `ksud`，也没有给模块文件用的 overlay 挂载。
- **在 Manager 里安装或修补内核、boot 镜像。** 这里没有内核镜像可改，也用不着。
- **开、关以外的 App Profile**（自定义用户、组或 SELinux 域）：Manager 显示默认配置，root 总是完整的 root。

## 备份和恢复 Android

备份是一个文件，里面有 Android 的应用、应用数据、设置和 root 的回答。共享文件夹不在里面，它本来就在“文件”App 里。iPhone 的备份（iCloud 或电脑）本来就包含 Android 的数据。Nestvane 的备份是为了回到某个时刻（比如用 root 折腾之前），或者只把 Android 挪到另一台设备。

**备份：** More → **Back Up Android…** → Back Up。写文件时 Android 停下，写好后你选放在哪里（我的 iPhone、iCloud 云盘或别的应用），然后 Android 再启动。文件名像 `Nestvane-Backup-2026-11-25-1030.aar`。要留出差不多和 Android 数据一样大的空闲空间，菜单里会写有多大。

**恢复：** More → **Restore Android…** → Choose Backup…，选文件。Android 停下，应用和数据换成备份里的，然后从备份启动。原来的数据一直留着，直到 Android 从备份启动到桌面；在那之前就停了的话，会把原来的放回去。各个应用的 root 回答随备份马上回来；root 开没开也会回来，Android 从备份启动时就生效。

哪个版本的 Nestvane 做的备份，就用同一个或更新的版本恢复。

## Android 像是卡住了

- 先等一会儿：应用自带的原生库是解释执行的，可能很慢；大应用第一次启动也要些时间。
- **More → Export Diagnostics…** 写出一个文本文件，有每个 Android 线程在等什么、内存和 Android 最近的日志，然后打开分享菜单，可以附到问题报告里。里面可能有你的应用和账号的名字，分享前先看一遍。
- **More → Submit a Bug…** 在 GitHub 上开一个新问题，版本信息已经填好。
- **More → Stop Android…** 再 Start Android，重新启动 Android，不会丢东西。

怎样反馈、发到哪里，见[反馈问题](reporting.md)。

## 短信

这里的 Android 没有电话号码，也没有基带。**More → Send Text Message…** 填一个发信人和内容，交给 Android 当作收到的短信：短信应用里能看到，等短信验证码的应用也能收到。不会发给任何人。
