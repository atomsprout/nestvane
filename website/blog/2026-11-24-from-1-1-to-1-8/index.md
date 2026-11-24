---
slug: from-1-1-to-1-8
title: "Nestvane 1.1 to 1.8: what changed in two days, and what comes next"
description: Eight releases after the first one, from 120 Hz and text messages to WebView, root and clearing background apps, and the plan for 1.9 to 2.3.
date: 2026-11-24
authors:
  - name: AtomSprout
    url: https://github.com/atomsprout
tags: [nestvane, android, ios, release]
---

Nestvane 1.0 came out on 22 November: Android 17 inside an iPhone app, about ten seconds to the home screen, with apps installed from APK files. Two days later it is at 1.8.

This post goes through what those eight releases did, what did not work out, and what the next five versions are meant to do.

<!-- truncate -->

The short version: Android now runs on iOS 17 and on an iPhone 14, it has a WebView, it has root when you want it, and it starts faster than it did. What still has not changed is the biggest problem: you need a paid Apple developer account to install it.

## 1.1 and 1.2: the small things a phone does

The first two releases were about things you notice within a minute of using it.

**120 Hz.** Android drew 60 frames a second whatever the screen could do. The cause was that resource overlays for the Android framework itself were never applied, and that is where the display's rate is set. With that fixed, a ProMotion screen gets 120. It is off until you turn it on, because it costs battery.

**Text messages you write yourself.** Android here has no radio and no phone number. A menu item hands Android a sender and a text, and Messaging shows it as an incoming message. In 1.2 it is also announced to apps the way a real SMS is, so an app that waits for a code by SMS receives it. Nothing is sent to anyone.

**A shorter way to the screen.** Each frame used to be read back from the graphics processor and handed to iOS as a picture. Now it is copied on the graphics processor into a surface iOS shows as it is, just before the screen refreshes.

**Changing network.** Going from Wi-Fi to cellular left apps waiting on connections that led nowhere. Nestvane now ends them, as Android on a phone does, and apps reconnect at once.

There was also an update notice, a lower frame rate when the phone is hot, and a menu item that opens a GitHub issue with the versions already filled in.

## 1.3 and 1.4: older phones, and apps that stopped everything

1.0 needed iOS 26 and was tested on phones with 8 GB of memory. **1.3 runs on iOS 17**, and on an iPhone 14 with 6 GB. Android used 2.3 GB of it.

The same release found why many apps quit as they started. Android here uses the iPhone's 16 KiB memory pages, and most apps still ship libraries laid out for 4 KiB pages. Android refused all of them. They now load the compatible way, and a list of tried apps started: twenty of twenty-two opened.

1.3 also showed a bad bug. Opening DuckDuckGo or Feeder froze the whole of Android. Both have a library written in Go, and the interpreter that runs apps' own libraries met an instruction it did not know and stopped the entire run. **1.4 fixed that**, along with a limit that kept Android from starting any app once about thirty were installed.

1.4 was also the first release for the **iPad**. Before, the display had a phone's pixels, enlarged. Now an 11-inch iPad Air gets 1160 × 1578, sharp and filling the screen. It is still laid out as a phone, and that is still true in 1.8.

## 1.5: more apps, turning, video, the microphone

1.5 tried thirty-two more apps, Telegram, Signal, Firefox, Element and OsmAnd among them. Fifty of fifty-four opened.

**Turning the device turns Android.** An app that asks for landscape, a game for example, is shown the right way up and touches land where you put your finger. The home screen itself still does not turn; more on that below.

**1080p video.** H.264 and HEVC play at 1080p and 30 frames a second, decoded in software. Before, an HEVC video stopped the decoder: its hand-written assembly used registers that the translated code keeps for itself.

**The microphone.** An Android app that records gets the device's microphone, and iOS asks the first time.

## 1.6: WebView

Many apps show web pages inside themselves, and Android had no WebView. Toutiao and Douyin opened to nothing. AnkiDroid and Jellyfin needed one too.

The difficulty was not Chromium itself. Android's own code is built with five processor registers left free for Nestvane. WebView's library is a prebuilt binary that uses all of them. So the translator learned to lend other registers in their place, and now code built the ordinary way can be translated ahead of time like the rest.

The Linux layer also needed new things: reservations of tens of GiB of address space, abstract Unix sockets, `seccomp()` with `SIGSYS`, a TCP peek. A Wikipedia article now loads in a WebView in about 4 seconds on an iPad Air and 8 on an iPhone 14. JavaScript runs in V8's interpreter, with no JIT. 51 of the 54 apps open.

## 1.7: root, storage, and faster native libraries

**Root, if you want it.** It is off until you turn it on. Then `su` exists, and the first time an app runs it, iOS asks whether that app may have root. An allowed app gets uid 0, every capability and an SELinux domain that may do anything.

Building root turned up three bugs that hit every app. Java's `Runtime.exec` and `ProcessBuilder` hung the calling thread, because `vfork` was missing and so was asking a pipe how much it holds. The usual commands such as `ls`, `id` and `ps` were not in `/system/bin` either. All three are fixed.

**Your device's storage.** Android saw a fixed 8 GiB. Now it sees the volume Nestvane is on and what iOS says is free.

