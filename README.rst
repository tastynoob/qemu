AArch64 SimPoint QEMU 使用说明
==============================

本分支为 AArch64 ``mini-virt`` raw-payload 环境提供 SimPoint 支持，包含两类工作模式：

* **profiling**：运行 workload，生成 SimPoint 3.2 Basic Block Vector。
* **checkpoint**：根据 SimPoint cluster 后得到的切点，生成可恢复的 CPU performance checkpoint。

checkpoint 只用于 CPU 性能测试，不是 QEMU migration，也不是完整 IO snapshot。它保存 RAM 和 CPU 体系结构状态，不保存 GIC、PL011 等设备内部状态。恢复时只需要最小设备环境，例如串口输出和架构 timer。

原始 QEMU README 保存在 ``README.upstream.rst``。


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
    -cpu cortex-a57 \
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

其中 ``bb-id`` 由插件按翻译到的 QEMU TB 分配，``instruction-count`` 是该 basic block 在当前 interval 内贡献的指令数。profiling 统计和 interval 判断都在 TB 边界进行；插件在下一 TB、执行流中断或 stop simtrap 处按 AArch64 内部指令计数结算上一 TB，因此同步异常或访存 fault 导致的 TB 提前退出不会预记尚未执行的后半段。simtrap 仍在伪指令执行处单独处理，以精确定义 profiling window 的开始和结束。

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
    -cpu cortex-a57 \
    -smp 1 \
    -m <memory-size> \
    -nographic \
    -kernel <payload.bin>

SimPoint cluster 切点命令模板：

.. code-block:: shell

  build/qemu-system-aarch64 \
    -icount shift=0,sleep=off \
    -machine mini-virt,checkpoint-mode=SimpointCheckpoint,simpoint-path=<simpoint-dir>,cpt-interval=<interval>,warmup-interval=<warmup>,checkpoint-dir=<checkpoint-dir> \
    -cpu cortex-a57 \
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
* AArch64 checkpoint metadata 写在文件偏移 ``0x100000``。
* per-core architectural state 写在文件偏移 ``0x101000``。
* 格式参考 ``libcheckpoint-for-aarch64`` 的 ``a64_checkpoint_format.h``。

当前保存内容：

* RAM。
* PC、PSTATE、current EL。
* X0-X30、SP_EL0-SP_EL3。
* ELR/SPSR EL1-EL3。
* restorer 支持的一组 EL1/EL2/EL3 sysregs，包括 architectural timer 相关寄存器。
* FPSIMD Q0-Q31、FPSR、FPCR。

当前不保存：

* GIC、PL011、QEMU device 内部状态。
* SVE、SME、MTE、PAUTH 扩展状态。


恢复 checkpoint
===============

恢复时直接把生成的 checkpoint ``.bin.zst`` 当作 ``mini-virt`` payload 启动，不要再开启 ``checkpoint-mode``。``mini-virt`` 会在加载时流式解压 ``.zst``：

.. code-block:: shell

  build/qemu-system-aarch64 \
    -icount shift=0,sleep=off \
    -machine mini-virt \
    -cpu cortex-a57 \
    -smp 1 \
    -m <memory-size> \
    -nographic \
    -kernel <checkpoint.bin.zst> \
    -plugin build/contrib/plugins/libstoptrigger.so,icount=<max-instructions>:0

正常恢复时，串口会先输出 restorer 信息：

.. code-block:: text

  [a64-gcpt] restorer start base=0x... el=0x...
  [a64-gcpt] checkpoint header cpt_base=0x...
  [a64-gcpt] restore pc=0x... pstate=0x...

可以用 ``stoptrigger`` 验证恢复 PC。将 ``<restore-pc>`` 替换为 restorer 打印出的 PC：

.. code-block:: shell

  build/qemu-system-aarch64 \
    -icount shift=0,sleep=off \
    -machine mini-virt \
    -cpu cortex-a57 \
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


推荐流程
========

1. 构建 QEMU 和插件。
2. 使用 ``libsimpoint.so,trigger=simtrap`` 运行 workload，得到 ``simpoint_bbv.gz``。
3. 使用 SimPoint 3.2 对 BBV 做 cluster，得到 ``simpoints0``。
4. 使用 ``checkpoint-mode=SimpointCheckpoint,simpoint-path=<path>,cpt-interval=<interval>,warmup-interval=<warmup>`` 生成 checkpoints；不需要 warmup 时可省略 ``warmup-interval``。
5. 直接启动生成的 checkpoint ``.bin.zst`` 做恢复验证和后续 CPU 性能测试。

gcpt gemm 的 profiling、SimPoint cluster 和 checkpoint 生成可以直接用脚本复现：

.. code-block:: shell

  scripts/a64-gcpt-simpoint-flow.sh

脚本默认从当前 checkout 所在 workspace 的同级 ``unified-workload`` 中查找 ``qemu-minivirt-aarch64-gcpt/gemm/gcpt/gcpt.bin``，也可以用 ``PAYLOAD=...`` 显式指定；``INTERVAL`` 默认为 ``10000``，``WARMUP`` 默认为 ``5000``。流程由 workload 内的 simtrap 控制开始和结束，不使用额外的 max-insn 截断；输出目录默认在 ``/tmp``，可用 ``OUT_DIR=...`` 覆盖。脚本不接受包含空白字符的路径。

主流程使用 ``dump-final=false``，只把完整 interval 交给 SimPoint。原始聚类结果保存在 ``simpoints0.raw`` 和 ``weights0.raw``；measurement point 小于 ``WARMUP`` 的代表点记录到 ``dropped-warmup.tsv`` 并从 checkpoint 输入中移除，剩余 ``weights0`` 重新归一化到 100%。这是有意保留的启动阶段丢弃策略。脚本要求每个保留代表点都成功生成 checkpoint，运行时越过 measurement point 或缺少文件都会使流程失败。``summary.txt`` 会记录 profile 尾段、丢弃点原始权重和保留点原始权重；``slices.tsv`` 和 ``restore-validation.tsv`` 分别校验每个保留 slice 的请求切点、实际 checkpoint 切点和 restore PC。
