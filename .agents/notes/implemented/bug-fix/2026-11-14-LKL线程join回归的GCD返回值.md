# 决策记录: LKL 线程 join 回归的 GCD 返回值

状态: implemented

## 问题

接续检查运行 `python3 -B scripts/test_lkl_host_join.py --output work/native-linux-nce/lkl-host-join-takeover-v1`。六个检查中，CLI 正常 join、CLI 旧实现的自 join 返回、CLI 新实现的 REQUIRE 终止和 GCD 正常 join 成功；GCD 的两个自 join 检查在 fixture 的 `assert(error == EDEADLK)` 处终止，尚未调用生产 `thread_join`。结果及来源摘要保存在该目录的 [result.json](../../../../work/native-linux-nce/lkl-host-join-takeover-v1/result.json)。

回归需要检查实际 `pthread_join` 失败后，旧实现返回 -1，新实现触发现有 REQUIRE 并收到 SIGABRT。旧 fixture 固定 EDEADLK 的前置条件超出了这个契约；CLI 主线程与 GCD 队列线程的实际错误码分别记录。

## 决策

较早的 `takeover-v2` fixture 保留真实 `pthread_join(pthread_self(), NULL)`，要求它返回非零，记录实际错误码后调用 `nce_lkl_host.thread_join`。该版本删除固定 EDEADLK 字段和对应比较，继续严格检查旧实现的 -1 返回、新实现的完整 REQUIRE 日志、SIGABRT、正常线程的结果发布及全部宿主进程回收。生产 `lkl-host.c` 保持。

`fedc97e` 保存的当前 fixture 由 CLI 主线程和 GCD 队列线程分别调用 `nce_lkl_host.thread_create`，在新建的 joinable worker 中执行两次真实自 join。该 worker 必须是非主线程，第一次 `pthread_join(pthread_self(), NULL)` 必须返回 EDEADLK，随后调用生产 `nce_lkl_host.thread_join`。控制版返回 -1 后通知调用方 join worker；修复版必须输出完整 REQUIRE 日志并以 SIGABRT 结束。当前 fixture 保留 `expected_EDEADLK` 字段与对应比较，这项前置条件仅用于新建 worker。

### 本机验证

`python3 -B scripts/test_lkl_host_join.py --output work/native-linux-nce/lkl-host-join-takeover-v2` 的六项检查成功。环境为 Apple Silicon、macOS 27.0，Apple clang 21.0.0；实际源码、生成 host_ops 头、编译命令、逐入口 stdout/stderr 和来源摘要见该目录的 [result.json](../../../../work/native-linux-nce/lkl-host-join-takeover-v2/result.json)。这份旧 fixture 在 CLI 主线程中自 join 返回 11（EDEADLK），在 GCD 队列线程中自 join 返回 22（EINVAL）。两个旧实现入口返回 -1 并完成 fixture；两个新实现入口实际输出 `LKL host failure: pthread_join((pthread_t)t, NULL) == 0`，退出码均为 -6（SIGABRT）。两个正常 join 返回 0，读取的 worker_value 均为 37。六个进程均已回收、进程组为空，运行前后的五项来源摘要保持。

当前 worker fixture 的[六项实际检查](../../../../../../../sdk-plain-validation-v1/lkl-host-join-v1/after-host-fix-v2/result.json)全部成功，报告 SHA-256 为 `aa09934c98d261aea220392e2208d7f342a728b2a66e9c29d453a1db887669d7`。报告中的脚本 SHA-256 与当前 `scripts/test_lkl_host_join.py` 相同，均为 `eb8641df5387da44c4db7b2e17a7af2212ac6144a996a38d80b73abb1c206e11`。CLI/GCD 新建 worker 的实际自 join 均返回 11（EDEADLK），两个控制版完成 worker join，两个修复版均以 -6（SIGABRT）结束；正常 join 的 worker_value 均为 37。六个 leader 均已回收、进程组为空，五项来源摘要前后保持。较早的[直接队列线程检查失败](../../../../../../../sdk-plain-validation-v1/lkl-host-join-v1/after-host-fix-v1/result.json)和[GCD 队列线程的 22 测量](../../../../../../../sdk-plain-validation-v1/lkl-host-join-v1/darwin-gcd-self-check-v1/result.json)继续保留。

`git diff --check` 成功。v1 的两个失败继续保留，v2 没有通过接受 fixture 前置断言失败来修改成功条件。

### 复杂度审查

回归直接使用现有宿主操作表、真实 Darwin pthread 和 GCD，旧实现控制仅替换原返回表达式。当前 worker fixture 用现有 `thread_create` 明确自 join 的线程条件，实际错误码和 `expected_EDEADLK` 均保留；通知仅用于安排 worker 的自 join 先于调用方 join，继续检查完整失败日志和进程退出。移除这项回归会失去宿主 thread_join 失败必须终止的实际入口检查。生产实现未新增状态或接口。

## 曾考虑的替代方案

直接在 CLI 主线程和 GCD 队列线程中按非零返回校验，保留了两种线程的实际错误码。当前 fixture 采用由两入口分别创建 joinable worker 的检查方式，CLI/GCD 调用生产 `thread_create` 和 `thread_join`，worker 内使用相同的自 join 前置条件。

## 影响

该回归仅检查实际宿主操作表的 thread_create/thread_join，没有启动 Linux 内核、操作磁盘或证明完整停止流程。BPF 固定引用后的终止释放、内核调用线程和 timer 生命周期继续单独检查。
