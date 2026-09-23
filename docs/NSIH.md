# NSIH（Nexell System Information Header）

BootROM 并不解析 ELF，它只读启动镜像开头一个固定格式的 512 字节头，叫 NSIH
（"Nexell System Information Header"，末尾有签名 `"NSIH"`）。这个头里放着
BootROM 在任何代码运行之前需要的全部信息：镜像加载到哪、跳到哪、时钟/PLL/DDR
时序参数、从哪个启动设备/端口读。

整条启动链上每一级都用这个 512 字节的外壳，但**加载/跳转字段的位置分两种**：
BootROM 读 BL1 的头时看 0x040~0x04C；BL1 读下一级镜像（`fip-loader.img`、
`u-boot-direct.img` 等 `SECURE_BINGEN -t 3rdboot` 生成的镜像）的头时看
0x050/0x058/0x060（`src/nx_bootheader.h` 的 `struct nx_tbbinfo`）。

本文讲的是 **BL1 自己的头**，也就是 BootROM 用来加载、启动 BL1 的那个。
下一级镜像的头格式见 [BOOT_MODES.md](BOOT_MODES.md#两种头格式)。

## 本仓库里相关的文件

- `reference-nsih/*.txt`：各种板子的 NSIH 可读文本
  （`raptor`/`drone` × `sd`/`emmc`/普通 × 32/64 位）以及不同时钟配置。
  其中 `raptor-sd-64.txt`/`raptor-emmc-64.txt`（本板所属的系列）会在编译时
  被 `tools/mk_bl1_image.py` 直接解析，用来生成 BL1 的头，见下文。其余只是参考资料。
- `nsih-generator/*.xls`：基于 Excel 的（Windows）生成器，把更易读的表格转换成
  上面 `.txt` 文件里的原始十六进制值。
- `tools/mk_bl1_image.py`：真正给编译出来的 BL1 加头的工具，`make` 最后一步
  会自动运行它。见[为什么 BL1 要在前面拼一个头](#为什么-bl1-要在前面拼一个头而不是嵌在代码里)
  和[修改头](#修改头)。

## 为什么 BL1 要在前面拼一个头，而不是嵌在代码里

链接脚本把 BL1 的代码从 `0xFFFF0200` 开始放，而不是 `0xFFFF0000`：

```
. = 0x00000000FFFF0200;
```

这不是随便定的：链接器假定代码运行时，SRAM 里的 `0xFFFF0000`~`0xFFFF01FF`
已经放着一个 512 字节的 NSIH 头。实际启动过程如下（通过对照链接地址、一个能用的
预编译镜像的头字段，以及 [celeron633/mk6818](https://github.com/celeron633/mk6818)
确认。后者是之前为这颗 SoC 写的独立工具，把头 + BL1 + 第二个头 + u-boot 拼成
一个可烧录镜像）：

1. BootROM 从启动设备读第 1 扇区（这个 512 字节的 NSIH 头），连同紧跟其后的
   BL1 代码，作为一整块加载到 `LOADADDR`（`0xFFFF0000`）。
2. BootROM 跳到 `LAUNCHADDR`，同样是 `0xFFFF0000`，也就是**头本身的开头**，
   而不是编译出来的代码。头的前 16 个字（`VECTOR[8]`/`VECTOR_Rel[8]`，偏移
   `0x000`~`0x03C`）是真正的 AArch32 指令。不管后面跑什么，这颗 SoC 的 BootROM
   总是以 AArch32 启动核心。
3. `OPMODE=aarch64` 时，这段 AArch32 小程序写 TIEOFF 寄存器，把核心配置成
   AArch64、复位地址指向 `0xFFFF0200`，然后让核心自复位。这是真正的复位，不是跳转：
   架构上 AArch32 代码没法直接 `eret`/跳进 AArch64，只有复位能跨过这道坎。
   `OPMODE=aarch32` 时，`mk_bl1_image.py --opmode aarch32` 把这段向量换成一条
   直接跳到 `0xFFFF0200` 的 `B` 指令，其余槽位填 `B .`。
4. 核心从 `0xFFFF0200` 开始执行，那里正是链接器放入口的位置。

所以**头和编译出来的代码是两块，烧录前必须拼在一起**。`make` 用 `objcopy`
直接输出的裸二进制没有签名、没有向量，BootROM 不认，启动不了。在加入
`tools/mk_bl1_image.py` 之前，这个仓库确实有这个 bug：所有编译说明都让你直接烧
链接器的裸输出。

**头必须来自 `reference-nsih/*.txt`，不能来自 `prebuilt/bl1-mmcboot.bin`**。
`mk_bl1_image.py` 早期版本是从这个预编译文件里借头字节的，结果生成的镜像在真机上
彻底卡死（串口一个字都没有）。原因是 `prebuilt/bl1-mmcboot.bin` 的向量根本不是
上面说的通用 TIEOFF+复位程序，它的第一条指令是一条普通的 AArch32 跳转，跳到当初
那次编译中后续代码的一个**固定字节偏移**。把这个头嫁接到另一份（新编译、大小不同的）
代码上，这条跳转就落到新代码里几乎随机的位置，被当作 AArch32 指令执行，在 UART
初始化之前就必然崩溃。`reference-nsih/raptor-sd-64.txt` 和 `raptor-emmc-64.txt`
（本板所属系列）带的是正确的、与芯片通用的那段程序，已确认和
[rafaello7/bl1-nanopi-m3](https://github.com/rafaello7/bl1-nanopi-m3)
里的逐字节相同。那是另一块 S5P6818 板子，已在真机上验证过；它把同样的头直接写进
`startup_aarch64.S`/链接脚本，而本仓库选择在外部拼接，只是为了让 `config.mak` 的
`BOARD` 选择仍然有意义，不必为每块板手改汇编。

现在 `make` 先生成裸代码这个中间产物（`out/bl1-gec6818-raw.bin`），再运行
`tools/mk_bl1_image.py` 在前面拼上头、修正 `LOADSIZE`（每次编译唯一需要变的字段，
见下文），输出最终可烧录的 `out/bl1-gec6818.bin`，不需要手动运行。从哪个设备启动
是编译选项：`config.mak` 里的 `BOOT_PORT ?= sd`，换另一个就 `make BOOT_PORT=emmc`。

## 头布局（BL1 自己的头）

结构体：`src/secondboot.h` 中的 `NX_SecondBootInfo`（`ARCH_NXP5430` 分支，即本芯片）。
所有字段小端，偏移相对于 512 字节（`0x200`）头的起始：

| 偏移 | 字段 | 含义 |
|---|---|---|
| `0x000`-`0x03C` | `VECTOR[8]`、`VECTOR_Rel[8]` | BootROM **最先**执行的原始 AArch32 指令，之后才进入 BL1 真正的入口。内容取决于 `OPMODE`，见上文第 3 步。 |
| `0x040` | `DEVICEADDR` | 启动设备上的字节偏移，BL1 之后会从这里读**下一级镜像**（ATF 链是 `fip-loader.img`，`SKIP_ATF` 模式是 `u-boot-direct.img`）。SD 和 eMMC 上的值相同，见下文。 |
| `0x044` | `LOADSIZE` | 对这个头而言：BootROM 要拷到 `LOADADDR` 的 BL1 镜像总大小（含头；`SKIP_ATF` 下也包括拼在后面的 stage2）。 |
| `0x048` | `LOADADDR` | BootROM 把 BL1 拷到哪（SRAM，本芯片是 `0xFFFF0000`）。 |
| `0x04C` | `LAUNCHADDR` | 加载后 BootROM 跳到哪，即 BL1 自己的入口。 |
| `0x050` | `DBI.SDMMCBI.PortNumber`（1 字节） | **从哪个物理 SDMMC 控制器/引脚启动**，见下文。`.txt` 文件里也写作 `"Channel number"`。 |
| `0x054`（最高字节） | `DBI.SPIBI.LoadDevice` | 启动源**类型**：`0`=USB `1`=SPI `2`=NAND `3`=SDMMC `4`=SDFS `5`=UART（`secondboot.h` 里的 `BOOT_FROM_*`）。`src/secondboot.c` 的 `BootMain()` 按它决定调用哪个 `i*BOOT()` 函数，启动时会打印出来（`LoadDevice=%d ...`）。 |
| `0x05C`-`0x070` | `PLL[4]`、`PLLSPREAD[2]` | PLL 倍频/分频/展频设置 |
| `0x074`-`0x0BC`（各字段不同，见 `secondboot.h`） | `DVO[9]`、`DII`（`NX_DDRInitInfo`） | 时钟分频、DDR 芯片结构/时序 |
| ... | DDR3/LPDDR3 驱动强度、PHY 驱动强度、leveling/training 标志 | 其余见 `secondboot.h`，和启动设备选择无关 |
| `0x1F8` | `BuildInfo` | 版本/编译标记，启动时由 `buildinfo()` 打印 |
| `0x1FC` | `SIGNATURE` | `"NSIH"`（按小端 u32 是 `0x4849534E`） |

## SD 和 eMMC 启动：只差一个字节

`DBI.SDMMCBI.PortNumber`（偏移 `0x050`）决定 BL1 实际使用哪个 SDMMC **控制器实例**
（并通过 `NX_SDPADSetALT()` 决定哪组引脚复用成 SDMMC 功能）：

| PortNumber | 控制器 | 基地址 | 物理上 |
|---|---|---|---|
| `0` | SDMMC0 | `0xC0062000` | 外置 SD 卡槽 |
| `2` | SDMMC2 | `0xC0069000` | 板载 eMMC |

（`src/iSDHCBOOT.c` 的 `pgSDXCReg[3]`，对应 `prototype/base/nx_chip.h` 中的
`PHY_BASEADDR_SDMMC{0,1,2}_MODULE`。端口 `1` 在数组里有，但本板没用。）
`iSDXCBOOT()` 直接从头里设置 `pSDXCBootStatus->SDPort = pSBI->DBI.SDMMCBI.PortNumber`，
没有别的逻辑参与选端口。

项目里保留的两个预编译文件 `prebuilt/bl1-mmcboot.bin`（SD）和
`prebuilt/bl1-mmcboot-emmc.bin`（eMMC）可以证实这一点：

```
$ cmp -l prebuilt/bl1-mmcboot.bin prebuilt/bl1-mmcboot-emmc.bin
   81   0   2
```

（`cmp -l` 的偏移从 1 开始计、值是八进制，所以这是字节偏移 `0x050`，值 `0x00`
对 `0x02`。）**两个文件其余每个字节都相同**：`DEVICEADDR`（`0x10200`）相同，
`LOADADDR`/`LAUNCHADDR` 相同，其他也都相同。SD 和 eMMC 启动之间真的只差这一个字段。

### 它同时决定 u-boot 认为自己从哪启动

`BootMain()`（`src/secondboot.c`）一开始就调用 `device_set_env()`，把同一个
`PortNumber` 写进 `pReg_ClkPwr->SCRATCH[1]`（`0xC0010234`）：

```c
void device_set_env(void)
{
	unsigned int dev_portnum = pSBI->DBI.SDMMCBI.PortNumber;
	WriteIO32(&pReg_ClkPwr->SCRATCH[1], dev_portnum);
}
```

`u-boot_gec6818` 的 `board/s5p6818/gec6818/board.c`（`bd_bootdev_init()`）读回同一个
寄存器（`arch/arm/mach-nexell/include/mach/nexell.h` 中的
`SCR_ARM_SECOND_BOOT_REG1` = `0xC0010234`），决定自己的 `mmc_boot_dev`（env 和
rootfs 用哪个设备）：`EMMC_PORT_NUM` 是 `2`，`SD_PORT_NUM` 是 `0`，取值和含义都一样。
所以改 BL1 头里这一个字节，会**同时**切换"BL1 从哪加载 u-boot"和"u-boot 自己从哪启动"，
u-boot 那边不用改。

## 修改头

两个工具，分工不同：

- `tools/mk_bl1_image.py` 负责**加上**头（编译时自动运行，见
  [上文](#为什么-bl1-要在前面拼一个头而不是嵌在代码里)）。它接受 `--port sd`（默认）
  或 `--port emmc`，对应 `config.mak` 里的 `BOOT_PORT`，所以 `make` 或
  `make BOOT_PORT=emmc` 就能选。这个选项同时决定头的来源（直接解析
  `reference-nsih/raptor-sd-64.txt` 或 `raptor-emmc-64.txt`，**不是**
  `prebuilt/bl1-mmcboot.bin`，原因见上文），并显式把该头的 `PortNumber` 字节
  （偏移 `0x050`）改成对应的值，所以即使 `--header-from` 指向别的文件，两者也不会不一致。
  它还会修正 `LOADSIZE`，这是另一个需要随编译变化的字段；其余（`DEVICEADDR`、
  `LOADADDR`/`LAUNCHADDR`、PLL/DDR 初始化参数）都由硬件决定，和 BL1 代码本身或
  启动端口无关。头里的 `CRC32` 字段（偏移 `0x058`）在参考文件里是 `0`，保持为 `0`：
  本仓库的 BL1 代码不读它（见该脚本的 docstring），BootROM 在 SD/eMMC 启动时是否
  校验它尚未确认。
- `tools/set_boot_port.py` 只改一个**已经编译好**的镜像里的这一个字节，不动别的，
  也不用重新编译。适合拿现成的 `out/bl1-gec6818.bin` 改成另一个端口再烧。

```sh
# 直接按目标端口编译（推荐，一步到位）
make BOOT_PORT=sd     # 默认
make BOOT_PORT=emmc

# 或者：把已经编译好的镜像改成另一个端口，不重新编译
tools/set_boot_port.py --port emmc out/bl1-gec6818.bin -o out/bl1-gec6818-emmc.bin
```

`set_boot_port.py` 本质上就是：

```python
data = bytearray(open("out/bl1-gec6818.bin", "rb").read())
assert data[0x1fc:0x200] == b"NSIH", "doesn't look like an NSIH-headed BL1 image"
data[0x050] = 0x02   # 0x00 = SD, 0x02 = eMMC
open("out/bl1-gec6818-emmc.bin", "wb").write(data)
```

`CRC_CHECK` 默认关闭（`config.mak`），BL1 本身也不校验**自己**这个头的 CRC
（`DBI.SDMMCBI.CRC32` 只在 `CRC_CHECK_ON` 下用来校验**下一级**镜像的正文，
和这个头无关）。所以只要没开 `CRC_CHECK=y`，只改这一个字节是安全的。

**还有第三种办法，但有个坑**：在源码 `iSDXCBOOT()`（`src/iSDHCBOOT.c`）里覆盖，
那里已经有一行注释掉的代码：

```c
//	pSBI->DBI.SDMMCBI.PortNumber = 1;
```

**坑在于**：这行代码执行时，`device_set_env()` 早已把头里原来的 `PortNumber`
拷进了 `SCRATCH[1]`（`device_set_env()` 在 `BootMain()` 很早就调用，远在最终调用
`iSDXCBOOT()` 的启动设备 `switch` 之前）。在这里覆盖只会改变 *BL1 自己*读哪个设备，
**不会**改变传给 u-boot 的值，两边会对不上。只在不关心 u-boot 设备选择是否一致的
本地快速测试里用；真正要烧、要用的镜像，请改头（第一种办法）。
