AArch64 SimPoint Profiling/Checkpoint 规范
==========================================

本文档是本分支 profiling 和 checkpoint 实现的固定规范。后续实现、脚本和
验证都必须以本文档为准，不能把 TB 模糊计数、精确计数、TB 边界和逐指令
checkpoint 混成同一个概念。

目标
----

本分支面向 AArch64 ``mini-virt`` raw payload，目标是生成可供 SimPoint 3.2
聚类的 BBV，并根据 cluster 结果生成可恢复的 CPU performance checkpoint。

核心目标只有一个：

* 计数必须是精确动态指令数；
* 所有昂贵操作必须限制在 TB 边界；
* exact 模式不能退化为逐指令 C 回调、逐指令文件操作或逐指令 snapshot；
* TB 模式只作为显式选择的更快近似模式保留。

本文档参考 XiangShan QEMU 的 ``refactor_profiling`` 实现，特别是：

* ``contrib/plugins/profiling.c`` 的 TB 预计数、scoreboard 和 middle-exit
  纠正路径；
* ``accel/tcg/translator.c`` 的逐指令 ``cpu_exec_count`` 更新；
* ``checkpoint_gen_empty_callback()`` 在 TB 边界执行 checkpoint 检查的路径。

端到端流程必须保持官方 SimPoint 的三阶段顺序：

.. code-block:: text

  profiling -> SimPoint cluster -> checkpointing

profiling 和 checkpointing 是两次独立的 workload 执行；两次必须使用相同的
workload、CPU feature、平台、``instruction-count`` 模式和 interval。cluster
只消费 profiling 产生的 ``simpoint_bbv.gz``，输出 ``simpoints0`` 和
``weights0``；checkpointing 再读取 ``simpoints0`` 生成代表性 checkpoint。
恢复是第四个独立阶段，不应再次开启 profiling 或 checkpoint 模式。

计数模式
--------

正式接口使用一个统一模式参数：

.. code-block:: text

  instruction-count=exact|tb

默认值必须是 ``exact``。只有显式指定 ``instruction-count=tb`` 时，才允许
使用 TB 模糊计数。profiling 和 checkpoint 必须使用相同模式，不能 profiling
用 exact、checkpoint 用 tb，也不能反过来。

命令中只通过 ``mini-virt`` 的一个 machine 参数选择模式：

.. code-block:: shell

  # exact，默认值；显式写出便于记录流程
  -machine mini-virt,platform=bm,instruction-count=exact

  # TB 模糊计数，仅用于明确的快速对照
  -machine mini-virt,platform=bm,instruction-count=tb

profiling 和 checkpoint 命令都使用同一个参数，不增加第二套 plugin 计数开关。
plugin 的 BBV 计数和 QEMU 内部 checkpoint 计数必须由同一个模式状态驱动。

``exact`` 的定义：

* TCG 对每条实际进入执行路径的 guest 指令更新一次内部计数器；
* exact counter 是 interval、SimPoint location、warmup、checkpoint 请求位置
  和 actual/overshoot 日志的唯一权威计数；
* exact counter 的更新使用翻译后的轻量 TCG 操作，不调用逐指令 C helper。
* 如果指令执行在 fault/exception 处被 QEMU 中止，恢复 TB 状态时必须回滚这条
  尚未完成的指令计数；不能把“计数更新已经生成”误认为“指令已经完成执行”。

``tb`` 的定义：

* 在 TB 路径按 TB 静态指令数快速累计；
* 完整执行 TB 时通常与 exact 相同；
* TB 中途因异常、fault、MMIO 或其他原因退出时，可能把未执行尾部计入；
* 该模式只用于性能对照或兼容旧流程。

Profiling 流程
--------------

启动和窗口
~~~~~~~~~~~~

profiling 使用 ``libsimpoint.so`` 生成 gzip 压缩的 SimPoint 3.2 BBV。
workload 的 ``PROFILE_START`` 和 ``PROFILE_STOP`` 定义 profiling window：

* ``PROFILE_START`` 本身不计入 BBV；从其后的 guest 指令开始计数；
* ``PROFILE_STOP`` 本身不计入 BBV；其前的 guest 指令是 profiling 的最后内容；
* 开始窗口时清零 exact counter、interval counter 和当前 BBV 累计；
* stop 时只输出完整 interval，正式 SimPoint 流程使用 ``dump-final=false``。

TB 元数据和精确动态贡献
~~~~~~~~~~~~~~~~~~~~~~~~~~

每个翻译 TB 建立稳定的 BB 元数据，至少包含：

* TB 起始 PC 和结束 PC；
* TB 静态指令总数；
* BBV 输出 ID；
* 当前 interval 中该 TB 实际贡献的动态指令数。

