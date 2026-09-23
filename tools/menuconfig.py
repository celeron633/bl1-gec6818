#!/usr/bin/env python3
"""
Terminal UI for picking BL1's build options (OPMODE, SKIP_ATF, UBOOT_ARCH,
BOOT_PORT, toolchains, boot logo). Saves them to .config.mak in the repo
root, which config.mak includes ahead of its own ?= defaults; variables
given on the make command line still override it.

Invalid combinations (see docs/BOOT_MODES.md) can't be picked: e.g.
SKIP_ATF=n and UBOOT_ARCH=aarch32 only exist for OPMODE=aarch32.

Usage:
  make menuconfig
  tools/menuconfig.py [--config PATH]
  tools/menuconfig.py --print          # show the saved/default values

Keys: Up/Down (j/k) move, Left/Right/Space change a choice, Enter edits
a text value, d resets the item to its default, s saves, q quits.
"""
import argparse
import curses
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CONFIG = os.path.join(ROOT, ".config.mak")

# name, kind, default, choices / None, help
OPTIONS = [
    ("OPMODE", "choice", "aarch64", ["aarch64", "aarch32"],
     "Instruction set BL1 is built for. aarch64: the NSIH vector resets "
     "the core into AArch64 and BL1 itself stays resident at EL3 for PSCI. "
     "aarch32: needed for the ATF chain (SKIP_ATF=n) and the pure 32-bit "
     "chain; with an AArch64 u-boot, an AArch64 stage2 is appended to BL1."),
    ("SKIP_ATF", "choice", "y", ["y", "n"],
     "y: BL1 jumps straight to u-boot-direct.img and provides PSCI itself "
     "(src/psci.c). n: the prebuilt fip-loader/fip-secure (ATF BL2/BL31 + "
     "OP-TEE) chain, which needs OPMODE=aarch32."),
    ("UBOOT_ARCH", "choice", "aarch64", ["aarch64", "aarch32"],
     "Whether the u-boot BL1 jumps to is AArch64 or AArch32. aarch32 needs "
     "OPMODE=aarch32: BL1 jumps to a 32-bit u-boot in secure SVC - no "
     "stage2, no PSCI, secondary cores left off."),
    ("BOOT_PORT", "choice", "sd", ["sd", "emmc"],
     "Device BL1 (and u-boot after it) boots from. Picks the NSIH template "
     "tools/mk_bl1_image.py builds BL1's header from and patches its "
     "PortNumber byte (docs/NSIH.md)."),
    ("CROSS_TOOL_aarch32", "string", "arm-eabi-", None,
     "Toolchain prefix for the AArch32 BL1 (becomes CROSS_TOOL when "
     "OPMODE=aarch32). May be an absolute path prefix. Left/Right cycles "
     "through arm*-gcc toolchains found on PATH."),
    ("CROSS_TOOL_aarch64", "string", "aarch64-none-elf-", None,
     "Toolchain prefix for AArch64 code: the BL1 itself when "
     "OPMODE=aarch64, or the stage2 (STAGE2_CROSS_TOOL) when OPMODE=aarch32 "
     "boots an AArch64 u-boot. Left/Right cycles through aarch64*-gcc "
     "toolchains found on PATH."),
    ("SYSLOG", "choice", "y", ["y", "n"],
     "Serial log messages from BL1 (-DSYSLOG_ON)."),
    ("BOOT_LOGO", "choice", "y", ["y", "n"],
     "After DDR init, light up the RGB LCD and show a title, one line per "
     "boot step and a progress bar (src/display.c)."),
    ("BOOT_LOGO_TEXT", "string", "S5P6818 BL1 Loading...", None,
     "Title line shown on the LCD."),
    ("BOOT_LOGO_DELAY_MS", "int", "500", None,
     "Pause after each boot step shown on the LCD, in milliseconds."),
    ("LCD_BACKLIGHT", "choice", "none", ["none", "high", "low"],
     "Drive the backlight pad (GPIOD1/PWM0) high or low. none leaves it "
     "alone, since which level means \"on\" isn't known yet."),
]
BY_NAME = {o[0]: o for o in OPTIONS}


def allowed(cfg, name):
    """Values an option may take given the rest of the config."""
    choices = BY_NAME[name][3]
    if name == "SKIP_ATF" and cfg["OPMODE"] != "aarch32":
        return ["y"]
    if name == "UBOOT_ARCH" and (cfg["OPMODE"], cfg["SKIP_ATF"]) != ("aarch32", "y"):
        return ["aarch64"]
    return choices


