# 决策记录: LKL 线程 join 回归的 GCD 返回值

状态: implemented

## 问题

接续检查运行 `python3 -B scripts/test_lkl_host_join.py --output work/native-linux-nce/lkl-host-join-takeover-v1`。六个检查中，CLI 正常 join、CLI 旧实现的自 join 返回、CLI 新实现的 REQUIRE 终止和 GCD 正常 join 成功；GCD 的两个自 join 检查在 fixture 的 `assert(error == EDEADLK)` 处终止，尚未调用生产 `thread_join`。结果及来源摘要保存在该目录的 `result.json`。

回归需要检查实际 `pthread_join` 失败后，旧实现返回 -1，新实现触发现有 REQUIRE 并收到 SIGABRT。固定 EDEADLK 的前置条件超出了这个契约；CLI 主线程与 GCD 工作线程的实际错误码分别记录。

## 决策

fixture 保留真实 `pthread_join(pthread_self(), NULL)`，要求它返回非零，记录实际错误码后调用 `nce_lkl_host.thread_join`。删除固定 EDEADLK 字段和对应比较，继续严格检查旧实现的 -1 返回、新实现的完整 REQUIRE 日志、SIGABRT、正常线程的结果发布及全部宿主进程回收。生产 `lkl-host.c` 保持。

### 本机验证

`python3 -B scripts/test_lkl_host_join.py --output work/native-linux-nce/lkl-host-join-takeover-v2` 的六项检查成功。环境为 Apple Silicon、macOS 27.0，Apple clang 21.0.0；实际源码、生成 host_ops 头、编译命令、逐入口 stdout/stderr 和来源摘要见该目录的 `result.json`。CLI 自 join 返回 11（EDEADLK），GCD 自 join 返回 22（EINVAL）。两个旧实现入口返回 -1 并完成 fixture；两个新实现入口实际输出 `LKL host failure: pthread_join((pthread_t)t, NULL) == 0`，退出码均为 -6（SIGABRT）。两个正常 join 返回 0，读取的 worker_value 均为 37。六个进程均已回收、进程组为空，运行前后的五项来源摘要保持。

`git diff --check` 成功。v1 的两个失败继续保留，v2 没有通过接受 fixture 前置断言失败来修改成功条件。

### 复杂度审查

回归直接使用现有宿主操作表、真实 Darwin pthread 和 GCD，旧实现控制仅替换原返回表达式。保留实际错误码一个字段，去掉不属于生产契约的预设错误码字段；继续检查完整失败日志和进程退出。移除这项回归会失去宿主 thread_join 失败必须终止的实际入口检查。此次只调整错误码假设，未新增状态或生产接口。

## 曾考虑的替代方案

将 GCD 检查移到独立 joinable pthread：能够检查 EDEADLK，但会改变本次 GCD 调用入口。按错误契约检查队列线程的真实返回值，继续覆盖两个宿主入口。

## 影响

该回归仅检查实际宿主操作表的 thread_create/thread_join，没有启动 Linux 内核、操作磁盘或证明完整停止流程。BPF 固定引用后的终止释放、内核调用线程和 timer 生命周期继续单独检查。
