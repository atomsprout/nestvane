# P2 原版 Keystore2 和软件 KeyMint 启动

2026-11-18。使用同一 ARM64 API 37 SDK 镜像的原版组件和 VINTF 声明，在 iPhone 11 的开发签名 Release 构建及 Mac 上复测。Android 客体使用自己的套接字、Binder、挂载和 SELinux 查询接口。

- 原版 `apexd-bootstrap` 挂载 Runtime 与 ICU APEX。原版 `init` 在参考域中启动 Keystore2 和软件 KeyMint，原版服务管理器从相应的 VINTF 清单中识别两项服务。真机的独立 ART 执行、持久化 `/data` 和两个既有 vdc checkpoint RPC 均通过。[iPhone 11 日志](p2-keystore-iphone11.txt)
- 原版 `vdc keymaster earlyBootEnded` 已收到经 `vold` 转发的 Keystore 应答，进程退出为 0，Keystore 返回服务错误 4。原版 `init` 继续到常规 `apexd`，后者发布 activated 状态。[Mac 日志](p2-keystore-host.txt)
- 目前仍缺少 Keystore 的 SELinux 类兼容。跟踪到客体读取 `/sys/fs/selinux/class/keystore2/index` 得到 ENOENT。下一步验证原版 Keystore 完成启动阶段操作并设置 `keystore.module_hash.sent`，再推进类路径生成和 Zygote 启动。

## Keystore 启动条件已通过

- 原版 Keystore2 用参考策略完成了启动权限检查，经软件 KeyMint 成功完成 `earlyBootEnded`。客体 SQLite 通过文件锁和定位写入保存数据库，Mac 上分别从首次启动和重启后的快照提取该数据库，`PRAGMA integrity_check` 两次均返回 `ok`。
- 常规 `apexd` 从客体 loop 设备状态恢复 Runtime 和 ICU 两个 APEX。Keystore 得到真实模块信息并交给软件 KeyMint。Mac 和 iPhone 11 上的独立 Bionic 读取器均确认 `keystore.module_hash.sent=true`。原版 `casefolding_remover` 设置 `ro.casefolding.adjusted=1`，原版 `init` 越过两个属性等待，进入用户 0 初始化。[iPhone 11](p2-keystore-policy-iphone11.txt) · [Mac](p2-keystore-policy-host.txt)
- iPhone 11 同一轮通过独立 ART DEX、持久化客体 `/data` 和两个既有 vdc checkpoint RPC。此构建的用户 0 初始化停在 `vold` 的数据目录准备阶段。
