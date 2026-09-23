#!/usr/bin/env python3
"""
Write the GEC6818 bootloader chain to an SD card, in either of two layouts:

  atf     BL1 -> fip-loader.img -> fip-secure.img -> fip-nonsecure.img (u-boot)
          The normal chain: ARM Trusted Firmware BL2, then BL31+OP-TEE, then u-boot.

  direct  BL1 -> u-boot-direct.img
          SKIP_ATF=y build of BL1 (see docs/BOOT_MODES.md): no ATF/OP-TEE at all,
          BL1 jumps straight into a flat, directly-launchable u-boot image.
          The fip-secure.img/fip-nonsecure.img slots are not used, so the
          u-boot image is allowed to be much bigger (see DIRECT_UBOOT_SLOT_SIZE).

Only the specific byte ranges below are touched - nothing else on the card
(partition table, rootfs, an existing NSIH-configured params/env area) is
read or written. That makes this safe to re-run against a card you've
already put a rootfs on, to iterate on just the bootloader.

Offsets match u-boot_gec6818/include/configs/artik710_raptor.h's
CONFIG_DFU_ALT list and CONFIG_2NDBOOT_OFFSET/CONFIG_FIP_OFFSET/
CONFIG_FIP_SIZE - see docs/BOOT_MODES.md for the derivation.

Usage:
  # normal ATF chain
  ./write_sdcard.py --device /dev/sdX --mode atf \\
      --bl1 ../out/bl1-gec6818.bin \\
      --fip-loader ../prebuilt/fip-loader.img \\
      --fip-secure ../prebuilt/fip-secure.img \\
      --fip-nonsecure ../../u-boot_gec6818/fip-nonsecure.img

  # direct-to-uboot, no ATF
  ./write_sdcard.py --device /dev/sdX --mode direct \\
      --bl1 ../out/bl1-gec6818.bin \\
      --uboot ../../u-boot_gec6818/u-boot-direct.img

Add --dry-run to see the plan without writing anything. --device may also
point at a plain image file (e.g. sdcard.img) with --allow-file, e.g. to
build an image you `dd` onto the card from somewhere else.
"""
import argparse
import os
import stat
import subprocess
import sys

BL1_OFFSET = 0x200
BL1_SLOT_SIZE = 0x10000  # = CONFIG_2NDBOOT_SIZE

FIP_LOADER_OFFSET = 0x10200  # = CONFIG_FIP_OFFSET
FIP_LOADER_SLOT_SIZE = 0x50000

FIP_SECURE_OFFSET = 0x60200
FIP_SECURE_SLOT_SIZE = 0xC0000

FIP_NONSECURE_OFFSET = 0x1E0200
FIP_NONSECURE_SLOT_SIZE = 0x100000

# End of the region BL1/ATF own; u-boot's ENV/params.bin starts here
# (CONFIG_ENV_OFFSET = CONFIG_FIP_OFFSET + CONFIG_FIP_SIZE). In "direct"
# mode nothing needs the fip-secure/fip-nonsecure slots, so the single
# u-boot image is allowed to use the whole span up to here instead of
# being boxed into just the old fip-loader.img slot.
FIP_REGION_END = 0x2E0200
DIRECT_UBOOT_SLOT_SIZE = FIP_REGION_END - FIP_LOADER_OFFSET


class Item:
    def __init__(self, name, path, offset, slot_size):
        self.name = name
        self.path = path
        self.offset = offset
        self.slot_size = slot_size
        self.data = None

    def load(self):
        with open(self.path, "rb") as f:
            self.data = f.read()
        if len(self.data) > self.slot_size:
            sys.exit(
                f"error: {self.path} is {len(self.data)} bytes, which "
                f"exceeds its {self.slot_size}-byte slot at offset "
                f"0x{self.offset:x} ({self.name}). Writing it would "
                f"overwrite whatever comes next on the card. Aborting."
            )


def build_plan(args):
    items = [Item("bl1", args.bl1, BL1_OFFSET, BL1_SLOT_SIZE)]
    if args.mode == "atf":
        items += [
            Item("fip-loader", args.fip_loader, FIP_LOADER_OFFSET, FIP_LOADER_SLOT_SIZE),
            Item("fip-secure", args.fip_secure, FIP_SECURE_OFFSET, FIP_SECURE_SLOT_SIZE),
            Item("fip-nonsecure", args.fip_nonsecure, FIP_NONSECURE_OFFSET, FIP_NONSECURE_SLOT_SIZE),
        ]
    else:
        items.append(Item("u-boot (direct)", args.uboot, FIP_LOADER_OFFSET, DIRECT_UBOOT_SLOT_SIZE))
    for it in items:
        it.load()
    items.sort(key=lambda it: it.offset)
    for a, b in zip(items, items[1:]):
        if a.offset + len(a.data) > b.offset:
            sys.exit(
                f"error: {a.name} ({len(a.data)} bytes at 0x{a.offset:x}) "
                f"overlaps {b.name} at 0x{b.offset:x}. Aborting."
            )
    return items


