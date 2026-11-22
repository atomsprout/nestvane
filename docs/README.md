# Nestvane documentation

Nestvane runs a full Android 17 system, built from the Android Open Source Project (AOSP), inside an ordinary iOS app.
It uses no just-in-time compiler, no jailbreak and no hypervisor.
iOS does not let an app create executable memory, so every guest instruction that runs natively was translated before the app was built and ships as signed code inside it.

These documents describe how that works, how the parts are built, and what state the project is in.
They are written for a developer who has not seen the project before.

中文版见 [zh-CN/README.md](zh-CN/README.md)。

## The documents

| Document | What it covers |
| --- | --- |
| [architecture.md](architecture.md) | The whole system in one read: the constraint, the four pieces, how a guest process runs and how a system call travels. |
| [static-rewriting.md](static-rewriting.md) | How a guest ARM64 ELF file becomes host code: reserved registers, the rewriting rules, stable load addresses, packaging, and code without a translation. |
| [kernel-layer.md](kernel-layer.md) | The Linux kernel that the guest sees, implemented in user space: tasks, memory, files and mounts, Binder, signals, sockets, SELinux, seccomp, devices. |
| [devices.md](devices.md) | Graphics, storage, network, sound, battery, time zone and touch: where the guest meets the phone. |
| [building.md](building.md) | What is needed to build the app and the order of the steps, quoted from the scripts. |
| [design-notes.md](design-notes.md) | Decisions and pitfalls: a selection of problems met on the way, each as problem, cause and fix. |
| [status.md](status.md) | What works, what does not, measured numbers and known limitations. |

A suggested order for a first read is architecture, static rewriting, kernel layer, devices, then status.
The building document and the design notes can be read on their own.

## Terms used throughout

| Term | Meaning here |
| --- | --- |
| Host | The iOS app and the phone it runs on. Host code is code the iOS loader accepted and signed. |
| Guest | The Android system and everything in it: its processes, memory, files and addresses. |
| Guest address | An address as an Android process sees it. It is not a host address; a table maps one to the other. |
| Module | One guest ELF file (an executable, a shared library, or a file of compiled Java code) together with its translation. |
| Translation | The host code produced ahead of time from a module's instructions. |
| Rewriter | The program that produces a translation: [`experiments/g0/e3/rewrite.py`](../experiments/g0/e3/rewrite.py). |
| Kernel layer | The user-space implementation of the Linux kernel interface: [`runtime/ProbeLinux.cpp`](../runtime/ProbeLinux.cpp) and the files around it. |
| Interpreter | The ARM64 interpreter that runs guest code which has no translation: [`runtime/AArch64.cpp`](../runtime/AArch64.cpp). |
| Guest root | The directory tree the guest boots from, assembled on the build side and shipped as a resource of the app. |
| Reference emulator | The standard Android emulator running the same Android build on a real Linux kernel. It is used to collect inputs and to compare behaviour. |
| APEX | Android's packaging format for updatable system modules. Each is a read-only ext4 image mounted under `/apex`. |
| ART | The Android Runtime, which runs Java and Kotlin code. Its compiler, `dex2oat`, produces ARM64 code in `.oat` and `.odex` files. |
| Zygote | The Android process from which every app process is forked. |
| Binder | Android's inter-process call mechanism, a kernel driver on a real device. |

## Where things are in the repository

| Path | Contents |
| --- | --- |
| [`runtime/`](../runtime) | The runtime linked into the app: kernel layer, interpreter, native-code entry, guest address space, device models. |
| [`ios/`](../ios) | The iOS app: the view that shows Android's display, touch, APK hand-off, settings, sound output. |
| [`tools/`](../tools) | Build and validation scripts, and the patches applied to gfxstream. |
| [`experiments/aosp/`](../experiments/aosp) | The edits to the AOSP tree and the scripts that build it. |
| [`experiments/g0/`](../experiments/g0) | The feasibility experiments, including the rewriter. |
| [`fixtures/`](../fixtures) | Small guest programs, resource overlays, the home-screen app and test apps. |
| [`results/`](../results) | Records of measurements and device reports. |
| `generated/` | Build outputs: the guest root, the translated code, the Xcode project inputs. Not checked in. |

## The records behind these documents

The documents summarise; the records hold the detail.

- [`results/G0.md`](../results/G0.md) is the running lab record, in Chinese. "G0" is the name the project gave to the set of five feasibility experiments (address space, copy-on-write remapping, register reservation and rewriting, fault recovery, loading signed code) that had to pass before the current design was adopted. The record continues past those experiments through every later measurement. Its sub-experiments are named E1 to E5; the rewriter lives under `experiments/g0/e3` because it began as the third.
- [`PLAN.md`](../PLAN.md) is the project plan, in Chinese.
- [`AOT.md`](../AOT.md) and [`REVIEW.md`](../REVIEW.md) are earlier design notes, in Chinese.
- The `results/P1.md` and `results/P2-*.md` files record an earlier phase in which all guest code was interpreted. The kernel layer was built in that phase and is still the one in use.

## Scope and accuracy

Every number in these documents comes from `results/G0.md` and names the device it was measured on.
Where something has not been verified, the text says so.
No license has been chosen for the project yet, and these documents do not state one.

## Contact

- Website: <https://atomsprout.com/nestvane/>
- Repository: <https://github.com/atomsprout/nestvane>
- Discord: <https://discord.gg/eGzEaP6TzR>
- Telegram: <https://t.me/atomsprout>
- Email: <atomsprout@gmail.com>
