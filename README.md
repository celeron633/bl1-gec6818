# bl1-gec6818

[![Build](https://github.com/celeron633/bl1-gec6818/actions/workflows/build.yml/badge.svg)](https://github.com/celeron633/bl1-gec6818/actions/workflows/build.yml)

BL1 ("2nd bootloader") for the Nexell S5P6818 SoC, adapted from FriendlyARM's
NanoPi3 support to run on the **GEC6818** board.

**English** | [中文](#中文)

## English

### What this is

BootROM loads this code into SRAM and jumps to it. BL1 brings up clocks and
DRAM, then loads one more image from SD/eMMC and jumps to it - either the
normal ARM Trusted Firmware chain, or (new in this fork) straight to u-boot.
See **[docs/BOOT_MODES.md](docs/BOOT_MODES.md)** for the full chain diagram, on-disk
offsets, and how to build/flash each mode. See **[docs/NSIH.md](docs/NSIH.md)** for
the header format BootROM reads to load/launch BL1 itself, including how
to switch between SD and eMMC boot (`tools/set_boot_port.py`).

This is one of three repos that make up the board's software:

- **bl1-gec6818** (this repo) - BL1
- [u-boot_gec6818](https://github.com/celeron633/u-boot_gec6818) (branch
  `gec6818-v2016.01`) - BL33 / u-boot
- [linux_kernel_gec6818](https://github.com/celeron633/linux_kernel_gec6818)
  (branch `gec6818-v4.4.y`) - kernel

### Build configurations

`OPMODE` is the instruction set BL1 itself runs in, `SKIP_ATF` whether
ARM Trusted Firmware is used, `UBOOT_ARCH` (default `aarch64`) whether the
u-boot that a `SKIP_ATF=y` BL1 jumps to is 64- or 32-bit:

| `OPMODE` | `SKIP_ATF` | chain | PSCI from | switch to AArch64 happens in | image at SD offset 0x10200 |
|---|---|---|---|---|---|
| `aarch32` | `n` | BL1 → `fip-loader.img` → `fip-secure.img` (BL31+OP-TEE) → u-boot → Linux | BL31 | `fip-loader.img` (its entry is AArch32 code) | `fip-loader.img` |
| `aarch32` | `y` | BL1 → stage2 → u-boot → Linux | stage2, resident in SRAM at EL3 (`src/stage2_main.c`, `src/psci.c`) | BL1, by warm-resetting CPU0 into stage2 | `u-boot-direct.img` |
| `aarch64` | `y` | BL1 → u-boot → Linux | BL1 itself, resident at EL3 (`src/psci.c`) | BootROM, via the NSIH header's vector, before BL1 runs | `u-boot-direct.img` |
| `aarch64` | `n` | not usable: BL1 would jump into `fip-loader.img`'s AArch32 entry in AArch64 | | | |
| `aarch32` | `y`, `UBOOT_ARCH=aarch32` | BL1 → 32-bit u-boot → (old 32-bit kernel), all in AArch32 secure SVC | none | never | 32-bit `u-boot-direct.img` |

The default (`config.mak`) is `OPMODE=aarch64 SKIP_ATF=y`. The two
`SKIP_ATF=y` configurations are not yet verified on hardware - see
[docs/BOOT_MODES.md](docs/BOOT_MODES.md).

### Building

Toolchains:
- `aarch64-none-elf` ([Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  13.2.rel1) - `OPMODE=aarch64`, and the stage2 of `OPMODE=aarch32 SKIP_ATF=y`
- `arm-linux-gnueabi` (tested: Linaro 7.5.0-2019.12) - `OPMODE=aarch32`

```sh
wget https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf.tar.xz
tar xf arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf.tar.xz -C ~/
export PATH=~/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf/bin:$PATH
```

`make clean` whenever you switch configurations (the Makefile doesn't
track CFLAGS changes):

```sh
make clean && make                                                           # default: no ATF, AArch64 BL1
make clean && make OPMODE=aarch32 CROSS_TOOL=arm-linux-gnueabi-              # no ATF, via stage2
make clean && make OPMODE=aarch32 SKIP_ATF=n CROSS_TOOL=arm-linux-gnueabi-   # ATF chain
make clean && make OPMODE=aarch32 UBOOT_ARCH=aarch32 CROSS_TOOL=arm-linux-gnueabi-   # pure AArch32, 32-bit u-boot
```

Add `BOOT_PORT=emmc` to boot from eMMC instead of SD. The no-ATF
configurations need u-boot's `u-boot-direct.img` (`make u-boot-direct.img`
in u-boot_gec6818), not `fip-nonsecure.img`.

Right after DDR init, BL1 turns on the RGB LCD (AT070TN92, 800x480),
shows `BOOT_LOGO_TEXT` as a title and then one line per boot step (DDR
done, loading u-boot, loaded size/address, jumping to it), pausing
`BOOT_LOGO_DELAY_MS` (default 500) after each so they can be read. It stays
until u-boot sets up the display for its own logo (`src/display.c`).
`make BOOT_LOGO=n` leaves it out, `make BOOT_LOGO_TEXT='...'` changes the
title, `make BOOT_LOGO_DELAY_MS=0` removes the pauses. The backlight pad (GPIOD1/PWM0) is left alone unless you pass
`LCD_BACKLIGHT=high` or `LCD_BACKLIGHT=low`, because nobody has checked
on hardware yet which level turns it on.

GitHub Actions builds the default configuration (plus `u-boot-direct.img`)
on every push and uploads them as a workflow artifact - see the badge above, or
`.github/workflows/build.yml`.

### Flashing

`tools/write_sdcard.py` writes BL1 + the rest of the chain to the right
byte offsets on an SD card for either mode - see docs/BOOT_MODES.md for the
exact commands. **Test on a spare SD card, not eMMC** - wrong offsets on
eMMC can brick the board.

### Emulator

`tools/s5p6818_emu.py` runs the boot chain on CPU0 in Unicorn, no board
needed: BootROM (emulated in Python) -> NSIH1 vector stub -> TIEOFF warm
reset -> BL1 -> NSIH2 -> u-boot, down to the u-boot prompt. It models the
UARTs, DW MMC (SD card built from the images), timer and the PLL/DDR status
polls; secondary cores, interrupts and eMMC are not emulated.

**Setup** (once). Needs Python 3 with venv; on Debian/Ubuntu install
`python3-venv` first if `python3 -m venv` fails:

```
python3 -m venv .venv
.venv/bin/pip install unicorn capstone
```

**Run.** Build BL1 and u-boot first. By default the SD card image is
`out/bl1-gec6818.bin` at offset 0x200 plus `../u-boot_gec6818/u-boot-direct.img`
at BL1's DEVICEADDR (0x10200), and symbols come from `out/*.elf` and
`../u-boot_gec6818/u-boot`:

```
.venv/bin/python tools/s5p6818_emu.py                      # boot to the u-boot prompt, then exit
.venv/bin/python tools/s5p6818_emu.py -i                   # interactive u-boot console, Ctrl-] quits
.venv/bin/python tools/s5p6818_emu.py --send '\nmmc dev 1\nmmc info\n'   # scripted u-boot commands
.venv/bin/python tools/s5p6818_emu.py --bl1 other/bl1.bin --next other/u-boot-direct.img
.venv/bin/python tools/s5p6818_emu.py --sd-image sd.img    # a raw SD card image instead
.venv/bin/python tools/s5p6818_emu.py --screenshot lcd      # also save the LCD as lcd-N.png
```

A 32-bit u-boot (`UBOOT_ARCH=aarch32`) runs the same way: pass its
`u-boot-direct.img` with `--next`; the `u-boot` ELF next to it provides
the symbols.

`--load FILE@ADDR` puts files in DDR before the boot starts, e.g. to boot
linux_kernel_gec6818's 32-bit kernel (`gec6818_aarch32_defconfig`) without
building an ext4 SD image:

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
the initcalls, then stops with a data abort in `futex_init`, which
touches NULL on purpose and relies on the abort handler. The emulator
doesn't deliver exceptions or interrupts to the guest, so that is as far
as Linux gets here. `lpj=` skips the delay-loop calibration, which would
need timer interrupts.

The 64-bit kernel (default BL1 and u-boot, `gec6818_linux_defconfig`)
boots the same way with `--load .../arch/arm64/boot/Image@0x40080000`,
the arm64 dtb and `booti 0x40080000 - 0x49000000`. Its PSCI calls are
the one exception the emulator does deliver: an AArch64 SMC enters BL1's
resident EL3 handler (`--trace-smc` lists them), so this checks BL1's
PSCI end to end. The kernel reports `PSCIv1.0 detected in firmware`,
gets into the initcalls and ends idle in `cpu_do_idle` (WFI), waiting
for a timer interrupt that never comes.

In `--send`, the first byte only stops the autoboot countdown and is
dropped, hence the leading `\n`. Commands that print a lot (`md`) poll for
Ctrl-C and eat whatever is queued behind them, so put them last. The SD card is `mmc 1` in u-boot; `mmc 0`
(eMMC) is empty, so autoboot fails - expected.

**Reading the output.** Guest UART output is printed as-is, emulator
messages start with `[emu]` (image headers, SD reads, CPU0 resets, u-boot
entry and relocation). The run ends when:

- the guest waits for console input (e.g. at the u-boot prompt) - exit code 0
- `--timeout` seconds pass (default 30) - exit code 0
- nothing is printed or read from SD for `--hang-seconds` (default 5), or the
  guest faults (unmapped access, undefined instruction, SMC, ...) - exit
  code 1, with registers, a backtrace and disassembly around PC

In the emulator, DDR gate leveling always prints "fail" and secondary CPUs
are reported dead; BL1 carries on and neither is a real problem.

**Debug options:** `--trace` (every function/label entered), `--trace-sd`
(SD commands and responses), `--log-mmio` (every peripheral register
access with the PC that did it), `--elf`/`--uboot-elf` (other symbol files).

### Source tree

```
.
├── src              # BL1 source
├── prototype        # Nexell SoC register/base headers
│   ├── base
│   └── module
├── nsih-generator    # Excel-based NSIH header generator (Windows)
├── reference-nsih    # reference NSIH text files for various boards
└── tools             # write_sdcard.py, s5p6818_emu.py
```

Key files:

| file | what |
|---|---|
| `src/startup_aarch64.S` | entry point (BootROM jumps here), EL3 exception vectors |
| `src/secondboot.c` | `BootMain()` - clock/DDR/secure-state init, loads and launches the next stage |
| `src/psci.c` | minimal PSCI implementation, only used in `SKIP_ATF` mode |
| `src/pmic.c` | PMIC control (ARM/DDR voltage) |
| `src/init_DDR3.c` / `src/init_LPDDR3.c` | DRAM init |
| `config.mak` | build config: `OPMODE`, `BOARD`, `SKIP_ATF`, toolchain prefix |

### Original README

The original, unmodified Nexell/vendor README is kept at
[`README.orig.txt`](README.orig.txt).

---

## 中文

### 这个仓库是什么

BootROM 把这份代码加载到 SRAM 里跳过去执行。BL1 负责时钟和内存初始化，然后从
SD/eMMC 再读一个镜像并跳过去——要么走正常的 ARM Trusted Firmware 链路，要么
（这个 fork 新加的）直接跳 u-boot。完整的链路图、磁盘偏移量表、两种模式怎么编译
/烧录，看 **[docs/BOOT_MODES.md](docs/BOOT_MODES.md)**。BootROM 加载/启动 BL1 自己用的头格式，
以及怎么切换 SD/eMMC 启动（`tools/set_boot_port.py`），看 **[docs/NSIH.md](docs/NSIH.md)**。

这块板子的软件由三个仓库组成：

- **bl1-gec6818**（本仓库）—— BL1
- [u-boot_gec6818](https://github.com/celeron633/u-boot_gec6818)（分支
  `gec6818-v2016.01`）—— BL33 / u-boot
- [linux_kernel_gec6818](https://github.com/celeron633/linux_kernel_gec6818)
  （分支 `gec6818-v4.4.y`）—— 内核

### 编译配置

`OPMODE` 决定 BL1 自己跑在哪个指令集，`SKIP_ATF` 决定用不用 ARM Trusted Firmware，
`UBOOT_ARCH`（默认 `aarch64`）决定 `SKIP_ATF=y` 时 BL1 跳去的 u-boot 是 64 位还是 32 位：

| `OPMODE` | `SKIP_ATF` | 链路 | PSCI 由谁提供 | 在哪里切到 AArch64 | SD 卡 0x10200 处放 |
|---|---|---|---|---|---|
| `aarch32` | `n` | BL1 → `fip-loader.img` → `fip-secure.img`（BL31+OP-TEE）→ u-boot → Linux | BL31 | `fip-loader.img`（它的入口是 AArch32 代码） | `fip-loader.img` |
| `aarch32` | `y` | BL1 → stage2 → u-boot → Linux | stage2，常驻 SRAM、运行在 EL3（`src/stage2_main.c`、`src/psci.c`） | BL1 热复位 CPU0 进入 stage2 | `u-boot-direct.img` |
| `aarch64` | `y` | BL1 → u-boot → Linux | BL1 自己，常驻 EL3（`src/psci.c`） | BootROM 执行 NSIH 头里的向量，BL1 运行之前就已切换 | `u-boot-direct.img` |
| `aarch64` | `n` | 不可用：BL1 会以 AArch64 跳进 `fip-loader.img` 的 AArch32 入口 | | | |
| `aarch32` | `y`，`UBOOT_ARCH=aarch32` | BL1 → 32 位 u-boot →（老的 32 位内核），全程 AArch32 安全态 SVC | 无 | 不切换 | 32 位的 `u-boot-direct.img` |

默认配置（`config.mak`）是 `OPMODE=aarch64 SKIP_ATF=y`。两种 `SKIP_ATF=y` 配置都还没上板验证，
详见 [docs/BOOT_MODES.md](docs/BOOT_MODES.md)。

### 编译

工具链：
- `aarch64-none-elf`（[Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  13.2.rel1）：用于 `OPMODE=aarch64`，以及 `OPMODE=aarch32 SKIP_ATF=y` 里的 stage2
- `arm-linux-gnueabi`（已测试 Linaro 7.5.0-2019.12）：用于 `OPMODE=aarch32`

```sh
wget https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf.tar.xz
tar xf arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf.tar.xz -C ~/
export PATH=~/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf/bin:$PATH
```

切换配置时一定先 `make clean`（Makefile 检测不到 CFLAGS 的变化）：

```sh
make clean && make                                                           # 默认：不走 ATF，AArch64 BL1
make clean && make OPMODE=aarch32 CROSS_TOOL=arm-linux-gnueabi-              # 不走 ATF，经 stage2
make clean && make OPMODE=aarch32 SKIP_ATF=n CROSS_TOOL=arm-linux-gnueabi-   # ATF 链路
make clean && make OPMODE=aarch32 UBOOT_ARCH=aarch32 CROSS_TOOL=arm-linux-gnueabi-   # 纯 AArch32，32 位 u-boot
```

加 `BOOT_PORT=emmc` 改为从 eMMC 启动。不走 ATF 的两种配置要配 u-boot 的
`u-boot-direct.img`（在 u-boot_gec6818 里 `make u-boot-direct.img`），不是 `fip-nonsecure.img`。

DDR 初始化完成后，BL1 会点亮 RGB 屏（AT070TN92，800x480），先显示标题
`BOOT_LOGO_TEXT`，再每个启动步骤一行（DDR 完成、开始读 u-boot、读到的大小和地址、
跳转），每行之后停 `BOOT_LOGO_DELAY_MS`（默认 500）毫秒方便看清，一直保留到 u-boot
重新初始化显示、画它自己的 logo（`src/display.c`）。`make BOOT_LOGO=n` 去掉这个功能，
`make BOOT_LOGO_TEXT='...'` 改标题，`make BOOT_LOGO_DELAY_MS=0` 去掉停顿。背光脚（GPIOD1/PWM0）默认不动，要驱动它就加 `LCD_BACKLIGHT=high` 或
`LCD_BACKLIGHT=low`——哪个电平是亮，还没在板子上确认过。

每次 push，GitHub Actions 会编译默认配置（以及配套的 `u-boot-direct.img`），打包成
workflow artifact 上传——看上面的徽章，或者 `.github/workflows/build.yml`。

### 烧录

`tools/write_sdcard.py` 会把 BL1 和链路上其余镜像写到 SD 卡上正确的字节偏移，两种
模式的具体命令见 docs/BOOT_MODES.md。**先在备用 SD 卡上测，别碰 eMMC**——eMMC 上偏移
写错有砖机风险。

### 模拟器

`tools/s5p6818_emu.py` 用 Unicorn 在 CPU0 上跑整条启动链，不需要板子：BootROM（用
Python 模拟）-> NSIH1 向量存根 -> TIEOFF 热复位 -> BL1 -> NSIH2 -> u-boot，一直到
u-boot 命令行。模拟了 UART、DW MMC（用镜像拼出 SD 卡）、定时器，以及 PLL/DDR 的状态
轮询；副核、中断和 eMMC 没有模拟。

**准备**（只需一次）。需要带 venv 的 Python 3；Debian/Ubuntu 上如果 `python3 -m venv`
报错，先装 `python3-venv`：

```
python3 -m venv .venv
.venv/bin/pip install unicorn capstone
```

**运行**。先编好 BL1 和 u-boot。默认把 `out/bl1-gec6818.bin` 放在 SD 偏移 0x200，
`../u-boot_gec6818/u-boot-direct.img` 放在 BL1 的 DEVICEADDR（0x10200），拼成 SD 卡；
符号取自 `out/*.elf` 和 `../u-boot_gec6818/u-boot`：

```
.venv/bin/python tools/s5p6818_emu.py                      # 跑到 u-boot 提示符后退出
.venv/bin/python tools/s5p6818_emu.py -i                   # 交互式 u-boot 控制台，Ctrl-] 退出
.venv/bin/python tools/s5p6818_emu.py --send '\nmmc dev 1\nmmc info\n'   # 自动输入 u-boot 命令
.venv/bin/python tools/s5p6818_emu.py --bl1 other/bl1.bin --next other/u-boot-direct.img
.venv/bin/python tools/s5p6818_emu.py --sd-image sd.img    # 直接用一个 SD 卡原始镜像
.venv/bin/python tools/s5p6818_emu.py --screenshot lcd      # 同时把屏幕内容存成 lcd-N.png
```

32 位 u-boot（`UBOOT_ARCH=aarch32`）也一样跑：用 `--next` 指定它的 `u-boot-direct.img`，
同目录下的 `u-boot` ELF 会被用来提供符号。

`--load FILE@ADDR` 在启动前把文件放进 DDR，比如不做 ext4 SD 镜像，直接启动
linux_kernel_gec6818 的 32 位内核（`gec6818_aarch32_defconfig`）：

```
K=../linux_kernel_gec6818   # 或者它的 O= 编译目录
.venv/bin/python tools/s5p6818_emu.py --next 32-bit/u-boot-direct.img \
    --elf out/bl1-gec6818.elf --elf $K/vmlinux \
    --load $K/arch/arm/boot/zImage@0x40008000 \
    --load $K/arch/arm/boot/dts/s5p6818-gec6818-rev01.dtb@0x49000000 \
    --send '\nsetenv bootargs "console=ttySAC0,115200n8 earlycon=s5p6818,0xc00a1000 lpj=1000000"\nbootz 0x40008000 - 0x49000000\n' \
    --timeout 200 --hang-seconds 20
```

内核能走完 `start_kernel`（内存、时钟、定时器、VFP）进入 initcall，然后在 `futex_init`
里停在一次数据异常上：那里故意访问 NULL，靠异常处理返回。模拟器不会把异常和中断
投递给客户机，所以 Linux 在这里只能跑到这一步。`lpj=` 用来跳过延时循环校准，
那一步需要定时器中断。

64 位内核（默认的 BL1 和 u-boot，`gec6818_linux_defconfig`）也一样启动：
`--load .../arch/arm64/boot/Image@0x40080000`、arm64 的 dtb，命令用
`booti 0x40080000 - 0x49000000`。它的 PSCI 调用是模拟器唯一会投递的异常：AArch64 的
SMC 会进入 BL1 常驻 EL3 的处理程序（`--trace-smc` 可以列出每一次），所以这能端到端地
检查 BL1 的 PSCI。内核会打印 `PSCIv1.0 detected in firmware`，进入 initcall，最后在
`cpu_do_idle`（WFI）里空闲，等一个永远不会来的定时器中断。

`--send` 的第一个字节只用来打断 autoboot 倒计时，会被吃掉，所以开头放一个 `\n`。
输出很多的命令（比如 `md`）会检查 Ctrl-C，把排在后面的输入吃掉，所以放在最后。
SD 卡在 u-boot 里是 `mmc 1`；`mmc 0`（eMMC）没接东西，所以 autoboot 会失败，这是正常的。

**看输出**。客户机的串口输出原样打印，模拟器自己的信息以 `[emu]` 开头（镜像头、SD
读取、CPU0 复位、进入 u-boot、u-boot 重定位）。以下情况会结束运行：

- 客户机在等控制台输入（比如停在 u-boot 提示符）——退出码 0
- 超过 `--timeout` 秒（默认 30）——退出码 0
- `--hang-seconds`（默认 5）秒内既没有串口输出也没有读 SD，或者客户机出错（访问未映射
  地址、未定义指令、SMC 等）——退出码 1，并打印寄存器、调用栈和 PC 附近的反汇编

模拟器里 DDR 门控训练总是打印 fail，副核会被报告为 dead；BL1 会照常往下走，这两处都
不是真问题。

**调试选项**：`--trace`（打印进入的每个函数/标号）、`--trace-sd`（SD 命令和响应）、
`--log-mmio`（每次外设寄存器访问及对应 PC）、`--elf`/`--uboot-elf`（指定其他符号文件）。

### 目录结构

```
.
├── src              # BL1 源码
├── prototype        # Nexell SoC 寄存器/基础头文件
│   ├── base
│   └── module
├── nsih-generator    # 基于 Excel 的 NSIH 头生成工具（Windows）
├── reference-nsih    # 各板子的参考 NSIH 文本
└── tools             # write_sdcard.py、s5p6818_emu.py
```

关键文件：

| 文件 | 作用 |
|---|---|
| `src/startup_aarch64.S` | 入口点（BootROM 跳到这）、EL3 异常向量表 |
| `src/secondboot.c` | `BootMain()`——时钟/DDR/安全态初始化，加载并跳转到下一级 |
| `src/psci.c` | 最小化 PSCI 实现，只在 `SKIP_ATF` 模式下用 |
| `src/pmic.c` | PMIC 控制（ARM/DDR 电压） |
| `src/init_DDR3.c` / `src/init_LPDDR3.c` | DRAM 初始化 |
| `config.mak` | 编译配置：`OPMODE`、`BOARD`、`SKIP_ATF`、工具链前缀 |

### 原始 README

原始、未修改的 Nexell/厂商 README 保留在
[`README.orig.txt`](README.orig.txt)。
