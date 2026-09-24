# Boot-chain emulator / 启动链模拟器

**English** | [中文](#中文)

## English

`tools/s5p6818_emu.py` runs the S5P6818 boot chain on CPU0 in
[Unicorn](https://www.unicorn-engine.org/), no board needed. It goes from
BootROM (emulated in Python) through the NSIH1 vector stub, the TIEOFF warm
reset and BL1 into the next stage, down to the u-boot prompt. Test boot-chain
changes here before flashing a card.

### What is emulated

| emulated | not emulated |
|---|---|
| BootROM: reads NSIH1 at SD 0x200, loads BL1 into SRAM, starts it in AArch32 | secondary cores (BL1 reports them dead) |
| CPU0 warm reset via CLKPWR/TIEOFF (AArch32/AArch64, RVBAR), as on the chip | interrupts |
| UART0-5 (TX to stdout, UART0 RX from the terminal or `--send`) | eMMC (u-boot's `mmc 0` is empty) |
| DW MMC with an SD card built from the images | SD writes |
| PWM timer (u-boot's time base), AXP228 PMIC on the GPIO I2C bus | guest exceptions, except AArch64 SMCs to EL3 |
| LCD, as screenshots only (`--screenshot`) | DDR training (always reports "fail", BL1 carries on) |
| TrustZone SRAM check: a non-secure fetch from secure SRAM stops the run | TrustZone on DDR |

### Setup

Once. Needs Python 3 with venv. On Debian/Ubuntu, install `python3-venv`
first if `python3 -m venv` fails.

```
python3 -m venv .venv
.venv/bin/pip install unicorn capstone
```

### Running

Build BL1 and u-boot first. By default the SD card is `out/bl1-gec6818.bin`
at offset 0x200 plus `../u-boot_gec6818/u-boot-direct.img` at BL1's
DEVICEADDR (0x10200). Symbols come from `out/*.elf` and
`../u-boot_gec6818/u-boot`.

```
.venv/bin/python tools/s5p6818_emu.py                      # boot to the u-boot prompt, then exit
.venv/bin/python tools/s5p6818_emu.py -i                   # interactive u-boot console, Ctrl-] quits
.venv/bin/python tools/s5p6818_emu.py --send '\nmmc dev 1\nmmc info\n'   # scripted u-boot commands
.venv/bin/python tools/s5p6818_emu.py --bl1 other/bl1.bin --next other/u-boot-direct.img
.venv/bin/python tools/s5p6818_emu.py --sd-image sd.img    # a raw SD card image instead
.venv/bin/python tools/s5p6818_emu.py --screenshot lcd     # also save the LCD as lcd-N.png
```

A 32-bit u-boot (`UBOOT_ARCH=aarch32`) runs the same way: pass its
`u-boot-direct.img` with `--next`. The `u-boot` ELF next to it provides the
symbols.

### The ATF chain (`SKIP_ATF=n`)

`--next` only fills the slot at 0x10200. The ATF chain also needs
`fip-secure.img` and `fip-nonsecure.img` at their own offsets, so write a
card image with `write_sdcard.py --allow-file` and pass it with `--sd-image`:

```
tools/write_sdcard.py --device atf.img --allow-file --yes --mode atf \
    --bl1 out/bl1-gec6818.bin \
    --fip-loader prebuilt/fip-loader.img --fip-secure prebuilt/fip-secure.img \
    --fip-nonsecure ../u-boot_gec6818/fip-nonsecure.img
.venv/bin/python tools/s5p6818_emu.py --sd-image atf.img --elf out/bl1-gec6818.elf
```

Both `OPMODE`s reach the u-boot prompt this way. The log shows CPU0 being
warm-reset into ATF BL2 at 0x7FD01000. BL2 and BL31 print nothing.

### Booting Linux

`--load FILE@ADDR` puts files in DDR before the boot starts, so a kernel
can be booted without building an ext4 SD image. linux_kernel_gec6818's
32-bit kernel (`gec6818_aarch32_defconfig`) with a 32-bit u-boot:

```
K=../linux_kernel_gec6818   # or its O= build directory
.venv/bin/python tools/s5p6818_emu.py --next 32-bit/u-boot-direct.img \
    --elf out/bl1-gec6818.elf --elf $K/vmlinux \
    --load $K/arch/arm/boot/zImage@0x40008000 \
    --load $K/arch/arm/boot/dts/s5p6818-gec6818-rev01.dtb@0x49000000 \
    --send '\nsetenv bootargs "console=ttySAC0,115200n8 earlycon=s5p6818,0xc00a1000 lpj=1000000"\nbootz 0x40008000 - 0x49000000\n' \
    --timeout 200 --hang-seconds 20
```

The kernel gets through `start_kernel` (memory, clocks, timer, VFP) into
the initcalls. It then stops with a data abort in `futex_init`, which
touches NULL on purpose and relies on the abort handler. The emulator
doesn't deliver exceptions or interrupts to the guest, so Linux gets no
further here. `lpj=` skips the delay-loop calibration, which would need
timer interrupts.

The 64-bit kernel (default BL1 and u-boot, `gec6818_linux_defconfig`)
boots the same way with `--load .../arch/arm64/boot/Image@0x40080000`, the
arm64 dtb and `booti 0x40080000 - 0x49000000`. Its PSCI calls are the one
exception the emulator does deliver: an AArch64 SMC enters BL1's resident
EL3 handler (`--trace-smc` lists them), so this tests BL1's PSCI end to
end. The kernel reports `PSCIv1.0 detected in firmware`, gets into the
initcalls and ends idle in `cpu_do_idle` (WFI), waiting for a timer
interrupt that never comes.

### Scripted input

In `--send`, the first byte only stops the autoboot countdown and is
dropped, hence the leading `\n`. Commands that print a lot (`md`) poll for
Ctrl-C and eat whatever is queued behind them, so put them last. The SD
card is `mmc 1` in u-boot. `mmc 0` (eMMC) is empty, so autoboot fails;
that is expected.

### Reading the output

Guest UART output is printed as-is. Emulator messages start with `[emu]`
(image headers, SD reads, CPU0 resets, u-boot entry and relocation). The
run ends when:

- the guest waits for console input (e.g. at the u-boot prompt): exit code 0
- `--timeout` seconds pass (default 30): exit code 0
- nothing is printed or read from SD for `--hang-seconds` (default 5), or
  the guest faults (unmapped access, undefined instruction, ...): exit
  code 1, with registers, a backtrace and the disassembly around PC

DDR gate leveling always prints "fail" and secondary CPUs are reported
dead. BL1 carries on, and neither is a real problem.

### Debug options

| option | what it prints |
|---|---|
| `--trace` | every function/label entered |
| `--trace-sd` | SD commands and responses |
| `--trace-smc` | every SMC (PSCI call) taken to EL3 |
| `--log-mmio` | every peripheral register access, with the PC that made it |
| `--elf`, `--uboot-elf` | (input) other symbol files |

---

## 中文

`tools/s5p6818_emu.py` 用 [Unicorn](https://www.unicorn-engine.org/) 在 CPU0
上跑 S5P6818 的启动链，不需要板子。从 BootROM（用 Python 模拟）开始，经过
NSIH1 向量存根、TIEOFF 热复位、BL1，进入下一级，一直跑到 u-boot 命令行。
改了启动链相关的代码，先在这里跑一遍再烧卡。

### 模拟了什么

| 模拟了 | 没有模拟 |
|---|---|
| BootROM：从 SD 0x200 读 NSIH1，把 BL1 加载进 SRAM，以 AArch32 启动 | 副核（BL1 会报告它们 dead） |
| 通过 CLKPWR/TIEOFF 热复位 CPU0（AArch32/AArch64、RVBAR），和芯片一致 | 中断 |
| UART0~5（输出到 stdout，UART0 的输入来自终端或 `--send`） | eMMC（u-boot 的 `mmc 0` 是空的） |
| DW MMC，SD 卡由各个镜像拼成 | 写 SD |
| PWM 定时器（u-boot 的时基）、GPIO I2C 上的 AXP228 PMIC | 客户机异常，只有 AArch64 进 EL3 的 SMC 例外 |
| LCD，只输出截图（`--screenshot`） | DDR 训练（总是报 fail，BL1 照常往下走） |
| TrustZone 的 SRAM 检查：非安全态从安全 SRAM 取指会停止运行 | DDR 上的 TrustZone |

### 准备

只需一次。需要带 venv 的 Python 3；Debian/Ubuntu 上如果 `python3 -m venv`
报错，先装 `python3-venv`。

```
python3 -m venv .venv
.venv/bin/pip install unicorn capstone
```

### 运行

先编好 BL1 和 u-boot。默认把 `out/bl1-gec6818.bin` 放在 SD 偏移 0x200，
`../u-boot_gec6818/u-boot-direct.img` 放在 BL1 的 DEVICEADDR（0x10200），
拼成 SD 卡；符号取自 `out/*.elf` 和 `../u-boot_gec6818/u-boot`。

```
.venv/bin/python tools/s5p6818_emu.py                      # 跑到 u-boot 提示符后退出
.venv/bin/python tools/s5p6818_emu.py -i                   # 交互式 u-boot 控制台，Ctrl-] 退出
.venv/bin/python tools/s5p6818_emu.py --send '\nmmc dev 1\nmmc info\n'   # 自动输入 u-boot 命令
.venv/bin/python tools/s5p6818_emu.py --bl1 other/bl1.bin --next other/u-boot-direct.img
.venv/bin/python tools/s5p6818_emu.py --sd-image sd.img    # 直接用一个 SD 卡原始镜像
.venv/bin/python tools/s5p6818_emu.py --screenshot lcd     # 同时把屏幕内容存成 lcd-N.png
```

32 位 u-boot（`UBOOT_ARCH=aarch32`）也一样：用 `--next` 指定它的
`u-boot-direct.img`，同目录下的 `u-boot` ELF 提供符号。

### ATF 链路（`SKIP_ATF=n`）

`--next` 只填 0x10200 这一个槽位。ATF 链路还要把 `fip-secure.img` 和
`fip-nonsecure.img` 放在各自的偏移上，所以先用 `write_sdcard.py --allow-file`
生成一个卡镜像，再用 `--sd-image` 传进去：

```
tools/write_sdcard.py --device atf.img --allow-file --yes --mode atf \
    --bl1 out/bl1-gec6818.bin \
    --fip-loader prebuilt/fip-loader.img --fip-secure prebuilt/fip-secure.img \
    --fip-nonsecure ../u-boot_gec6818/fip-nonsecure.img
.venv/bin/python tools/s5p6818_emu.py --sd-image atf.img --elf out/bl1-gec6818.elf
```

两种 `OPMODE` 这样都能跑到 u-boot 提示符。日志里能看到 CPU0 热复位进
0x7FD01000 的 ATF BL2。BL2 和 BL31 自己不打印任何东西。

### 启动 Linux

`--load FILE@ADDR` 在启动前把文件放进 DDR，不用做 ext4 SD 镜像就能启动内核。
linux_kernel_gec6818 的 32 位内核（`gec6818_aarch32_defconfig`）配 32 位 u-boot：

```
K=../linux_kernel_gec6818   # 或者它的 O= 编译目录
.venv/bin/python tools/s5p6818_emu.py --next 32-bit/u-boot-direct.img \
    --elf out/bl1-gec6818.elf --elf $K/vmlinux \
    --load $K/arch/arm/boot/zImage@0x40008000 \
    --load $K/arch/arm/boot/dts/s5p6818-gec6818-rev01.dtb@0x49000000 \
    --send '\nsetenv bootargs "console=ttySAC0,115200n8 earlycon=s5p6818,0xc00a1000 lpj=1000000"\nbootz 0x40008000 - 0x49000000\n' \
    --timeout 200 --hang-seconds 20
```

内核能走完 `start_kernel`（内存、时钟、定时器、VFP）进入 initcall，然后在
`futex_init` 里停在一次数据异常上：那里故意访问 NULL，靠异常处理返回。
模拟器不会把异常和中断投递给客户机，所以 Linux 在这里只能跑到这一步。
`lpj=` 用来跳过延时循环校准，那一步需要定时器中断。

64 位内核（默认的 BL1 和 u-boot，`gec6818_linux_defconfig`）也一样启动：
`--load .../arch/arm64/boot/Image@0x40080000`、arm64 的 dtb，命令用
`booti 0x40080000 - 0x49000000`。它的 PSCI 调用是模拟器唯一会投递的异常：
AArch64 的 SMC 会进入 BL1 常驻 EL3 的处理程序（`--trace-smc` 可以列出每一次），
所以这能端到端地检查 BL1 的 PSCI。内核会打印 `PSCIv1.0 detected in firmware`，
进入 initcall，最后在 `cpu_do_idle`（WFI）里空闲，等一个永远不会来的定时器中断。

### 自动输入

`--send` 的第一个字节只用来打断 autoboot 倒计时，会被吃掉，所以开头放一个
`\n`。输出很多的命令（比如 `md`）会检查 Ctrl-C，把排在后面的输入吃掉，所以
放在最后。SD 卡在 u-boot 里是 `mmc 1`；`mmc 0`（eMMC）没接东西，所以
autoboot 会失败，这是正常的。

### 看输出

客户机的串口输出原样打印，模拟器自己的信息以 `[emu]` 开头（镜像头、SD 读取、
CPU0 复位、进入 u-boot、u-boot 重定位）。以下情况会结束运行：

- 客户机在等控制台输入（比如停在 u-boot 提示符）：退出码 0
- 超过 `--timeout` 秒（默认 30）：退出码 0
- `--hang-seconds`（默认 5）秒内既没有串口输出也没有读 SD，或者客户机出错
  （访问未映射地址、未定义指令等）：退出码 1，并打印寄存器、调用栈和 PC
  附近的反汇编

DDR 门控训练总是打印 fail，副核会被报告为 dead；BL1 会照常往下走，这两处
都不是真问题。

### 调试选项

| 选项 | 打印什么 |
|---|---|
| `--trace` | 进入的每个函数/标号 |
| `--trace-sd` | SD 命令和响应 |
| `--trace-smc` | 每一次进入 EL3 的 SMC（PSCI 调用） |
| `--log-mmio` | 每次外设寄存器访问，以及发起访问的 PC |
| `--elf`、`--uboot-elf` | （输入）指定其他符号文件 |