**Apps' own native libraries run about twice as fast.** They are still interpreted, because iOS lets no app make machine code as it runs. But common instructions are now decoded once. A thread that keeps interpreting moves to a core of its own, and the system libraries it calls still run as translated code. Each pre-decoded instruction is checked against the old interpreter on random inputs, 406,093 of them, all the same.

**A calmer start.** Apps you installed now wait until the home screen is shown. A chat app syncing at boot used to take the processor from Android's own start.

## 1.8: small fixes

1.8 was meant to be small, and it was.

**Storage that adds up.** 1.7 gave Android's data partition the whole volume, and Android adds the system partition before rounding up, so a 256 GB iPhone showed 512 GB. Now it shows 256.

**Clear background apps.** Touch and hold Home in Nestvane's bar and choose Clear Background Apps. On an iPad Air memory went from 4.42 GB to 2.83 GB.

While testing it, two clears in three with a WebView in the background brought Android down. `system_server` aborted with "Unexpected BR_REPLY" and everything restarted. The cause was in our Binder. When a process died while another thread was calling it, the death notice went to that very thread. It ran the death handler inside its call, and the call's own reply then arrived at the wrong place. Linux treats a death notice as work for the whole process, and now Nestvane does too. This was not only a problem for the new button: Android ending an app on its own could hit it.

**Root shells and names.** An app can keep `su` open and write commands to it, as terminal apps do. The root prompt shows the app's name, not only its package.

## Boot time may only get faster

After 1.6 shipped I realized I had no like-for-like comparison of its boot time against 1.5. So there is now a rule: before every release, start Android six times on a test device and take the median of the last five. If it is slower than the recorded baseline for that model, the release waits until the reason is found.

This is harder than it sounds, because one build can vary by 3 seconds from start to start on the same phone. The first check for 1.8 failed at 12.5 seconds. Rather than loosen the rule, I ran 1.7 and 1.8 in turn on the same phone. 1.7 was slower still then, at 13.4. A test app had been left installed with an unanswered iOS permission prompt. With it removed, the two took the same time, and the recorded run for 1.8 was 9.1 seconds against 1.7's 10.9.

| Device | Boot to the home screen, median of five |
| --- | --- |
| iPhone 17 Pro | 7.3 s |
| iPad Air (M3) | 7.6 s |
| iPhone 14 | 9.1 s |

## What did not work out

- **The microphone's recording has not been verified.** A test app records silence until iOS is told to give Nestvane the microphone. That needs a tap on the device, and it has not been done yet.
- **ScriptWidget's widget previews still do not appear.** Its script engine is a native library, and it sets a deadline on every script. Interpreted, the scripts run past it. Doubling the speed was not enough.
- **Douyin still shows no video**, for the same reason.
- **The home screen does not turn.** Launcher3 controls it with a preference, not a resource we can override. It needs more than a day.
- **KDE Connect keeps stopping.** It asks for Android's Wi-Fi service, which does not exist here, and gets nothing back.

## What comes next

| Version | Goal |
| --- | --- |
| 1.9 | Apps' own native libraries faster again |
| 2.0 | No paid developer account |
| 2.1 | KernelSU compatibility |
| 2.2 | The home screen turns, a live shared folder, IPv6, a Wi-Fi service |
| 2.3 | A tablet layout on the iPad, hardware video decoding |

**1.9 makes the interpreter faster, for every app.** Floating-point and SIMD instructions into the fast path, since they are now the commonest reason a worker thread stops. Interpretation cached by block. Fewer faults at block edges. Translating one app's library ahead of time is off the table: when the app updates, the library changes and the work is lost. Speed will be measured on a benchmark and a mix of apps, not tuned to one.

**2.0 tries to do without the paid account.** This is the one people ask about most. Nestvane needs a capability for a large address space, and only paid accounts get it. To fit a free account, everything has to come in under about 7 GiB of address space, and today the code tables, the translated code and the system image alone take about that much. The plan is a software MMU: every Android process shares one pool of memory, mapped on demand through page tables. It is the riskiest item. It starts with one or two days of measuring, and if the numbers say no, it stops there.

**2.1 is KernelSU compatibility**, starting with KernelSU Manager being able to grant and revoke root for apps. Magisk and Zygisk, which inject code into zygote, are not planned.

**2.2 is everyday fixes:** the home screen turning with the device; files put in the shared folder showing up without a restart; IPv6; and a Wi-Fi service that at least tells apps Wi-Fi is off, so KDE Connect and others stop crashing.

**2.3 needs Android rebuilt from source:** a tablet layout on the iPad, and video decoded by the device's hardware.

The days these take are estimates from the pace so far, not promises. The full plan, in Chinese, is [in the repository](https://github.com/atomsprout/nestvane/blob/main/docs/zh-CN/roadmap.md).

## Try it

[Nestvane 1.8 is on GitHub](https://github.com/atomsprout/nestvane/releases/tag/v1.8). It needs iOS 17 or later, 6 GB of memory, and, for now, a paid Apple Developer Program membership to sign and sideload it. If something breaks, the More menu has a Submit a Bug item that opens an issue with your versions filled in.
