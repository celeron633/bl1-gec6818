#!/usr/bin/env python3
"""
Minimal S5P6818 boot-chain emulator (Unicorn), CPU0 only:

    BootROM (HLE) -> NSIH1 vector stub (AArch32) -> TIEOFF + warm reset
    -> BL1 (AArch64 EL3, or AArch32 + AArch64 stage2 for OPMODE=aarch32)
    -> NSIH2 image (u-boot-direct.img) -> u-boot (AArch64 EL2, or AArch32
       SVC for a 32-bit u-boot and a BL1 built with UBOOT_ARCH=aarch32)

What is emulated:
  - BootROM is not code here, just what it does: read NSIH1 from SD
    offset 0x200, copy LOADSIZE bytes to LOADADDR, start AArch32 at
    LAUNCHADDR.
  - CPU0 warm reset via CLKPWR CPUWARMRESETREQ: AArch32/AArch64 and the
    reset vector come from the TIEOFF registers, exactly as on the chip.
    SRAM, DDR and peripheral state survive the reset.
  - UARTs (Samsung-style, all six): TX goes to stdout; UART0 RX from
    --send or the terminal (-i).
  - DW MMC (SDMMC0/1/2), FIFO and IDMAC, with an SDHC card behind the
    port NSIH1 boots from, backed by an SD image built from --bl1/--next
    (or --sd-image). u-boot calls that port "mmc 1"; its "mmc 0" (eMMC,
    SDMMC2) has nothing attached.
  - PWM timer counting at 1 MHz host time (u-boot's time base).
  - LCD, as pictures only: with --screenshot, every time MLC0 or one of
    its RGB layers is updated (dirty flag written) its background and
    XRGB8888 RGB layers are rendered from DDR to PREFIX-N.png if the
    picture changed, and once more at the end.
  - PLL/DDR/CCI/GMAC status polls report "done" (GMAC: no PHY). Everything
    else is plain read-back-what-was-written storage, so DDR training
    reports failure and BL1 carries on anyway.

Not emulated: secondary cores (BL1 reports them dead), interrupts, eMMC,
SD writes, exceptions other than an AArch64 SMC to EL3 (any other guest
exception stops the run and is reported; a 32-bit Linux stops at the
deliberate NULL access in futex_init).

The run ends when the guest waits for console input (e.g. at the u-boot
prompt), after --hang-seconds without UART output or SD reads, or after
--timeout.

Needs: pip install unicorn capstone (capstone only for the disassembly
printed when the run stops).
"""
import argparse
import bisect
import ctypes
import mmap
import os
import struct
import sys
import threading
import time
import zlib

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_ARCH_ARM64, UC_MODE_ARM,
                     UC_PROT_ALL, UC_HOOK_BLOCK, UC_HOOK_INTR,
                     UC_HOOK_MEM_UNMAPPED)
from unicorn import arm_const as A32
from unicorn import arm64_const as A64

SRAM_BASE, SRAM_SIZE = 0xFFFF0000, 0x10000
DDR_BASE, DDR_SIZE = 0x40000000, 0x40000000
MMIO_RANGES = [(0xC0000000, 0x00400000), (0xE0000000, 0x00100000)]

CLKPWR = 0xC0010000
CLKMODEREG0, PWRMODE, CPUWARMRESETREQ = 0x000, 0x228, 0x2AC
TIEOFF = 0xC0011000
TIEOFF_AARCH64 = 0x13C      # TIEOFFREG[79]: bit 12+n = CPUn AArch64
TIEOFF_RVBAR0 = 0x140       # TIEOFFREG[80]: CPU0 RVBAR >> 2
UART_BASES = [0xC00A1000, 0xC00A0000, 0xC00A2000, 0xC00A3000, 0xC006D000,
              0xC006F000]
SDMMC_BASES = [0xC0062000, 0xC0068000, 0xC0069000]
DREX, DDRPHY = 0xC00E0000, 0xC00E1000
CCI400 = 0xE0090000
TIMER = 0xC0017000
GMAC = 0xC0060000
GICD = 0xC0009000
MLC0 = 0xC0102000
# register -> dirty flag: MLCCONTROLT, RGB layer 0 and 1 MLCCONTROL
MLC_DIRTY = {MLC0: 1 << 3, MLC0 + 0x24: 1 << 4, MLC0 + 0x58: 1 << 4}

# Unicorn/QEMU exception numbers (UC_HOOK_INTR)
EXCEPTIONS = {1: "undefined instruction", 2: "SVC", 3: "prefetch abort",
              4: "data abort", 5: "IRQ", 6: "FIQ", 7: "BKPT", 11: "HVC",
              13: "SMC"}


def log(msg):
    sys.stdout.write(f"\n[emu] {msg}\n")
    sys.stdout.flush()


# --------------------------------------------------------------------------
# Symbols

