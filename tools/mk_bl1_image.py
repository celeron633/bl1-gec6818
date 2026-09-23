#!/usr/bin/env python3
"""
Prepend a 512-byte NSIH1 header to a raw-compiled BL1 binary, producing
the actual flashable image.

Why this is needed: `peridot_2ndboot_aarch64.lds` links BL1's code
starting at 0xFFFF0200, not 0xFFFF0000 - i.e. the linker already assumes
a 512-byte header sits in front of it in SRAM. BootROM loads NSIH1 (one
512-byte sector, containing a tiny AArch32 vector stub, then
DEVICEADDR/LOADSIZE/LOADADDR/LAUNCHADDR, PLL and DDR init parameters)
and the BL1 code as *one contiguous block* to LOADADDR (0xFFFF0000),
then jumps to LAUNCHADDR - which is the vector stub *inside NSIH1*, not
the compiled code. That stub does a TIEOFF-register + self-reset trick
to bring the core up in AArch64 mode with its reset vector pointing at
0xFFFF0200 - where the compiled code actually starts.

The header is built directly from a reference-nsih/*.txt file (the
same hex-per-line format described in docs/NSIH.md, parsed the same way as
celeron633/mk6818's process_nsih()) - NOT copied from a prebuilt
binary. That distinction matters and was the cause of a real, very
early hang: prebuilt/bl1-mmcboot.bin's own vector code is NOT this
generic TIEOFF+reset stub - its first word is a plain AArch32 branch to
a *fixed byte offset* into the code that followed it in that specific,
original build. Copying just its header onto a differently-sized,
freshly compiled binary left that branch pointing at essentially
random bytes of the new code, interpreted as AArch32 instructions.
reference-nsih/raptor-sd-64.txt and raptor-emmc-64.txt (this board's
own family - DEVICEADDR 0x10200, matching everything else in this
repo) both carry the correct, chip-generic stub - confirmed
byte-for-byte identical to the one in rafaello7/bl1-nanopi-m3 (a
different S5P6818 board, confirmed working on real hardware), which
bakes the same header directly into its startup_aarch64.S/linker
script rather than prepending it externally. This script takes the
"prepend externally" route instead, only to keep config.mak's existing
`BOARD` selection meaningful without hand-editing assembly per board.

--port selects which reference file to parse (DEVICEADDR/PLL/DDR
params are otherwise identical between the two - only the channel/
PortNumber field differs) and independently patches the PortNumber
byte itself, so the two can't disagree even if --header-from points at
some other file. LOADSIZE is also patched, since it has to track the
compiled code's size, which varies per build.

The header's CRC32 field (offset 0x058) is 0 in the reference files
and is left at 0. Nothing in this repo's BL1 source reads it (the only
CRC check in the source, CRC_Check() in secondboot.c, validates the
*3rd-stage* payload under CRC_CHECK_ON, which defaults off -
unrelated); whether BootROM itself enforces it for SD/eMMC boot is
unconfirmed.
"""
import argparse
import sys

NSIH_SIZE = 512
NSIH_WORDS = NSIH_SIZE // 4
LOADSIZE_OFFSET = 0x044
LOADADDR_OFFSET = 0x048
PORT_NUMBER_OFFSET = 0x050
PORTS = {"sd": 0x00, "emmc": 0x02}
NSIH_TEXT_BY_PORT = {
    "sd": "reference-nsih/raptor-sd-64.txt",
    "emmc": "reference-nsih/raptor-emmc-64.txt",
}

# Offset from LOADADDR where the linker script assumes compiled code
# starts (i.e. right after this 512-byte header).
CODE_OFFSET = NSIH_SIZE

# B <self>: a safe placeholder for vector slots that should never
# legitimately be reached this early - if they ever are, the core spins
# here predictably instead of executing whatever else happened to be at
# that header offset.
B_SELF = 0xEAFFFFFE

# SKIP_ATF + OPMODE=aarch32 (--stage2): two words BL1 hands u-boot's entry
# to stage2 through, right below stage2 (STAGE2_HANDOFF_ADDR in
# src/cfgBootDefine.h).
STAGE2_HANDOFF_SIZE = 0x10
# Everything stage2 owns (code+data+bss) must end below here: above it
# are AArch32 BL1's stack while it still runs from SRAM, then stage2's
# own per-CPU stacks (startup_aarch64.S, CPU0's grows down from
# 0xFFFFE400).
STAGE2_LIMIT = 0xFFFFD000


def arm_branch(from_addr, to_addr):
    """Encode an ARM `B` instruction at from_addr targeting to_addr."""
    offset = (to_addr - from_addr - 8) >> 2
    return 0xEA000000 | (offset & 0x00FFFFFF)


def patch_vector_for_aarch32(header, load_addr):
    """Overwrite the header's 16-word vector area for an OPMODE=aarch32
    build: word 0 branches straight to the compiled code (LOADADDR +
    CODE_OFFSET) instead of the reference header's switch-to-AArch64
    stub. The rest become B-self: with high vectors (SCTLR.V) these words
    also serve as the Undefined/SWI/Abort/IRQ/FIQ handlers, so a stray
    exception hangs predictably.
    """
    entry = load_addr + CODE_OFFSET
    header[0:4] = arm_branch(load_addr, entry).to_bytes(4, "little")
    for i in range(1, 16):
        header[i * 4:i * 4 + 4] = B_SELF.to_bytes(4, "little")


