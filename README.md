# bl1-gec6818

[![Build](https://github.com/celeron633/bl1-gec6818/actions/workflows/build.yml/badge.svg)](https://github.com/celeron633/bl1-gec6818/actions/workflows/build.yml)

BL1 for the **Samsung/Nexell S5P6818** SoC (8x Cortex-A53), running on the
**GEC6818** board.

**English** | [中文](#中文)

## English

### What this is

This is the first boot stage after the S5P6818's on-chip BootROM. Nexell
calls it the "2nd boot", ARM Trusted Firmware would call it BL1. BootROM
loads it from SD/eMMC into the chip's 64KB internal SRAM (0xFFFF0000) and
starts it. BL1 then sets up the clocks, the PMIC and DDR, loads the next
image from SD/eMMC into DDR and starts it. That next image is either the
vendor's ARM Trusted Firmware chain or, new in this fork, u-boot directly,
with BL1 providing PSCI itself.

The code is Nexell's S5P6818 BL1, via FriendlyARM's NanoPi3 port, adapted to
the GEC6818: its DDR3, the AXP228 PMIC, the RGB LCD, and booting from the SD
slot or eMMC.

The board's software is split across three repos:

- **bl1-gec6818** (this repo): BL1
- [u-boot_gec6818](https://github.com/celeron633/u-boot_gec6818) (branch
  `gec6818-v2016.01`): u-boot
- [linux_kernel_gec6818](https://github.com/celeron633/linux_kernel_gec6818)
  (branch `gec6818-v4.4.y`): kernel

### Boot modes

Two build variables pick the chain. `OPMODE` is the instruction set BL1 runs
in, and `SKIP_ATF` says whether ARM Trusted Firmware is used.

| configuration | chain | PSCI from | tested on |
|---|---|---|---|
| `OPMODE=aarch64 SKIP_ATF=y` (default) | BL1 → `u-boot-direct.img` → Linux | BL1, resident at EL3 | board: u-boot, Linux with 8 CPUs |
| `OPMODE=aarch64 SKIP_ATF=n` | BL1 → `fip-loader.img` (ATF BL2) → `fip-secure.img` (BL31 + OP-TEE) → `fip-nonsecure.img` (u-boot) → Linux | BL31 | emulator |
| `OPMODE=aarch32 SKIP_ATF=n` | same as above; this is how the vendor's BL1 boots | BL31 | emulator |
| `OPMODE=aarch32 SKIP_ATF=y` | BL1 → stage2 → `u-boot-direct.img` → Linux | stage2, AArch64, resident at EL3 | emulator |
| `OPMODE=aarch32 UBOOT_ARCH=aarch32` | BL1 → 32-bit `u-boot-direct.img` → 32-bit kernel, all in AArch32 | none | emulator |

[docs/BOOT_MODES.md](docs/BOOT_MODES.md) has the details of each chain:
where the switch to AArch64 happens, load addresses, SD offsets, and what to
check when a mode doesn't boot.

### Building

Toolchains:

- `aarch64-none-elf` ([Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  13.2.rel1): `OPMODE=aarch64`, and the stage2 of `OPMODE=aarch32 SKIP_ATF=y`
- `arm-linux-gnueabi` (tested: Linaro 7.5.0-2019.12): `OPMODE=aarch32`

```sh
wget https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf.tar.xz
tar xf arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf.tar.xz -C ~/
export PATH=~/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf/bin:$PATH
```

The easiest way to pick a configuration is the menu. It only offers valid
combinations and shows the resulting boot chain:

```sh
make menuconfig          # or tools/menuconfig.py; --print just shows the config
make                     # -> out/bl1-gec6818.bin
```

It saves to `.config.mak` (git-ignored), which `config.mak` reads in place
of its defaults. Objects depend on that file, so a plain `make` after saving
rebuilds everything.

Variables on the make command line override `.config.mak`. Run `make clean`
when switching configurations this way, since the Makefile doesn't track
CFLAGS:

```sh
make clean && make                                           # default
make clean && make SKIP_ATF=n                                # ATF chain, AArch64 BL1
make clean && make OPMODE=aarch32 SKIP_ATF=n CROSS_TOOL=arm-linux-gnueabi-
make clean && make OPMODE=aarch32 CROSS_TOOL=arm-linux-gnueabi-
make clean && make OPMODE=aarch32 UBOOT_ARCH=aarch32 CROSS_TOOL=arm-linux-gnueabi-
```

Other options:

| variable | default | what |
|---|---|---|
| `BOOT_PORT` | `sd` | `emmc` boots from eMMC instead, see [docs/NSIH.md](docs/NSIH.md) |
| `INITPMIC` | `YES` | set up the AXP228: bridge DCDC2/DCDC3 (both feed the ARM cores), DCDC4/DCDC5 in PWM mode |
| `BOOT_LOGO` | `y` | LCD boot screen, see below |
| `SYSLOG` | `y` | BL1's serial log |

u-boot_gec6818 builds the image that goes after BL1. The `SKIP_ATF=y` modes
need `make u-boot-direct.img`, and the ATF chain needs `fip-nonsecure.img`.

GitHub Actions builds the default configuration and `u-boot-direct.img` on
every push and uploads them as a workflow artifact (`.github/workflows/build.yml`).

### Flashing

`tools/write_sdcard.py` writes BL1 and the rest of the chain to the right
byte offsets on an SD card, and nothing else:

```sh
# SKIP_ATF=y
tools/write_sdcard.py --device /dev/sdX --mode direct \
    --bl1 out/bl1-gec6818.bin --uboot ../u-boot_gec6818/u-boot-direct.img

# SKIP_ATF=n
tools/write_sdcard.py --device /dev/sdX --mode atf --bl1 out/bl1-gec6818.bin \
    --fip-loader prebuilt/fip-loader.img --fip-secure prebuilt/fip-secure.img \
    --fip-nonsecure ../u-boot_gec6818/fip-nonsecure.img
```

It refuses mounted devices and asks you to type the device path again before
writing. `--dry-run` only shows the plan. **Test on a spare SD card first.**
Once the chain boots from SD, write it to the eMMC as described in
[docs/BURN.md](docs/BURN.md). The eMMC images need a different port number,
and wrong offsets there can brick the board.

### LCD boot screen

After DDR init, BL1 turns on the RGB LCD (AT070TN92, 800x480). It shows a
title, one line per boot step (build date, boot chain, clocks, DDR,
TrustZone, CPUs, loading and starting the next stage) and a progress bar.
The screen stays up until u-boot draws its own logo (`src/display.c`).

| variable | default | what |
|---|---|---|
| `BOOT_LOGO_TEXT` | `S5P6818 BL1 Loading...` | title |
| `BOOT_LOGO_DELAY_MS` | `500` | pause after each step; `0` removes the pauses |
| `LCD_BACKLIGHT` | `none` | drive the backlight pad (GPIOD1/PWM0) `high` or `low`. Left alone by default: which level turns it on hasn't been checked on the board yet |

### Emulator

`tools/s5p6818_emu.py` runs the whole boot chain in Unicorn, from BootROM to
the u-boot prompt, without a board. It covers all the modes above and can
also start a kernel. See [docs/EMULATOR.md](docs/EMULATOR.md).

```sh
python3 -m venv .venv && .venv/bin/pip install unicorn capstone
.venv/bin/python tools/s5p6818_emu.py
```

### Documentation

| document | topic |
|---|---|
| [docs/BOOT_MODES.md](docs/BOOT_MODES.md) | each boot chain in detail, SD layout, troubleshooting |
| [docs/NSIH.md](docs/NSIH.md) | the header BootROM reads to load BL1; switching between SD and eMMC boot (`tools/set_boot_port.py`) |
| [docs/SMP_PSCI.md](docs/SMP_PSCI.md) | how BL1's PSCI brings up the secondary CPUs |
| [docs/EMULATOR.md](docs/EMULATOR.md) | the emulator |
| [docs/BURN.md](docs/BURN.md) | writing the SD-tested chain to eMMC, from u-boot (TFTP) or Linux (`dd`) |

All docs are in both English and Chinese.

### Source tree

```
.
├── src              # BL1 source
├── prototype        # Nexell SoC register/base headers
├── prebuilt         # vendor binaries: fip-loader.img, fip-secure.img, the original BL1
├── reference-nsih   # NSIH header templates (BL1's header is built from these)
├── nsih-generator   # Excel-based NSIH header generator (Windows)
├── tools            # build, flashing and emulator scripts
└── docs
```

| file | what |
|---|---|
| `src/startup_aarch64.S`, `src/startup_aarch32.S` | entry point, EL3 exception vectors |
| `src/secondboot.c` | `BootMain()`: clock/DDR/secure-state init, loads and starts the next stage |
| `src/psci.c` | PSCI for `SKIP_ATF=y` (in BL1 or in stage2) |
| `src/stage2_main.c` | AArch64 stage2 for `OPMODE=aarch32 SKIP_ATF=y` |
| `src/pmic.c` | AXP228 setup over GPIOC15/16 |
| `src/display.c` | LCD boot screen |
| `src/init_DDR3.c` | DRAM init |
| `config.mak` | build configuration; reads `.config.mak` first |
| `tools/menuconfig.py` | `make menuconfig` |
| `tools/mk_bl1_image.py` | puts the NSIH header (and stage2) around the BL1 binary |
| `tools/write_sdcard.py` | writes the images to an SD card or an image file |
| `tools/set_boot_port.py` | switches a built BL1 between SD and eMMC boot |
| `tools/s5p6818_emu.py` | the emulator |

The original Nexell README is kept in [`README.orig.txt`](README.orig.txt).

---

## 中文

### 这个仓库是什么

这是 **S5P6818** 处理器（三星/Nexell，8 核 Cortex-A53）的 BL1，跑在 **GEC6818**
开发板上。

它是 S5P6818 片内 BootROM 之后的第一级引导：Nexell 叫它 "2nd boot"，按
ARM Trusted Firmware 的叫法是 BL1。BootROM 从 SD/eMMC 把它加载进
芯片内部 64KB 的 SRAM（0xFFFF0000）并启动它。BL1 负责初始化时钟、PMIC 和 DDR，
再从 SD/eMMC 把下一级镜像读进 DDR 并启动：要么是厂商的 ARM Trusted Firmware
链路，要么（这个 fork 新加的）直接启动 u-boot，由 BL1 自己提供 PSCI。

代码来自 Nexell 的 S5P6818 BL1，经 FriendlyARM 的 NanoPi3 移植，再适配到
GEC6818：DDR3、AXP228 PMIC、RGB 屏，以及从 SD 卡槽或 eMMC 启动。

这块板子的软件分在三个仓库里：

- **bl1-gec6818**（本仓库）：BL1
- [u-boot_gec6818](https://github.com/celeron633/u-boot_gec6818)（分支
  `gec6818-v2016.01`）：u-boot
- [linux_kernel_gec6818](https://github.com/celeron633/linux_kernel_gec6818)
  （分支 `gec6818-v4.4.y`）：内核

### 启动模式

两个编译变量决定走哪条链路：`OPMODE` 是 BL1 自己运行的指令集，`SKIP_ATF`
决定用不用 ARM Trusted Firmware。

| 配置 | 链路 | PSCI 由谁提供 | 验证情况 |
|---|---|---|---|
| `OPMODE=aarch64 SKIP_ATF=y`（默认） | BL1 → `u-boot-direct.img` → Linux | BL1，常驻 EL3 | 板子：u-boot、Linux 起 8 个核 |
| `OPMODE=aarch64 SKIP_ATF=n` | BL1 → `fip-loader.img`（ATF BL2）→ `fip-secure.img`（BL31 + OP-TEE）→ `fip-nonsecure.img`（u-boot）→ Linux | BL31 | 模拟器 |
| `OPMODE=aarch32 SKIP_ATF=n` | 同上；厂商的 BL1 就是这样启动的 | BL31 | 模拟器 |
| `OPMODE=aarch32 SKIP_ATF=y` | BL1 → stage2 → `u-boot-direct.img` → Linux | stage2，AArch64，常驻 EL3 | 模拟器 |
| `OPMODE=aarch32 UBOOT_ARCH=aarch32` | BL1 → 32 位 `u-boot-direct.img` → 32 位内核，全程 AArch32 | 无 | 模拟器 |

每条链路的细节（在哪里切到 AArch64、加载地址、SD 偏移、启动不起来时查什么）
见 [docs/BOOT_MODES.md](docs/BOOT_MODES.md)。

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

选配置最方便的是用菜单。不合法的组合选不出来，还会显示对应的启动链路：

```sh
make menuconfig          # 或 tools/menuconfig.py；加 --print 只打印当前配置
make                     # -> out/bl1-gec6818.bin
```

配置保存在 `.config.mak`（已被 git 忽略），`config.mak` 会用它代替自己的默认值。
所有目标文件都依赖这个文件，所以保存后直接 `make` 就会全部重新编译。

命令行上的变量优先于 `.config.mak`。用命令行切换配置时要先 `make clean`，
因为 Makefile 检测不到 CFLAGS 的变化：

```sh
make clean && make                                           # 默认
make clean && make SKIP_ATF=n                                # ATF 链路，AArch64 BL1
make clean && make OPMODE=aarch32 SKIP_ATF=n CROSS_TOOL=arm-linux-gnueabi-
make clean && make OPMODE=aarch32 CROSS_TOOL=arm-linux-gnueabi-
make clean && make OPMODE=aarch32 UBOOT_ARCH=aarch32 CROSS_TOOL=arm-linux-gnueabi-
```

其他选项：

| 变量 | 默认值 | 作用 |
|---|---|---|
| `BOOT_PORT` | `sd` | `emmc` 改为从 eMMC 启动，见 [docs/NSIH.md](docs/NSIH.md) |
| `INITPMIC` | `YES` | 配置 AXP228：桥接 DCDC2/DCDC3（两路都给 ARM 核供电），DCDC4/DCDC5 强制 PWM |
| `BOOT_LOGO` | `y` | LCD 启动画面，见下文 |
| `SYSLOG` | `y` | BL1 的串口日志 |

BL1 后面那一级镜像由 u-boot_gec6818 编译：`SKIP_ATF=y` 的模式用
`make u-boot-direct.img`，ATF 链路用 `fip-nonsecure.img`。

每次 push，GitHub Actions 会编译默认配置和 `u-boot-direct.img`，作为 workflow
artifact 上传（`.github/workflows/build.yml`）。

### 烧录

`tools/write_sdcard.py` 把 BL1 和链路上其余镜像写到 SD 卡上正确的字节偏移，
其他地方一律不碰：

```sh
# SKIP_ATF=y
tools/write_sdcard.py --device /dev/sdX --mode direct \
    --bl1 out/bl1-gec6818.bin --uboot ../u-boot_gec6818/u-boot-direct.img

# SKIP_ATF=n
tools/write_sdcard.py --device /dev/sdX --mode atf --bl1 out/bl1-gec6818.bin \
    --fip-loader prebuilt/fip-loader.img --fip-secure prebuilt/fip-secure.img \
    --fip-nonsecure ../u-boot_gec6818/fip-nonsecure.img
```

它拒绝写已挂载的设备，写之前要求再输入一遍设备路径确认；`--dry-run` 只显示
计划。**先在备用 SD 卡上测。** SD 卡上跑通后，再按 [docs/BURN.md](docs/BURN.md)
写到 eMMC：eMMC 用的镜像端口号不一样，而且偏移写错有变砖的风险。

### LCD 启动画面

DDR 初始化完成后，BL1 点亮 RGB 屏（AT070TN92，800x480），显示标题、每个启动
步骤一行（编译时间、启动链路、时钟、DDR、TrustZone、CPU、加载和启动下一级）
以及进度条。画面一直保留到 u-boot 画出自己的 logo（`src/display.c`）。

| 变量 | 默认值 | 作用 |
|---|---|---|
| `BOOT_LOGO_TEXT` | `S5P6818 BL1 Loading...` | 标题 |
| `BOOT_LOGO_DELAY_MS` | `500` | 每步之后的停顿；`0` 去掉停顿 |
| `LCD_BACKLIGHT` | `none` | 把背光脚（GPIOD1/PWM0）拉 `high` 或 `low`。默认不动，因为哪个电平是亮还没在板子上确认过 |

### 模拟器

`tools/s5p6818_emu.py` 用 Unicorn 跑整条启动链，从 BootROM 一直到 u-boot 命令行，
不需要板子。上面所有模式都能跑，也能启动内核。详见
[docs/EMULATOR.md](docs/EMULATOR.md)。

```sh
python3 -m venv .venv && .venv/bin/pip install unicorn capstone
.venv/bin/python tools/s5p6818_emu.py
```

### 文档

| 文档 | 内容 |
|---|---|
| [docs/BOOT_MODES.md](docs/BOOT_MODES.md) | 各条启动链路的细节、SD 卡布局、排错 |
| [docs/NSIH.md](docs/NSIH.md) | BootROM 加载 BL1 用的头格式；切换 SD/eMMC 启动（`tools/set_boot_port.py`） |
| [docs/SMP_PSCI.md](docs/SMP_PSCI.md) | BL1 的 PSCI 怎么启动副核 |
| [docs/EMULATOR.md](docs/EMULATOR.md) | 模拟器 |
| [docs/BURN.md](docs/BURN.md) | 把在 SD 卡上测通的链路写到 eMMC：u-boot 里用 TFTP，或 Linux 里用 `dd` |

所有文档都是中英双语。

### 目录结构

```
.
├── src              # BL1 源码
├── prototype        # Nexell SoC 寄存器/基础头文件
├── prebuilt         # 厂商二进制：fip-loader.img、fip-secure.img、原厂 BL1
├── reference-nsih   # NSIH 头模板（BL1 的头由它生成）
├── nsih-generator   # 基于 Excel 的 NSIH 头生成工具（Windows）
├── tools            # 编译、烧录、模拟器脚本
└── docs
```

| 文件 | 作用 |
|---|---|
| `src/startup_aarch64.S`、`src/startup_aarch32.S` | 入口点、EL3 异常向量表 |
| `src/secondboot.c` | `BootMain()`：时钟/DDR/安全态初始化，加载并启动下一级 |
| `src/psci.c` | `SKIP_ATF=y` 用的 PSCI（在 BL1 或 stage2 里） |
| `src/stage2_main.c` | `OPMODE=aarch32 SKIP_ATF=y` 的 AArch64 stage2 |
| `src/pmic.c` | 经 GPIOC15/16 配置 AXP228 |
| `src/display.c` | LCD 启动画面 |
| `src/init_DDR3.c` | DRAM 初始化 |
| `config.mak` | 编译配置；先读 `.config.mak` |
| `tools/menuconfig.py` | `make menuconfig` |
| `tools/mk_bl1_image.py` | 给 BL1 二进制加上 NSIH 头（和 stage2） |
| `tools/write_sdcard.py` | 把镜像写到 SD 卡或镜像文件 |
| `tools/set_boot_port.py` | 把编好的 BL1 在 SD 和 eMMC 启动之间切换 |
| `tools/s5p6818_emu.py` | 模拟器 |

原始的 Nexell README 保留在 [`README.orig.txt`](README.orig.txt)。
