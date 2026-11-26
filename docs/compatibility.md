# Apps that were tried

Apps installed from their APK files and opened once each: open-source ones first, and since 1.5 also widely used ones from their makers' sites. This says how far each one got, not how
well it works: "opens" means its first screen was drawn, and nothing beyond that was tried unless a note says so.

Tried on 23 November 2026 on an iPhone 17 Pro (iOS 27.0.1) with the build that became version 1.3. The APKs came
from F-Droid or the projects' own releases. Each app was opened by its package name through the app's test
channel, and a frame was saved 15 to 18 seconds later.

| App | Version | Its own native libraries | Result |
| --- | --- | --- | --- |
| 2048 (Privacy Friendly) | 1.4.2 | none | Opens |
| Aegis Authenticator | from F-Droid, build 82 | 2 | Opens, at its introduction |
| Amaze File Manager | from F-Droid, build 126 | 1 | Opens, and asks for a permission |
| AntennaPod | 3.12.3 | 1 | Opens. Before 1.3 it stopped as it started: see below |
| Fossify Calendar | 1.12.2 | 1 | Opens |
| Fossify Clock | 1.6.0 | 1 | Opens |
| Fossify Contacts | from F-Droid, build 13 | 1 | Opens, and asks for a permission |
| Fossify File Manager | from F-Droid, build 16 | 1 | Opens |
| Fossify Gallery | 1.14.2 | 12 | Opens, and asks for a permission |
| Fossify Notes | 1.7.0 | 1 | Opens |
| F-Droid | 2.0.3 | 1 | Opens, and asks for a permission |
| K-9 Mail | 25.0 | 4 | Opens, at its welcome screen |
| KeePassDX | 4.5.8 | 2 | Opens |
| Loop Habit Tracker | from F-Droid, build 20302 | none | Opens, at its introduction |
| Markor | 2026.10.0 | none | Opens, at its introduction |
| NewPipe | 0.29.3 | 2 | Opens |
| ScriptWidget | 1.1.1 | 1 | Opens. Its first widget preview takes about two minutes |
| Tasks | 15.14 | 4 | Opens |
| Termux | from F-Droid, build 1002 | 2 | Opens, and asks for a permission |
| Wikipedia | r/50616 | 4 | Opens; search and the home feed work. Articles needed a WebView, which Android has since 1.6; the app itself was not tried again |
| DuckDuckGo | 5.301.1 | 10 | Did not open: it is a browser and needs a WebView, which Android has since 1.6; not tried again. In 1.3 it stopped Android: see below |
| Feeder | from F-Droid, build 4191 | 2 | Opens. In 1.3 it stopped Android: see below |

Twenty-one of the twenty-two open.

## Thirty-two more, tried for 1.5

Tried on 23 November 2026 on an iPad Air 11-inch (M3, iPadOS 27.0.1) with builds of version 1.5 as it was being
made, each app again after the fix it led to; Telegram and Element last on an iPhone 14 (iOS 17.3.1). From F-Droid, except Telegram, Signal, Toutiao and Douyin, which
are the builds their makers offer on their own sites. A frame was saved 16 to 30 seconds after opening, and up
to two minutes for the slow ones.

