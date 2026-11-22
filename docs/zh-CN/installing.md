# 安装发布版

每个 [Release](https://github.com/atomsprout/nestvane/releases) 里都有 `Nestvane-<版本>.ipa`。
这个 IPA 里没有签名，也没有描述文件，iOS 不会直接安装它。
你要先用自己的 Apple 开发者身份给它签名。本页说明做法，以及哪些试过、哪些没试过。

## 需要什么

- 一台 iOS 26 及以上的 iPhone。在 iPhone 15 Pro 和 iPhone 17 Pro 上测试过，两台都是 8 GB 内存。
  Android 的内存占用约 2.2–2.7 GB；iPhone 11 的单个 App 上限约 2.3 GB，放不下。
- 约 4 GB 的可用存储。IPA 约 1.1 GB，装好的 App 约 3.2 GB。
- 一台装有 Xcode 的 Mac、一个 Apple 开发者签名身份，以及一份包含你的设备的描述文件，它的 App ID 要开启两项能力：
  **Extended Virtual Addressing** 和 **Increased Memory Limit**。没有前一项，App 的地址空间只有约 7 GiB，
  而不是约 53 GiB，不够用。

**需要付费的 Apple Developer Program 会员（每年 99 美元），免费的 Apple ID 不够用。** Extended Virtual Addressing 只提供给付费的开发者账号。没有它，Nestvane 预留不出翻译后的代码所需的地址空间，
只能退回逐条解释执行。我们试过：在 iPhone 15 Pro 上不带这两项能力签名，Android 五分钟后仍没有到桌面，而带上它们时约 10 秒。
用免费 Apple ID 签名的 App 还会在 7 天后失效。

## 安装的几种方式

没有 App Store 版本。IPA 要靠侧载：用你自己的 Apple 开发者账号签名，再安装。

| 方式 | 运行在 | 我们知道的 |
| --- | --- | --- |
| 命令行里用 Xcode 的工具，[见下文](#签名与安装) | macOS | 本页就是这样测的。带两项能力的 App ID 要你自己创建。 |
| [Impactor](https://github.com/claration/Impactor) | macOS、Linux、Windows | 开源。它的页面说可以申请“提高内存上限”，没有提到扩展虚拟寻址。没有用 Nestvane 试过。 |
| [Sideloadly](https://sideloadly.io/) | macOS、Windows | 支持免费和付费账号签名。没有用 Nestvane 试过。 |
| [AltStore](https://altstore.io/) 和 [SideStore](https://sidestore.io/) | 在手机上，需用电脑完成初始设置 | 没有用 Nestvane 试过。 |

不管用哪一种，判断标准都一样：用付费账号并且 App ID 带上两项能力时，Android 大约 10–20 秒到桌面。如果启动画面停留好几分钟，
说明签名时没有带上 Extended Virtual Addressing。哪些工具对你有效，请告诉我们。

## 签名与安装

```sh
unzip Nestvane-1.0.ipa
app=Payload/Nestvane.app

# 你的描述文件；如果它的包标识符不是 com.atomsprout.nestvane，也要改。
cp your.mobileprovision "$app/embedded.mobileprovision"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier your.bundle.identifier" "$app/Info.plist"

# 描述文件里的授权；其中必须包含 ios/Native.entitlements 里的两个键。
security cms -D -i your.mobileprovision > profile.plist
/usr/libexec/PlistBuddy -x -c "Print :Entitlements" profile.plist > entitlements.plist

identity="Apple Development: …"   # 用 security find-identity -v -p codesigning 查看
for framework in "$app"/Frameworks/*.framework; do codesign -f -s "$identity" "$framework"; done
codesign -f -s "$identity" --entitlements entitlements.plist "$app"

xcrun devicectl device install app --device <设备名或标识符> "$app"
```

试过的：把 1.0 版的 IPA 解包，用本项目自己的开发签名身份、描述文件和包标识符按上面的方法签名，装到 iPhone 17 Pro 上，
Android 启动到了桌面（这次首次启动 15.5 秒）。改包标识符，以及替你签名并安装 IPA 的工具，都没有试过。

## 校验下载

每个 Release 都列出了 IPA 的 SHA-256，用 `shasum -a 256 Nestvane-1.0.ipa` 对比。

## 第一次启动

- 安装后的第一次启动在 iPhone 17 Pro 上约 17 秒，之后约 10 秒。
- Android 应用来自 APK 文件：在“文件”或分享菜单里用 Nestvane 打开 `.apk`，或在 App 的菜单里选 *Install APK…*。
  Android 启动后第一分钟内装的应用，图标约 11 秒后出现；之后装的立刻出现。
- Android 的共享存储是“文件”App 里的 *我的 iPhone → Nestvane → Android* 文件夹。
- 只有当某个 Android 应用去连你网络里的设备时，iOS 才会询问本地网络权限。
- 删除 App 会连同 Android 的数据一起删除。

## 里面有什么

App 里是用本仓库的补丁从 Android 开源项目构建并提前翻译的 Android 17 系统，以及 gfxstream 和 ANGLE。
它们的许可声明在 App 里的 `Licenses/` 目录中，也作为 `NOTICE-AOSP.txt` 和 `THIRD-PARTY-NOTICES.txt` 附在每个 Release 上。

这个 Android 系统里有少数组件（包括 iptables 和 libexif）使用 GNU 通用公共许可证（GPL）或 GNU 宽通用公共许可证（LGPL）。
它们的源码就是 Android 开源项目的 `android-17.0.0_r1` 标签加上本仓库的补丁。自每个版本发布之日起三年内，任何人来信索取，
AtomSprout 都会提供这份源码的副本，收费不超过传送的成本：请写信到 <atomsprout@gmail.com>。怎样自己构建这一切，见[构建](building.md)。
