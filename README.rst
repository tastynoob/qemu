AArch64 SimPoint QEMU 使用说明
==============================

本分支为 AArch64 ``mini-virt`` raw-payload 环境提供 SimPoint 支持，包含两类工作模式：

* **profiling**：运行 workload，生成 SimPoint 3.2 Basic Block Vector。
* **checkpoint**：根据 SimPoint cluster 后得到的切点，生成可恢复的 CPU performance checkpoint。

checkpoint 只用于 CPU 性能测试，不是 QEMU migration，也不是完整 IO snapshot。它保存 RAM 和 CPU 体系结构状态，不保存 GIC、PL011 等设备内部状态。恢复时只需要最小设备环境，例如串口输出和架构 timer。

原始 QEMU README 保存在 ``README.upstream.rst``。

profiling/checkpoint 的实现规范、性能要求和验收标准见
``docs/a64-simpoint-flow.rst``。该文档优先于历史说明，后续实现不得偏离其中的
计数口径、TB 边界和性能约束。


基本环境
========

构建 QEMU system emulator 和插件：

.. code-block:: shell

  ninja -C build qemu-system-aarch64 contrib-plugins

示例命令中的 ``<payload.bin>`` 表示 AArch64 raw payload。``mini-virt`` 会把 ``-kernel`` 指定的 raw image 加载到物理地址 ``0x40000000``。对于 checkpoint restore，``mini-virt`` 也可以直接加载 ``.zst`` 压缩的 checkpoint image。

建议 profiling、checkpoint 和恢复测试都使用 deterministic icount：

.. code-block:: shell

  -icount shift=0,sleep=off

这样 guest timer 推进不依赖 host wall clock，有利于保证同一 workload 的 profiling 和 checkpoint 可复现。

``mini-virt`` 平台参数
======================

``mini-virt`` 默认使用全系统平台 ``fs``：

.. code-block:: shell

  -machine mini-virt,platform=fs

此模式把标准 PL011 UART 映射到 ``0x09000000``。裸机 workload 使用：

.. code-block:: shell

  -machine mini-virt,platform=bm

此模式把同一个 PL011 UART 映射到 ``0x13000000``，向 UART 数据寄存器写入
数据即可输出。两种模式都保留 GIC 和架构 timer；裸机软件自行忽略它们。


Sim Trap 约定
=============

AArch64 使用 ``HLT #imm`` 作为 sim trap 伪指令。signal 编码与 XiangShan QEMU 对齐：

.. list-table::
   :header-rows: 1

   * - signal
     - 编码
     - 含义
   * - ``SIMTRAP_DISABLE_TIME_INTR``
     - ``0x100``
     - 关闭 timer interrupt，QEMU 侧 mask DAIF。
   * - ``SIMTRAP_NOTIFY_PROFILER``
     - ``0x101``
     - profiling/checkpoint window start。
   * - ``SIMTRAP_NOTIFY_WORKLOAD_EXIT``
     - ``0x102``
     - profiling/checkpoint window stop，并请求 QEMU 正常退出。
   * - ``SIMTRAP_GOOD_TRAP``
     - ``0x000``
     - 兼容旧 workload 的成功结束信号，并请求 QEMU 正常退出。

边界语义：

* ``PROFILE_START`` 执行完成后的下一条 guest 指令开始计数和记录。
* ``PROFILE_STOP`` 以前一条 guest 指令作为结束，stop trap 自身不计入 BBV；profiling/checkpoint 采集模式下，执行到该 trap 会请求 QEMU 退出。
* checkpoint 输入切点 ``N`` 表示 profiling window 内的计性能起点。默认 ``warmup-interval=0`` 时，请求 snapshot 位置为 ``N``；如果设置了 warmup，则请求 snapshot 位置为 ``N - warmup-interval``，实际 snapshot 在不早于该请求位置的第一个 checkpoint 检查边界生成。常规检查边界是 TB 入口，``PROFILE_STOP`` 关闭 window 前也会补查一次。恢复后先执行 warmup 段，再进入计性能区间；TB 边界越过会让实际 warmup 略短。如果实际检查边界已经越过 ``N``，该 slice 会被跳过，避免从计性能区间内部恢复。若 ``N < warmup-interval``，该切点会被丢弃，不会为单个 slice 自动缩短 warmup。
* profiling/checkpoint 采集运行中，``PROFILE_START`` 会关闭中断。
* checkpoint restore 运行中，关中断由 gcpt restorer 负责，不依赖 QEMU 再处理 profiling sim trap。
* 非 profiling/checkpoint 模式下，``0x101`` 被当作 nop；``0x102`` 和 ``0x000`` 仍会请求 QEMU 正常退出，``0x100`` 仍会关闭中断。