| App | Version | Its own native libraries | Result |
| --- | --- | --- | --- |
| App Manager | 4.1.1 | 4 | Opens, and asks for a permission |
| Aurora Store | 4.8.4 | 1 | Opens, at its welcome screen |
| Binary Eye | 1.75.7 | 3 | Opens, at its welcome screen |
| Breezy Weather | 6.2.2 | 3 | Opens |
| Fennec (Firefox from F-Droid) | 160.0.0 | 15 | Opens, at its welcome screen, after one and a half to two minutes |
| FairEmail | 1.2341 | 1 | Opens, at its welcome screen |
| Fossify Calculator | 1.4.0 | 1 | Opens |
| Fossify Camera | 1.5.0 | 3 | Opens, and asks for a permission |
| Fossify Messages | 1.10.2 | 1 | Opens, and asks to be the SMS app |
| Fossify Music Player | 1.8.5 | 1 | Opens, and asks for a permission |
| Fossify Phone | 1.11.1 | 1 | Opens, and asks to be the phone app |
| Fossify Voice Recorder | 1.7.4 | 2 | Opens and records, from the device's microphone once iOS has been allowed to give it |
| Joplin | 3.7.14 | 26 | Opens; its list of notes shows within two minutes |
| LibreTube | 32.1 | none | Opens, and shows what is trending |
| Luanti | 5.18.0 | 2 | Opens, in landscape |
| Mastodon | 2.13.7 | none | Opens, at its welcome screen |
| Mindustry | 8, build 160.8 | 2 | Opens, at its loading screen, in landscape |
| MuPDF viewer | 1.28.5a | 1 | Opens, and asks for a file |
| Nextcloud | 35.1.0 | 6 | Opens, at its welcome screen |
| Organic Maps | 2026.11.11 | 1 | Opens, and asks for a permission |
| OsmAnd~ | 5.4.11 | 7 | Opens, at its welcome screen |
| PPSSPP | 1.19.3 | 5 | Opens, in landscape |
| Shattered Pixel Dungeon | 4.0.2 | 2 | Opens, at its title screen |
| Signal | 8.36.3 | 12 | Opens, at its welcome screen. Before 1.5 it could not make a key: see below |
| Telegram | 13.3.1 | 4 | Opens, at its welcome screen. Before 1.5 it stopped as it started: see below |
| Telegram X | 0.28.11 | 8 | Opens, at its welcome screen |
| Element Classic | 1.6.62 | 21 | Opens, at its first screen, after about a minute. Before 1.5 it stopped as it started: see below |
| Toutiao (今日头条) | 19.2.0 | 161 | Opens, with its tabs and bars. Since 1.6 (with a WebView) the feed loads: its headlines are there within two minutes |
| Douyin (抖音) | 32.2.6 | 204 | Opens, with its tabs and bars. No video loads within two minutes, with a WebView too (1.6): its own native libraries are interpreted. Needs about 5 GB of memory: an iPad |
| AnkiDroid | 2.25.3 | 2 | Opens to its introduction (1.6, with a WebView; before, no screen within a minute) |
| Jellyfin | 2.7.3 | 2 | Opens to Connect to Server (1.6, with a WebView and SO_BROADCAST; before, only to say that a WebView is needed) |
| KDE Connect | 1.37.3 | 1 | Stops as it starts: it asks for the Wi-Fi service, which this Android does not have |

Twenty-nine of the thirty-two open; of all fifty-four, fifty.

Not tried: TikTok and YouTube. Neither maker offers an APK to download, and YouTube needs Google Play services.

## Fifty more, tried for 1.13

Tried on 25 and 26 November 2026 on an iPhone 14 (iOS 17.3.1) with builds of 1.13 as it was being made, each app
again after the fix it led to. The APKs came from the makers' own sites, the projects' own GitHub releases, or
F-Droid; none from a mirror. Apps were installed, opened by their package name and given 25 to 90 seconds, a frame
was saved, and they were uninstalled again, six or four at a time in one start of Android.