def device_mountpoints(device):
    """Return mountpoints for `device` or any of its partitions."""
    mounts = []
    try:
        out = subprocess.run(
            ["lsblk", "-nrpo", "NAME,MOUNTPOINT", device],
            capture_output=True, text=True, check=True,
        ).stdout
        for line in out.splitlines():
            parts = line.split(None, 1)
            if len(parts) == 2 and parts[1].strip():
                mounts.append((parts[0], parts[1].strip()))
    except (subprocess.CalledProcessError, FileNotFoundError):
        # lsblk missing or device not a recognized block device; fall back
        # to a plain /proc/mounts scan for the exact device node only.
        try:
            with open("/proc/mounts") as f:
                for line in f:
                    fields = line.split()
                    if len(fields) > 1 and fields[0] == device:
                        mounts.append((device, fields[1]))
        except OSError:
            pass
    return mounts


def describe_device(device):
    try:
        out = subprocess.run(
            ["lsblk", "-ndo", "SIZE,MODEL,TRAN,TYPE", device],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        return out or "(lsblk gave no output)"
    except (subprocess.CalledProcessError, FileNotFoundError):
        try:
            size = os.path.getsize(device)
            return f"{size} bytes (lsblk unavailable)"
        except OSError:
            return "(could not stat device)"


def check_target(args):
    if not os.path.exists(args.device):
        if args.allow_file:
            return False  # fresh image file, do_write() will create it
        sys.exit(f"error: {args.device} does not exist")
    mode = os.stat(args.device).st_mode
    is_block = stat.S_ISBLK(mode)
    if not is_block and not args.allow_file:
        sys.exit(
            f"error: {args.device} is not a block device. If you meant to "
            f"write to a plain image file, pass --allow-file explicitly."
        )
    if is_block and not args.force_mounted:
        mounts = device_mountpoints(args.device)
        if mounts:
            print("error: found existing mounts on this device:", file=sys.stderr)
            for dev, mp in mounts:
                print(f"    {dev} -> {mp}", file=sys.stderr)
            sys.exit(
                "Unmount them first, or pass --force-mounted if you're "
                "certain this is still the right card."
            )
    return is_block


def print_plan(args, items, is_block):
    print(f"target: {args.device}")
    if is_block:
        print(f"        {describe_device(args.device)}")
    print(f"mode:   {args.mode}")
    print()
    print(f"{'item':<16}{'offset':>12}{'size':>12}   source")
    for it in items:
        print(f"{it.name:<16}0x{it.offset:<10x}{len(it.data):>12}   {it.path}")
    print()


def do_write(args, items, is_block):
    flags = os.O_WRONLY if is_block else os.O_RDWR
    if not is_block:
        # make sure the file is at least large enough to seek+write into
        needed = max(it.offset + len(it.data) for it in items)
        if not os.path.exists(args.device):
            open(args.device, "wb").close()
        if os.path.getsize(args.device) < needed:
            with open(args.device, "r+b") as f:
                f.truncate(needed)
    fd = os.open(args.device, flags)
    try:
        for it in items:
            os.pwrite(fd, it.data, it.offset)
            print(f"wrote {len(it.data)} bytes to {args.device}@0x{it.offset:x} ({it.name})")
        os.fsync(fd)
    finally:
        os.close(fd)
    subprocess.run(["sync"])


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--device", required=True, help="target block device, e.g. /dev/sdX")
    p.add_argument("--mode", required=True, choices=["atf", "direct"])
    p.add_argument("--bl1", required=True, help="BL1 binary (out/bl1-gec6818.bin)")
    p.add_argument("--fip-loader", help="prebuilt/fip-loader.img (mode=atf)")
    p.add_argument("--fip-secure", help="prebuilt/fip-secure.img (mode=atf)")
    p.add_argument("--fip-nonsecure", help="u-boot's fip-nonsecure.img (mode=atf)")
    p.add_argument("--uboot", help="u-boot's u-boot-direct.img (mode=direct)")
    p.add_argument("--yes", action="store_true", help="skip the confirmation prompt")
    p.add_argument("--dry-run", action="store_true", help="print the plan, write nothing")
    p.add_argument("--allow-file", action="store_true",
                    help="allow --device to be a plain file instead of a block device")
    p.add_argument("--force-mounted", action="store_true",
                    help="write even if the device (or a partition on it) is mounted")
    args = p.parse_args()

    if args.mode == "atf":
        missing = [n for n, v in (
            ("--fip-loader", args.fip_loader),
            ("--fip-secure", args.fip_secure),
            ("--fip-nonsecure", args.fip_nonsecure),
        ) if not v]
        if missing:
            p.error(f"mode=atf requires {', '.join(missing)}")
    else:
        if not args.uboot:
            p.error("mode=direct requires --uboot")

    items = build_plan(args)
    is_block = check_target(args)
    print_plan(args, items, is_block)

    if args.dry_run:
        print("dry run, nothing written.")
        return

    if not args.yes:
        reply = input(f"About to write to {args.device}. Type the device path again to confirm: ")
        if reply.strip() != args.device:
            sys.exit("confirmation did not match, aborting.")

    do_write(args, items, is_block)
    print("done.")


if __name__ == "__main__":
    main()