Profiling 模式
==============

profiling 通过 ``libsimpoint.so`` 插件生成 gzip 压缩的 SimPoint 3.2 BBV。

命令模板：

.. code-block:: shell

  build/qemu-system-aarch64 \
    -icount shift=0,sleep=off \
    -machine mini-virt \
    -cpu <cpu-model> \
    -smp 1 \
    -m <memory-size> \
    -nographic \
    -kernel <payload.bin> \
    -plugin build/contrib/plugins/libsimpoint.so,trigger=simtrap,interval=<interval>,target=<profile-dir>,dump-final=false

常用参数：

``trigger=simtrap``
  由 workload 中的 ``PROFILE_START`` 和 ``PROFILE_STOP`` 控制 profiling window。

``interval=<interval>``
  BBV interval 的目标长度，以 guest 指令数计。插件把一个 QEMU TB 作为一个 basic block；累计指令数达到 interval 后，在该 TB 结束边界输出 vector，不会把同一个 TB 拆到相邻 vector。跨界 TB 产生的 drift 会带入下一段的阈值计算，所以单个 vector 可能略长或略短，但第 ``k`` 个累计边界始终位于 ``k * interval`` 之后的首个 TB 边界附近，不会累计偏移。后续 checkpoint 使用 SimPoint location 时，需要用同一个 interval 按 ``location * interval`` 计算请求切点。

``target=<profile-dir>``
  profiling 输出目录。插件会写入 ``<profile-dir>/simpoint_bbv.gz``。

``outfile=<bbv-file>``
  直接指定 BBV 输出文件。如果已经使用 ``target``，通常不需要再设置 ``outfile``。

``cpu=<id>``
  选择采样 vCPU。当前 ``mini-virt`` 只支持单核，通常使用默认值 ``0``。

``dump-final=true|false``
  profiling stop 时是否输出最后一个不满 interval 的 BBV，默认 ``false``。用于
  SimPoint 聚类时应保持 ``false``，避免把残缺尾段当作一个等权完整区间；由脚本校验并记录
  未输出的尾段指令数。

产出文件：

.. code-block:: text

  <profile-dir>/simpoint_bbv.gz

BBV 内容是 SimPoint 3.2 文本格式，gzip 压缩。每行表示一个 interval：

.. code-block:: text

  T:<bb-id>:<instruction-count> :<bb-id>:<instruction-count> ...

其中 ``bb-id`` 由插件按翻译到的 QEMU TB 分配，``instruction-count`` 是该 basic block 在当前 interval 内贡献的指令数。当前插件使用 TB 粒度的快路径计数：每次进入 TB 时，把该 TB 的静态指令数一次性累加到 interval 和 BBV。同步异常或访存 fault 导致 TB 提前退出时，尚未执行的 TB 尾部也可能被预记，因此这是近似计数。simtrap 仍在伪指令执行处单独处理，以定义 profiling window 的开始和结束。


QEMU 精确计数与插件 TB 计数
----------------------------

当前代码中的两种计数分别属于不同层次，不能通过插件参数互相切换：

``exact``
  QEMU AArch64 TCG 内部的逐指令计数器，每条实际进入执行的 guest 指令累加一次。
  该方式开销较大，但不会把因异常或 fault 而未执行的 TB 尾部计入。它目前只用于
  QEMU 的 instruction-count compare 诊断，不会被 ``libsimpoint.so`` 读取，也不
  会生成 BBV。

``tb``
  插件和 checkpoint 快路径在 TB 入口一次性累加该 TB 的静态指令数。该方式运行
  速度更快，但属于近似计数；正常完整执行 TB 时误差通常较小，TB 中途退出时可能
  产生较大偏差。

``libsimpoint.so`` 只能使用 ``tb`` 计数；它没有 exact 计数插件选项。上面的
profiling 命令即为插件 TB 计数用法。现有 ``scripts/a64-gcpt-simpoint-flow.sh``
也属于 TB 近似流程，不能标记为严格的 exact SimPoint 流程。