def parse_nsih_txt(path):
    """Parse a reference-nsih/*.txt file into its 512 raw header bytes.

    Format: one 8-hex-digit word per line, each word a big-endian-style
    hex literal (as conventionally written/read) stored little-endian
    in memory - "//" starts a trailing comment, blank/comment-only
    lines are skipped. Exactly 128 words (512 bytes) are expected.
    """
    words = []
    with open(path) as f:
        for line in f:
            token = line.split("//", 1)[0].strip().split()
            if not token:
                continue
            token = token[0]
            if len(token) == 8 and all(c in "0123456789abcdefABCDEF" for c in token):
                words.append(int(token, 16))
    if len(words) != NSIH_WORDS:
        sys.exit(f"error: {path}: expected {NSIH_WORDS} hex words ({NSIH_SIZE} "
                  f"bytes), found {len(words)} - not a valid NSIH text file")
    header = bytearray()
    for w in words:
        header += w.to_bytes(4, "little")
    return header


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("raw_bin", help="headerless BL1 binary, e.g. out/bl1-gec6818-raw.bin")
    p.add_argument("-o", "--output", required=True, help="flashable output path")
    p.add_argument("--port", choices=sorted(PORTS), default="sd",
                    help="which device BL1 (and u-boot after it) should boot from "
                         "(default: %(default)s) - see docs/NSIH.md")
    p.add_argument("--header-from",
                    help="reference-nsih-style .txt file to build the header from "
                         "(default: matches --port, see NSIH_TEXT_BY_PORT)")
    p.add_argument("--opmode", choices=("aarch32", "aarch64"), default="aarch64",
                    help="must match the OPMODE the raw binary was compiled with "
                         "(default: %(default)s). aarch32 replaces the reference "
                         "header's AArch64-switch vector stub with a plain branch "
                         "to the compiled code - see patch_vector_for_aarch32()")
    p.add_argument("--stage2",
                    help="SKIP_ATF + OPMODE=aarch32: AArch64 stage2 binary to "
                         "append, so BootROM loads it into SRAM along with BL1")
    p.add_argument("--stage2-addr", type=lambda v: int(v, 0),
                    help="stage2's link address (its Startup symbol)")
    p.add_argument("--stage2-end", type=lambda v: int(v, 0),
                    help="stage2's __bss_end__, checked against STAGE2_LIMIT")
    p.add_argument("--bl1-end", type=lambda v: int(v, 0),
                    help="BL1's own __bss_end__, checked against the handoff "
                         "words below stage2")
    args = p.parse_args()
    if args.stage2 and None in (args.stage2_addr, args.stage2_end, args.bl1_end):
        sys.exit("error: --stage2 needs --stage2-addr, --stage2-end and --bl1-end")

    header_from = args.header_from or NSIH_TEXT_BY_PORT[args.port]
    header = parse_nsih_txt(header_from)

    if header[0x1fc:0x200] != b"NSIH":
        sys.exit(f"error: {header_from} has no \"NSIH\" signature at offset 0x1fc "
                  f"- not a valid header source")

    with open(args.raw_bin, "rb") as f:
        code = f.read()

    load_addr = int.from_bytes(header[LOADADDR_OFFSET:LOADADDR_OFFSET + 4], "little")

    if args.stage2:
        handoff = args.stage2_addr - STAGE2_HANDOFF_SIZE
        if args.bl1_end > handoff:
            sys.exit(f"error: BL1 ends at 0x{args.bl1_end:x}, past the stage2 "
                      f"handoff words at 0x{handoff:x}")
        if args.stage2_end > STAGE2_LIMIT:
            sys.exit(f"error: stage2 ends at 0x{args.stage2_end:x}, past "
                      f"0x{STAGE2_LIMIT:x} (stack area)")
        with open(args.stage2, "rb") as f:
            stage2 = f.read()
        offset = args.stage2_addr - (load_addr + CODE_OFFSET)
        code = code.ljust(offset, b"\0") + stage2
        print(f"stage2 {args.stage2} ({len(stage2)} bytes) appended at "
              f"0x{args.stage2_addr:x}, ends 0x{args.stage2_end:x} with .bss")

    loadsize = NSIH_SIZE + len(code)
    header[LOADSIZE_OFFSET:LOADSIZE_OFFSET + 4] = loadsize.to_bytes(4, "little")
    header[PORT_NUMBER_OFFSET] = PORTS[args.port]

    if args.opmode == "aarch32":
        patch_vector_for_aarch32(header, load_addr)

    with open(args.output, "wb") as f:
        f.write(header)
        f.write(code)

    print(f"header from {header_from} ({NSIH_SIZE} bytes, port={args.port}, "
          f"opmode={args.opmode}) + {args.raw_bin} ({len(code)} bytes) -> "
          f"{args.output} (LOADSIZE=0x{loadsize:x})")


if __name__ == "__main__":
    main()