def builds_stage2(cfg):
    return (cfg["SKIP_ATF"], cfg["OPMODE"], cfg["UBOOT_ARCH"]) == ("y", "aarch32", "aarch64")


def visible(cfg, name):
    if name == "UBOOT_ARCH":
        return cfg["SKIP_ATF"] == "y"
    if name == "CROSS_TOOL_aarch32":
        return cfg["OPMODE"] == "aarch32"
    if name == "CROSS_TOOL_aarch64":
        return cfg["OPMODE"] == "aarch64" or builds_stage2(cfg)
    if name in ("BOOT_LOGO_TEXT", "BOOT_LOGO_DELAY_MS", "LCD_BACKLIGHT"):
        return cfg["BOOT_LOGO"] == "y"
    return True


def label(cfg, name):
    if name == "CROSS_TOOL_aarch32":
        return "CROSS_TOOL"
    if name == "CROSS_TOOL_aarch64":
        return "CROSS_TOOL" if cfg["OPMODE"] == "aarch64" else "STAGE2_CROSS_TOOL"
    return name


def lock_reason(cfg, name):
    if name == "SKIP_ATF":
        return "(n needs OPMODE=aarch32)"
    return "(aarch32 needs OPMODE=aarch32, SKIP_ATF=y)"


def shortpath(path):
    rel = os.path.relpath(path)
    return path if rel.startswith("..") else rel


def normalize(cfg):
    for name, kind, *_ in OPTIONS:
        if kind == "choice" and cfg[name] not in allowed(cfg, name):
            cfg[name] = allowed(cfg, name)[0]


def validate(cfg, name):
    """Error message for a value make can't use, or None."""
    value = cfg[name]
    kind = BY_NAME[name][1]
    if kind == "int" and not re.fullmatch(r"\d+", value):
        return "must be a non-negative integer"
    if name == "BOOT_LOGO_TEXT" and re.search(r"[\"'\\$#`]", value):
        return "can't contain quotes, \\, $, # or `"
    if name.startswith("CROSS_TOOL") and not re.fullmatch(r"[\w./+-]+", value):
        return "must be a toolchain prefix like aarch64-none-elf-"
    return None


def toolchain_found(prefix):
    return shutil.which(prefix + "gcc") is not None


def find_toolchains(arch):
    """Prefixes of <arch>*-gcc compilers on PATH, e.g. 'arm-none-eabi-'."""
    start = "aarch64" if arch == "aarch64" else "arm"
    found = set()
    for d in os.environ.get("PATH", "").split(os.pathsep):
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for n in names:
            m = re.fullmatch(r"(%s[\w.+-]*-)gcc" % start, n)
            if m and os.access(os.path.join(d, n), os.X_OK):
                found.add(m.group(1))
    return sorted(found)


def chain_summary(cfg):
    if cfg["OPMODE"] == "aarch64":
        return "BL1 (AArch64, EL3 + PSCI) -> u-boot-direct.img (AArch64 EL2)"
    if cfg["SKIP_ATF"] == "n":
        return "BL1 (AArch32) -> fip-loader -> fip-secure (BL31/OP-TEE) -> fip-nonsecure"
    if cfg["UBOOT_ARCH"] == "aarch32":
        return "BL1 (AArch32 SVC) -> u-boot-direct.img (AArch32 SVC), no PSCI"
    return "BL1 (AArch32) -> stage2 (AArch64 EL3 + PSCI) -> u-boot-direct.img (AArch64 EL2)"


def load(path):
    cfg = {name: default for name, _, default, *_ in OPTIONS}
    try:
        with open(path) as f:
            for line in f:
                m = re.match(r"\s*(\w+)\s*[:?]?=\s*(.*?)\s*$", line)
                if m and m.group(1) in cfg:
                    cfg[m.group(1)] = m.group(2)
    except FileNotFoundError:
        pass
    normalize(cfg)
    return cfg


def save(path, cfg):
    lines = ["# BL1 build configuration, written by tools/menuconfig.py",
             "# (`make menuconfig`). Included by config.mak; variables on the",
             "# make command line still override these.",
             "# Boot chain: " + chain_summary(cfg)]
    width = max(len(name) for name in cfg)
    for name, *_ in OPTIONS:
        lines.append("%-*s = %s" % (width, name, cfg[name]))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