exact profiling 必须采用 XiangShan 的快路径组织：

* TB 入口用 inline scoreboard 保存 ``begin_block``、``end_block``、
  ``current_tb_total_cnt``，并将 ``current_tb_insn_cnt`` 清零；
* 每条指令只用 inline ``ADD`` 更新 ``current_tb_insn_cnt``，同时记录当前
  PC 和下一条 PC；
* TB 执行回调先按静态 TB 长度做预计数，避免在正常路径逐条处理 BBV；
* 如果 ``middle_exit_flag`` 表明 TB 没有走到最后一条指令，执行
  middle-exit 纠正，减去未执行尾部；
* MMIO split TB 必须把子 TB 的实际贡献合并回原始 BB，不能重复计算或丢失
  已执行指令。

``middle_exit_flag`` 不是唯一的准确性保障：XiangShan 源码明确记录了“最后一条
指令 fault 时，单靠该 flag 可能无法检测中途退出”的边界。因此实现必须同时
覆盖两条纠正路径：

* plugin scoreboard 纠正 TB 尾部和 MMIO split 的 BBV block contribution；
* QEMU ``cpu_restore_state_from_tb``/等价恢复路径回滚被中止的 exact counter。

两条路径缺一不可，否则 BBV 和 checkpoint 可能分别出现不同的 off-by-one。

这样，正常完整 TB 只走 TB 快路径，只有真正发生中途退出时才进入纠正路径；
BBV 中的每个 block count 最终仍然是实际动态指令数，而不是静态 TB 长度的
无条件累加。

``PROFILE_START`` 位于 TB 中间时，必须只保留 start trap 后的 TB 后缀；
``PROFILE_STOP`` 位于 TB 中间时，必须只保留 stop trap 前的 TB 前缀。两个 trap
本身都不进入 BBV。

Interval 和 BBV 输出
~~~~~~~~~~~~~~~~~~~~

interval 的目标长度以 exact dynamic instruction count 表示。判断和输出规则：

* exact 累计值达到或越过 interval 后，不在 TB 内拆分；
* 在当前 TB 完成、进入下一次 TB 边界处理时输出 vector；
* vector 中的 block count 使用已经纠正后的实际动态贡献；
* vector 的实际长度可以超过目标 interval，但 overshoot 必须由 exact count
  计算，不能用静态 TB 长度重新推导；
* 下一个 interval 继续沿用 exact 累计位置，不能把 TB overshoot 错误地累计成
  额外的长期漂移。

输出文件为：

.. code-block:: text

  <profile-dir>/simpoint_bbv.gz

SimPoint 3.2 使用该 BBV 生成 ``simpoints0`` 和 ``weights0``。后续 checkpoint
必须使用同一 ``interval`` 和同一 ``instruction-count`` 模式。

Checkpoint 流程
---------------

输入和计数基准
~~~~~~~~~~~~~~~

checkpoint 使用手工切点或 SimPoint ``simpoints0``。SimPoint location 换算为：

.. code-block:: text

  measurement_point = location * cpt-interval

该换算的单位是 profiling window 内的 exact dynamic instruction count。

如果设置 ``warmup-interval=W``：

.. code-block:: text

  requested_checkpoint = measurement_point - W

snapshot 的请求位置、实际位置、overshoot、warmup 和 measurement point 必须
全部使用 exact count。

Exact counter 和 TB 边界检查
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

exact checkpoint 必须按 XiangShan 的结构实现：

* translator 每条指令只更新一次内部 exact counter；
* 每个 TB 只注入一次 checkpoint 检查；
* checkpoint 检查读取当前 exact counter，判断是否已达到请求位置；
* snapshot 在 TB 边界生成，不能对每条指令调用 checkpoint helper；
* snapshot 的 PC 是该 TB 边界对应的恢复 PC，状态和计数位置必须一致。

如果目标位置在当前 TB 中间被越过，当前 TB 继续完成，下一 TB 边界生成
snapshot。实际位置是该边界的 exact counter 值：

.. code-block:: text

  actual = exact counter at TB boundary
  overshoot = actual - requested_checkpoint

如果配置了 warmup 且该边界已经晚于 measurement point，则丢弃该切点，不能
生成一个 warmup 不足的 checkpoint。没有 warmup 时允许保留 TB 边界 overshoot。

一个代表点 ``N`` 的完整性能片段是：

.. code-block:: text

  checkpoint snapshot: N - W
  warmup:              (N - W, N)
  measured interval:   (N, N + I)

