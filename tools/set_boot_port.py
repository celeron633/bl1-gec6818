#!/usr/bin/env python3
"""
Patch a compiled BL1 image's NSIH header to boot from SD or eMMC.

See docs/NSIH.md for what this field actually is: byte offset 0x050 in the
header is DBI.SDMMCBI.PortNumber - which physical SDMMC controller BL1
reads from (0 = SD card slot, 2 = onboard eMMC), and it also gets relayed
to u-boot (via a CLKPWR scratch register) so u-boot picks the same device
for its own env/rootfs. Every other byte in the image is untouched.

Usage:
  ./set_boot_port.py --port sd   out/bl1-gec6818.bin -o out/bl1-gec6818-sd.bin
  ./set_boot_port.py --port emmc out/bl1-gec6818.bin -o out/bl1-gec6818-emmc.bin
  ./set_boot_port.py --port sd   out/bl1-gec6818.bin --in-place
"""
import argparse
import sys

PORT_NUMBER_OFFSET = 0x050
PORTS = {"sd": 0x00, "emmc": 0x02}


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("image", help="compiled BL1 image, e.g. out/bl1-gec6818.bin")
    p.add_argument("--port", required=True, choices=sorted(PORTS),
                    help="which device BL1 (and u-boot after it) should boot from")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("-o", "--output", help="write the patched image here instead")
    g.add_argument("--in-place", action="store_true", help="patch the file in place")
    args = p.parse_args()

    with open(args.image, "rb") as f:
        data = bytearray(f.read())

    if len(data) <= PORT_NUMBER_OFFSET or data[0x1fc:0x200] != b"NSIH":
        sys.exit(f"error: {args.image} doesn't look like an NSIH-headed BL1 "
                  f"image (no \"NSIH\" signature at offset 0x1fc)")

    old = data[PORT_NUMBER_OFFSET]
    new = PORTS[args.port]
    data[PORT_NUMBER_OFFSET] = new

    out_path = args.image if args.in_place else args.output
    with open(out_path, "wb") as f:
        f.write(data)

    print(f"PortNumber: 0x{old:02x} -> 0x{new:02x} ({args.port}), wrote {out_path}")


if __name__ == "__main__":
    main()
