# AArch64 Difftest 寄存器同步说明

## 背景

ARM gem5 difftest 由 gem5 执行 DUT 指令，同时让 QEMU 执行一条参考指令。发生异常时，异常入口由 gem5 本地完成，然后 gem5 在 QEMU 继续执行异常向量前调用 `syncDifftestState()`，把架构状态写入 QEMU。因此，QEMU 必须提供接口同步那些没有由 QEMU 自己执行异常入口而产生的状态。

gem5 的相关入口为：

- `BaseCPU::syncDifftestState()`：同步普通指令和异常边界的状态；
- `BaseCPU::difftestSyncTrapState()`：gem5 安装陷阱 PC、PSTATE、SPSR、ELR、FAR、ESR 后立即同步；
- `BaseCPU::diffWithReference()`：执行一条 QEMU 参考指令并比较结果。

`include/qemu/difftest.h` 中的枚举属于共享库 ABI。新增 ID 都追加在末尾，以保证已有数值不变，并与 gem5 的 `DifftestMiscReg` 顺序一致。

## 同步寄存器

### 执行和浮点状态

- `PSTATE`：当前异常级别、条件码、中断屏蔽位、执行状态和分支类型状态。QEMU 写入后必须重建 hflags，因为它会影响翻译模式。
- `FPSR`：浮点异常累计状态。
- `FPCR`：浮点舍入模式、异常屏蔽和 SIMD 行为控制。

在稳定的逐条执行阶段，gem5 和 QEMU 确实会分别执行同一条浮点指令，理论上这些状态应该自然保持一致。这里增加同步并不是为了掩盖正常指令执行差异，而是为了处理执行边界：

1. gem5 从 SPEC checkpoint 恢复时已经拥有完整的 PSTATE、FPCR 和 FPSR，而 QEMU reference 是新建的 reset CPU，必须先恢复到相同的起点；
2. 发生异常时，异常入口由 gem5 完成，QEMU 不会执行同一段异常入口代码，因此 PSTATE 的异常级别和屏蔽位不会自动变化；
3. 对 strict-ordered 指令，gem5 可能直接把最终状态同步给 QEMU，而不是让 QEMU 重复执行可能包含设备或不可复现副作用的指令。

其中 PSTATE 会影响 QEMU 的翻译模式，所以写入后必须重建 hflags。FPCR/FPSR 在普通异常入口中通常不会变化，但在 checkpoint 初始恢复和完整状态重同步时仍必须传递，避免 QEMU 继续使用 reset 或旧值。正常浮点指令仍然由两边独立执行，并在 `diffWithReference()` 中比较 FPSR、FPCR 和向量寄存器；如果执行结果不同，difftest 仍会报告差异。

### 异常返回上下文

- `SPSR_EL1/2/3`：异常发生时保存的旧 PSTATE，供 `ERET` 恢复；
- `ELR_EL1/2/3`：异常返回地址；
- `SP_EL0/1/2/3`：各异常级别的 banked 栈指针。

gem5 的异常入口会修改这些寄存器，但 QEMU 没有执行同一个异常入口。如果不传递它们，`ERET`、嵌套异常以及异常向量的第一次栈访问都可能与 gem5 分叉。

### 线程标识状态

- `TPIDR_EL0`：用户态线程指针，Linux TLS 经常使用；
- `TPIDRRO_EL0`：用户态可读取的线程标识寄存器；
- `TPIDR_EL1`：内核线程或本地 CPU 上下文；
- `TPIDR_EL2`：hypervisor 上下文；
- `TPIDR_EL3`：安全监控器或安全固件上下文。

gem5 的 Linux workload 会设置 TLS，异常向量和普通代码也可能通过 `MRS` 读取这些寄存器。若 QEMU 保留旧值，差异通常要到后续 `MRS` 将值写入 GPR 时才暴露，导致 mismatch 看起来像普通 GPR 错误。当前 gem5 还在提交 TPIDR 写入指令时比较对应值，以便定位真正引入差异的指令。

### 异常故障信息

- `FAR_EL1/2/3`：对应异常级别的故障地址；
- `ESR_EL1/2/3`：异常类别和 syndrome，例如异常类型、指令长度和访问故障信息。

gem5 在 `ArmFault::getFaultAddrReg64()` 和 `getSyndromeReg64()` 中选择目标寄存器，并在异常边界写入。异常向量可能立即执行 `MRS FAR_ELx` 或 `MRS ESR_ELx`，因此 QEMU 必须获得 gem5 产生的值。

EL1 名称存在 VHE 别名：当处理器处于 EL2 且 `HCR_EL2.E2H` 置位时，访问 `FAR_EL1`/`ESR_EL1` 可能实际观察 EL2 bank。gem5 使用架构访问器处理该重定向，QEMU 的 `difftest_el1_fault_reg_el()` 采用相同规则。

### 实现相关状态

`CTR_EL0` 是只读的 cache 类型寄存器，描述最小指令/数据 cache line、cache 组织方式以及 `DIC`/`IDC` 一致性属性。

gem5 根据 cache model 和 cache line 大小动态计算该值，而不是从普通 misc-register 存储槽读取。QEMU 将 `CTR_EL0` 注册为 `ARM_CP_CONST`，翻译器可能在 TB 生成时把它的值常量折叠进去。

因此 QEMU setter 必须：

1. 更新 `ARMCPU::ctr`；
2. 更新 `CTR_EL0` descriptor 的 `resetvalue`；
3. 值发生变化时刷新已有 TB。

这样后续 `MRS CTR_EL0` 以及依赖 `IDC/DIC` 的 cache maintenance 行为才会与 gem5 一致。

## 为什么只同步 GPR/PC 不够

GPR、PC 和向量寄存器只能表示普通数据处理和控制流状态。当前 gem5 difftest 还会在本地完成异常入口、设置线程上下文，并使用自己的 cache model 计算 `CTR_EL0`。这些变化不会自动在 QEMU 中发生，因此必须通过 misc-register ABI 传递。

同步关系可以概括为：

```text
gem5 执行指令或完成异常入口
        -> 同步 GPR、向量寄存器和 misc 状态
        -> QEMU 执行一条参考指令
        -> 比较 PC、GPR、向量寄存器和相关状态
```

同一边界上的低地址 RAM alias 和 `EXCP_YIELD` 处理，也是在匹配 gem5 checkpoint 的内存视图和单步完成语义。

## 修改评价与验证重点

TPIDR、FAR/ESR、SPSR/ELR/SP 的同步直接对应 gem5 已完成的架构状态修改，属于必要的 difftest 配套。`EXCP_YIELD` 的处理对应 AArch64 translator 先更新 PC 再退出的实现，也符合单步语义。

`CTR_EL0` 同步在功能上有必要，但它修改 QEMU 内部 descriptor 并刷新所有 TB，调用上下文必须满足 QEMU 的 serial/exclusive 要求，需通过单步和异常回归验证。

建议覆盖以下测试：

- 异常向量中的 `MRS FAR_ELx` 和 `MRS ESR_ELx`；
- Linux/TLS 场景中的 `MRS TPIDR_EL0`；
- 异常返回 `ERET`；
- `MRS CTR_EL0` 及 cache maintenance 指令；
- AArch64 `YIELD/WFE` 单步执行。
