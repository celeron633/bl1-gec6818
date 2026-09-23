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
ARM Trusted Firmware is used:

| `OPMODE` | `SKIP_ATF` | chain | PSCI from | switch to AArch64 happens in | image at SD offset 0x10200 |
|---|---|---|---|---|---|
| `aarch32` | `n` | BL1 → `fip-loader.img` → `fip-secure.img` (BL31+OP-TEE) → u-boot → Linux | BL31 | `fip-loader.img` (its entry is AArch32 code) | `fip-loader.img` |
| `aarch32` | `y` | BL1 → stage2 → u-boot → Linux | stage2, resident in SRAM at EL3 (`src/stage2_main.c`, `src/psci.c`) | BL1, by warm-resetting CPU0 into stage2 | `u-boot-direct.img` |
| `aarch64` | `y` | BL1 → u-boot → Linux | BL1 itself, resident at EL3 (`src/psci.c`) | BootROM, via the NSIH header's vector, before BL1 runs | `u-boot-direct.img` |
| `aarch64` | `n` | not usable: BL1 would jump into `fip-loader.img`'s AArch32 entry in AArch64 | | | |

The two `SKIP_ATF=y` configurations are not yet verified on hardware -
see [docs/BOOT_MODES.md](docs/BOOT_MODES.md). `config.mak` defaults to
`OPMODE=aarch64`, `SKIP_ATF=n` (the unusable one), so always pass both.

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
make clean && make OPMODE=aarch32 CROSS_TOOL=arm-linux-gnueabi-              # ATF chain
make clean && make OPMODE=aarch32 SKIP_ATF=y CROSS_TOOL=arm-linux-gnueabi-   # no ATF, via stage2
make clean && make OPMODE=aarch64 SKIP_ATF=y                                  # no ATF, AArch64 BL1
```

Add `BOOT_PORT=emmc` to boot from eMMC instead of SD. The no-ATF
configurations need u-boot's `u-boot-direct.img` (`make u-boot-direct.img`
in u-boot_gec6818), not `fip-nonsecure.img`.

GitHub Actions builds both modes (plus the matching u-boot images) on every
push and uploads them as a workflow artifact - see the badge above, or
`.github/workflows/build.yml`.

### Flashing

`tools/write_sdcard.py` writes BL1 + the rest of the chain to the right
byte offsets on an SD card for either mode - see docs/BOOT_MODES.md for the
exact commands. **Test on a spare SD card, not eMMC** - wrong offsets on
eMMC can brick the board.

### Source tree

```
.
├── src              # BL1 source
├── prototype        # Nexell SoC register/base headers
│   ├── base
│   └── module
├── nsih-generator    # Excel-based NSIH header generator (Windows)
├── reference-nsih    # reference NSIH text files for various boards
└── tools             # write_sdcard.py
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

`OPMODE` 决定 BL1 自己跑在哪个指令集，`SKIP_ATF` 决定用不用 ARM Trusted Firmware：

| `OPMODE` | `SKIP_ATF` | 链路 | PSCI 由谁提供 | 在哪里切到 AArch64 | SD 卡 0x10200 处放 |
|---|---|---|---|---|---|
| `aarch32` | `n` | BL1 → `fip-loader.img` → `fip-secure.img`（BL31+OP-TEE）→ u-boot → Linux | BL31 | `fip-loader.img`（它的入口是 AArch32 代码） | `fip-loader.img` |
| `aarch32` | `y` | BL1 → stage2 → u-boot → Linux | stage2，常驻 SRAM、运行在 EL3（`src/stage2_main.c`、`src/psci.c`） | BL1 热复位 CPU0 进入 stage2 | `u-boot-direct.img` |
| `aarch64` | `y` | BL1 → u-boot → Linux | BL1 自己，常驻 EL3（`src/psci.c`） | BootROM 执行 NSIH 头里的向量，BL1 运行之前就已切换 | `u-boot-direct.img` |
| `aarch64` | `n` | 不可用：BL1 会以 AArch64 跳进 `fip-loader.img` 的 AArch32 入口 | | | |

两种 `SKIP_ATF=y` 配置都还没上板验证，详见 [docs/BOOT_MODES.md](docs/BOOT_MODES.md)。
`config.mak` 的默认值是 `OPMODE=aarch64`、`SKIP_ATF=n`，正好是不可用的那种，所以编译时两个都要显式指定。

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
make clean && make OPMODE=aarch32 CROSS_TOOL=arm-linux-gnueabi-              # ATF 链路
make clean && make OPMODE=aarch32 SKIP_ATF=y CROSS_TOOL=arm-linux-gnueabi-   # 不走 ATF，经 stage2
make clean && make OPMODE=aarch64 SKIP_ATF=y                                  # 不走 ATF，AArch64 BL1
```

加 `BOOT_PORT=emmc` 改为从 eMMC 启动。不走 ATF 的两种配置要配 u-boot 的
`u-boot-direct.img`（在 u-boot_gec6818 里 `make u-boot-direct.img`），不是 `fip-nonsecure.img`。

每次 push，GitHub Actions 会把两种模式（以及配套的 u-boot 镜像）都编译好，打包成
workflow artifact 上传——看上面的徽章，或者 `.github/workflows/build.yml`。

### 烧录

`tools/write_sdcard.py` 会把 BL1 和链路上其余镜像写到 SD 卡上正确的字节偏移，两种
模式的具体命令见 docs/BOOT_MODES.md。**先在备用 SD 卡上测，别碰 eMMC**——eMMC 上偏移
写错有砖机风险。

### 目录结构

```
.
├── src              # BL1 源码
├── prototype        # Nexell SoC 寄存器/基础头文件
│   ├── base
│   └── module
├── nsih-generator    # 基于 Excel 的 NSIH 头生成工具（Windows）
├── reference-nsih    # 各板子的参考 NSIH 文本
└── tools             # write_sdcard.py
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