其中 ``W`` 是 ``warmup-interval``，``I`` 是 profiling/checkpoint 共用的
``cpt-interval``。恢复后必须执行完 warmup，再只把 ``N`` 到 ``N + I`` 的区间
用于性能统计；不能把 warmup 指令计入代表点性能结果，也不能只执行到 ``N``
就结束 slice。

TB 模式
~~~~~~~~

``instruction-count=tb`` 时保留当前 TB 快路径：

* TB 静态指令数用于累计和边界判断；
* checkpoint 仍只在 TB 边界生成；
* 日志中的 actual/overshoot 明确表示 TB 近似值；
* 该模式不能和 exact profiling 结果混用。

恢复 checkpoint 时不再开启 checkpoint-mode，也不需要继续计数；直接把
``.bin.zst`` 作为 ``mini-virt`` payload 启动。

性能要求
--------

性能是本设计的首要约束。实现必须满足：

* exact 模式的逐指令工作只允许是翻译后的轻量 counter update；
* 正常 TB 不执行逐指令 C 回调、锁竞争、哈希查找、gzip/zstd 操作或 RAM 扫描；
* interval 判断只放在 TB 边界快路径；
* checkpoint helper 只在达到阈值后调用；
* snapshot 压缩和文件写入只在真正命中的 TB 边界执行；
* middle-exit 纠正只在检测到 TB 没有正常走到尾部时执行；
* 不得为了 exact 计数把整个模拟退化成每条指令一次的通用插件 callback；
* ``tb`` 模式必须保留，作为不带 exact counter 的最快对照路径。

性能验证至少记录：

* exact profiling 的 guest instructions per second；
* TB profiling 的 guest instructions per second；
* exact checkpoint 和 TB checkpoint 的单 TB 运行开销；
* 生成 checkpoint 时的 snapshot 压缩耗时不计入普通 TB 快路径对比；
* exact 与 TB 的实际计数差值和触发场景。

exact 模式的性能红线不是“完全没有逐指令 TCG 加法”，而是不能出现逐指令的
慢路径工作。允许的逐指令操作只有计数器更新和必要的 scoreboard inline 操作；
所有 C helper、锁、哈希、压缩、RAM 扫描和日志输出都必须留在 TB 边界或异常
纠正路径。

正确性和验收标准
------------------

Profiling
~~~~~~~~~

* 默认参数生成 exact BBV；
* BBV block count 等于实际动态执行指令数；
* 中途退出 TB 的未执行尾部不会进入 BBV；
* start/stop trap 不进入 BBV；
* interval location 和 profile 总指令数可由 exact counter 重现；
* ``dump-final=false`` 不输出残缺尾段。
* 至少覆盖 start/stop trap 位于 TB 中间、普通中途 fault、MMIO split，以及
  最后一条 TB 指令 fault 四种路径。

Checkpoint
~~~~~~~~~~

* 默认参数生成 exact-count checkpoint；
* checkpoint 只在 TB 边界生成；
* actual、overshoot、warmup 和 measurement point 使用 exact count；
* 恢复 PC 对应 snapshot TB 边界；
* warmup 不足的 slice 被跳过；
* restore 后 workload 可正常运行到退出 trap。
* 恢复后执行区间是 ``N`` 到 ``N + I``，而不是只执行 warmup 或把 warmup
  计入测量。

Exact/TB 对比
~~~~~~~~~~~~~

同一 payload、同一 PC、同一 limit 分别运行 exact 和 TB：

* 两种模式都输出实际计数和退出边界；
* 完整执行的 TB 上 exact 与 TB 相同是正常现象；
* 必须另外覆盖中途退出、fault 或 MMIO 场景，确认 TB 会预计未执行尾部而
  exact 不会；
* compare 不能只在 TB 结束盲目把两个 counter 当成相同结果。

官方流程还要求在正式生成 checkpoint 前先完整运行一次 workload，确认 workload
本身正确、QEMU/运行时可以正常执行；这是流程前置条件，不是 exact 计数校验的
替代品。

当前实现状态
------------

当前 ``arm-qemu-sliced`` 的正式 plugin 仍是 TB inline 计数，QEMU exact counter
仍主要存在于 compare 诊断路径；这不满足本文档的最终目标。后续实现完成前，
不得把现有 profiling/checkpoint 流程标记为 exact 流程，也不能用现有 TB BBV
和目标 exact checkpoint 混合生成严格 SimPoint 结果。

非目标
------

本文档不要求：

* 把 checkpoint 做成 QEMU migration snapshot；
* 保存 GIC、PL011 或其他设备内部状态；
* 引入完整文件系统、通用多进程支持或额外外设；
* 为 exact 计数增加独立的复杂命令行参数集合；
* 在核心计数和边界流程完成前扩展无关的校验、哈希或测试框架。
