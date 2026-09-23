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
PORT_NUMBER_OFFSET = 0x050
PORTS = {"sd": 0x00, "emmc": 0x02}
NSIH_TEXT_BY_PORT = {
    "sd": "reference-nsih/raptor-sd-64.txt",
    "emmc": "reference-nsih/raptor-emmc-64.txt",
}


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
    args = p.parse_args()

    header_from = args.header_from or NSIH_TEXT_BY_PORT[args.port]
    header = parse_nsih_txt(header_from)

    if header[0x1fc:0x200] != b"NSIH":
        sys.exit(f"error: {header_from} has no \"NSIH\" signature at offset 0x1fc "
                  f"- not a valid header source")

    with open(args.raw_bin, "rb") as f:
        code = f.read()

    loadsize = NSIH_SIZE + len(code)
    header[LOADSIZE_OFFSET:LOADSIZE_OFFSET + 4] = loadsize.to_bytes(4, "little")
    header[PORT_NUMBER_OFFSET] = PORTS[args.port]

    with open(args.output, "wb") as f:
        f.write(header)
        f.write(code)

    print(f"header from {header_from} ({NSIH_SIZE} bytes, port={args.port}) + "
          f"{args.raw_bin} ({len(code)} bytes) -> {args.output} "
          f"(LOADSIZE=0x{loadsize:x})")


if __name__ == "__main__":
    main()