class UI:
    def __init__(self, scr, path):
        self.scr = scr
        self.path = path
        self.cfg = load(path)
        self.saved = dict(self.cfg)
        self.cur = 0
        self.msg = ""
        self.toolchains = {a: find_toolchains(a) for a in ("aarch32", "aarch64")}

        curses.curs_set(0)
        curses.use_default_colors()
        curses.init_pair(1, curses.COLOR_CYAN, -1)     # title / values
        curses.init_pair(2, curses.COLOR_RED, -1)      # errors
        curses.init_pair(3, curses.COLOR_GREEN, -1)    # ok marks
        curses.init_pair(4, curses.COLOR_YELLOW, -1)   # changed

    def items(self):
        return [o[0] for o in OPTIONS if visible(self.cfg, o[0])]

    def dirty(self):
        return self.cfg != self.saved

    def put(self, y, x, text, attr=0):
        h, w = self.scr.getmaxyx()
        if 0 <= y < h and x < w:
            self.scr.addnstr(y, x, text, w - x - 1, attr)

    def draw(self):
        scr = self.scr
        scr.erase()
        h, w = scr.getmaxyx()
        items = self.items()
        self.cur = min(self.cur, len(items) - 1)

        title = " GEC6818 BL1 configuration "
        self.put(0, 0, title, curses.A_BOLD | curses.color_pair(1))
        self.put(0, len(title) + 1, shortpath(self.path)
                 + (" [modified]" if self.dirty() else ""),
                 curses.color_pair(4) if self.dirty() else curses.A_DIM)
        self.put(1, 1, chain_summary(self.cfg), curses.A_DIM)
        self.put(2, 0, "-" * (w - 1), curses.A_DIM)

        y = 3
        for i, name in enumerate(items):
            value = self.cfg[name]
            locked = BY_NAME[name][1] == "choice" and len(allowed(self.cfg, name)) == 1
            sel = i == self.cur
            marker = "*" if value != self.saved[name] else " "
            self.put(y, 0, (">" if sel else " ") + marker, curses.color_pair(4))
            self.put(y, 3, "%-20s" % label(self.cfg, name),
                     curses.A_REVERSE if sel else 0)
            shown = "<%s>" % value if BY_NAME[name][1] == "choice" else "[%s]" % value
            self.put(y, 25, shown, curses.A_DIM if locked else curses.color_pair(1))
            x = 26 + len(shown)
            err = validate(self.cfg, name)
            if err:
                self.put(y, x, err, curses.color_pair(2))
            elif name.startswith("CROSS_TOOL"):
                if toolchain_found(value):
                    self.put(y, x, "found", curses.color_pair(3))
                else:
                    self.put(y, x, value + "gcc not found", curses.color_pair(2))
            elif locked:
                self.put(y, x, lock_reason(self.cfg, name), curses.A_DIM)
            y += 1

        y += 1
        self.put(y, 0, "-" * (w - 1), curses.A_DIM)
        name = items[self.cur]
        words, line, y = BY_NAME[name][4].split(), "", y + 1
        for word in words:
            if len(line) + len(word) + 1 > w - 3:
                self.put(y, 1, line)
                line, y = "", y + 1
            line = (line + " " + word).strip()
        self.put(y, 1, line)

        keys = "Up/Down move  Left/Right/Space change  Enter edit  d default  s save  q quit"
        self.put(h - 2, 0, self.msg, curses.color_pair(2) if self.msg.startswith("!")
                 else curses.color_pair(3))
        self.put(h - 1, 0, keys, curses.A_DIM)
        scr.refresh()

    def cycle(self, name, step):
        kind = BY_NAME[name][1]
        if kind == "choice":
            vals = allowed(self.cfg, name)
        elif name.startswith("CROSS_TOOL"):
            vals = list(self.toolchains[name[len("CROSS_TOOL_"):]])
            if self.cfg[name] not in vals:
                vals.insert(0, self.cfg[name])
            if len(vals) == 1:
                self.msg = "! no other %s toolchains on PATH - Enter to type one" % name[-7:]
                return
        else:
            return
        i = vals.index(self.cfg[name]) if self.cfg[name] in vals else 0
        self.cfg[name] = vals[(i + step) % len(vals)]
        normalize(self.cfg)

    def edit(self, name):
        """One-line editor on the message row. Esc cancels."""
        h, w = self.scr.getmaxyx()
        buf, pos = list(self.cfg[name]), len(self.cfg[name])
        prompt = label(self.cfg, name) + ": "
        curses.curs_set(1)
        try:
            while True:
                self.scr.move(h - 2, 0)
                self.scr.clrtoeol()
                self.put(h - 2, 0, prompt, curses.A_BOLD)
                self.put(h - 2, len(prompt), "".join(buf))
                self.scr.move(h - 2, min(len(prompt) + pos, w - 2))
                ch = self.scr.get_wch()
                if ch in ("\n", "\r", curses.KEY_ENTER):
                    self.cfg[name] = "".join(buf).strip()
                    return
                if ch == "\x1b":
                    return
                if ch in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                    if pos:
                        del buf[pos - 1]
                        pos -= 1
                elif ch == curses.KEY_DC:
                    if pos < len(buf):
                        del buf[pos]
                elif ch == curses.KEY_LEFT:
                    pos = max(0, pos - 1)
                elif ch == curses.KEY_RIGHT:
                    pos = min(len(buf), pos + 1)
                elif ch in (curses.KEY_HOME, "\x01"):
                    pos = 0
                elif ch in (curses.KEY_END, "\x05"):
                    pos = len(buf)
                elif ch == "\x15":          # ^U
                    buf, pos = [], 0
                elif isinstance(ch, str) and ch.isprintable():
                    buf.insert(pos, ch)
                    pos += 1
        finally:
            curses.curs_set(0)

    def try_save(self):
        bad = [(n, validate(self.cfg, n)) for n in self.cfg if validate(self.cfg, n)]
        if bad:
            self.msg = "! not saved: %s %s" % bad[0]
            return False
        save(self.path, self.cfg)
        self.saved = dict(self.cfg)
        self.msg = "saved %s" % shortpath(self.path)
        return True

    def ask(self, question):
        h, _ = self.scr.getmaxyx()
        self.scr.move(h - 2, 0)
        self.scr.clrtoeol()
        self.put(h - 2, 0, question, curses.A_BOLD)
        while True:
            ch = self.scr.get_wch()
            if ch in ("y", "Y", "n", "N", "\x1b"):
                return ch.lower() if ch != "\x1b" else None

    def run(self):
        while True:
            self.draw()
            ch = self.scr.get_wch()
            self.msg = ""
            items = self.items()
            name = items[self.cur]
            kind = BY_NAME[name][1]
            if ch in (curses.KEY_UP, "k"):
                self.cur = (self.cur - 1) % len(items)
            elif ch in (curses.KEY_DOWN, "j"):
                self.cur = (self.cur + 1) % len(items)
            elif ch in (curses.KEY_HOME, "g"):
                self.cur = 0
            elif ch in (curses.KEY_END, "G"):
                self.cur = len(items) - 1
            elif ch in (curses.KEY_RIGHT, " ", "l"):
                self.cycle(name, 1)
            elif ch in (curses.KEY_LEFT, "h"):
                self.cycle(name, -1)
            elif ch in ("\n", "\r", curses.KEY_ENTER):
                if kind == "choice":
                    self.cycle(name, 1)
                else:
                    self.edit(name)
            elif ch == "d":
                self.cfg[name] = BY_NAME[name][2]
                normalize(self.cfg)
            elif ch == "s":
                self.try_save()
            elif ch in ("q", "\x1b"):
                if not self.dirty():
                    return False
                answer = self.ask("Save changes to %s? (y/n, Esc to go back) "
                                  % shortpath(self.path))
                if answer == "n":
                    return False
                if answer == "y" and self.try_save():
                    return True


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--config", default=DEFAULT_CONFIG,
                   help="file to load/save (default: .config.mak in the repo root)")
    p.add_argument("--print", action="store_true",
                   help="print the current configuration and exit")
    args = p.parse_args()

    if args.print:
        cfg = load(args.config)
        for name, *_ in OPTIONS:
            print("%-20s = %s" % (name, cfg[name]))
        print("# boot chain: " + chain_summary(cfg))
        return 0

    if not sys.stdin.isatty() or not sys.stdout.isatty():
        sys.exit("menuconfig needs a terminal (use --print to just show the config)")
    os.environ.setdefault("ESCDELAY", "25")
    saved = curses.wrapper(lambda scr: UI(scr, args.config).run())
    if saved:
        cfg = load(args.config)
        print("Saved %s:" % shortpath(args.config))
        for name, *_ in OPTIONS:
            if visible(cfg, name):
                print("  %-20s = %s" % (label(cfg, name), cfg[name]))
        print("Boot chain: " + chain_summary(cfg))
        print("Run `make` to rebuild (objects depend on .config.mak).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