| App | Source | Result |
| --- | --- | --- |
| WeChat (微信) | weixin.qq.com | Opens to its splash screen after about 90 seconds: its own loader maps its libraries, and part of them is interpreted |
| QQ | im.qq.com | Opens to its splash screen |
| Alipay (支付宝) | alipay.com | Opens to its sign-in screen |
| Bilibili (哔哩哔哩) | app.bilibili.com | Installs (before 1.13 it was refused: see below); no screen within 90 seconds |
| Zhihu (知乎) | zhihu.com | Installs (refused before 1.13); stops as it starts, in its own native code |
| NetEase Cloud Music (网易云音乐) | music.163.com | Stops as it starts: its security library runs code from memory it has not made executable |
| Kugou Music (酷狗音乐) | kugou.com | Opens to its privacy notice |
| Baidu Maps (百度地图) | map.baidu.com | Opens to its terms |
| Baidu Netdisk (百度网盘) | pan.baidu.com | Not supported: the package offered is 32-bit only, and the iPhone's processor runs no 32-bit code |
| Quark (夸克) | quark.cn | No screen within 60 seconds |
| UC Browser | ucweb.com | No screen within 60 seconds |
| DingTalk (钉钉) | dingtalk.com | Opens to its privacy policy |
| Feishu (飞书) | feishu.cn | Opens to its sign-in, after 18 seconds |
| Tencent Meeting (腾讯会议) | meeting.tencent.com | Opens |
| WPS Office | wps.com | Stops as it starts |
| Tencent Video (腾讯视频) | v.qq.com | Opens to its splash screen (1.13: see below) |
| iQIYI (爱奇艺) | iqiyi.com | No screen within 60 seconds: a library it loads makes the whole app interpreted |
| Youku (优酷) | youku.com | Opens to its welcome screen |
| 12306 (铁路12306) | 12306.cn | Stops as it starts: it is wrapped by an app protector (SecNeo) that does not start here |
| WhatsApp | whatsapp.com | Opens to its terms |
| Zoom | zoom.us | Its window stays blank for two minutes. Before 1.13 it waited for ever on network interface requests |
| Firefox | Mozilla | Opens (1.13: see below); its helper processes stop, the browser itself goes on |
| Firefox Focus | Mozilla | Opens (1.13) |
| Brave | GitHub | Opens to its welcome screen (a nightly build) |
| Opera | opera.com | No screen within 60 seconds: interpreted |
| Tor Browser | torproject.org | Opens |
| Bitwarden | GitHub | Opens (1.13) |
| Proton VPN | GitHub | Opens to its sign-in (1.13) |
| Mullvad VPN | mullvad.net | Opens |
| Tailscale | tailscale.com | Opens; stops when it asks for VPN permission, which this Android has no dialog for |
| WireGuard | wireguard.com | Opens (1.13) |
| v2rayNG | GitHub | Opens, and asks for a permission |
| Session | getsession.org | Stops as it starts: it asks for the Wi-Fi service, which this Android does not have |
| Element X | GitHub | Opens (1.13) |
| Wire | wire.com | Opens |
| Briar | briarproject.org | Opens to its setup |
| Jitsi Meet | F-Droid | Opens |
| Thunderbird | GitHub | Opens to its welcome screen |
| Tusky | GitHub | Opens to its sign-in |
| Tuta Mail | F-Droid | Opens |
| VLC | videolan.org | Opens to its introduction (1.13) |
| Kodi | kodi.tv | Opens |
| RetroArch | libretro.com | Opens |
| Moonlight | moonlight-stream.org | Opens |
| Kiwix | kiwix.org | Opens |
| Syncthing-Fork | GitHub | Opens to its introduction |
| Obtainium | GitHub | Opens, and asks for a permission |
| Mihon | GitHub | Stops as it starts, in its own native code |
| LocalSend | GitHub | No screen within 60 seconds: its Flutter engine is interpreted |
| AppFlowy | GitHub | Opens, after 46 seconds (Flutter) |

Thirty-six of the fifty open, and one more (Baidu Netdisk) cannot run on an iPhone at all; of all one hundred and four apps tried, eighty-seven open.

## What changed in 1.13

Opening many apps one after another, rather than one per start of Android, found more than any single app did.

**Android no longer stops, and Nestvane no longer ends, when several apps have been opened.**

- iOS ended Nestvane when it used more memory than the app may (on an iPhone 14, about 4.3 GB), without a warning.
  Now, when little is left, the app Android ranks lowest is ended first, as Android's low-memory killer would:
  cached apps, then the previous app, then services; never what is on screen.
- A host allocation that failed ended all of Android. Now a system call that finds no memory makes room and tries
  again, and a process that cannot be given memory is ended alone.
- A memory fault in one process that it had no handler for ended all of Android. Now only that process ends, as on
  Linux. A fault on memory that was mapped but could not be given host memory yet makes room and runs again.
- After the first WebView renderer ended, no app could start any more: WebView's zygote reported the end with a
  call that was not implemented, waited for ever, and every start of an app waited behind it.
- A 400 MB package (QQ, Feishu) ran out of memory while it was copied for installing, and Nestvane ended. Packages
  are now handed to Android from the file without a copy.

**Apps that use JNA or Rust run natively.** Bitwarden, Element X, Firefox Focus and Proton VPN build small
trampolines with libffi, in a file mapped executable at an address of their own choosing. That made the whole app
interpreted, minutes to a first screen. Such code is now placed where only it is interpreted. The same goes for
libraries that apps load with their own loader (WeChat).

**System calls and requests that were missing**, each of which left a process waiting for ever: `pselect6` (VLC),
`tgkill` to a thread running on another core and `madvise` hints (Go programs: WireGuard, Tailscale),
edge-triggered `epoll` on TCP and UDP sockets (Telegram, Tailscale), `ptrace` (now refused, as for an unrelated
process), `clone` with a new stack and no signal and `wait4` with `__WALL` (Firefox, Quark, UC), `SIOCGIFCONF`, loopback's
address and `FIONREAD` on sockets (Zoom, Tencent Video, AppFlowy).

