# Boot modes: SKIP_ATF (no ATF, default) vs. the ATF chain / 启动模式：SKIP_ATF（不走 ATF，默认） vs. 走 ATF

**English** | [中文](#中文)

## English

This BL1 ("2nd bootloader") is one link in the boot chain. BootROM loads it
into SRAM. BL1 sets up the clocks and DDR, then reads the next image from
SD/eMMC and jumps to it. This page covers what that next image is and whether
Linux ends up with working PSCI. For the header BootROM uses to load and start
BL1 itself (including how to switch between SD and eMMC boot), see
[NSIH.md](NSIH.md).

### Two header layouts

There are two kinds of NSIH header in the chain, with the fields in different
places. Don't mix them up:

| | read by | load/jump fields |
|---|---|---|
| **NSIH1**: BL1's own header | BootROM | DEVICEADDR@0x040, LOADSIZE@0x044, LOADADDR@0x048, LAUNCHADDR@0x04C (`struct NX_SecondBootInfo`) |
| **NSIH2**: the next image's header | BL1's `SDMMCBOOT()` | LoadSize@0x050, CRC32@0x054, LoadAddr(u64)@0x058, StartAddr(u64)@0x060 (`struct nx_tbbinfo`, `src/nx_bootheader.h`) |

NSIH2 is the format `SECURE_BINGEN -t 3rdboot -l <LoadAddr> -e <StartAddr>`
writes. The header is always 0x400 bytes. BL1 copies the header to LoadAddr,
puts the body at LoadAddr+0x400, then jumps to StartAddr.

Whatever sits at 0x040-0x04C in these images is leftover NSIH template text
(`0xFFFF0000`, for example) and **must not be trusted**: loading by it would
overwrite BL1 itself, which is running in SRAM. That is why `SDMMCBOOT()`
refuses any LoadAddr range that overlaps SRAM.

### The full ATF chain (`SKIP_ATF=n`)

```
BootROM -> BL1 (this repo, SRAM)
             |
             v  reads NSIH2 at DEVICEADDR (SD/eMMC byte offset 0x10200)
        fip-loader.img            <- LoadAddr 0x7FCC0000, StartAddr 0x7FD00800
             |                       (entry is an AArch32 stub; the real BL2 is AArch64, entry 0x7FD01000)
             v  BL2 loads the FIPs by the offsets/port in its own header (dbi[0]/dbi[1]), not via BL1
        fip-secure.img            <- FIP: BL31 (secure monitor / PSCI) + BL32 (OP-TEE)
             |
             v  BL31 erets into EL2 non-secure
        fip-nonsecure.img         <- BL33 = u-boot
             |
             v
           Linux
```

`fip-loader.img` and `fip-secure.img` in `prebuilt/` are prebuilt binaries;
this repo has no source for them. `strings` confirms they are a real ARM
Trusted Firmware (BL2) and a FIP package of BL31+OP-TEE. BL31 stays resident
at EL3 for as long as the system runs and answers the kernel's PSCI SMCs
(`psci { method = "smc"; }` in the device tree): secondary CPU bring-up,
`reboot`, `poweroff`, cpuidle and so on.

The entry of `fip-loader.img` (StartAddr 0x7FD00800, file offset 0x40800) is
an AArch32 stub: a `b` instruction followed by `"BOOTMAGICNUMBER!"`. It sets
CPU0's RVBAR to 0x7FD01000, sets CPU0's AArch64 bit in TIEOFF, then
warm-resets CPU0. 0x7FD01000 is the real ATF BL2, AArch64 EL3 code. The stub
exists for the vendor BL1 (`prebuilt/bl1-mmcboot.bin`, AArch32).

Both `OPMODE`s can run this chain:

- `OPMODE=aarch32`: same as the vendor. BL1 jumps straight to StartAddr and
  the stub does the switch.
- `OPMODE=aarch64`: BL1 can't execute the AArch32 stub. It recognises the
  stub at StartAddr, takes the address the stub would write to RVBAR from its
  `ldr r6, [pc, #imm]; lsr r6, r6, #2`, and does the same TIEOFF setup and
  warm reset itself (`AtfLoaderEntry64()`, `ResetCPU0ToAArch64()`,
  `src/secondboot.c`). If StartAddr doesn't hold such a stub, BL1 treats the
  image as AArch64 and branches to it directly at EL3.

Layout on SD/eMMC (byte offsets, from `CONFIG_DFU_ALT` in `u-boot_gec6818`'s
`include/configs/artik710_raptor.h`):

| image                  | offset (bytes)  | offset (512B sectors)  |
|------------------------|-----------------|------------------------|
| `bl1-emmcboot.img`     | 0x200           | 0x1                    |
| `fip-loader-emmc.img`  | 0x10200         | 0x81                   |
| `fip-secure.img`       | 0x60200         | 0x301                  |
| `fip-nonsecure.img`    | 0x1E0200        | 0xf01                  |

#### Building

```
make clean && make OPMODE=aarch32 SKIP_ATF=n CROSS_TOOL=<arm-linux-gnueabi- prefix>
make clean && make OPMODE=aarch32 SKIP_ATF=n CROSS_TOOL=... BOOT_PORT=emmc   # boot from eMMC instead, see NSIH.md
make clean && make SKIP_ATF=n                                                # AArch64 BL1
```

Build u-boot as usual (the `fip-nonsecure.img` target in
`u-boot_gec6818/Makefile`).

#### Flashing

```
tools/write_sdcard.py --device /dev/sdX --mode atf \
    --bl1       out/bl1-gec6818.bin \
    --fip-loader     prebuilt/fip-loader.img \
    --fip-secure     prebuilt/fip-secure.img \
    --fip-nonsecure  ../u-boot_gec6818/fip-nonsecure.img
```

See `tools/write_sdcard.py --help`. It writes only these four byte ranges
(nothing else on the card is touched), refuses mounted devices, checks that
each image fits its slot, and asks you to type the device path again before
writing. `--dry-run` only shows the plan; once the command is right, `--yes`
skips the confirmation.

### SKIP_ATF: no ATF/OP-TEE at all

With BL1 built as `OPMODE=aarch32 SKIP_ATF=y`, the chain is:

```
BootROM (AArch32)
   |  reads NSIH1 = BL1's own header (0x040-0x04C),
   |  loads BL1 + the stage2 appended to it into SRAM 0xFFFF0000
   v
BL1 (AArch32, SRAM 0xFFFF0200)   clocks, DDR, CCI, TZPC/TZASC/GIC security setup
   |  reads u-boot-direct.img's NSIH2 (0x050/0x058/0x060) at DEVICEADDR (0x10200)
   |  -> header to LoadAddr, u-boot.bin to LoadAddr+0x400 = 0x43C00000 (DDR)
   |  -> writes the u-boot entry just below stage2, sets TIEOFF: CPU0 to AArch64,
   |     reset address 0xFFFF9000, then warm-resets CPU0
   v
stage2 (AArch64 EL3, SRAM 0xFFFF9000, resident)   src/stage2_main.c
   |  EL3 vector table + PSCI (src/psci.c); secondaries stay powered off until
   |  PSCI CPU_ON; finally erets into EL2 non-secure
   v
u-boot (AArch64 EL2, 0x43C00000) -> Linux (PSCI SMCs handled by stage2)
```

In this chain stage2 plays the part of BL31 in the ATF chain. A sub-make in
the Makefile builds it with the AArch64 toolchain (`STAGE2_CROSS_TOOL`,
defaulting to `CROSS_TOOL_aarch64`, i.e. `aarch64-none-elf-`), and
`tools/mk_bl1_image.py --stage2` appends it to the BL1 image. The build fails
outright if BL1 or stage2 outgrows its SRAM region.

PSCI calls stage2 implements: PSCI_VERSION (1.0), PSCI_FEATURES, CPU_ON,
CPU_OFF, AFFINITY_INFO, CPU_SUSPEND (standby only, the only state the device
tree declares), MIGRATE_INFO_TYPE, SYSTEM_RESET, SYSTEM_OFF (just halts; no
power-off through the PMIC). Everything else returns NOT_SUPPORTED, as SMCCC
requires.

Secondary cores are not powered at boot. When the kernel issues CPU_ON,
`cpuOn()` writes the entry address into that core's slot, then powers it on.
The secondary comes in through `Startup`, enables its cluster's CCI port,
sees its slot is ready and erets straight into the kernel. This is what the
vendor BL31 does too; why and how is in [SMP_PSCI.md](SMP_PSCI.md). CPU_OFF
doesn't really cut power; the core waits in `wfi` at EL3. Cluster 1's cores
have MPIDR 0x100-0x103 (`CONFIG_RESET_AFFINITY_ID`) and are numbered CPU 4-7
internally.

#### Building

```
cd bl1-gec6818
make clean && make OPMODE=aarch32 CROSS_TOOL=<arm-linux-gnueabi- prefix>
                                  # always make clean before switching modes: the
                                  # CFLAGS differ and the Makefile can't tell
                                  # (not needed with make menuconfig, see README)
cd ../u-boot_gec6818
make u-boot-direct.img            # not fip-nonsecure.img, see below
```

**This mode can't use `fip-nonsecure.img`.** That image wraps `u-boot.bin`
in a TF-A FIP container, which BL1 can't parse. `u-boot-direct.img` is
`SECURE_BINGEN` run directly on `u-boot.bin` with
`-l CONFIG_SYS_TEXT_BASE-0x400 -e CONFIG_SYS_TEXT_BASE`, so u-boot lands
exactly at its link address.

#### The default: `OPMODE=aarch64 SKIP_ATF=y`

BootROM runs the vector in the NSIH header, which resets the core straight
into AArch64 and starts BL1 at 0xFFFF0200, so no stage2 is needed. BL1 itself
stays resident at EL3 and handles PSCI (the same `src/psci.c`), secondaries
are powered on at CPU_ON (as above), and finally `EnterNonSecure()` erets
from EL3 straight into u-boot (EL2 non-secure). The u-boot image
(`u-boot-direct.img`) and flashing are the same as above, and only one
`aarch64-none-elf` toolchain is needed:

```
make clean && make          # config.mak's defaults are this configuration (without a .config.mak)
```

This configuration used to print nothing at all on the serial port. The
2KB-aligned exception vector table pushed `.text`, and `Startup` with it, to
0xFFFF0800, while the NSIH vector resets to 0xFFFF0200. The vector table now
lives in its own section, and the linker script `ASSERT`s that `Startup` is at
0xFFFF0200.

On the board it then stopped at `Launch to 0x43C00000, currently EL3` with no
further output. BL1 used to eret into EL2 non-secure with `SwitchToEL2()` and
then **return to BootMain and carry on**. But `SetTZPC()` makes the whole
on-chip SRAM secure-only (`TZPC_R0SIZE_ALL`), and BL1's code, stack and
exception vectors all live there, so the first instruction fetch after the
eret faulted, silently. Now BL1 stays at EL3 throughout. The last serial line
is `entering u-boot at 0x43C00000, EL2 non-secure`, then it erets straight to
u-boot in DDR. SRAM stays secure-only, which also keeps the resident PSCI
code safe from the non-secure world. The emulator (`tools/s5p6818_emu.py`)
reports an error on non-secure instruction fetches from secure SRAM.

#### Pure AArch32: `OPMODE=aarch32 UBOOT_ARCH=aarch32`

The whole chain stays in AArch32, for old 32-bit kernels:

```
BootROM (AArch32) -> BL1 (AArch32, secure SVC)
   |  reads u-boot-direct.img's NSIH2 as above, u-boot.bin to 0x43C00000
   v  jumps straight to it (pLaunch(0, 4330)): no reset, no instruction set switch
32-bit u-boot (AArch32 secure SVC, 0x43C00000) -> 32-bit kernel (bootz)
```

No stage2 and no PSCI. BL1 doesn't wake the secondaries
(`MULTICORE_BRING_UP=0`); bringing them up is left to the kernel. u-boot must
be built with u-boot_gec6818's `s5p6818_gec6818_aarch32_defconfig` and a
32-bit toolchain, see the README there:

```
cd bl1-gec6818
make clean && make OPMODE=aarch32 UBOOT_ARCH=aarch32 CROSS_TOOL=arm-linux-gnueabi-
cd ../u-boot_gec6818
make distclean
ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- make s5p6818_gec6818_aarch32_defconfig
ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- make -j"$(nproc)"
ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- make u-boot-direct.img
```

`UBOOT_ARCH=aarch32` only works together with `OPMODE=aarch32 SKIP_ATF=y`;
otherwise make stops with an error. Flash it the same way as below
(`--mode direct`). So far it has only reached the u-boot prompt in the
emulator, not on the board.

#### Flashing: test on an SD card first

BL1 still reads the next stage from `DEVICEADDR` (0x10200 / sector 0x81), so
`write_sdcard.py --mode direct` writes `u-boot-direct.img` into the
`fip-loader.img` slot and lets it extend into the unused
`fip-secure.img`/`fip-nonsecure.img` slots:

| image                             | offset (bytes)  | offset (sectors)   | max size |
|-----------------------------------|-----------------|--------------------|----------|
| BL1 + stage2 (built with `SKIP_ATF=y`) | 0x200      | 0x1                | 64KB     |
| `u-boot-direct.img`               | 0x10200         | 0x81               | 2880KB (up to 0x2E0200, where u-boot's own ENV/params.bin starts) |

```
tools/write_sdcard.py --device /dev/sdX --mode direct \
    --bl1   out/bl1-gec6818.bin \
    --uboot ../u-boot_gec6818/u-boot-direct.img
```

Test with a spare SD card. Wrong offsets on the eMMC can brick the board
(`u-boot_gec6818/README.md` gives the same warning for the 32-bit u-boot).
Once it works from SD, see [BURN.md](BURN.md) for writing it to the eMMC
(both modes).

#### When it doesn't boot

- Stops at `reset CPU0 into AArch64 stage2`: stage2 didn't start, or died
  before its first print. stage2's first line is `stage2: AArch64 EL3`.
- Stops at `stage2: entering u-boot`: make sure `u-boot-direct.img` was
  regenerated with the current Makefile rule (StartAddr at header offset
  0x060 must be 0x43C00000).
- Secondaries don't come up in Linux: the boot log prints each `CPU_ON` as
  `psci: CPU0 fn=0xc4000003 ...`; start there.

---

## 中文

这个 BL1（"2nd bootloader"）是启动链中的一环。BootROM 把它加载进 SRAM；
它初始化好时钟和 DDR 之后，再从 SD/eMMC 读下一级镜像并跳过去。下一级是
什么、最后 Linux 能不能正常用上 PSCI，就是本文要讲的内容。BootROM 加载、
启动 BL1 本身所用的头格式（包括怎么切换 SD / eMMC 启动），见
[NSIH.md](NSIH.md)。

### 两种头格式

整条链上有两种 NSIH 头，字段位置不一样，一定要分清：

| | 谁来读 | 加载/跳转字段 |
|---|---|---|
| **NSIH1**：BL1 自己的头 | BootROM | DEVICEADDR@0x040，LOADSIZE@0x044，LOADADDR@0x048，LAUNCHADDR@0x04C（`struct NX_SecondBootInfo`） |
| **NSIH2**：下一级镜像的头 | BL1 的 `SDMMCBOOT()` | LoadSize@0x050，CRC32@0x054，LoadAddr(u64)@0x058，StartAddr(u64)@0x060（`struct nx_tbbinfo`，`src/nx_bootheader.h`） |

NSIH2 就是 `SECURE_BINGEN -t 3rdboot -l <LoadAddr> -e <StartAddr>` 写出来的
格式，头部固定 0x400 字节。BL1 把头拷到 LoadAddr，把正文放到
LoadAddr+0x400，然后跳到 StartAddr。

这类镜像 0x040~0x04C 里的内容只是 NSIH 模板文本留下来的（比如
`0xFFFF0000`），**不能当真**：按它加载会把正在 SRAM 里运行的 BL1 自己覆盖掉。
所以 `SDMMCBOOT()` 会拒绝任何和 SRAM 重叠的 LoadAddr 范围。

### 走 ATF 的完整链路（`SKIP_ATF=n`）

```
BootROM -> BL1（本仓库，SRAM）
             |
             v  在 DEVICEADDR（SD/eMMC 字节偏移 0x10200）读 NSIH2
        fip-loader.img            <- LoadAddr 0x7FCC0000，StartAddr 0x7FD00800
             |                       （入口是 AArch32 跳板，BL2 本体是 AArch64，入口 0x7FD01000）
             v  BL2 按自己头里 dbi[0]/dbi[1] 的偏移和端口加载 FIP，不经过 BL1
        fip-secure.img            <- FIP：BL31（安全监控 / PSCI）+ BL32（OP-TEE）
             |
             v  BL31 以 eret 进入 EL2 非安全态
        fip-nonsecure.img         <- BL33 = u-boot
             |
             v
           Linux
```

`prebuilt/` 下的 `fip-loader.img` 和 `fip-secure.img` 是预编译的二进制，
本仓库没有源码。`strings` 确认过它们分别是真正的 ARM Trusted Firmware
（BL2），以及 BL31+OP-TEE 的 FIP 包。BL31 在整个系统运行期间常驻 EL3，
负责响应内核的 PSCI SMC 调用（设备树里的 `psci { method = "smc"; }`），
包括副核启动、`reboot`、`poweroff`、cpuidle 等。

`fip-loader.img` 的入口（StartAddr 0x7FD00800，文件偏移 0x40800）是一段
AArch32 跳板：一条 `b` 指令，后面跟着 `"BOOTMAGICNUMBER!"`。它把 CPU0 的
RVBAR 设成 0x7FD01000，打开 TIEOFF 里 CPU0 的 AArch64 位，再热复位 CPU0。
0x7FD01000 才是真正的 ATF BL2，是 AArch64 EL3 代码。这个跳板是给厂商的 BL1
（`prebuilt/bl1-mmcboot.bin`，AArch32）准备的。

两种 `OPMODE` 都能走这条链：

- `OPMODE=aarch32`：和厂商一样，BL1 直接跳到 StartAddr，由跳板完成切换。
- `OPMODE=aarch64`：BL1 执行不了 AArch32 跳板。它在 StartAddr 处认出跳板，
  从跳板的 `ldr r6, [pc, #imm]; lsr r6, r6, #2` 取出它要写进 RVBAR 的地址，
  然后自己做同样的 TIEOFF 设置和热复位（`AtfLoaderEntry64()`、
  `ResetCPU0ToAArch64()`，`src/secondboot.c`）。StartAddr 处如果不是这种跳板，
  就当作 AArch64 镜像，在 EL3 直接跳过去。

SD/eMMC 上的布局（字节偏移，来自 `u-boot_gec6818` 的
`include/configs/artik710_raptor.h` 中的 `CONFIG_DFU_ALT`）：

| 镜像                   | 偏移（字节）     | 偏移（512B 扇区）      |
|------------------------|-----------------|------------------------|
| `bl1-emmcboot.img`     | 0x200           | 0x1                    |
| `fip-loader-emmc.img`  | 0x10200         | 0x81                   |
| `fip-secure.img`       | 0x60200         | 0x301                  |
| `fip-nonsecure.img`    | 0x1E0200        | 0xf01                  |

#### 编译

```
make clean && make OPMODE=aarch32 SKIP_ATF=n CROSS_TOOL=<arm-linux-gnueabi- 前缀>
make clean && make OPMODE=aarch32 SKIP_ATF=n CROSS_TOOL=... BOOT_PORT=emmc   # 改成从 eMMC 启动，见 NSIH.md
make clean && make SKIP_ATF=n                                                # AArch64 BL1
```

u-boot 照常编译（`u-boot_gec6818/Makefile` 的 `fip-nonsecure.img` 目标）。

#### 烧录

```
tools/write_sdcard.py --device /dev/sdX --mode atf \
    --bl1       out/bl1-gec6818.bin \
    --fip-loader     prebuilt/fip-loader.img \
    --fip-secure     prebuilt/fip-secure.img \
    --fip-nonsecure  ../u-boot_gec6818/fip-nonsecure.img
```

详见 `tools/write_sdcard.py --help`。它只写这四段字节范围（卡上其他地方
一律不碰），拒绝写已挂载的设备，写之前会检查每个镜像是否超出自己的槽位，
还会要求你把设备路径再输入一遍确认。加 `--dry-run` 只看计划；确认命令没问题
之后可以加 `--yes` 跳过确认。

### SKIP_ATF：完全不用 ATF/OP-TEE

用 `OPMODE=aarch32 SKIP_ATF=y` 编译 BL1 时，链路是：

```
BootROM (AArch32)
   |  读 NSIH1 = BL1 自己的头（0x040~0x04C），
   |  把 BL1 + 拼在后面的 stage2 一起加载到 SRAM 0xFFFF0000
   v
BL1（AArch32，SRAM 0xFFFF0200）   时钟、DDR、CCI、TZPC/TZASC/GIC 安全配置
   |  在 DEVICEADDR（0x10200）读 u-boot-direct.img 的 NSIH2（0x050/0x058/0x060）
   |  -> 头拷到 LoadAddr，u-boot.bin 放到 LoadAddr+0x400 = 0x43C00000（DDR）
   |  -> 把 u-boot 入口写到 stage2 下方，设置 TIEOFF：CPU0 切 AArch64、
   |     复位地址 0xFFFF9000，然后 CPU0 热复位
   v
stage2（AArch64 EL3，SRAM 0xFFFF9000，常驻）   src/stage2_main.c
   |  EL3 向量表 + PSCI（src/psci.c），副核保持断电，等 PSCI CPU_ON 再上电，
   |  最后 eret 进入 EL2 非安全态
   v
u-boot（AArch64 EL2，0x43C00000）-> Linux（PSCI 的 SMC 由 stage2 处理）
```

stage2 在这条链里的角色相当于 ATF 链中的 BL31。它由 Makefile 里的子 make
用 AArch64 工具链编译（`STAGE2_CROSS_TOOL`，默认取 `CROSS_TOOL_aarch64`，即 `aarch64-none-elf-`），再由
`tools/mk_bl1_image.py --stage2` 拼进 BL1 镜像。BL1 或 stage2 超出各自的
SRAM 区域时构建会直接失败。

stage2 实现的 PSCI 调用：PSCI_VERSION（1.0）、PSCI_FEATURES、CPU_ON、
CPU_OFF、AFFINITY_INFO、CPU_SUSPEND（只支持 standby，设备树里也只声明了这一种）、
MIGRATE_INFO_TYPE、SYSTEM_RESET、SYSTEM_OFF（只是停住，没有通过 PMIC 断电）。
其他函数一律返回 NOT_SUPPORTED，这是 SMCCC 的要求。

副核在启动时不上电。内核发 CPU_ON 时，`cpuOn()` 先把入口地址写进这个核的
slot，再给它上电；副核从 `Startup` 进来，打开自己簇的 CCI 端口，看到 slot 已经
就绪就直接 eret 进内核。和厂商 BL31 的做法一样，原因和过程见
[SMP_PSCI.md](SMP_PSCI.md)。CPU_OFF 不会真正断电，核在 EL3 里 wfi 等待。
cluster1 的核 MPIDR 是 0x100~0x103（`CONFIG_RESET_AFFINITY_ID`），内部编号为
CPU 4~7。

#### 编译

```
cd bl1-gec6818
make clean && make OPMODE=aarch32 CROSS_TOOL=<arm-linux-gnueabi- 前缀>
                                  # 切换模式前一定先 make clean：CFLAGS 不同，
                                  # Makefile 自己检测不到
                                  # （用 make menuconfig 选配置就不用 clean，见 README）
cd ../u-boot_gec6818
make u-boot-direct.img            # 不是 fip-nonsecure.img，原因见下
```

**这个模式不能用 `fip-nonsecure.img`**。它把 `u-boot.bin` 包在 TF-A 的 FIP
容器里，BL1 解析不了。`u-boot-direct.img` 是直接对 `u-boot.bin` 运行
`SECURE_BINGEN`，参数为 `-l CONFIG_SYS_TEXT_BASE-0x400 -e CONFIG_SYS_TEXT_BASE`，
这样 u-boot 正好落在它的链接地址上。

#### 默认做法：`OPMODE=aarch64 SKIP_ATF=y`

BootROM 执行 NSIH 头里的向量，直接把核心复位成 AArch64、从 0xFFFF0200 开始跑
BL1，所以不需要 stage2：BL1 本身常驻 EL3 处理 PSCI（同一份 `src/psci.c`），
副核等 CPU_ON 时再上电（同上），最后由 `EnterNonSecure()` 从 EL3 直接 eret 到 u-boot（EL2 非安全态）。u-boot 镜像
（`u-boot-direct.img`）和烧录方式与上面相同，只需一套 `aarch64-none-elf` 工具链：

```
make clean && make          # config.mak 的默认值就是这个配置（没有 .config.mak 时）
```

这个配置曾经完全不出串口，原因是 2KB 对齐的异常向量表把 `.text` 连同 `Startup`
推到了 0xFFFF0800，而 NSIH 向量复位到的是 0xFFFF0200。现在向量表放在单独的段里，
链接脚本也用 `ASSERT` 保证 `Startup` 在 0xFFFF0200。

上板后又发现停在 `Launch to 0x43C00000, currently EL3`，之后再无输出。原因是
BL1 原来用 `SwitchToEL2()` 以 eret 进入 EL2 非安全态后**返回 BootMain 继续执行**，
而 `SetTZPC()` 把整块片内 SRAM 设成了只允许安全态访问（`TZPC_R0SIZE_ALL`），
BL1 的代码、栈、异常向量都在里面，eret 后第一次取指就出错，而且没有任何打印。
现在 BL1 一直留在 EL3，最后一行串口输出是 `entering u-boot at 0x43C00000, EL2
non-secure`，然后直接 eret 到 DDR 里的 u-boot。SRAM 保持只允许安全态访问，
常驻的 PSCI 代码也因此不会被非安全态改写。模拟器（`tools/s5p6818_emu.py`）
会对非安全态从安全 SRAM 取指报错。

#### 纯 AArch32：`OPMODE=aarch32 UBOOT_ARCH=aarch32`

整条链都不切到 AArch64，给老的 32 位内核用：

```
BootROM (AArch32) -> BL1（AArch32，安全态 SVC）
   |  和上面一样读 u-boot-direct.img 的 NSIH2，u-boot.bin 放到 0x43C00000
   v  直接跳过去（pLaunch(0, 4330)），不复位、不切换指令集
32 位 u-boot（AArch32 安全态 SVC，0x43C00000）-> 32 位内核（bootz）
```

没有 stage2，也没有 PSCI；BL1 不唤醒副核（`MULTICORE_BRING_UP=0`），副核怎么起来
交给内核自己。u-boot 要用 u-boot_gec6818 的 `s5p6818_gec6818_aarch32_defconfig`
和 32 位工具链编译，详见那边的 README：

```
cd bl1-gec6818
make clean && make OPMODE=aarch32 UBOOT_ARCH=aarch32 CROSS_TOOL=arm-linux-gnueabi-
cd ../u-boot_gec6818
make distclean
ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- make s5p6818_gec6818_aarch32_defconfig
ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- make -j"$(nproc)"
ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- make u-boot-direct.img
```

`UBOOT_ARCH=aarch32` 只能和 `OPMODE=aarch32 SKIP_ATF=y` 一起用，否则 make 直接报错。
烧录方式和下面一样（`--mode direct`）。目前只在模拟器里跑到了 u-boot 命令行，
还没上板。

#### 烧录：先在 SD 卡上测

BL1 仍然从 `DEVICEADDR`（0x10200 / 扇区 0x81）读下一级，所以
`write_sdcard.py --mode direct` 把 `u-boot-direct.img` 写在 `fip-loader.img`
的槽位上，并允许它延伸到用不上的 `fip-secure.img`/`fip-nonsecure.img` 槽位：

| 镜像                              | 偏移（字节）     | 偏移（扇区）        | 最大尺寸 |
|-----------------------------------|-----------------|--------------------|----------|
| BL1 + stage2（`SKIP_ATF=y` 编译）  | 0x200           | 0x1                | 64KB     |
| `u-boot-direct.img`               | 0x10200         | 0x81               | 2880KB（到 0x2E0200 为止，那里开始是 u-boot 自己的 ENV/params.bin） |

```
tools/write_sdcard.py --device /dev/sdX --mode direct \
    --bl1   out/bl1-gec6818.bin \
    --uboot ../u-boot_gec6818/u-boot-direct.img
```

用一张备用 SD 卡测试。eMMC 上地址写错可能导致板子变砖
（`u-boot_gec6818/README.md` 对 32 位 u-boot 也有同样的警告）。
SD 卡上测通之后怎么写到 eMMC（两种模式都有），见 [BURN.md](BURN.md)。

#### 启动不起来时

- 停在 `reset CPU0 into AArch64 stage2`：stage2 没跑起来，或者在第一次打印前
  就挂了。stage2 打印的第一行是 `stage2: AArch64 EL3`。
- 停在 `stage2: entering u-boot`：确认 `u-boot-direct.img` 是用当前的
  Makefile 规则重新生成的（头部 0x060 处的 StartAddr 必须是 0x43C00000）。
- Linux 里副核起不来：启动日志会把每次 `CPU_ON` 打印成
  `psci: CPU0 fn=0xc4000003 ...`，先从这里查。
