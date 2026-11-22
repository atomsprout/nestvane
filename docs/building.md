# Building

This document lists what a build needs and the order of the steps.
The commands are quoted from the scripts' own usage lines; read each script before running it, since several assume a particular machine layout.

A complete build is not self-contained in this repository.
It needs an AOSP checkout and a Linux machine to build it, a reference emulator running the result, and the gfxstream and ANGLE sources.
Each of these is marked below as **not in the repository**.
There is no single command that performs the whole build, and no continuous integration; the steps were run by hand and their results are recorded in [`results/G0.md`](../results/G0.md).

## What is needed

| Requirement | Used for |
| --- | --- |
| macOS with Xcode | Building the app, and the Apple `clang` that assembles the translated code. |
| [XcodeGen](https://github.com/yonaskolb/XcodeGen) | Generating `nestvane.xcodeproj` from [`project.yml`](../project.yml). |
| LLD (`ld.lld` on the `PATH`) | Linking the small guest test programs under [`fixtures/`](../fixtures). |
| Python 3 | The build scripts. |
| The Android SDK build tools (`aapt2`, `zipalign`, `apksigner`, `d8`) | Building the resource overlays and test apps. |
| The Android SDK emulator and `adb` | The reference emulator. |
| A Linux build machine with an AOSP `android-17.0.0_r1` checkout — **not in the repository** | Building Android with reserved registers. The record used a 32-core cloud machine running Ubuntu 24.04. |
| The LLVM sources at the toolchain's revision, `toolchain/llvm_android`, and an NDK — **not in the repository** | Rebuilding the toolchain's prebuilt runtime libraries. |
| gfxstream and ANGLE sources — **not in the repository** | The renderer. |
| An Apple developer team with the Extended Virtual Addressing and Increased Memory Limit capabilities | Signing the app ([`ios/Native.entitlements`](../ios/Native.entitlements)). |
| A device with iOS 26 or later | Running it. |

## Step 1: build Android with reserved registers

On the Linux machine, in the AOSP checkout. The scripts are in [`experiments/aosp/`](../experiments/aosp).

Apply the edits to the tree (26 edits over 21 files):

```
usage: patch.py <aosp root>      (safe to run again; every edit checks whether it is already applied)
```

Rebuild the runtime libraries that AOSP takes as prebuilts (compiler-rt builtins, libunwind, libc++, libc++abi, the minimal UBSan runtime) with the same reserved registers, and install them over the prebuilts:

```
build-runtimes.sh <aosp root> <llvm-project at the toolchain's base revision> <llvm_android> <ndk> <clang version>
```

Build. The script selects the product `sdk_phone16k_arm64-cp2a-userdebug`, the 16 KiB-page emulator image, and expects the checkout at `~/aosp` and writes logs to `~/logs`:

```
build.sh <log name> [out directory inside the tree] -- <m targets...>
```

Check the result for code that still names the reserved registers:

```
usage: audit.py <tree> [--top N]
```

The patch also changes the build so that the boot image, the system server and the apps are compiled ahead of time in full (`speed`), because without a JIT a method that was not compiled would be interpreted for good.

## Step 2: boot the image in the reference emulator

**Not in the repository:** running the built image in the Android emulator on a Mac, with `adb root` available.
The repository has no script for starting the emulator with a custom image.
The same image boots normally there, because on a real kernel the patched dynamic linker finds no address table and behaves as stock.

The emulator serves two purposes: it is where the next two scripts collect files and policy decisions from, and it is the reference for correct behaviour.

## Step 3: collect the boot inputs

[`tools/prepare-init-root.py`](../tools/prepare-init-root.py) pulls from the running emulator what the guest needs to boot: `init`, libraries, property contexts, the init scripts of each partition, the APEX modules, and the SELinux decisions queried from the reference kernel (written under `nestvane-metadata/`).

The script has no usage line of its own; its argument parser takes a positional `serial` and the options `--readelf` (default `llvm-readelf`), `--small-art-heap` and `--output`. The earlier prototype notes ([`results/PROTOTYPE.md`](../results/PROTOTYPE.md), in Chinese) show it run as:

```
tools/prepare-init-root.py emulator-5580
```

`serial` is the emulator's `adb` serial, for example `emulator-5580`. The default output is `generated/android17-init-root`; the output path must not exist yet.

## Step 4: assemble the guest root

[`tools/prepare-boot-image.py`](../tools/prepare-boot-image.py) builds the directory the guest boots from: it adds the framework apps (launcher, SystemUI, Settings, package installer and others), takes the compiled code, adds the services the guest needs and removes declarations of hardware it does not have.

The arguments that matter here, with the help text of the script's argument parser:

```
serial
--root ROOT                  Unpacked boot input used to check the reference build
--output OUTPUT
--framework-diagnostic       Produce a complete offline diagnostic guest root using the compiled image
--framework-apps             Include launcher, SystemUI and package-management apps from the matching reference
--prebuilt-code              The image carries compiled code for its own collector (a build patched by
                             experiments/aosp/patch.py): keep its boot image and take its compiled code
--tethering-loader PATH      Use the output of tools/build-netbpfload.py and run the standard BPF init actions
```

`--prebuilt-code` is the option for an image from step 1; the script requires `--framework-diagnostic` with it, and `--framework-apps` requires `--framework-diagnostic` too.
The header of `prepare-ios-native.sh` names the sequence as "`tools/prepare-init-root.py`, then `tools/prepare-boot-image.py --prebuilt-code`".
The exact command line used for the shipped build is not written down in a script.

[`tools/build-netbpfload.py`](../tools/build-netbpfload.py) builds the loader for Android's network eBPF programs that `--tethering-loader` takes. It fetches sources from the AOSP repository and needs `--serial`, `--toolchain` (an NDK toolchain directory) and `--output`.

## Step 5: build the overlays and the home-screen app

```
usage: build-overlays.sh <root>...   (needs the Android SDK build tools; signs with the debug key)
```

[`tools/build-overlays.sh`](../tools/build-overlays.sh) compiles the resource overlays under `fixtures/overlay-*` and puts them into the guest root as static overlays of the product partition; they change framework settings without rebuilding the framework (for example, the fixed network configuration).
It also builds [`fixtures/home`](../fixtures/home), an app without code that gives the home screen its clock and its default layout.
The SDK location is taken from `ANDROID_HOME`, or `~/Library/Android/sdk`.

## Step 6: translate the code and generate the Xcode project

```
usage: prepare-ios-native.sh <guest root> [<tree with unpacked APEXes>...]
```

[`tools/prepare-ios-native.sh`](../tools/prepare-ios-native.sh) does, in order:

1. builds the small helper programs that run inside the guest ([`tools/prepare-property-reader.sh`](../tools/prepare-property-reader.sh)), with 16 KiB pages;
2. builds the test fixtures ([`tools/build-fixtures.sh`](../tools/build-fixtures.sh));
3. translates every ELF file of the guest root for iOS devices;
4. copies the guest root to `generated/native-app/android17-init-root`, where the app project picks it up as a resource;
5. runs `xcodegen generate --spec project.yml`.

The translation in item 3 is [`tools/build-native-code.py`](../tools/build-native-code.py):

```
usage: build-native-code.py <output directory> <tree>... [--only <substring>]... [--ios] [--used <list>]
```

It writes `<output>/modules.c`, `<output>/addresses.txt` and, for iOS, the frameworks `<output>/ios/lib/NestvaneCode<n>.framework`.
The second and later `<tree>` arguments are directories with unpacked APEX modules from the build: the files in them only have to hold the same bytes as the image the guest boots, because modules are matched by content.
Translations are kept between runs while the file, its address and the rewriter stay the same. The script's own comment puts a full translation at a quarter of an hour.

`--used <list>` leaves translations out of the frameworks. The list is made of the lines `native module used: <key> <base> <name>` that the runtime prints when run with `NESTVANE_NATIVE_USED=1`. `prepare-ios-native.sh` does not pass this option; the smaller build recorded in `results/G0.md` was made by running `build-native-code.py` with it.

## Step 7: build the renderer

**Not in the repository:** the gfxstream and ANGLE sources and their builds.
What the repository holds is:

- the patches for gfxstream in [`tools/patches/`](../tools/patches), which the record says are applied in the order they were added;
- the GN arguments for an ANGLE build for iOS devices with only the Metal backend, [`tools/angle-iphoneos.gn`](../tools/angle-iphoneos.gn);
- the revisions and commands that were used, in [`results/P2-network.md`](../results/P2-network.md) (in Chinese).

## Step 8: build the app and embed the renderer

Open `nestvane.xcodeproj` and build the `nestvane-native` scheme for a device, choosing a development team in Xcode.
The target links the 12 `NestvaneCode` frameworks and carries the guest root as a resource.
(The other target, `nestvane`, is the earlier validation app that interprets everything.)

Then add the renderer frameworks to the built app and sign it again:

```
Usage: embed-ios-graphics.sh input.app gfxstream.dylib angle-build-directory output.app
```

[`tools/embed-ios-graphics.sh`](../tools/embed-ios-graphics.sh) checks that the frameworks' minimum iOS version does not exceed the app's, wraps the gfxstream library as `NestvaneGfxstream.framework`, copies ANGLE's `libEGL` and `libGLESv2` frameworks, and signs everything with the Apple Development identity the input app was signed with. The output path must end in `.app` and must not exist.

How the resulting app is installed on a device and how TestFlight builds are produced is not scripted in the repository.

## Checking the runtime on the Mac

```
tools/validate-host.sh
```

[`tools/validate-host.sh`](../tools/validate-host.sh) takes no arguments. It builds the fixtures and then compiles and runs three test programs on the Mac: the interpreter validation, the kernel layer's interface tests ([`tools/validate-boot-abi.cpp`](../tools/validate-boot-abi.cpp)), and the tests of the guest address space ([`tools/validate-guest-space.cpp`](../tools/validate-guest-space.cpp)).
It needs only Xcode's command-line tools and `ld.lld`, and is the quickest way to check a change to the runtime.

## Useful environment variables

These are read by the runtime or the app and are mentioned in the record.

| Variable | Effect |
| --- | --- |
| `NESTVANE_NATIVE_USED=1` | Lists the translations a session mapped, for `build-native-code.py --used`. |
| `NESTVANE_NATIVE_WORKERS=<n>` | The number of host threads that run native code beside the main loop; 0 for none. The default is 3. |
| `NESTVANE_DISPLAY_HZ=<n>` | The rate at which the guest composes and the app looks for frames. |
| `NESTVANE_DISPLAY_RATE=1` | Writes the composed and shown frames per second to `display-rate.txt`. |
| `NESTVANE_TRACE_DENIED=1` | Logs system calls that end in "permission denied", with where the path leads. |
| `NESTVANE_NO_SOUND=1` | Runs without the sound device. |
