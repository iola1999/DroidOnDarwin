# 决策记录: ART volatile 浮点字段编译终止

状态: proposed

## 问题

Framework JIT 阈值降低到 1024 后，system_server 的 Jit thread pool 在 StoreRelease → UseScratchRegisterScope::AcquireFrom 中发送 SIGABRT。NCE 解码高地址对象占用一个通用临时寄存器，StoreRelease 另取一个地址寄存器，浮点值搬运又需要一个；VIXL 的两个通用临时寄存器无法满足该路径。LoadAcquire 存在相同需求。此前检查覆盖普通浮点字段，缺少 volatile 浮点字段。

## 提案

NCE 的 volatile 字段访问先在已有地址寄存器中累加字段偏移，以零偏移内存操作数调用 LoadAcquire / StoreRelease。这两个通用函数在地址已经完成计算时直接使用基址，保留一个通用临时寄存器用于浮点值搬运。原有 acquire/release 指令与空引用异常记录继续执行。

新增专项 Java 类，显式编译实例及静态 float/double 字段读写，检查正负零、有限数、无穷和 NaN 的原始位模式，以及空接收者异常。编译成功后再运行结果检查，不能只依赖解释器覆盖。

## 曾考虑的替代方案

- 恢复较高 JIT 阈值：只能推迟触发，仍可能在后续使用相同代码时终止。
- 拒绝编译 volatile 浮点访问：可绕过本次错误，但字段访问可以在现有两个寄存器范围内完成，优先修复生成逻辑。
- 扩大临时寄存器集合：会影响寄存器分配及调用约定，当前无需增加此复杂度。

## 验收标准

专项检查确认各方法已编译，双向字段读写、原始位模式和空引用异常通过；完整系统提前 JIT 启动继续验证。真机随桌面整包集中测试。

## 风险

零偏移路径仍需保持源寄存器和目标地址的生存期；回归同时覆盖实例和静态字段、读取和写入。此次修复不声明 JIT 对完整桌面的性能收益。

### 本机专项回归（2026-10-10 07:44）

八个方法均安装 JIT 入口后，16 组 float/double 位模式的实例与静态字段双向读写、四次空接收者异常均通过。完整 Framework 启动已越过此前终止位置，仍在执行系统服务与桌面检查。故障调用栈保存在 [失败记录](../../../../experiments/native-linux/results-macos-art-volatile-failure.json)。

实现复用原有地址寄存器和 LoadAcquire / StoreRelease；没有扩展寄存器集合或添加字段类型的解释器分支。Linux 的 SIGABRT 调用栈日志仅在客体主动发送终止信号时输出，使用现有用户内存读取接口，最多读取 24 层。

完整系统随后发现另一个寄存器耗尽位置：VisitArrayGet → MacroAssembler::LoadStoreMacro → AcquireSameSizeAs。字符串压缩标志占用的寄存器在 TBNZ 发出后已经无后续用途，之前仍保留到整个作用域结束；常量索引超过加载指令的立即数范围时，VIXL 无法申请计算地址所需的临时寄存器。现在在分支后释放该临时寄存器。新增 charAt(8192) 的压缩字符串和 UTF-16 检查，以及短字符串越界和空引用异常；完整启动继续复测。
