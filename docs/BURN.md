# Flashing to eMMC / 烧写到 eMMC

**English** | [中文](#中文)

## English

Test a boot chain on an SD card first (`tools/write_sdcard.py`, see the
README). Once it boots from SD, this page covers writing the same chain to the
onboard eMMC: either from u-boot over TFTP, or with `dd` from Linux on the
board.

### What changes for eMMC

The eMMC uses the same byte offsets as the SD card. The only difference is the
**port number** stored in the headers: `0` = SDMMC0 (SD slot),
`2` = SDMMC2 (eMMC).

| image | eMMC variant needed? | how |
|---|---|---|
| BL1 (`out/bl1-gec6818.bin`) | yes | byte `0x050` = `0x02`: `make BOOT_PORT=emmc`, or `tools/set_boot_port.py --port emmc` |
| `u-boot-direct.img` (`SKIP_ATF=y`) | no | |
| `fip-loader.img` (`SKIP_ATF=n`) | **yes** | bytes `0x08B` and `0x0AB` = `0x02`, see below |
| `fip-secure.img`, `fip-nonsecure.img` | no | |

BL1's port byte decides where BL1 loads the next image from. BL1 also passes
it to u-boot in a scratch register (`0xC0010234`), and u-boot uses it to pick
the device for its env and rootfs (see [NSIH.md](NSIH.md)). An SD-port BL1
written to eMMC loads its next stage from the SD slot, not from the eMMC.

In the ATF chain, BL2 (`fip-loader.img`) does not ask BL1 where to load from.
It reads its own header: `dbi[0]` at 0x080 (fip-secure, device address
0x60200) and `dbi[1]` at 0x0A0 (fip-nonsecure, 0x1E0200). Each has a port
number byte, at 0x08B and 0x0AB. `prebuilt/fip-loader.img` has `0` in both
(SD). Make the eMMC variant like this:

```sh
cp prebuilt/fip-loader.img fip-loader-emmc.img
printf '\x02' | dd of=fip-loader-emmc.img bs=1 seek=$((0x8b)) conv=notrunc
printf '\x02' | dd of=fip-loader-emmc.img bs=1 seek=$((0xab)) conv=notrunc
xxd -s 0x80 -l 0x30 fip-loader-emmc.img   # 0x8B and 0xAB should now be 02
```

**Do not run `set_boot_port.py` on `fip-loader.img`.** It carries an "NSIH"
signature too, so the tool accepts it. But 0x050 in fip-loader's header is
`LoadSize`, not a port number, and the tool would corrupt it.

### Layout

In u-boot, `mmc 0` is the eMMC and `mmc 1` is the SD card. Use
`mmc dev 0; mmc info` to check: the eMMC reports `Name: 8GTF4` and
`MMC version`, the SD card reports `SD version`.

Sector numbers are 512-byte sectors, in hex (u-boot's `mmc` parses hex).
For `dd`, use the decimal column.

`SKIP_ATF=y`:

| image | byte offset | sector | `dd seek=` | slot (sectors) |
|---|---|---|---|---|
| BL1 (eMMC port) | 0x200 | 0x1 | 1 | 0x80 (64KB) |
| `u-boot-direct.img` | 0x10200 | 0x81 | 129 | 0x1680 (2880KB) |

`SKIP_ATF=n`:

| image | byte offset | sector | `dd seek=` | slot (sectors) |
|---|---|---|---|---|
| BL1 (eMMC port) | 0x200 | 0x1 | 1 | 0x80 |
| `fip-loader-emmc.img` | 0x10200 | 0x81 | 129 | 0x280 |
| `fip-secure.img` | 0x60200 | 0x301 | 769 | 0xc00 |
| `fip-nonsecure.img` | 0x1E0200 | 0xf01 | 3841 | 0x800 |

Both modes: u-boot's env is at 0x2E0200 (sector 0x1701), 16KB. Sector 0 holds
the partition table. **Never write to sector 0.** The first partition must
start after the env, at sector 5921 (0x1721) or later.

### Before you start

- Keep a working SD card. On this board a bootable SD card wins over the
  eMMC: the SD chain boots even with the vendor bootloader still on the
  eMMC. If the eMMC doesn't boot, insert the card and rewrite from its u-boot.
- Check that the eMMC already uses this layout: sector 1 should end in "NSIH".
  In u-boot:

  ```
  mmc dev 0
  mmc read 0x48000000 1 1
  md.b 0x480001fc 4          # 4e 53 49 48 = "NSIH"
  ```

### Option A: from u-boot over TFTP

Boot from the SD card, put the images in the TFTP server's root, then run the
commands below. They write whole slots, so the counts don't change after a
rebuild. The bytes after the end of the file are leftover DDR contents, which
is harmless inside the slot. `cmp.b` checks only the file's own bytes.

```
setenv ipaddr 192.168.1.100
setenv serverip 192.168.1.10
mmc dev 0
```

`SKIP_ATF=y`:

```
tftpboot 0x48000000 bl1-gec6818-emmc.bin
mmc write 0x48000000 0x1 0x80
mmc read 0x49000000 0x1 0x80
cmp.b 0x48000000 0x49000000 ${filesize}

tftpboot 0x48000000 u-boot-direct.img
mmc write 0x48000000 0x81 0x1680
mmc read 0x49000000 0x81 0x1680
cmp.b 0x48000000 0x49000000 ${filesize}
```

`SKIP_ATF=n` (a BL1 built with `SKIP_ATF=n BOOT_PORT=emmc`):

```
tftpboot 0x48000000 bl1-gec6818-emmc.bin
mmc write 0x48000000 0x1 0x80
mmc read 0x49000000 0x1 0x80
cmp.b 0x48000000 0x49000000 ${filesize}

tftpboot 0x48000000 fip-loader-emmc.img
mmc write 0x48000000 0x81 0x280
mmc read 0x49000000 0x81 0x280
cmp.b 0x48000000 0x49000000 ${filesize}

tftpboot 0x48000000 fip-secure.img
mmc write 0x48000000 0x301 0xc00
mmc read 0x49000000 0x301 0xc00
cmp.b 0x48000000 0x49000000 ${filesize}

tftpboot 0x48000000 fip-nonsecure.img
mmc write 0x48000000 0xf01 0x800
mmc read 0x49000000 0xf01 0x800
cmp.b 0x48000000 0x49000000 ${filesize}
```

Every `cmp.b` must print `Total of N byte(s) were the same`. The u-boot
config has no `setexpr`, so the sector count can't be calculated from
`${filesize}`. That is why whole slots are written.

### Option B: from Linux on the board with dd

Boot Linux from the SD card and copy the images over (scp, USB stick...).
Then find the eMMC. It's the device whose name is `8GTF4`, and it also has
`bootN` partitions:

```sh
grep . /sys/block/mmcblk*/device/name
ls /dev/mmcblk*boot*
EMMC=/dev/mmcblk0            # adjust to the result above
```

Write to the whole device (`/dev/mmcblkN`), not to `mmcblkNboot0/1` or
`mmcblkNpX`.

Back up the start of the eMMC and check the partitions first:

```sh
dd if=$EMMC of=emmc-head-backup.bin bs=512 count=5921   # sectors 0..0x1720, up to the end of the env
fdisk -l $EMMC                # first partition must start at >= 5921
```

`SKIP_ATF=y`:

```sh
dd if=bl1-gec6818-emmc.bin of=$EMMC bs=512 seek=1   conv=fsync
dd if=u-boot-direct.img    of=$EMMC bs=512 seek=129 conv=fsync
sync
```

`SKIP_ATF=n`:

```sh
dd if=bl1-gec6818-emmc.bin of=$EMMC bs=512 seek=1    conv=fsync
dd if=fip-loader-emmc.img  of=$EMMC bs=512 seek=129  conv=fsync
dd if=fip-secure.img       of=$EMMC bs=512 seek=769  conv=fsync
dd if=fip-nonsecure.img    of=$EMMC bs=512 seek=3841 conv=fsync
sync
```

Read back and compare. No output means they match:

```sh
verify() {  # verify <file> <sector>
    dd if=$EMMC bs=512 skip=$2 count=$(( ($(stat -c %s "$1") + 511) / 512 )) 2>/dev/null |
        cmp -n "$(stat -c %s "$1")" "$1" -
}
verify bl1-gec6818-emmc.bin 1
verify u-boot-direct.img 129      # SKIP_ATF=y
# SKIP_ATF=n: verify fip-loader-emmc.img 129; verify fip-secure.img 769; verify fip-nonsecure.img 3841
```

To undo, write the backup back:
`dd if=emmc-head-backup.bin of=$EMMC bs=512 seek=1 skip=1 conv=fsync`
(`skip=1`/`seek=1` leave the partition table alone; the backup stops before the first partition).

### After writing

- Take the SD card out and power-cycle the board.
- The serial console and the LCD boot screen should look the same as when
  booting from SD. u-boot now loads its env from the eMMC.
- The env at 0x2E0200 may still be the vendor u-boot's. If `bootcmd` or
  `bootargs` are wrong, run `env default -a; saveenv`.

### Status

- `SKIP_ATF=y` with an AArch64 BL1, written from u-boot: works on the board
  (2026-09-24).
- `SKIP_ATF=n` on eMMC: not tested on the board yet. That BL2 honours
  fip-loader's port bytes was checked in the emulator only. With 0x8B/0xAB =
  2 it looks for SDMMC2 and prints `cannot detect sdmmc` /
  `Failed to load BL31`, because the emulator has no eMMC. With 0 it
  reaches the u-boot prompt.

---

## 中文

先用 SD 卡把启动链测通（`tools/write_sdcard.py`，见 README），再按本文把同一条
链写到板载 eMMC。两种办法：在 u-boot 里通过 TFTP 写，或者在板子的 Linux 里用
`dd` 写。

### eMMC 和 SD 卡的区别

eMMC 上的字节偏移和 SD 卡完全相同，区别只在头里的**端口号**：`0` = SDMMC0
（SD 卡槽），`2` = SDMMC2（eMMC）。

| 镜像 | 要不要做 eMMC 版 | 怎么做 |
|---|---|---|
| BL1（`out/bl1-gec6818.bin`） | 要 | 字节 `0x050` = `0x02`：`make BOOT_PORT=emmc`，或 `tools/set_boot_port.py --port emmc` |
| `u-boot-direct.img`（`SKIP_ATF=y`） | 不用 | |
| `fip-loader.img`（`SKIP_ATF=n`） | **要** | 字节 `0x08B` 和 `0x0AB` = `0x02`，见下文 |
| `fip-secure.img`、`fip-nonsecure.img` | 不用 | |

BL1 的端口字节决定 BL1 从哪个设备加载下一级。BL1 还会把它写进 scratch 寄存器
（`0xC0010234`）传给 u-boot，u-boot 据此决定 env 和 rootfs 用哪个设备（见
[NSIH.md](NSIH.md)）。SD 版 BL1 写到 eMMC 上，会去 SD 卡槽加载下一级，而不是
从 eMMC 加载。

ATF 链里的 BL2（`fip-loader.img`）不问 BL1 从哪加载，而是读自己头里的
`dbi[0]`（0x080，fip-secure，设备地址 0x60200）和 `dbi[1]`（0x0A0，
fip-nonsecure，0x1E0200），两者各有一个端口号字节，在 0x08B 和 0x0AB。
`prebuilt/fip-loader.img` 里这两个字节都是 `0`（SD）。eMMC 版这样做：

```sh
cp prebuilt/fip-loader.img fip-loader-emmc.img
printf '\x02' | dd of=fip-loader-emmc.img bs=1 seek=$((0x8b)) conv=notrunc
printf '\x02' | dd of=fip-loader-emmc.img bs=1 seek=$((0xab)) conv=notrunc
xxd -s 0x80 -l 0x30 fip-loader-emmc.img   # 0x8B 和 0xAB 应该变成 02
```

**不要对 `fip-loader.img` 用 `set_boot_port.py`。** 它也带 "NSIH" 签名，所以
工具不会拒绝。但 fip-loader 头里 0x050 是 `LoadSize`，不是端口号，工具会把它改坏。

### 布局

u-boot 里 `mmc 0` 是 eMMC，`mmc 1` 是 SD 卡。可以用 `mmc dev 0; mmc info`
确认：eMMC 显示 `Name: 8GTF4` 和 `MMC version`，SD 卡显示 `SD version`。

扇区号按 512 字节一个扇区，用十六进制（u-boot 的 `mmc` 命令按十六进制解析）。
`dd` 请用十进制那一列。

`SKIP_ATF=y`：

| 镜像 | 字节偏移 | 扇区 | `dd seek=` | 槽位（扇区） |
|---|---|---|---|---|
| BL1（eMMC 端口） | 0x200 | 0x1 | 1 | 0x80（64KB） |
| `u-boot-direct.img` | 0x10200 | 0x81 | 129 | 0x1680（2880KB） |

`SKIP_ATF=n`：

| 镜像 | 字节偏移 | 扇区 | `dd seek=` | 槽位（扇区） |
|---|---|---|---|---|
| BL1（eMMC 端口） | 0x200 | 0x1 | 1 | 0x80 |
| `fip-loader-emmc.img` | 0x10200 | 0x81 | 129 | 0x280 |
| `fip-secure.img` | 0x60200 | 0x301 | 769 | 0xc00 |
| `fip-nonsecure.img` | 0x1E0200 | 0xf01 | 3841 | 0x800 |

两种模式下 u-boot 的 env 都在 0x2E0200（扇区 0x1701），16KB。扇区 0 是分区表，
**永远不要写扇区 0**。第一个分区必须从 env 之后开始，也就是扇区 5921（0x1721）
或更后。

### 动手之前

- 留一张能启动的 SD 卡。这块板子上可启动的 SD 卡优先于 eMMC：eMMC 里还是厂商
  bootloader 时，SD 卡上的链照样能启动。eMMC 起不来，就插上 SD 卡进 u-boot 重写。
- 确认 eMMC 上原来就是这个布局，扇区 1 末尾应该是 "NSIH"。在 u-boot 里：

  ```
  mmc dev 0
  mmc read 0x48000000 1 1
  md.b 0x480001fc 4          # 4e 53 49 48 = "NSIH"
  ```

### 办法一：在 u-boot 里用 TFTP 写

从 SD 卡启动，把镜像放到 TFTP 服务器根目录，然后执行下面的命令。命令按整个
槽位写，所以重新编译后扇区数不用改。文件末尾之后写进去的是 DDR 里剩下的内容，
在槽位里面没有影响。`cmp.b` 只比较文件本身的字节。

```
setenv ipaddr 192.168.1.100
setenv serverip 192.168.1.10
mmc dev 0
```

`SKIP_ATF=y`：

```
tftpboot 0x48000000 bl1-gec6818-emmc.bin
mmc write 0x48000000 0x1 0x80
mmc read 0x49000000 0x1 0x80
cmp.b 0x48000000 0x49000000 ${filesize}

tftpboot 0x48000000 u-boot-direct.img
mmc write 0x48000000 0x81 0x1680
mmc read 0x49000000 0x81 0x1680
cmp.b 0x48000000 0x49000000 ${filesize}
```

`SKIP_ATF=n`（BL1 用 `SKIP_ATF=n BOOT_PORT=emmc` 编译）：

```
tftpboot 0x48000000 bl1-gec6818-emmc.bin
mmc write 0x48000000 0x1 0x80
mmc read 0x49000000 0x1 0x80
cmp.b 0x48000000 0x49000000 ${filesize}

tftpboot 0x48000000 fip-loader-emmc.img
mmc write 0x48000000 0x81 0x280
mmc read 0x49000000 0x81 0x280
cmp.b 0x48000000 0x49000000 ${filesize}

tftpboot 0x48000000 fip-secure.img
mmc write 0x48000000 0x301 0xc00
mmc read 0x49000000 0x301 0xc00
cmp.b 0x48000000 0x49000000 ${filesize}

tftpboot 0x48000000 fip-nonsecure.img
mmc write 0x48000000 0xf01 0x800
mmc read 0x49000000 0xf01 0x800
cmp.b 0x48000000 0x49000000 ${filesize}
```

每个 `cmp.b` 都必须输出 `Total of N byte(s) were the same`。这个 u-boot 没开
`setexpr`，没法从 `${filesize}` 算扇区数，所以才整个槽位写。

### 办法二：在板子的 Linux 里用 dd 写

从 SD 卡启动 Linux，把镜像拷过去（scp、U 盘等）。先找出 eMMC：名字是 `8GTF4`
的那个，它还有 `bootN` 分区：

```sh
grep . /sys/block/mmcblk*/device/name
ls /dev/mmcblk*boot*
EMMC=/dev/mmcblk0            # 按上面的结果改
```

写整个设备（`/dev/mmcblkN`），不要写 `mmcblkNboot0/1` 或 `mmcblkNpX`。

先备份 eMMC 开头，再检查分区：

```sh
dd if=$EMMC of=emmc-head-backup.bin bs=512 count=5921   # 扇区 0..0x1720，到 env 结尾为止
fdisk -l $EMMC                # 第一个分区的起始扇区必须 >= 5921
```

`SKIP_ATF=y`：

```sh
dd if=bl1-gec6818-emmc.bin of=$EMMC bs=512 seek=1   conv=fsync
dd if=u-boot-direct.img    of=$EMMC bs=512 seek=129 conv=fsync
sync
```

`SKIP_ATF=n`：

```sh
dd if=bl1-gec6818-emmc.bin of=$EMMC bs=512 seek=1    conv=fsync
dd if=fip-loader-emmc.img  of=$EMMC bs=512 seek=129  conv=fsync
dd if=fip-secure.img       of=$EMMC bs=512 seek=769  conv=fsync
dd if=fip-nonsecure.img    of=$EMMC bs=512 seek=3841 conv=fsync
sync
```

读回来比较，没有输出就是一致：

```sh
verify() {  # verify <文件> <扇区>
    dd if=$EMMC bs=512 skip=$2 count=$(( ($(stat -c %s "$1") + 511) / 512 )) 2>/dev/null |
        cmp -n "$(stat -c %s "$1")" "$1" -
}
verify bl1-gec6818-emmc.bin 1
verify u-boot-direct.img 129      # SKIP_ATF=y
# SKIP_ATF=n：verify fip-loader-emmc.img 129; verify fip-secure.img 769; verify fip-nonsecure.img 3841
```

要恢复原样，把备份写回去：
`dd if=emmc-head-backup.bin of=$EMMC bs=512 seek=1 skip=1 conv=fsync`
（`skip=1`/`seek=1` 避开分区表；备份在第一个分区之前就结束了）。

### 写完之后

- 拔掉 SD 卡，断电重启。
- 串口输出和 LCD 启动画面应该和从 SD 启动时一样。u-boot 现在从 eMMC 读 env。
- 0x2E0200 处的 env 可能还是厂商 u-boot 留下的。如果 `bootcmd` 或 `bootargs`
  不对，执行 `env default -a; saveenv`。

### 验证情况

- AArch64 BL1 + `SKIP_ATF=y`，在 u-boot 里写入：已在板子上跑通（2026-09-24）。
- eMMC 上的 `SKIP_ATF=n`：还没上板测试。BL2 按 fip-loader 头里的端口字节加载，
  这一点只在模拟器里确认过：0x8B/0xAB 改成 2 后，BL2 去找 SDMMC2，因为模拟器没有
  eMMC，打印 `cannot detect sdmmc` / `Failed to load BL31`；改回 0 就能到 u-boot
  命令行。
