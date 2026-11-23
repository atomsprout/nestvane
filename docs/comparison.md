# Nestvane, Twoyi and Andarium

Two similar projects ran Android inside an iPhone app before Nestvane did, and Nestvane owes both of them.
This page says what each is, how Nestvane relates to them, and how they differ.

What is said about the other two comes from their own pages as read on 22 November 2026, and from one measurement
of ours that is marked as such. Their authors know their projects better than this page does; corrections are welcome.

## The two similar projects

**[Twoyi](https://twoyi.app/)** (两仪) runs a separate Android system on an iPhone, an iPad or an Android phone.
Its site says the iOS version needs no jailbreak and no JIT, that Android's code is "translated ahead of time into
native ARM64 instructions when Twoyi is built", that it runs Android 13 and reaches the home screen in about
3 seconds. It is distributed as an IPA to sign yourself. We know of no published source for the iOS version.

**[Kernarium](https://github.com/imxieyi/Kernarium)** runs a real Linux kernel on iOS: User-mode Linux compiled to
native arm64, with guest programs run by a threaded interpreter. **Andarium** is its Android app. It was on the
App Store until Apple removed it, and is now an IPA to sideload. It is open source under GPL-2.0-or-later.

## How Nestvane relates to them

- Nestvane began with a study of Twoyi. Twoyi showed that translating Android ahead of time, with no JIT, is
  enough to make it usable on an iPhone, and Nestvane follows that idea.
- Nestvane shares no code with either project. It was written from nothing: its own kernel layer, its own
  rewriter, its own interpreter.
- Kernarium was the open-source system to read while Nestvane was planned, and its published optimisation notes
  explained where an interpreter's time goes. Nestvane chose the opposite trade: no real kernel, translated code.

## Side by side

| | Nestvane | Twoyi (iOS) | Andarium |
| --- | --- | --- | --- |
| Source | Open, Apache-2.0 | No published source that we know of | Open, GPL-2.0-or-later |
| Android | 17, built from AOSP | 13 | Built from its own image; the version is not stated in its README |
| Kernel the guest sees | Written for the app, in user space | Not stated | The real Linux 6.18, as User-mode Linux |
| How Android's code runs | Rewritten ahead of time to native code; an interpreter for the rest | Translated ahead of time | Threaded interpreter; a JIT where one can be enabled |
| An app's own native libraries | Interpreted, slowly | Not stated | Interpreted like everything else |
| Boot to the home screen | About 10 s; about 17 s the first time (iPhone 17 Pro) | "About 3 seconds" by its site | About 25 s in one run of ours on an iPhone 17 Pro in November 2026, with a different build and data; its README states no time |
| iOS | 26 or later | 15 or later | 26 or later |
| Devices tested by the project | iPhone 15 Pro, iPhone 17 Pro | Not stated | Not stated |
| How much of Android is kept | Launcher3 with its widgets, Settings, Files, the package installer, the media services | Not stated | A full system image |
| Distribution | IPA to sign yourself | IPA to sign yourself | IPA to sideload |

## What Nestvane chose

- **The newest of both.** Android 17 on iOS 17 and later. It has been tested on an iPhone 14, an iPhone 15 Pro and an
  iPhone 17 Pro only; no other phone is known to work.
- **Less trimming.** The home screen keeps its widgets, and Settings, Files and the installer are the stock ones.
  The trimming that would need another rebuild of Android was set aside.
- **Boot time is not the goal.** About 10 seconds is where it stopped, and the project does not plan to cut the
  system further to go faster.
- **A kernel layer instead of a kernel.** It is faster to start and simple to embed, and it is less complete than
  the real thing: what Android does not ask for is not there.

## How it was built

Nestvane was written by coding agents directed by one person, over five days in November 2026. The first two days
used Codex with GPT-6.2 Sol and ended with Android reaching its launcher in about ten minutes, on an interpreter.
The next two used Claude Fable 5.2: a review of that design, the reserved-register rebuild, the rewriter, and the
work recorded in [`results/G0.md`](../results/G0.md) that brought the boot to about 10 seconds.

The author has written software for Windows, iOS and macOS for many years and knows very little about Android.
The project began after seeing the two similar projects, as a way to find out how far an AI coding agent could get
on a problem like this one. The author's conclusion is that Claude Fable 5.2 is very capable.