def write_png(path, w, h, rgb):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" +
                chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def read_elf_symbols(path):
    """{name: address} of the FUNC/NOTYPE symbols in an ELF32/ELF64 LE
    .symtab. Hand-rolled: pyelftools rejects u-boot's .hash section."""
    with open(path, "rb") as f:
        elf = f.read()
    if elf[:4] != b"\x7fELF":
        raise ValueError(f"{path}: not an ELF file")
    is64 = elf[4] == 2
    if is64:
        shoff, = struct.unpack_from("<Q", elf, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", elf, 0x3A)
        sh_fmt, sym_fmt, sym_size = "<IIQQQQII", "<IBBHQQ", 24
    else:
        shoff, = struct.unpack_from("<I", elf, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", elf, 0x2E)
        sh_fmt, sym_fmt, sym_size = "<IIIIIIII", "<IIIBBH", 16
    sections = [struct.unpack_from(sh_fmt, elf, shoff + i * shentsize)
                for i in range(shnum)]
    syms = {}
    for sh in sections:
        if sh[1] != 2:          # SHT_SYMTAB
            continue
        off, size, link = sh[4], sh[5], sh[6]
        stroff = sections[link][4]
        for i in range(size // sym_size):
            ent = struct.unpack_from(sym_fmt, elf, off + i * sym_size)
            if is64:
                name, info, _, shndx, value, _ = ent
            else:
                name, value, _, info, _, shndx = ent
                value &= ~1     # Thumb bit
            if info & 0xF not in (0, 2) or not value or not shndx:
                continue
            end = elf.index(b"\0", stroff + name)
            n = elf[stroff + name:end].decode(errors="replace")
            if n and not n.startswith("$"):
                syms.setdefault(n, value)
    return syms


class Symbols:
    def __init__(self):
        self.addrs, self.names, self.starts = [], [], {}
        self.elf_syms = {}          # path -> {name: addr}, unrelocated

    def load_elf(self, path, offset=0):
        if path not in self.elf_syms:
            self.elf_syms[path] = read_elf_symbols(path)
            log(f"symbols: {path}")
        for name, addr in self.elf_syms[path].items():
            self.starts.setdefault(addr + offset, name)
        pairs = sorted(self.starts.items())
        self.addrs = [a for a, _ in pairs]
        self.names = [n for _, n in pairs]
        return self.elf_syms[path]

    def __call__(self, addr):
        i = bisect.bisect_right(self.addrs, addr) - 1
        if i < 0 or addr - self.addrs[i] > 0x10000:
            return f"0x{addr:x}"
        off = addr - self.addrs[i]
        return f"0x{addr:x} <{self.names[i]}{'+0x%x' % off if off else ''}>"


# --------------------------------------------------------------------------
# SD card + DW MMC controller

class SDCard:
    """SDHC card, block addressed."""

    def __init__(self, image):
        self.image = image
        self.rca = 0x1234
        self.state = 0          # idle
        self.app = False
        csize = max(len(image) // (512 * 1024), 1) - 1
        # CSD v2.0: C_SIZE at [69:48] in 512 KiB units, READ_BL_LEN=9
        self.csd = (1 << 126) | (0x32 << 96) | (0x5B5 << 84) | (9 << 80) | \
                   (csize << 48) | (0x7F << 39) | (0x3F << 32) | 1
        # CID: MID, OID "SD", PNM "EMU01"
        self.cid = (0x03 << 120) | (0x5344 << 104) | \
                   (int.from_bytes(b"EMU01", "big") << 64) | 1

    def r1(self):
        return (self.state << 9) | (1 << 8) | (0x20 if self.app else 0)

    def read(self, offset, length):
        data = bytes(self.image[offset:offset + length])
        return data.ljust(length, b"\0")

    def command(self, idx, arg):
        """-> (response words or None for timeout, data bytes or None)"""
        app, self.app = self.app, False
        if app:
            if idx == 41:
                if self.state == 0:
                    self.state = 1
                return [0xC0FF8000], None
            if idx == 6:
                return [self.r1()], None
            if idx == 51:
                return [self.r1()], bytes([0x02, 0x35, 0x80, 0x00, 0, 0, 0, 0])
            if idx == 13:
                return [self.r1()], bytes(64)
        if idx == 0:
            self.state = 0
            return [0], None
        if idx == 8:
            return [arg & 0xFFF], None
        if idx == 55:
            self.app = True
            return [self.r1()], None
        if idx == 2:
            self.state = 2
            return self.long(self.cid), None
        if idx == 3:
            self.state = 3
            return [(self.rca << 16) | (self.state << 9) | (1 << 8)], None
        if idx == 9:
            return self.long(self.csd), None
        if idx == 10:
            return self.long(self.cid), None
        if idx == 7:
            sel = (arg >> 16) == self.rca
            r = self.r1()
            self.state = 4 if sel else 3
            return [r], None
        if idx in (13, 16, 23):
            return [self.r1()], None
        if idx == 12:
            r = self.r1()
            self.state = 4
            return [r], None
        if idx == 6:
            return [self.r1()], bytes(64)
        if idx in (17, 18):
            return [self.r1()], "read"
        return None, None       # no response -> RTO

    @staticmethod
    def long(v):
        # R2: RESP0..3 = CID/CSD bits [127:0], RESP3 the most significant
        return [(v >> (32 * i)) & 0xFFFFFFFF for i in range(4)]


class DWMMC:
    FIFO_WORDS = 32
    R_CTRL, R_CMDARG, R_CMD, R_RESP0, R_MINTSTS, R_RINTSTS, R_STATUS = \
        0x00, 0x28, 0x2C, 0x30, 0x40, 0x44, 0x48
    R_INTMASK, R_BYTCNT, R_DATA = 0x24, 0x20, 0x200
    R_BMOD, R_DBADDR, R_IDSTS = 0x80, 0x88, 0x8C
    CD, DTO, RXDR, RTO, ACD = 1 << 2, 1 << 3, 1 << 5, 1 << 8, 1 << 14

    def __init__(self, card, board):
        self.card, self.board = card, board
        self.regs = {self.R_STATUS: 0, 0x6C: 0x5342270A, 0x70: 0x00E43481}
        self.rintsts = 0
        self.data = b""
        self.pos = 0

    def remaining(self):
        return (len(self.data) - self.pos) // 4

    def update_data_irqs(self):
        if self.remaining() > 0:
            self.rintsts |= self.RXDR
        if self.data and self.remaining() <= self.FIFO_WORDS:
            self.rintsts |= self.DTO

    def read(self, off, size):
        if off == self.R_RINTSTS:
            return self.rintsts
        if off == self.R_MINTSTS:
            return self.rintsts & self.regs.get(self.R_INTMASK, 0)
        if off == self.R_STATUS:
            n = min(self.remaining(), self.FIFO_WORDS)
            st = (n << 17) | (0 if n else 1 << 2) | (1 << 8)
            return st
        if off >= self.R_DATA:
            if self.pos + 4 > len(self.data):
                return 0
            v = struct.unpack_from("<I", self.data, self.pos)[0]
            self.pos += 4
            if self.remaining() == 0:
                self.data, self.pos = b"", 0
            return v
        return self.regs.get(off, 0)

    def write(self, off, size, val):
        if off == self.R_RINTSTS:
            self.rintsts &= ~val
            self.update_data_irqs()
            return
        if off == self.R_CTRL:
            if val & 0x2:
                self.data, self.pos = b"", 0
            val &= ~0x7
        if off >= self.R_DATA:
            return
        self.regs[off] = val
        if off == self.R_CMD and val & (1 << 31):
            self.regs[off] = val & ~(1 << 31)
            self.execute(val)

    def execute(self, cmd):
        if cmd & (1 << 21):         # update clock only
            return
        idx, arg = cmd & 0x3F, self.regs.get(self.R_CMDARG, 0)
        if self.card is None:
            self.rintsts |= self.CD | self.RTO
            return
        resp, data = self.card.command(idx, arg)
        self.board.trace_sd(idx, arg, resp)
        if resp is None:
            self.rintsts |= self.CD | self.RTO
            return
        for i, w in enumerate(resp):
            self.regs[self.R_RESP0 + 4 * i] = w
        self.rintsts |= self.CD
        if data is None:
            return
        if data == "read":
            nbytes = self.regs.get(self.R_BYTCNT, 512)
            offset = arg * 512      # SDHC: block address
            data = self.card.read(offset, nbytes)
            self.board.sd_read(offset, nbytes)
            self.card.state = 4     # back to tran (auto stop, or CMD17)
            if cmd & (1 << 12):
                self.rintsts |= self.ACD
        if self.regs.get(self.R_CTRL, 0) & (1 << 25) and \
           self.regs.get(self.R_BMOD, 0) & (1 << 7):
            self.idmac(data)
        else:
            self.data, self.pos = data, 0
            self.update_data_irqs()

    def idmac(self, data):
        """Internal DMA: chained 32-bit descriptors {flags, size, buffer,
        next} at DBADDR, as u-boot's dw_mmc sets them up."""
        addr, pos = self.regs.get(self.R_DBADDR, 0), 0
        for _ in range(4096):
            flags, size, buf, nxt = struct.unpack(
                "<4I", self.board.mem_read(addr, 16))
            if not flags & (1 << 31):           # OWN
                break
            n = size & 0x1FFF
            self.board.mem_write(buf, data[pos:pos + n])
            pos += n
            self.board.mem_write(addr, struct.pack("<I", flags & ~(1 << 31)))
            if flags & (1 << 2):                # last descriptor
                break
            addr = nxt if flags & (1 << 4) else addr + 16
        self.rintsts |= self.DTO
        self.regs[self.R_IDSTS] = self.regs.get(self.R_IDSTS, 0) | 0x102


# --------------------------------------------------------------------------
# UART (Samsung-style register layout)

class UART:
    def __init__(self, idx, board):
        self.idx, self.board, self.regs = idx, board, {}

    def read(self, off, size):
        rx = self.board.rx if self.idx == 0 else b""
        if off in (0x10, 0x18) and not rx:
            self.board.last_rx_poll = time.time()
        if off == 0x10:
            return 0x6 | (1 if rx else 0)   # TX empty, RX ready
        if off == 0x18:
            return min(len(rx), 0xFF)       # RX FIFO count, TX never full
        if off == 0x24:
            return self.board.rx_pop() if rx else 0
        return self.regs.get(off, 0)

    def write(self, off, size, val):
        if off == 0x20:
            self.board.uart_out(self.idx, val & 0xFF)
        else:
            self.regs[off] = val


# --------------------------------------------------------------------------
# PWM/timer block (Samsung-style): 5 down-counters, u-boot's time base.
# Counts at 1 MHz of host time, which is what u-boot programs it for.

class Timer:
    TCON = 0x08
    TCNTB = [0x0C, 0x18, 0x24, 0x30, 0x3C]
    TCNTO = {0x14: 0, 0x20: 1, 0x2C: 2, 0x38: 3, 0x40: 4}

    def __init__(self):
        self.regs, self.t0 = {}, {}

    def read(self, off, size):
        ch = self.TCNTO.get(off)
        if ch is None:
            return self.regs.get(off, 0)
        tcntb = self.regs.get(self.TCNTB[ch], 0)
        if ch not in self.t0:
            return tcntb
        ticks = int((time.monotonic() - self.t0[ch]) * 1e6)
        return (tcntb - ticks) % (tcntb + 1)

    def write(self, off, size, val):
        if off == self.TCON:
            for ch in range(5):
                bit = 0 if ch == 0 else ch * 4 + 4
                if val & (1 << bit) and not self.regs.get(off, 0) & (1 << bit):
                    self.t0[ch] = time.monotonic()
                elif not val & (1 << bit):
                    self.t0.pop(ch, None)
        self.regs[off] = val


# --------------------------------------------------------------------------
# Board

class Board:
    def __init__(self, args, sd, syms, uboot_syms):
        self.args, self.sd, self.syms = args, sd, syms
        self.uboot_syms = uboot_syms
        self.regs = {}
        self.reset_request = False
        self.stop_reason = None
        self.quit = False
        self.last_progress = time.time()
        self.last_rx_poll = 0
        self.rx = bytearray(args.send.encode().decode("unicode_escape").encode()
                            if args.send else b"")
        self.uc, self.aarch64 = None, False
        self.impdef = {}
        self.screens, self.last_screen = 0, None

        nsih = sd[0x200:0x400]
        port = nsih[0x50] if len(nsih) > 0x50 else 0
        card = SDCard(sd)
        self.mmc = {b: DWMMC(card if i == port else None, self)
                    for i, b in enumerate(SDMMC_BASES)}
        self.uarts = {b: UART(i, self) for i, b in enumerate(UART_BASES)}
        self.timer = Timer()
        self.regs[DDRPHY + 0x04C] = 0x492       # SHIFTC_CON reset value
        self.regs[DDRPHY + 0x3AC] = 0x00000001  # VERSION_INFO

        self.sram = self.alloc(SRAM_SIZE)
        self.ddr = self.alloc(DDR_SIZE)

    @staticmethod
    def alloc(size):
        buf = mmap.mmap(-1, size)
        return buf, ctypes.addressof(ctypes.c_char.from_buffer(buf))

    # ---- memory image shared by the AArch32 and AArch64 cores

    def map_memory(self, uc):
        uc.mem_map_ptr(SRAM_BASE, SRAM_SIZE, UC_PROT_ALL, self.sram[1])
        uc.mem_map_ptr(DDR_BASE, DDR_SIZE, UC_PROT_ALL, self.ddr[1])
        for base, size in MMIO_RANGES:
            uc.mmio_map(base, size, self.mmio_read, base,
                        self.mmio_write, base)

    def ram(self, addr, length):
        for (base, size), (buf, _) in (((SRAM_BASE, SRAM_SIZE), self.sram),
                                       ((DDR_BASE, DDR_SIZE), self.ddr)):
            if base <= addr and addr + length <= base + size:
                return buf, addr - base
        raise ValueError(f"0x{addr:x}+0x{length:x} not in SRAM/DDR")

    def mem_write(self, addr, data):
        buf, off = self.ram(addr, len(data))
        buf[off:off + len(data)] = data

    def mem_read(self, addr, length):
        buf, off = self.ram(addr, length)
        return bytes(buf[off:off + length])

    # ---- MMIO

    def device(self, addr):
        page = addr & ~0xFFF
        if page in self.uarts:
            return self.uarts[page]
        if page in self.mmc:
            return self.mmc[page]
        if page == TIMER:
            return self.timer
        return None

    def mmio_read(self, uc, offset, size, base):
        addr = base + offset
        dev = self.device(addr)
        if dev is not None:
            v = dev.read(addr & 0xFFF, size)
        else:
            v = self.regs.get(addr & ~3, 0)
            if addr == CLKPWR + CLKMODEREG0:
                v &= ~(1 << 31)         # PLL update done
            elif addr == CLKPWR + PWRMODE:
                v &= ~(1 << 15)
            elif addr == DREX + 0x040:  # PHYSTATUS: dfi_init_complete, locked
                v |= (1 << 3) | (1 << 2)
            elif addr == DREX + 0x048:  # CHIPSTATUS: idle
                v = 0
            elif addr == DDRPHY + 0x0C4:  # ZQ_STATUS: zq_done
                v |= 1
            elif addr == DDRPHY + 0x0B4:  # MDLL_CON[1]: DLL locked
                v |= 7
            elif addr == CCI400 + 0x0C:   # STSR: no change pending
                v = 0
            elif addr == GMAC + 0x10:     # GMII address: MII not busy
                v &= ~1
            elif addr == GMAC + 0x14:     # GMII data: no PHY answers
                v = 0xFFFF
            elif addr == GMAC + 0x1000:   # DMA bus mode: soft reset done
                v &= ~1
            elif GICD + 0x800 <= addr < GICD + 0x820:
                v = 0x01010101          # ITARGETSR0-7 (banked): CPU0
        if self.args.log_mmio:
            self.log_mmio("R", addr, v)
        return v

    def mmio_write(self, uc, offset, size, value, base):
        addr = base + offset
        if self.args.log_mmio:
            self.log_mmio("W", addr, value)
        dev = self.device(addr)
        if dev is not None:
            dev.write(addr & 0xFFF, size, value)
            return
        self.regs[addr & ~3] = value
        if value & MLC_DIRTY.get(addr, 0) and self.args.screenshot:
            self.screenshot()
        if addr == CLKPWR + CPUWARMRESETREQ and value & 1:
            self.regs[addr] = value & ~1
            self.reset_request = True
            uc.emu_stop()

    def screenshot(self, path=None):
        """Render MLC0 as the panel would show it: background color, then
        RGB layers 0 and 1 if enabled (XRGB8888 only), clipped to the
        screen size."""
        def reg(off):
            return self.regs.get(MLC0 + off, 0)
        size = reg(0x04)
        w, h = (size & 0x7FF) + 1, ((size >> 16) & 0x7FF) + 1
        img = bytearray((reg(0x08) & 0xFFFFFF).to_bytes(3, "big") * (w * h))
        for layer in (0, 1):
            base = 0x0C + layer * 0x34
            ctrl = reg(base + 0x18)
            if not ctrl & (1 << 5):
                continue
            if ctrl >> 16 != 0x0653:
                log(f"screenshot: RGB layer {layer} format 0x{ctrl >> 16:04x} "
                    f"not supported, skipped")
                continue
            lr, tb = reg(base), reg(base + 0x04)
            sx, ex = (lr >> 16) & 0xFFF, min(lr & 0xFFF, w - 1)
            sy, ey = (tb >> 16) & 0xFFF, min(tb & 0xFFF, h - 1)
            vstride, addr = reg(base + 0x20), reg(base + 0x2C)
            n = ex - sx + 1
            if n <= 0:
                continue
            for y in range(sy, ey + 1):
                try:
                    row = self.mem_read(addr + (y - sy) * vstride, n * 4)
                except ValueError:
                    log(f"screenshot: RGB layer {layer} at 0x{addr:x} "
                        f"is outside DDR, skipped")
                    break
                rgb = bytearray(n * 3)
                rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
                o = (y * w + sx) * 3
                img[o:o + n * 3] = rgb
        if path is None:
            if img == self.last_screen:
                return
            self.screens += 1
            path = f"{self.args.screenshot}-{self.screens}.png"
        self.last_screen = img
        write_png(path, w, h, img)
        log(f"screenshot {path} ({w}x{h})")

    def log_mmio(self, rw, addr, v):
        if not isinstance(self.device(addr), UART):
            sys.stdout.write(f"\n[mmio] {rw} 0x{addr:08x} = 0x{v:08x}  "
                             f"@ {self.syms(self.pc())}")

    # ---- devices -> board

    def uart_out(self, idx, ch):
        sys.stdout.write(chr(ch) if ch in (9, 10, 13) or 32 <= ch < 127
                         else f"\\x{ch:02x}")
        sys.stdout.flush()
        self.last_progress = time.time()

    def rx_pop(self):
        self.last_progress = time.time()
        return self.rx.pop(0)

    def trace_sd(self, idx, arg, resp):
        if self.args.trace_sd:
            r = "timeout" if resp is None else " ".join(f"{w:08x}" for w in resp)
            log(f"sd: CMD{idx} arg=0x{arg:08x} -> {r}")

    def sd_read(self, offset, nbytes):
        self.last_progress = time.time()
        log(f"sd: read 0x{nbytes:x} bytes @ SD offset 0x{offset:x}")

    def pc(self):
        if self.uc is None:
            return 0
        return self.uc.reg_read(A64.UC_ARM64_REG_PC if self.aarch64
                                else A32.UC_ARM_REG_PC)


# --------------------------------------------------------------------------
# CPU cores

def new_core(board, aarch64):
    if aarch64:
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.ctl_set_cpu_model(A64.UC_CPU_ARM64_A53)
    else:
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.ctl_set_cpu_model(A32.UC_CPU_ARM_CORTEX_A15)
    board.map_memory(uc)
    board.uc, board.aarch64 = uc, aarch64
    return uc


def enter_el3_aarch64(uc):
    # Unicorn resets to EL1. Write PSTATE = EL3h with DAIF masked, then
    # touch a system register so the cached hflags (current EL) rebuild.
    uc.reg_write(A64.UC_ARM64_REG_PSTATE, 0x3CD)
    uc.cpr_write(3, 0, 1, 0, 0, uc.cpr_read(3, 0, 1, 0, 0))


AARCH32_MODES = {0x10: "USR", 0x11: "FIQ", 0x12: "IRQ", 0x13: "SVC",
                 0x16: "MON", 0x17: "ABT", 0x1A: "HYP", 0x1B: "UND",
                 0x1F: "SYS"}


def current_el(board):
    if board.aarch64:
        return (board.uc.reg_read(A64.UC_ARM64_REG_PSTATE) >> 2) & 3
    return None


def cpu_state(board):
    """"AArch64 EL2", "AArch32 SVC", ..."""
    if board.aarch64:
        return f"AArch64 EL{current_el(board)}"
    mode = board.uc.reg_read(A32.UC_ARM_REG_CPSR) & 0x1F
    return f"AArch32 {AARCH32_MODES.get(mode, hex(mode))}"


def arm32_virt_to_phys(board, va):
    """Physical address of `va` for the AArch32 core: identity with the
    MMU off, else an ARMv7 short-descriptor walk from TTBR0 (TTBCR.N = 0,
    as Linux sets it up). Unicorn's mem_read only takes physical
    addresses."""
    uc = board.uc
    # cp15 bank 0: the one Unicorn's Cortex-A15 model actually runs on
    if not uc.cpr_read(15, 0, 1, 0, 0, 0, False) & 1:     # SCTLR.M
        return va
    ttbr = uc.cpr_read(15, 0, 2, 0, 0, 0, False) & ~0x3FFF
    l1, = struct.unpack("<I", board.mem_read(ttbr + (va >> 20) * 4, 4))
    if l1 & 3 == 2:
        if l1 & (1 << 18):                                 # supersection
            return (l1 & 0xFF000000) | (va & 0xFFFFFF)
        return (l1 & 0xFFF00000) | (va & 0xFFFFF)
    if l1 & 3 == 1:
        l2, = struct.unpack("<I", board.mem_read(
            (l1 & 0xFFFFFC00) + ((va >> 12) & 0xFF) * 4, 4))
        if l2 & 2:
            return (l2 & 0xFFFFF000) | (va & 0xFFF)
        if l2 & 3 == 1:
            return (l2 & 0xFFFF0000) | (va & 0xFFFF)
    raise ValueError(f"0x{va:x} is not mapped")


def dump_regs(board):
    uc = board.uc
    if board.aarch64:
        regs = [uc.reg_read(getattr(A64, f"UC_ARM64_REG_X{i}")) for i in range(31)]
        lines = [" ".join(f"x{i + j:<2}={regs[i + j]:016x}"
                          for j in range(4) if i + j < 31)
                 for i in range(0, 31, 4)]
        lines.append(f"sp ={uc.reg_read(A64.UC_ARM64_REG_SP):016x} "
                     f"pc ={uc.reg_read(A64.UC_ARM64_REG_PC):016x} "
                     f"pstate={uc.reg_read(A64.UC_ARM64_REG_PSTATE):08x} "
                     f"(EL{current_el(board)})")
        # frame-pointer chain: [x29] = caller's x29, [x29 + 8] = return address
        fp, frames = uc.reg_read(A64.UC_ARM64_REG_X29), []
        try:
            while fp and len(frames) < 16:
                fp, lr = struct.unpack("<QQ", uc.mem_read(fp, 16))
                if lr:
                    frames.append(board.syms(lr))
        except UcError:
            pass
        if frames:
            lines.append("backtrace: " + " <- ".join(frames))
    else:
        regs = [uc.reg_read(getattr(A32, f"UC_ARM_REG_R{i}")) for i in range(13)]
        regs += [uc.reg_read(A32.UC_ARM_REG_SP), uc.reg_read(A32.UC_ARM_REG_LR),
                 uc.reg_read(A32.UC_ARM_REG_PC)]
        lines = [" ".join(f"r{i + j:<2}={regs[i + j]:08x}" for j in range(4))
                 for i in range(0, 16, 4)]
        lines.append(f"cpsr={uc.reg_read(A32.UC_ARM_REG_CPSR):08x} "
                     f"({cpu_state(board)})")
    for line in lines:
        print("    " + line)
    pc = board.pc()
    try:
        from capstone import (Cs, CS_ARCH_ARM64, CS_ARCH_ARM, CS_MODE_ARM,
                              CS_MODE_THUMB)
        if board.aarch64:
            md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        elif uc.reg_read(A32.UC_ARM_REG_CPSR) & 0x20:
            md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        else:
            md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        start = pc - 16
        if not board.aarch64:
            start = arm32_virt_to_phys(board, start)
        code = bytes(uc.mem_read(start, 32))
        for insn in md.disasm(code, pc - 16):
            mark = "=>" if insn.address == pc else "  "
            print(f"    {mark} {board.syms(insn.address)}: "
                  f"{insn.mnemonic} {insn.op_str}")
    except (ImportError, UcError, ValueError):
        pass


# --------------------------------------------------------------------------

def build_sd(args):
    if args.sd_image:
        with open(args.sd_image, "rb") as f:
            return bytearray(f.read())
    with open(args.bl1, "rb") as f:
        bl1 = f.read()
    if bl1[0x1FC:0x200] != b"NSIH":
        sys.exit(f"{args.bl1}: no NSIH signature at 0x1fc")
    devaddr = struct.unpack_from("<I", bl1, 0x40)[0]
    sd = bytearray(0x200) + bytearray(bl1)
    if args.next:
        with open(args.next, "rb") as f:
            nxt = f.read()
        if len(sd) > devaddr:
            sys.exit(f"BL1 image (0x{len(bl1):x} bytes at SD 0x200) overlaps "
                     f"DEVICEADDR 0x{devaddr:x}")
        sd = sd.ljust(devaddr, b"\0") + nxt
    return sd.ljust((len(sd) + 0xFFFFF) & ~0xFFFFF, b"\0")


def describe_images(sd):
    nsih = sd[0x200:0x400]
    dev, size, load, launch = struct.unpack_from("<4I", nsih, 0x40)
    log(f"NSIH1 @ SD 0x200: DEVICEADDR=0x{dev:x} LOADSIZE=0x{size:x} "
        f"LOADADDR=0x{load:x} LAUNCHADDR=0x{launch:x} port={nsih[0x50]} "
        f"device={nsih[0x57]}")
    nsih2 = sd[dev:dev + 0x200]
    info = {"bl1": (size, load, launch)}
    if len(nsih2) == 0x200 and nsih2[0x1FC:0x200] == b"NSIH":
        lsize, crc = struct.unpack_from("<2I", nsih2, 0x50)
        laddr, saddr = struct.unpack_from("<2Q", nsih2, 0x58)
        log(f"NSIH2 @ SD 0x{dev:x}: LoadSize=0x{lsize:x} LoadAddr=0x{laddr:x} "
            f"StartAddr=0x{saddr:x}")
        info["next"] = (lsize, laddr, saddr)
    else:
        log(f"no NSIH2 at SD 0x{dev:x}")
    return info


def boot_rom(board, info):
    size, load, launch = info["bl1"]
    if size > SRAM_SIZE or not (SRAM_BASE <= load < SRAM_BASE + SRAM_SIZE):
        sys.exit(f"BootROM: LOADSIZE 0x{size:x} @ 0x{load:x} doesn't fit SRAM")
    board.mem_write(load, bytes(board.sd[0x200:0x200 + size]))
    log(f"BootROM: loaded 0x{size:x} bytes to 0x{load:x}, jumping to "
        f"0x{launch:x} (AArch32 SVC)")
    uc = new_core(board, aarch64=False)
    uc.reg_write(A32.UC_ARM_REG_CPSR, 0x1D3)
    return launch


def install_hooks(board, uc, info):
    syms = board.syms
    watch = {}      # address -> one-shot callback

    def milestone(msg):
        log(f"{msg}, {cpu_state(board)}")
        board.last_progress = time.time()

    if "next" in info:
        saddr = info["next"][2]
        watch[saddr] = lambda: milestone(
            f"entered NSIH2 StartAddr 0x{saddr:x} (u-boot)")
    if board.uboot_syms and "relocate_code" in board.uboot_syms:
        # u-boot copies itself to the top of DDR: once relocate_code(x0/r0 =
        # new address) runs, add its symbols again at the relocated address.
        def relocated():
            dest = uc.reg_read(A64.UC_ARM64_REG_X0 if board.aarch64
                               else A32.UC_ARM_REG_R0)
            off = dest - board.uboot_syms["_start"]
            syms.load_elf(board.args.uboot_elf, off)
            log(f"u-boot relocating to 0x{dest:x} (offset 0x{off:x})")
        watch[board.uboot_syms["relocate_code"]] = relocated

    last = [None]

    def on_block(uc, addr, size, _):
        if addr in watch:
            watch.pop(addr)()
        if board.args.trace and addr in syms.starts and addr != last[0]:
            last[0] = addr      # once per loop over a labelled block
            sys.stdout.write(f"\n[trace] {syms.starts[addr]}")

    uc.hook_add(UC_HOOK_BLOCK, on_block)

    def on_intr(uc, intno, _):
        if board.aarch64 and intno == 1 and impdef_sysreg(board, uc):
            return
        if board.aarch64 and intno == 13:
            smc_to_el3(board, uc)
            return
        board.stop_reason = (f"CPU exception {intno} "
                             f"({EXCEPTIONS.get(intno, 'unknown')})")
        uc.emu_stop()

    uc.hook_add(UC_HOOK_INTR, on_intr)

    def on_unmapped(uc, access, addr, size, value, _):
        board.stop_reason = f"unmapped access to 0x{addr:x} (size {size})"
        return False

    uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)



def impdef_sysreg(board, uc):
    """Emulate an MRS/MSR of a Cortex-A53 IMPLEMENTATION DEFINED register
    (L2CTLR_EL1, CPUECTLR_EL1, ...: op0=3, op1=1, CRn=11/15), which
    Unicorn's A53 model doesn't have and so traps as undefined. Plain
    storage is all BL1 needs. Only BL1 touches these, with the MMU off, so
    PC is a physical address. Returns False for any other instruction."""
    pc = uc.reg_read(A64.UC_ARM64_REG_PC)
    try:
        insn, = struct.unpack("<I", board.mem_read(pc, 4))
    except ValueError:
        return False
    is_read = insn & 0xFFF00000 == 0xD5300000
    if not (is_read or insn & 0xFFF00000 == 0xD5100000):
        return False
    op1, crn = (insn >> 16) & 7, (insn >> 12) & 0xF
    if not ((insn >> 19) & 3 == 3 and op1 == 1 and crn in (11, 15)):
        return False
    key, rt = (crn, (insn >> 8) & 0xF, (insn >> 5) & 7), insn & 31
    reg = getattr(A64, f"UC_ARM64_REG_X{rt}", None)
    if is_read and reg:
        uc.reg_write(reg, board.impdef.get(key, 0))
    elif not is_read:
        board.impdef[key] = uc.reg_read(reg) if reg else 0
    uc.reg_write(A64.UC_ARM64_REG_PC, pc + 4)
    return True


def smc_to_el3(board, uc):
    """Take an SMC from EL1/EL2 to EL3, as the CPU would (Unicorn stops
    at exceptions instead of delivering them): ELR/SPSR/ESR_EL3, the
    current SP into its bank, EL3h with DAIF masked, then VBAR_EL3 + 0x400
    (synchronous, from a lower EL in AArch64). The handler's ERET is
    executed by Unicorn itself. This is how an AArch64 kernel's PSCI calls
    reach BL1's resident psciHandler."""
    pc = uc.reg_read(A64.UC_ARM64_REG_PC)     # already past the SMC
    pstate = uc.reg_read(A64.UC_ARM64_REG_PSTATE)
    el = (pstate >> 2) & 3
    bank = getattr(A64, f"UC_ARM64_REG_SP_EL{el if pstate & 1 else 0}")
    uc.reg_write(bank, uc.reg_read(A64.UC_ARM64_REG_SP))
    uc.reg_write(A64.UC_ARM64_REG_ELR_EL3, pc)
    uc.cpr_write(3, 6, 4, 0, 0, pstate)                        # SPSR_EL3
    uc.reg_write(A64.UC_ARM64_REG_ESR_EL3, (0x17 << 26) | (1 << 25))
    enter_el3_aarch64(uc)
    uc.reg_write(A64.UC_ARM64_REG_SP, uc.reg_read(A64.UC_ARM64_REG_SP_EL3))
    uc.reg_write(A64.UC_ARM64_REG_PC,
                 uc.reg_read(A64.UC_ARM64_REG_VBAR_EL3) + 0x400)
    if board.args.trace_smc:
        log(f"smc from EL{el}: x0=0x{uc.reg_read(A64.UC_ARM64_REG_X0):x} "
            f"x1=0x{uc.reg_read(A64.UC_ARM64_REG_X1):x} "
            f"x2=0x{uc.reg_read(A64.UC_ARM64_REG_X2):x}, "
            f"return to {board.syms(pc)}")


def warm_reset(board):
    aarch64 = bool(board.regs.get(TIEOFF + TIEOFF_AARCH64, 0) & (1 << 12))
    if aarch64:
        entry = board.regs.get(TIEOFF + TIEOFF_RVBAR0, 0) << 2
    else:
        entry = 0xFFFF0000      # AArch32 high vectors
    log(f"CPU0 warm reset -> {'AArch64' if aarch64 else 'AArch32'} "
        f"@ 0x{entry:x}")
    uc = new_core(board, aarch64)
    if aarch64:
        enter_el3_aarch64(uc)
    else:
        uc.reg_write(A32.UC_ARM_REG_CPSR, 0x1D3)
    return entry


def watchdog(board, done):
    """Stop the core on timeout or when nothing observable happens for a
    while. Runs in its own thread: slicing emu_start() with Unicorn's
    timeout instead can replay an MMIO read when the slice ends, which
    desyncs the SD FIFO."""
    args = board.args
    deadline = time.time() + args.timeout
    while not done.wait(0.2):
        now = time.time()
        if args.interactive:
            if board.quit:
                board.stop_reason = "quit"
            else:
                continue
        elif now - board.last_progress > args.hang_seconds:
            if now - board.last_rx_poll < 1:
                board.stop_reason = "waiting for console input"
            else:
                board.stop_reason = (f"no UART output or SD access for "
                                     f"{args.hang_seconds:g}s, looks stuck")
        elif now > deadline:
            board.stop_reason = f"timeout ({args.timeout:g}s)"
        else:
            continue
        board.uc.emu_stop()
        return


def console(board):
    """-i: stdin -> UART0 RX, unbuffered and without echo. Ctrl-C goes to
    the guest, Ctrl-] quits."""
    import termios
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    mode = termios.tcgetattr(fd)
    mode[3] &= ~(termios.ICANON | termios.ECHO | termios.ISIG)
    termios.tcsetattr(fd, termios.TCSANOW, mode)

    def reader():
        while True:
            ch = os.read(fd, 1)
            if not ch or ch == b"\x1d":
                board.quit = True
                return
            board.rx += ch.replace(b"\n", b"\r")

    threading.Thread(target=reader, daemon=True).start()
    return lambda: termios.tcsetattr(fd, termios.TCSANOW, saved)


def run(board, info, entry):
    restore = console(board) if board.args.interactive else None
    done = threading.Event()
    threading.Thread(target=watchdog, args=(board, done), daemon=True).start()
    install_hooks(board, board.uc, info)
    pc = entry
    try:
        while True:
            board.reset_request = False
            begin = pc
            if not board.aarch64 and \
               board.uc.reg_read(A32.UC_ARM_REG_CPSR) & 0x20:
                begin |= 1
            try:
                board.uc.emu_start(begin, 0xFFFFFFFFFFFFFFFF if board.aarch64
                                   else 0xFFFFFFFF)
            except UcError as e:
                board.stop_reason = f"{e}" + (f" - {board.stop_reason}"
                                              if board.stop_reason else "")
            if board.reset_request and not board.stop_reason:
                pc = warm_reset(board)
                install_hooks(board, board.uc, info)
                continue
            reason = board.stop_reason or (
                "guest stopped, e.g. idle in WFI - no interrupt will wake "
                "it, they aren't emulated")
            log(f"stopped at {board.syms(board.pc())}: {reason}")
            if reason in ("quit", "waiting for console input") or \
               reason.startswith("timeout"):
                return 0
            dump_regs(board)
            return 1
    finally:
        done.set()
        if restore:
            restore()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--bl1", default=os.path.join(repo, "out/bl1-gec6818.bin"),
                   help="flashable BL1 image (with NSIH1), placed at SD 0x200 "
                        "(default: %(default)s)")
    p.add_argument("--next", default=os.path.join(
                       repo, "../u-boot_gec6818/u-boot-direct.img"),
                   help="3rd-stage NSIH2 image, placed at BL1's DEVICEADDR "
                        "(default: %(default)s)")
    p.add_argument("--sd-image", help="raw SD card image instead of --bl1/--next")
    p.add_argument("--load", action="append", default=[], metavar="FILE@ADDR",
                   help="copy FILE into DDR at ADDR before the boot starts "
                        "(repeatable), e.g. a zImage and a .dtb for u-boot to "
                        "boot via --send")
    p.add_argument("--elf", action="append", default=[],
                   help="ELF to take symbol names from (repeatable). Default: "
                        "the .elf next to --bl1, and stage2.elf there if present")
    p.add_argument("--uboot-elf",
                   help="u-boot ELF for symbol names, also after relocation "
                        "(default: 'u-boot' next to --next u-boot-direct.img)")
    p.add_argument("--timeout", type=float, default=30,
                   help="stop after this many seconds (default: %(default)s)")
    p.add_argument("--hang-seconds", type=float, default=5,
                   help="report a hang after this long without UART output or "
                        "SD reads (default: %(default)s)")
    p.add_argument("-i", "--interactive", action="store_true",
                   help="connect the terminal to UART0 (Ctrl-] quits); no "
                        "timeout or hang detection")
    p.add_argument("--send", metavar="TEXT",
                   help="bytes queued on UART0 RX, backslash escapes allowed, "
                        "e.g. '\\nmmc dev 1\\n' (the first byte stops "
                        "u-boot's autoboot and is swallowed)")
    p.add_argument("--trace", action="store_true",
                   help="print every entry to a known symbol (function or "
                        "label)")
    p.add_argument("--trace-sd", action="store_true", help="print SD commands")
    p.add_argument("--trace-smc", action="store_true",
                   help="print every SMC (PSCI call) taken to EL3")
    p.add_argument("--screenshot", metavar="PREFIX",
                   help="save the LCD (MLC0) as PREFIX-N.png each time it "
                        "changes, and as PREFIX-final.png at the end")
    p.add_argument("--log-mmio", action="store_true",
                   help="print every non-UART MMIO access")
    args = p.parse_args()
    if args.interactive and not sys.stdin.isatty():
        p.error("-i needs a terminal on stdin")

    syms = Symbols()
    elfs = args.elf
    if not elfs and not args.sd_image:
        base = os.path.splitext(args.bl1)[0]
        elfs = [f for f in (base + ".elf",
                            os.path.join(os.path.dirname(args.bl1), "stage2.elf"))
                if os.path.exists(f)]
    for e in elfs:
        syms.load_elf(e)
    if args.uboot_elf is None and not args.sd_image and \
       os.path.basename(args.next or "") == "u-boot-direct.img":
        args.uboot_elf = os.path.join(os.path.dirname(args.next), "u-boot")
    uboot_syms = {}
    if args.uboot_elf and os.path.exists(args.uboot_elf):
        uboot_syms = syms.load_elf(args.uboot_elf)

    sd = build_sd(args)
    info = describe_images(sd)
    board = Board(args, sd, syms, uboot_syms)
    for spec in args.load:
        path, _, addr = spec.rpartition("@")
        if not path:
            p.error(f"--load {spec}: expected FILE@ADDR")
        with open(path, "rb") as f:
            data = f.read()
        board.mem_write(int(addr, 0), data)
        log(f"loaded {path} (0x{len(data):x} bytes) at 0x{int(addr, 0):x}")
    entry = boot_rom(board, info)
    rc = run(board, info, entry)
    if args.screenshot and board.screens:
        board.screenshot(f"{args.screenshot}-final.png")
    return rc


if __name__ == "__main__":
    sys.exit(main())