QEMU 目前提供 exact/TB 对比模式，用于从同一 PC 开始测量 QEMU 内部精确计数与
TB 计数的差异和速度。它不生成 BBV 或 checkpoint，也不会改变 ``libsimpoint.so``
和常规 checkpoint 调度所使用的 TB 计数。精确计数的用法如下：

.. code-block:: shell

  build/qemu-system-aarch64 \
    -icount shift=0,sleep=off \
    -machine mini-virt,platform=bm,checkpoint-mode=SimpointCheckpoint,instruction-count-compare-pc=<start-pc>,instruction-count-compare-limit=<count>,instruction-count-compare-counter=exact \
    -cpu <cpu-model> \
    -smp 1 \
    -m <memory-size> \
    -nographic \
    -kernel <payload.bin-or-checkpoint.bin.zst>

TB 计数的用法只需把最后一个 machine 参数改为：

.. code-block:: text

  instruction-count-compare-counter=tb

``instruction-count-compare-pc`` 必须是实际执行路径上的 TB 入口 PC。QEMU 第一次
到达该 PC 时清零计数器；所选计数器达到 ``instruction-count-compare-limit`` 后
QEMU 自动退出。``exact`` 模式的日志同时打印 ``exact``、``tb`` 和
``delta=tb-exact``，可直接比较误差；``tb`` 模式不会生成逐指令更新，适合单独
测量 TB 快路径速度。省略 ``instruction-count-compare-counter`` 时默认使用
``exact``。全系统 payload 应把 ``platform=bm`` 改为 ``platform=fs``。

注意，``instruction-count-compare-counter=exact`` 目前只是 QEMU 内部诊断选项，
不能把 ``libsimpoint.so`` 的 BBV 切换为精确计数。若要实现严格 exact SimPoint，
需要另行修改 BBV 生成和 checkpoint 切点调度，让它们共享精确计数；当前插件和
脚本仍然是 TB 近似流程。

后续使用 SimPoint 3.2 对 ``simpoint_bbv.gz`` 做 cluster，通常会得到：

.. code-block:: text

  simpoints0
  weights0

checkpoint 模式只需要 ``simpoints0``。``weights0`` 可由外部流程用于切片权重统计或命名。


Checkpoint 模式
===============

checkpoint 模式根据切点在 profiling window 内生成 snapshot。切点可以来自手工指定，也可以来自 SimPoint cluster 输出。

手工切点命令模板：

.. code-block:: shell

  build/qemu-system-aarch64 \
    -icount shift=0,sleep=off \
    -machine mini-virt,checkpoint-mode=SimpointCheckpoint,cutpoints=<cutpoint-list>,checkpoint-dir=<checkpoint-dir> \
    -cpu <cpu-model> \
    -smp 1 \
    -m <memory-size> \
    -nographic \
    -kernel <payload.bin>

SimPoint cluster 切点命令模板：

.. code-block:: shell

  build/qemu-system-aarch64 \
    -icount shift=0,sleep=off \
    -machine mini-virt,checkpoint-mode=SimpointCheckpoint,simpoint-path=<simpoint-dir>,cpt-interval=<interval>,warmup-interval=<warmup>,checkpoint-dir=<checkpoint-dir> \
    -cpu <cpu-model> \
    -smp 1 \
    -m <memory-size> \
    -nographic \
    -kernel <payload.bin>

``mini-virt`` checkpoint 参数：

``checkpoint-mode=SimpointCheckpoint``
  开启 checkpoint 模式。也接受 ``checkpoint`` 和 ``simpoint`` 作为简写。

``checkpoint-dir=<checkpoint-dir>``
  checkpoint 输出根目录。默认值为 ``a64-checkpoints``。

``cutpoints=<cutpoint-list>``
  直接指定 profiling window 内的计性能起点。可以用逗号、分号、冒号或空白分隔。

``cutpoints-file=<file>``
  从文件读取计性能起点。每行第一个整数作为相对指令数；空行和 ``#`` 注释行会忽略。

``simpoint-file=<file>``
  读取 SimPoint ``simpoints0`` 风格文件。每行第一个整数是 simpoint location，计性能起点为 ``location * cpt-interval``。不会跳过任何 location，包括 ``0``。

``simpoint-path=<path>``
  如果是目录，则读取 ``<path>/simpoints0``；如果是文件，则按 ``simpoint-file`` 处理。

