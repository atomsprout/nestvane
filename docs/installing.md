# Installing the release

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

Whether a free Apple ID can sign with these two capabilities has not been tried. Everything here was done with a paid
developer account.

## Signing and installing

```sh
unzip Nestvane-1.0.ipa
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

Each release lists the SHA-256 of its IPA. Compare it with `shasum -a 256 Nestvane-1.0.ipa`.

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
translated ahead of time, together with gfxstream and ANGLE. Their notices are in `NOTICE-AOSP.txt`, attached to
each release. How to build all of it yourself is in [building.md](building.md).
