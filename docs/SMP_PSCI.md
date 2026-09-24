# Only one CPU comes up under SKIP_ATF / SKIP_ATF 下内核只拉起一个 CPU

**English** | [中文](#中文)

## English

Applies to SKIP_ATF mode: there is no BL31, and PSCI is implemented by BL1's
resident EL3 code (`src/psci.c`). Verified on the board on 2026-09-23: a 4.4
arm64 kernel prints `Brought up 8 CPUs`.

### Symptom

While booting, the kernel reported `CPUn: failed to come online` for all 7
secondary cores and ended up running on a single CPU. CPU_ON returned success,
so the SMC did reach BL1, but the secondaries never entered the kernel.

### Cause

**1. Secondaries parked in BL1 never woke up**

The old design powered up all 7 secondaries when BL1 started and parked them
in `wfi` at EL3. On CPU_ON, BL1 sent an SGI to wake the target core.
On the board, that SGI never woke a single core.

Replacing `wfi` with a polling loop didn't help either. As soon as one core
kept running at EL3 (not in `wfi`), bringing up the next core failed
(`cpu N is dead`), regardless of bus load. The SoC's power controller seems to
require the other cores to be in WFI. This is inferred from the behaviour;
there is no documentation for it.

**2. Cluster 1's CCI port was enabled while cluster 1 was powered off**

`initCCI400()` enabled snoop/DVM on cluster 1's port (CPU4-7) right at the
start. With cluster 1 powered off, the first broadcast TLB/cache maintenance
instruction the kernel executed made the CCI wait for an answer from
cluster 1, which never came. The kernel hung at `Starting kernel ...` before
it could issue a single SMC. u-boot does no such broadcasts, so it wasn't
affected. The problem stayed hidden before because all 7 cores used to be
brought up, so cluster 1 had power.

### Fix

The same approach as the vendor BL31 / TF-A:

- BL1 no longer brings up the secondaries at boot (`src/subcpu.c`
  `SubCPUBringUp()`).
- CPU_ON (`psci.c` `cpuOn()`) writes the entry address into that core's slot,
  sets its state to ON_PENDING, then calls `PowerOnSecondaryCPU()` to power
  the core on, with its reset vector at BL1's `Startup`.
- Once powered, the secondary goes through `Startup` -> `SubCPUBoot()` ->
  `JoinCCI()` -> `psciSecondaryEntry()`. It sees that it is ON_PENDING and
  erets straight to the kernel entry (EL2 non-secure). Nobody has to wake it.
- At boot, `initCCI400()` enables only cluster 0's port. Cluster 1's port is
  enabled in `JoinCCI()` by the first cluster 1 core that powers on.

### Notes

- The kernel reprograms UART0's clock in `time_init()`. From then until the
  serial driver probes (about 7.3s), everything printed from EL3 comes out
  garbled: the CPU_ON log, `psci: CPUn on, entering ...` and the
  secondaries' digits. This is expected, and those prints are kept.
- CPU_OFF followed by CPU_ON (hotplug) is untested. After CPU_OFF the core
  waits in `wfi` at EL3, and whether a new power-on request restarts it is
  not known yet.
- For early problems like this, add `earlycon=s5p6818,0xc00a1000` to the
  kernel command line.

---

## 中文

适用于 SKIP_ATF 模式：没有 BL31，由 BL1 常驻 EL3 的代码（`src/psci.c`）实现 PSCI。
2026-09-23 在板子上验证，4.4 arm64 内核输出 `Brought up 8 CPUs`。

### 现象

内核启动时，7 个副核都报 `CPUn: failed to come online`，最后只有一个 CPU 在跑。
CPU_ON 返回的是成功，也就是说 SMC 进了 BL1，但副核没有进内核。

### 原因

**1. 副核停在 BL1 里等，醒不过来**

原来的做法是：BL1 启动时就把 7 个副核拉起来，让它们在 EL3 执行 `wfi` 停着；
CPU_ON 到来时，再发一个 SGI 把对应的核叫醒。
在板子上，没有一个核被这个 SGI 叫醒。

把 `wfi` 换成循环轮询也不行。只要有一个核在 EL3 一直跑（没进 `wfi`），
BL1 拉下一个核时就会失败（`cpu N is dead`），和总线负载无关。
这块 SoC 的电源控制器看起来要求其他核处于 WFI；这是根据现象推断的，没有文档可查。

**2. 簇 1 没上电时，它的 CCI 端口就已经打开**

`initCCI400()` 一开始就把簇 1（CPU4~7）端口的 snoop/DVM 打开了。
如果簇 1 没上电，内核第一次执行会广播的 TLB/cache 维护指令时，CCI 要等簇 1 应答，
而簇 1 永远不会应答，于是内核卡死在 `Starting kernel ...`，一个 SMC 都发不出来。
u-boot 不做这类广播，所以不受影响。以前 7 个核都被拉起来，簇 1 是有电的，这个问题才没暴露。

### 修复

做法和厂商 BL31 / TF-A 一样：

- BL1 启动时不拉副核（`src/subcpu.c` `SubCPUBringUp()`）。
- CPU_ON（`psci.c` `cpuOn()`）先把入口地址写进对应核的 slot，状态置为 ON_PENDING，
  然后调用 `PowerOnSecondaryCPU()` 给这个核上电，启动向量指向 BL1 的 `Startup`。
- 副核上电后依次走 `Startup` → `SubCPUBoot()` → `JoinCCI()` → `psciSecondaryEntry()`：
  看到自己是 ON_PENDING，直接 eret 到内核入口（EL2 非安全），不需要谁来唤醒。
- 启动时 `initCCI400()` 只打开簇 0 的端口；簇 1 的端口由簇 1 第一个上电的核在
  `JoinCCI()` 里自己打开。

### 备注

- 内核在 `time_init()` 里会重新设置 UART0 的时钟，从这以后到串口驱动 probe（约 7.3s）之前，
  EL3 打出来的内容全是乱码，例如 CPU_ON 的日志、`psci: CPUn on, entering ...` 和副核的数字。
  这是正常现象，这些打印保留着。
- 先 CPU_OFF 再 CPU_ON（热插拔）还没测过：CPU_OFF 后核停在 EL3 的 `wfi` 里，
  再次上电请求能不能让它重启，目前不确定。
- 查这类早期问题，可以给内核加 `earlycon=s5p6818,0xc00a1000`。