``cpt-interval=<interval>``
  profiling 时使用的 SimPoint interval。使用 ``simpoint-file`` 或 ``simpoint-path`` 时必须设置。

``warmup-interval=<warmup>``
  每个 slice 的 warmup 指令数，默认 ``0``。请求 snapshot 位置为 ``measurement-point - warmup``，实际 snapshot 在不早于该请求位置的第一个 checkpoint 检查边界生成。如果 measurement point 小于 warmup，该 slice 会被丢弃，以保证所有生成的 slice 至少有可请求的恒定 warmup；不会把 snapshot 位置钳到 ``0`` 后生成短 warmup checkpoint。如果 TB 边界越过了 measurement point，也会跳过该 slice，因为这种 checkpoint 已经无法提供正确的 warmup/measurement 区间。

``checkpoint-exit-after-last=<bool>``
  生成最后一个 checkpoint 后是否退出 QEMU。默认 ``true``。

输出路径：

.. code-block:: text

  <checkpoint-dir>/<measurement-point>/_<measurement-point>_.bin.zst

如果设置了 ``warmup-interval``，文件名会额外包含 warmup 和请求 snapshot 位置：

.. code-block:: text

  <checkpoint-dir>/<measurement-point>/_<measurement-point>_warmup_<warmup>_cpt_<requested-checkpoint-point>_.bin.zst

其中 ``<measurement-point>`` 和 ``<requested-checkpoint-point>`` 都是 profiling window 内的相对指令数，而不是从 reset 开始的全局指令数。请求位置满足 ``measurement-point - requested-checkpoint-point == warmup``；生成日志会打印实际 snapshot 位置和相对请求位置的 ``overshoot``。若 ``actual snapshot > measurement-point``，QEMU 不会写出该 checkpoint，而是记录 skip 日志。

snapshot 默认写成 zstd 压缩文件。解压后的内容是完整 raw RAM image：

* 逻辑大小等于 ``-m`` 指定的 RAM 大小。
* restorer 和原始 payload 保持在 image 低地址区域。
* 单核 ``a64_snapshot_header`` 写在文件偏移 ``0x100000``，各寄存器块紧随
  header，以16字节对齐的紧凑流形式存放；header中的所有offset均相对于
  ``0x100000``。
* 格式参考 ``libcheckpoint-for-aarch64`` 的 ``a64_checkpoint_format.h``，
  当前writer只生成 ``A64_CPT_SNAPSHOT_VERSION == 1`` 的流式格式，不兼容旧
  snapshot header。

当前保存内容：

* RAM。
* PC、PSTATE、current EL。
* X0-X30、SP_EL0-SP_EL3。
* ELR/SPSR EL1-EL3。
* 固定顺序的EL1/EL2/EL3通用sysreg块，包括地址转换、异常、线程指针和
  architectural timer状态；EL2部分包含VHE使用的 ``TTBR1_EL2``。
* FPSIMD Q0-Q31、FPSR、FPCR。
* SVE Z0-Z31、P0-P15、FFR、active/max VL和ZCR_EL1-EL3。
* PAuth APIA/APIB/APDA/APDB/APGA key low/high pairs。

当前不保存：

* GIC、PL011、QEMU device 内部状态。
* SME状态。
* MTE系统寄存器和allocation tag memory。
* FPMR和SCXTNUM。

如果checkpoint边界存在新版格式无法承载的非零SME、MTE、FPMR或SCXTNUM
状态，writer会报错并停止checkpoint流程，不会静默生成缺失状态的文件。
生成和恢复必须使用兼容的 CPU feature 集；SVE restore CPU 的 vector length
能力不能小于 checkpoint 中记录的 active VL。带 SVE/PAuth 状态的 checkpoint
需要使用包含新版 ``libcheckpoint-for-aarch64`` restorer 的 gcpt payload。


恢复 checkpoint
===============

恢复时直接把生成的 checkpoint ``.bin.zst`` 当作 ``mini-virt`` payload 启动，不要再开启 ``checkpoint-mode``。``mini-virt`` 会在加载时流式解压 ``.zst``：

.. code-block:: shell

  build/qemu-system-aarch64 \
    -icount shift=0,sleep=off \
    -machine mini-virt \
    -cpu <cpu-model> \
    -smp 1 \
    -m <memory-size> \
    -nographic \
    -kernel <checkpoint.bin.zst> \
    -plugin build/contrib/plugins/libstoptrigger.so,icount=<max-instructions>:0

