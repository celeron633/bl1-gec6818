# 启动模式：SKIP_ATF（不走 ATF，默认） vs. 走 ATF

这个 BL1（"2nd bootloader"）是启动链中的一环。BootROM 把它加载进 SRAM；
它初始化好时钟和 DDR 之后，再从 SD/eMMC 读下一级镜像并跳过去。下一级是
什么、最后 Linux 能不能正常用上 PSCI，就是本文要讲的内容。BootROM 加载、
启动 BL1 本身所用的头格式（包括怎么切换 SD / eMMC 启动），见
[NSIH.md](NSIH.md)。

## 两种头格式

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

## 走 ATF 的完整链路（`SKIP_ATF=n`）

```
BootROM -> BL1（本仓库，SRAM）
             |
             v  在 DEVICEADDR（SD/eMMC 字节偏移 0x10200）读 NSIH2
        fip-loader.img            <- LoadAddr 0x7FCC0000，StartAddr 0x7FD00800
             |                       （入口是 AArch32 代码，内含 ARM Trusted Firmware BL2）
             v  BL2 按自己写死的 FIP 偏移加载，不经过 BL1 的 NSIH
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

`fip-loader.img` 的入口（StartAddr 0x7FD00800，文件偏移 0x40800）是一条
AArch32 `b` 指令，后面跟着 `"BOOTMAGICNUMBER!"`，所以 **BL1 跳过去时必须处于
AArch32**，也就是要用 `OPMODE=aarch32` 编译。

SD/eMMC 上的布局（字节偏移，来自 `u-boot_gec6818` 的
`include/configs/artik710_raptor.h` 中的 `CONFIG_DFU_ALT`）：

| 镜像                   | 偏移（字节）     | 偏移（512B 扇区）      |
|------------------------|-----------------|------------------------|
| `bl1-emmcboot.img`     | 0x200           | 0x1                    |
| `fip-loader-emmc.img`  | 0x10200         | 0x81                   |
| `fip-secure.img`       | 0x60200         | 0x301                  |
| `fip-nonsecure.img`    | 0x1E0200        | 0xf01                  |

### 编译

```
make clean && make OPMODE=aarch32 SKIP_ATF=n CROSS_TOOL=<arm-linux-gnueabi- 前缀>
make clean && make OPMODE=aarch32 SKIP_ATF=n CROSS_TOOL=... BOOT_PORT=emmc   # 改成从 eMMC 启动，见 NSIH.md
```

u-boot 照常编译（`u-boot_gec6818/Makefile` 的 `fip-nonsecure.img` 目标）。

### 烧录

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

## SKIP_ATF：完全不用 ATF/OP-TEE

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
   |  EL3 向量表 + PSCI（src/psci.c），副核上电后停在 EL3 等 PSCI CPU_ON，
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

处于 "off" 状态的 CPU 并不真正断电，只是在 EL3 里 wfi 等待；CPU_ON 通过
SGI1 唤醒它。cluster1 的核 MPIDR 是 0x100~0x103（`CONFIG_RESET_AFFINITY_ID`），
内部编号为 CPU 4~7。

### 编译

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

### 默认做法：`OPMODE=aarch64 SKIP_ATF=y`

BootROM 执行 NSIH 头里的向量，直接把核心复位成 AArch64、从 0xFFFF0200 开始跑
BL1，所以不需要 stage2：BL1 本身常驻 EL3 处理 PSCI（同一份 `src/psci.c`），
副核上电后停在 EL3，最后由 `EnterNonSecure()` 从 EL3 直接 eret 到 u-boot（EL2 非安全态）。u-boot 镜像
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

### 纯 AArch32：`OPMODE=aarch32 UBOOT_ARCH=aarch32`

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

### 烧录：先在 SD 卡上测，别碰 eMMC

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

### 启动不起来时

- 停在 `reset CPU0 into AArch64 stage2`：stage2 没跑起来，或者在第一次打印前
  就挂了。stage2 打印的第一行是 `stage2: AArch64 EL3`。
- 停在 `stage2: entering u-boot`：确认 `u-boot-direct.img` 是用当前的
  Makefile 规则重新生成的（头部 0x060 处的 StartAddr 必须是 0x43C00000）。
- Linux 里副核起不来：启动日志会把每次 `CPU_ON` 打印成
  `psci: CPU0 fn=0xc4000003 ...`，先从这里查。
