# SKIP_ATF 下内核只拉起一个 CPU

适用于 SKIP_ATF 模式：没有 BL31，由 BL1 常驻 EL3 的代码（`src/psci.c`）实现 PSCI。
2026-09-23 在板子上验证，4.4 arm64 内核输出 `Brought up 8 CPUs`。

## 现象

内核启动时，7 个副核都报 `CPUn: failed to come online`，最后只有一个 CPU 在跑。
CPU_ON 返回的是成功，也就是说 SMC 进了 BL1，但副核没有进内核。

## 原因

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

## 修复

做法和厂商 BL31 / TF-A 一样：

- BL1 启动时不拉副核（`src/subcpu.c` `SubCPUBringUp()`）。
- CPU_ON（`psci.c` `cpuOn()`）先把入口地址写进对应核的 slot，状态置为 ON_PENDING，
  然后调用 `PowerOnSecondaryCPU()` 给这个核上电，启动向量指向 BL1 的 `Startup`。
- 副核上电后依次走 `Startup` → `SubCPUBoot()` → `JoinCCI()` → `psciSecondaryEntry()`：
  看到自己是 ON_PENDING，直接 eret 到内核入口（EL2 非安全），不需要谁来唤醒。
- 启动时 `initCCI400()` 只打开簇 0 的端口；簇 1 的端口由簇 1 第一个上电的核在
  `JoinCCI()` 里自己打开。

## 备注

- 内核在 `time_init()` 里会重新设置 UART0 的时钟，从这以后到串口驱动 probe（约 7.3s）之前，
  EL3 打出来的内容全是乱码，例如 CPU_ON 的日志、`psci: CPUn on, entering ...` 和副核的数字。
  这是正常现象，这些打印保留着。
- 先 CPU_OFF 再 CPU_ON（热插拔）还没测过：CPU_OFF 后核停在 EL3 的 `wfi` 里，
  再次上电请求能不能让它重启，目前不确定。
- 查这类早期问题，可以给内核加 `earlycon=s5p6818,0xc00a1000`。
