# Installing the release

**For now Nestvane is for developers only.** The only way to run it is to sign the IPA with a paid Apple Developer Program membership (US$99 a year) and sideload it. There is no App Store or TestFlight version, and people without a developer account cannot use it yet.

Each [release](https://github.com/atomsprout/nestvane/releases) carries `Nestvane-<version>.ipa`.
The IPA has no signature and no provisioning profile in it: iOS will not install it as it is.
You sign it with your own Apple developer identity first. This page says how, and what was and was not tried.

## What you need

- An iPhone with iOS 26 or later. Tested on iPhone 15 Pro and iPhone 17 Pro, both with 8 GB of memory.
  Android's footprint is about 2.2 to 2.7 GB; an iPhone 11, whose limit for one app is about 2.3 GB, cannot hold it.
- About 4 GB of free storage. The IPA is about 1.1 GB and the installed app about 3.2 GB.
- A Mac with Xcode, an Apple developer identity, and a provisioning profile for your device whose App ID has two
  capabilities: **Extended Virtual Addressing** and **Increased Memory Limit**. Without the first the app has about
  7 GiB of address space instead of about 53 GiB, which is not enough for it.

**You need a paid Apple Developer Program membership (US$99 a year). A free Apple ID is not enough.** Extended
Virtual Addressing is given to paid developer accounts only, and without
it Nestvane cannot reserve the address space its translated code needs, so it falls back to interpreting every
instruction. We tried it: signed without the two capabilities, on an iPhone 15 Pro, Android had not reached its home
screen after five minutes, against about 10 seconds with them. An app signed with a free Apple ID also stops working
after 7 days.

## Ways to install

There is no App Store version. The IPA is sideloaded: signed with your own Apple developer account, then installed.

| Way | Runs on | What we know |
| --- | --- | --- |
| Xcode's tools on the command line, [below](#signing-and-installing) | macOS | The way this page was tested. You make the App ID with both capabilities yourself. |
| [Impactor](https://github.com/claration/Impactor) | macOS, Linux, Windows | Open source. Its page says it can request the increased memory limit; it does not mention extended virtual addressing. Not tried with Nestvane. |
| [Sideloadly](https://sideloadly.io/) | macOS, Windows | Signs with a free or a paid account. Not tried with Nestvane. |
| [AltStore](https://altstore.io/) and [SideStore](https://sidestore.io/) | on the phone, set up from a computer | Not tried with Nestvane. |

Whichever you use, the test is the same: with a paid account and both capabilities on the App ID, Android reaches
its home screen in about 10 to 20 seconds. If the start screen stays for minutes, the app was signed without
Extended Virtual Addressing. Tell us which tools work for you.

## Signing and installing

```sh
unzip Nestvane-1.2.ipa
app=Payload/Nestvane.app

# Your profile, and its bundle identifier if it is not com.atomsprout.nestvane.
cp your.mobileprovision "$app/embedded.mobileprovision"
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier your.bundle.identifier" "$app/Info.plist"

# The entitlements of your profile; they must include the two keys of ios/Native.entitlements.
security cms -D -i your.mobileprovision > profile.plist
/usr/libexec/PlistBuddy -x -c "Print :Entitlements" profile.plist > entitlements.plist

identity="Apple Development: …"   # from: security find-identity -v -p codesigning
for framework in "$app"/Frameworks/*.framework; do codesign -f -s "$identity" "$framework"; done
codesign -f -s "$identity" --entitlements entitlements.plist "$app"

xcrun devicectl device install app --device <device name or identifier> "$app"
```

What was tried: the IPA of version 1.0 was unpacked, signed this way with the project's own development identity,
profile and bundle identifier, installed on an iPhone 17 Pro, and Android reached its home screen (15.5 seconds on
that first start). Changing the bundle identifier, and tools that sign and install an IPA for you, were not tried.

## Checking the download

Each release lists the SHA-256 of its IPA. Compare it with `shasum -a 256 Nestvane-1.2.ipa`.

## First start

- The first start after installing takes about 17 seconds on an iPhone 17 Pro; later starts take about 10.
- Android apps come from APK files: open an `.apk` with Nestvane from Files or a share sheet, or choose
  *Install APK…* in the app's menu. An app installed in the first minute after Android starts shows its icon about
  11 seconds later; after that, at once.
- Android's shared storage is the folder *On My iPhone → Nestvane → Android* in the Files app.
- iOS asks for local network access only when an Android app connects to a device on your network.
- Deleting the app deletes Android's data with it.

## What is inside

The app contains an Android 17 system built from the Android Open Source Project with this repository's patch,
translated ahead of time, together with gfxstream and ANGLE. Their notices are inside the app, in `Licenses/`, and
attached to each release as `NOTICE-AOSP.txt` and `THIRD-PARTY-NOTICES.txt`.

A few parts of that Android system, iptables and libexif among them, are under the GNU General Public License or
the GNU Lesser General Public License. Their source is the Android Open Source Project at the tag
`android-17.0.0_r1` with this repository's patch applied. For three years from each release, AtomSprout will also
send a copy of that source to anyone who asks, for no more than the cost of the transfer: write to
<atomsprout@gmail.com>. How to build all of it yourself is in [building.md](building.md).
