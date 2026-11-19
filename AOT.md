# AOT：出发点与 nestvane 的第一版设计

> 这是项目第一版设计（解释器加少量预编译热点）时的记录，留作历史。现在的执行方式是保留寄存器重编 AOSP 加静态改写，见 [docs/zh-CN/static-rewriting.md](docs/zh-CN/static-rewriting.md)。

## 出发点

[两仪（Twoyi）](https://twoyi.app/)的网站说明，它的 iOS 版不用 JIT，Android 的代码在构建时被提前翻译成原生 ARM64 指令。这说明在 iOS 上，预编译的宿主代码可以和解释执行并存。对 nestvane 最重要的是两点：**同一 ARM64 指令集仍需要维持客体状态，以及预编译代码需要可验证地退回解释器。**

Android ART 的 AOT 和这里的宿主 AOT 各有一个边界。前者把 DEX 变成客体代码，后者把选定的客体 ARM64 翻译成随 iOS App 签名的宿主代码。新装 APK 的原生库和 ART 后生成的客体代码走解释器。即便把某个应用的代码离线翻译，Linux ABI、图形和其他宿主桥接仍由运行时提供。

两仪没有公开它的实现。完整的翻译方法、热点怎样选择、失效策略和实测性能，这里都不知道，下面的设计是 nestvane 自己的。

## nestvane 的首版实现决定

1. **以解释器为语义基线。** 指令解码后的操作统一落到客体 CPU、内存与事件接口。先生成少量 C/C++ 宿主函数交给 Clang 优化，以较低成本验证路径。只有测到生成代码质量是瓶颈，才评估专用汇编生成器或 LLVM IR。单步和块引擎共用指令语义，独立 Linux 对照用于发现共同错误。
2. **用显式身份查找入口。** 以构建产物的内容摘要、目标 ELF 段/模块内偏移、翻译器 ABI 版本索引宿主函数。装载时匹配静态表，执行时验证客体 PC 当前仍对应有效映射。缓存键须区分地址空间和映射世代。ASLR 后从客体基址和相对位置重算。普通数据缓存与 AOT 可执行映射失效分开处理。
3. **每次退出带回可继续执行的状态。** 至少返回客体下一 PC、退出原因和必要的寄存器/标志。系统调用、同步异常、未知控制流和执行预算耗尽交回调度器。遇到写入或重新映射，按受影响的页使相关代码记录失效。首轮可将可变代码映射统一交给解释器。
4. **性能优化以后再扩覆盖。** 先证明两个自制热点，包括通过运行时访问客体内存的场景。跑通 Android 17 后，再根据真实时间分布选择 `linker64`、`libc` 或 `libart` 等候选。每次客体系统库升级，重新生成、交叉验证并记录 AOT 表。Apple 的可执行内存保护仍适用，宿主代码生成留在构建期。

### 性能口径

优先测时间花在哪，再测 AOT 的局部加速。比如某热点原本占完整任务耗时 40%，即使热点快 4 倍，按 Amdahl 定律估算，任务整体也只快约 1.43 倍。这是算例，假设其它路径开销不变。

首轮统计至少包含解析/块查找、宿主 AOT 时间、客体内存读写、回退次数与原因、系统调用时间和图形路径时间。客体指令命中率用来选优化目标，最终以真机的启动、交互和任务完成时间衡量收益。


### 预解码指令处理器对照

`tools/probe-prepared-dispatch.cpp` 将两个不可写 ELF 夹具解码成连续记录，由已编译的处理器直接调用后继处理器。实验对照现有块解释，逐项核对 CPU 状态、跨页读写和故障退出。它还未接入 Android 执行路径。

```sh
tools/build-fixtures.sh
xcrun clang++ -O3 -std=c++20 -Iruntime runtime/Runtime.cpp tools/no-aot.cpp \
  tools/probe-prepared-dispatch.cpp -o generated/probe-prepared-dispatch
generated/probe-prepared-dispatch generated/guests
```

## 资料

- Twoyi 产品说明 : https://twoyi.app/
- Apple 运行时保护 : https://support.apple.com/guide/security/sec15bfe098e/web
- Android ART 配置 : https://source.android.com/docs/core/runtime/configure
- QEMU TCI : https://github.com/qemu/qemu/blob/master/tcg/tci/README
