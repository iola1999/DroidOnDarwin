# 决策记录: BoringSSL 与 SDK 结构布局不兼容

状态: implemented

## 问题

接入原版 Keystore 与软件 KeyMint 后，实际 AES 密钥生成触发 `stack corruption detected (-fstack-protector)`，随后 KeyMint 退出，Keystore 返回 `TRANSACTION_FAILED`。已有 SHA-256、AES-GCM 和 Framework JCA 测试通过，但未覆盖 SDK 组件在栈上分配的 HMAC 上下文。

重建库提交 `1e3b727981831886a0910cd9e82d5bbee4cc7029` 的 `EVP_MD_CTX` 内嵌 208 字节摘要状态，`HMAC_CTX` 为 704 字节。固定 SDK 原版 `HMAC_CTX_init` 只清理 104 字节；原版调用方按较小结构分配，调用重建库时写出分配范围。反汇编同时显示 SDK 的 `EVP_AEAD_CTX_zero` 清理 584 字节，旧重建库对应值为 576 字节。

## 决策

采用 Android 16 初始发布的 BoringSSL 提交 `ecc1358826150d6a1851c517c325b0e6c0e1b8be`，其头文件使用 32 字节 `EVP_MD_CTX`、104 字节 `HMAC_CTX` 和 584 字节 `EVP_AEAD_CTX`，与上述 SDK 函数布局一致。归档 SHA-256 为 `e1208516f7419fdcb4ebcb8d0afd7153d0abba7d4eee52bc57a5136cb1263557`。继续使用 `FIPS=OFF` 和 ARM64 汇编执行，重新编译依赖这些头文件的本地检查。

来源为 [Android 16 BoringSSL 源码](https://android.googlesource.com/platform/external/boringssl/+/ecc1358826150d6a1851c517c325b0e6c0e1b8be/src/include/openssl/digest.h)、固定 SDK 的 `libcrypto.so`，以及实际 KeyMint 调用日志。明确的结构大小检查阻止后续误用不兼容头文件；新增栈上 HMAC 运算和已知结果检查，并重跑原版 Keystore 密钥生成、加解密及跨启动读取。

### 本机验证

1. 编译期检查 SDK 已确认的上下文大小，运行栈上 HMAC-SHA256 与已有加密结果检查。
2. 原版 KeyMint 生成密钥、Keystore 持久保存，新的 Java 进程读取并解密既有数据。
3. Android 用户密钥初始化继续推进；CLI/GCD 完整桌面验收由桌面提案跟踪。

2026-10-10 04:57 的 Mac 完整启动中，上下文大小、栈上 HMAC 与原有加密检查通过，原版 Keystore 生成 AES 密钥并保存、解密成功（`restored=false`）。LockSettings 继续创建用户解锁密钥。随后 system_server 在 Bionic 读取 `CTR_EL0` 时收到 SIGILL，该次未完成桌面启动。随后 05:10 的新宿主进程与新 Android 启动读取同一 AES 密钥并解密成功（`restored=true`）。结果摘要保存于 [启动映射对照](../../../../experiments/native-linux/results-macos-desktop-mapping-baseline.json)。

Android 归档省略上游测试和 PKI 子模块，构建脚本保留原有 crypto/ssl 目标并去掉依赖这些缺失目录的 CMake 目标。该版本的算法自测入口为 `BORINGSSL_self_test`。库构建、SDK 结构检查和真实 Keystore 调用都已执行；其余 ABI 风险继续由完整系统回归检查。

## 曾考虑的替代方案

- 继续使用较新库并只修改 KeyMint：其他预编译组件也可能使用这些结构，逐个修改调用方无法处理完整兼容范围。
- 关闭栈保护：会保留越界写入，不能修复调用契约。
- 只运行同版头文件编译的加密自测：覆盖算法结果，但缺少与预编译 SDK 的结构大小检查。

## 影响

结构大小核对覆盖已确认的公开上下文，其他 API 和预编译库仍需要实际运行验证。关闭 FIPS 后的构建没有原版库的 FIPS 认证状态。
