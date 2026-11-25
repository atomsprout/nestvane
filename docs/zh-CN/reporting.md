# 反馈问题

Nestvane 还在早期，到现在修好的问题大多是用户告诉我们哪里不能用才发现的。这篇说出了问题时要收集什么、怎样从 App 里导出诊断信息，以及发到哪里。

## 发到哪里

| 地方 | 适合 |
| --- | --- |
| [GitHub Issues](https://github.com/atomsprout/nestvane/issues) | Bug：不能用、闪退、卡死。一个 issue 里放着问题、诊断文件和回复，修好以后也会写明是哪个版本修的。需要 GitHub 账号。 |
| [Discord](https://discord.gg/eGzEaP6TzR) | 提问、简单的反馈，和其他用 Nestvane 的人交流。 |
| [Telegram](https://t.me/atomsprout) | 同上，也可以关注更新。 |
| [邮件](mailto:atomsprout@gmail.com?subject=Nestvane%20problem) | 不想公开的内容，包括诊断文件。 |

拿不准是不是 bug，可以先在 Discord 或 Telegram 里问。确认是 bug 的话会转成 issue，免得丢了。

## 最快的办法：Submit a Bug

在 Nestvane 里点 **More → Submit a Bug…**，浏览器里会打开一个新的 GitHub issue，系统信息已经填好：Nestvane 的版本和构建号、iOS 版本、设备型号、内存、120 Hz 是否打开、Android 当时是否在运行、手机温度。在上面写清楚发生了什么，再把诊断文件（见下面）拖进 issue，或者用 *Paste, drop, or click to add files* 附上。

## 导出诊断信息

**More → Export Diagnostics…**，再点 **Export**。几秒后（最多约 15 秒）会打开分享菜单，里面是一个文本文件，文件名像 `Nestvane-Diagnostics-20261125-103000.txt`。可以存到“文件”，也可以直接发到邮件、Telegram、Discord 或其他反馈的地方。

文件里有：

- 时间、Nestvane 版本、iOS 版本、设备型号、内存和温度，以及 Android 当时是否在运行。
- 内存：Nestvane 用了多少，iOS 还允许它用多少。
- 每个 Android 线程在等什么、Android 系统服务（system_server）的 Java 线程栈，以及 Android 自己最近的日志。
- Nestvane 自己记下的上一次启动：走到了哪一步、每一步用了多久、桌面什么时候出来、网络情况，以及看门狗看到的状态。

**趁问题还在的时候导出。** More 菜单是 Nestvane 自己的，Android 卡住了它也能用。如果先关掉 Nestvane，下次启动会覆盖出问题那次运行的记录。运行时本身卡住的话，约 15 秒后文件里会写明，并附上之前记下的内容。

**分享前先看一遍。** Android 的日志里可能有你的应用、账号和文件的名字。想私下发送的话，用邮件，不要贴在公开的 issue 或群里。

文件一般几百 KB，GitHub 可以直接作为附件上传。

## 怎样的反馈最有用

- **你做了什么、期望看到什么、实际看到什么。** 能重复出现的话，写出每一步。
- **是哪个应用**（如果和某个应用有关）：名字、版本，以及 APK 从哪里来（F-Droid、开发者网站、APKMirror……）。能给出同一个 APK 的链接最好。
- **是不是每次都这样**，还是只出现过一次，或者 Nestvane 运行一段时间以后才出现。
- **截图或录屏。** iOS 控制中心里的屏幕录制能像录其他 App 一样录 Nestvane。
- **诊断文件。**
- **如果 Nestvane 自己退出了**（回到 iOS 桌面），iOS 会留下崩溃报告：设置 → 隐私与安全性 → 分析与改进 → 分析数据，名字以 `nestvane-native` 开头的条目。把最新的一份（右上角可以分享）一起发来。只有打开了“共享 iPhone 分析”才有这个列表。

如果只是某个应用打不开、别的都正常，通常给出应用名字和 APK 链接就够了：[兼容性](compatibility.md)里列着试过的应用。

## 反馈之前

- 确认用的是[最新版本](https://github.com/atomsprout/nestvane/releases)：有新版本时 Nestvane 的 More → Settings 里会写。
- 看一下[还不能用的](status.md)：Google Play 服务、硬件视频解码、桌面跟着转、KernelSU 模块等都是已知的。
- 搜一下[已有的 issue](https://github.com/atomsprout/nestvane/issues?q=is%3Aissue)。已经有了的话，在里面补充，不用新开：多一台手机、多一个应用出现同样的问题，也有用。