新版restorer成功路径不打印状态，只在校验或恢复失败时输出错误并停机。可以用
``stoptrigger`` 验证恢复PC；将 ``<restore-pc>`` 替换为checkpoint生成日志中的
``pc=`` 值或snapshot header中的 ``pc``：

.. code-block:: shell

  build/qemu-system-aarch64 \
    -icount shift=0,sleep=off \
    -machine mini-virt \
    -cpu <cpu-model> \
    -smp 1 \
    -m <memory-size> \
    -nographic \
    -kernel <checkpoint.bin.zst> \
    -plugin build/contrib/plugins/libstoptrigger.so,addr=<restore-pc>:77 \
    -d plugin \
    -D <pc-check-log>

如果恢复后执行命中该 PC，QEMU 会以退出码 ``77`` 退出，并在 ``<pc-check-log>`` 中记录：

.. code-block:: text

  <restore-pc> reached, exiting

仅当 ``<restore-pc>`` 不落在restorer自身启动代码地址范围内时，单独的地址
trigger才能证明已经完成恢复；否则启动阶段可能先命中同一地址。通用验证应使用
下述流程脚本，同时检查snapshot header、确认恢复过程中没有再次执行
PROFILE_START，并要求恢复后的workload正常运行到PROFILE_STOP。


推荐流程
========

下面是当前可直接运行的 TB 近似流程；它适合验证 QEMU、生成 checkpoint 和测量
运行速度，但不能作为严格 exact SimPoint 结果。严格流程需要先将逐指令计数接入
BBV 生成和 checkpoint 调度，详见上面的“精确指令计数与 TB 指令计数”。

1. 构建 QEMU 和插件。
2. 使用 ``libsimpoint.so,trigger=simtrap`` 运行 workload，得到 ``simpoint_bbv.gz``。
3. 使用 SimPoint 3.2 对 BBV 做 cluster，得到 ``simpoints0``。
4. 使用 ``checkpoint-mode=SimpointCheckpoint,simpoint-path=<path>,cpt-interval=<interval>,warmup-interval=<warmup>`` 生成 checkpoints；不需要 warmup 时可省略 ``warmup-interval``。
5. 直接启动生成的 checkpoint ``.bin.zst`` 做恢复验证和后续 CPU 性能测试。

gcpt gemm 的 profiling、SimPoint cluster 和 checkpoint 生成可以直接用脚本复现：

.. code-block:: shell

  scripts/a64-gcpt-simpoint-flow.sh

脚本默认从当前 checkout 所在 workspace 的同级 ``unified-workload`` 中查找 ``qemu-minivirt-aarch64-gcpt/gemm/gcpt/gcpt.bin``，也可以用 ``PAYLOAD=...`` 显式指定；``CPU_MODEL`` 默认为 ``cortex-a57``，profiling、checkpoint 和 restore validation 始终使用同一个值；``INTERVAL`` 默认为 ``10000``，``WARMUP`` 默认为 ``5000``。流程由 workload 内的 simtrap 控制开始和结束，不使用额外的 max-insn 截断；输出目录默认在 ``/tmp``，可用 ``OUT_DIR=...`` 覆盖。脚本不接受包含空白字符的路径。恢复校验直接读取流式header中的PC并与生成日志比对，同时要求恢复后的workload正常执行到PROFILE_STOP，不再依赖旧restorer的成功日志。

主流程使用 ``dump-final=false``，只把完整 interval 交给 SimPoint。原始聚类结果保存在 ``simpoints0.raw`` 和 ``weights0.raw``；measurement point 小于 ``WARMUP`` 的代表点记录到 ``dropped-warmup.tsv`` 并从 checkpoint 输入中移除，剩余 ``weights0`` 重新归一化到 100%。这是有意保留的启动阶段丢弃策略。脚本要求每个保留代表点都成功生成 checkpoint，运行时越过 measurement point 或缺少文件都会使流程失败。``summary.txt`` 会记录 profile 尾段、丢弃点原始权重和保留点原始权重；``slices.tsv`` 和 ``restore-validation.tsv`` 分别校验每个保留 slice 的请求/实际切点，以及snapshot header PC和恢复执行结果。
