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
     - profiling/checkpoint window stop。

边界语义：

* ``PROFILE_START`` 执行完成后的下一条 guest 指令开始计数和记录。
* ``PROFILE_STOP`` 以前一条 guest 指令作为结束，stop trap 自身不计入 BBV。
* checkpoint 切点 ``N`` 表示 profiling window 内已经执行 ``N`` 条指令后的状态；恢复后 PC 指向第 ``N + 1`` 条将执行的指令。
* profiling/checkpoint 采集运行中，``PROFILE_START`` 会关闭中断。
* checkpoint restore 运行中，关中断由 gcpt restorer 负责，不依赖 QEMU 再处理 profiling sim trap。
* 非 profiling/checkpoint 模式下，``0x101`` 和 ``0x102`` 被当作 nop；``0x100`` 仍会关闭中断。


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
    -plugin build/contrib/plugins/libsimpoint.so,trigger=simtrap,interval=<interval>,target=<profile-dir>,dump-final=true \
    -plugin build/contrib/plugins/libstoptrigger.so,icount=<max-instructions>:0

常用参数：

``trigger=simtrap``
  由 workload 中的 ``PROFILE_START`` 和 ``PROFILE_STOP`` 控制 profiling window。

``interval=<interval>``
  BBV interval，以 guest 指令数计。后续 checkpoint 使用 SimPoint location 时，需要用同一个 interval 计算实际切点。

``target=<profile-dir>``
  profiling 输出目录。插件会写入 ``<profile-dir>/simpoint_bbv.gz``。

``outfile=<bbv-file>``
  直接指定 BBV 输出文件。如果已经使用 ``target``，通常不需要再设置 ``outfile``。

``cpu=<id>``
  选择采样 vCPU。当前 ``mini-virt`` 只支持单核，通常使用默认值 ``0``。

``dump-final=true``
  profiling stop 时，如果最后一个 interval 不满，也输出最后一段 BBV。

``libstoptrigger.so``
  给 QEMU 设置指令数退出条件，避免 workload 结束后停在等待循环而不退出。

产出文件：

.. code-block:: text

  <profile-dir>/simpoint_bbv.gz

BBV 内容是 SimPoint 3.2 文本格式，gzip 压缩。每行表示一个 interval：

.. code-block:: text

  T:<bb-id>:<instruction-count> :<bb-id>:<instruction-count> ...

其中 ``bb-id`` 由插件按翻译到的 basic block 分配，``instruction-count`` 是该 basic block 在当前 interval 内贡献的指令数。

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
    -machine mini-virt,checkpoint-mode=SimpointCheckpoint,simpoint-path=<simpoint-dir>,cpt-interval=<interval>,checkpoint-dir=<checkpoint-dir> \
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
  直接指定 profiling window 内的相对指令切点。可以用逗号、分号、冒号或空白分隔。

``cutpoints-file=<file>``
  从文件读取切点。每行第一个整数作为相对指令切点；空行和 ``#`` 注释行会忽略。

``simpoint-file=<file>``
  读取 SimPoint ``simpoints0`` 风格文件。每行第一个整数是 simpoint location，实际切点为 ``location * cpt-interval``。不会跳过任何 location，包括 ``0``。

``simpoint-path=<path>``
  如果是目录，则读取 ``<path>/simpoints0``；如果是文件，则按 ``simpoint-file`` 处理。

``cpt-interval=<interval>``
  profiling 时使用的 SimPoint interval。使用 ``simpoint-file`` 或 ``simpoint-path`` 时必须设置。

``checkpoint-exit-after-last=<bool>``
  生成最后一个 checkpoint 后是否退出 QEMU。默认 ``true``。

输出路径：

.. code-block:: text

  <checkpoint-dir>/<cutpoint>/_<cutpoint>_.bin.zst

其中 ``<cutpoint>`` 是 profiling window 内的相对指令数，而不是从 reset 开始的全局指令数。

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

恢复时直接把生成的 ``_<cutpoint>_.bin.zst`` 当作 ``mini-virt`` payload 启动，不要再开启 ``checkpoint-mode``。``mini-virt`` 会在加载时流式解压 ``.zst``：

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
4. 使用 ``checkpoint-mode=SimpointCheckpoint,simpoint-path=<path>,cpt-interval=<interval>`` 生成 checkpoints。
5. 直接启动 ``_<cutpoint>_.bin.zst`` 做恢复验证和后续 CPU 性能测试。