**Shared libraries.** Seven libraries that Android declares were missing from the image (the USB accessory library,
the test libraries, and others), so Android refused to install any app that requires one: Bilibili and Zhihu.

**Graphics.** A reset of the phone's graphics while an app drew ended Nestvane (Firefox). That draw fails now, and
Android goes on.

## What changed in 1.5

**Keys.** An app could not make a key in Android's keystore: the keystore asked whether the app was allowed, and
the answer was no for every app, because the table of answers had been taken for the system's own processes
only. Apps now get what Android's policy gives every app. Signal and Element stopped on this as they started,
and so would any app that keeps its settings encrypted.

**Named pipes.** `mkfifo` was refused. Element's database library makes one in the app's own folder, and
watches it for changes, which a pipe could not be watched for either.

**Instructions.** Telegram stopped on an instruction the interpreter did not have. The native libraries of all
these apps were searched for more (`tools/scan-interpreter-coverage.py`), and what ordinary code among them
uses was added: about seventy vector and floating-point instructions, the conversion to and from half
precision, and the cache instructions a program uses after writing code of its own. Each vector instruction is
run on a Mac's processor and in the interpreter with the same operands (557 cases).

**Large apps.** Douyin's package is 271 MB and it starts several hundred threads. The limits it met were raised:
the size of a package (to 1 GiB), the threads Android may have (to 4,096), the memory an app's Java code may use
(to 256 MB), and how many requests may wait for one thread of a system service.

**Names of hosts.** A lookup whose answer went through several aliases came back cut short, and the app was told
the host did not exist. Answers are now packed the way the protocol allows.

**Landscape.** An app that asks for landscape is shown the right way up, and turning the device turns Android.

## What changed after 1.3

**Apps that stopped Android.** DuckDuckGo and Feeder each bring a library written in Go, and in 1.3 opening
either one stopped Android: nothing was drawn any more. The interpreter that runs apps' own libraries had met an
instruction it does not have, and that ended the whole run. Two things changed. An instruction the interpreter
does not have now ends only the app that met it, with the signal a processor without the instruction would
raise. And the instructions themselves were added: the ones Go's libraries use, and about forty more vector
instructions that the libraries of the apps in this list contain. Each is run on a Mac's processor and in the
interpreter with the same operands, and the results compared (`tools/validate-vector-instructions.cpp`).
Tried on an iPhone 14 with iOS 17.3.1: Feeder opens and stays open; DuckDuckGo ends by itself, for want of a
WebView, and Android goes on.

## What changed in 1.3

**Libraries laid out for 4 KiB pages.** Android here runs with the iPhone's 16 KiB pages, and its linker refuses a
library laid out for 4 KiB pages. Many apps still ship such libraries. AntennaPod is one: it stopped as it
started with `dlopen failed: … program alignment (4096) cannot be smaller than system page size`. Android has a
compatible way of loading them, which is now on for every app (`bionic.linker.16kb.app_compat.enabled`), and
the kernel layer no longer refuses memory that gains execute permission, which that way of loading needs.

## What stands in the way of more

- **WebView came in 1.6.** Android System WebView 128 from AOSP, translated ahead of time like the rest. JavaScript
  runs without V8's JIT. A Wikipedia article loads in a WebView in 4 to 8 seconds.
- **A limit on how many files Android's data may hold** was reached with some thirty apps installed: no further app
  would start. It was 4,096 files and 2,048 directories and is now sixteen times that.
- **Native libraries are interpreted,** at about forty million instructions a second. An app that does real work
  in its own library is slow.
- **No Google Play services.** Apps that need them were not tried.
- **No Wi-Fi service.** Android here reaches the network through a wired interface. An app that asks for the
  Wi-Fi service and does not expect to be told there is none stops (KDE Connect, Session). Planned for 1.16.
- **No VPN dialog.** A VPN app stops when it asks for permission to set up a VPN (Tailscale).
- **App protectors** that wrap an app's code (SecNeo in 12306) do not start.
- **32-bit apps** cannot run: the iPhone's processor has no 32-bit mode (Baidu Netdisk's package).
- **Large apps start slowly.** Their Java code is not compiled ahead of time here. Firefox takes up to two
  minutes to its first screen.

VLC was left out of the first two lists because the APK at hand was built for x86 only; its ARM build was tried for 1.13.
