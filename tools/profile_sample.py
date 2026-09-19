#!/usr/bin/env python3
"""Statistical sampling profiler for PicoDoom over a debug probe.

Repeatedly halts core0 via OpenOCD's Tcl RPC interface, reads PC, resumes,
and builds a histogram of where the CPU is stopped -- the classic "poor
man's profiler" technique, adapted for an embedded target reachable only
over SWD (no OS, no perf/ptrace). Each halt/resume pauses execution for a
few milliseconds (visible as brief stutter/audio glitches on real hardware),
but over enough samples that's statistically negligible; do this while
actually playing/running the scenario you want profiled (e.g. the same demo
scene used for the coarse phase-timing stats).

2026-09 performance work: added after three targeted CPU/memory-latency
hypotheses (R_DrawColumn's dependent-load chain, colormap PSRAM residency,
WAD lump cache thrashing) each failed to move the "bsp+walls" render-phase
cost measured by src/i_frame_stats.hpp's coarse per-phase timers. Real
sampling data replaces further guessing.

Usage:
    1. Start OpenOCD separately, pointing at your probe + RP2350:
         openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg
       (cmsis-dap.cfg is right for the official Raspberry Pi Debug Probe;
       swap it for your probe's own interface config if different.) This
       opens a GDB server on :3333 and the Tcl RPC port this script uses
       on :6666 -- leave it running.
    2. Flash the ELF being profiled (build-st7796/PicoDoom.elf) the normal
       way first (drag-and-drop UF2, or `openocd ... -c "program ...uf2
       verify reset exit"` separately) -- this script only samples an
       already-running target, it doesn't flash.
    3. Find the right target name for core0 (RP2350 exposes both Arm cores
       to OpenOCD as separate targets): with OpenOCD still running, in
       another terminal: `telnet localhost 4444`, then `targets` -- note
       the core0 name (commonly `rp2350.core0` or similar; the exact string
       depends on the OpenOCD target file's naming). Pass it via --target.
    4. While actually playing/running the scenario on the device:
         python3 tools/profile_sample.py --elf build-st7796/PicoDoom.elf --target rp2350.core0
       Let it run for the whole scenario (a demo loop works well since it
       repeats), then it prints a sorted function-level histogram.

Requires: arm-none-eabi-addr2line on PATH (already installed, part of the
same toolchain PicoDoom itself builds with).
"""

import argparse
import socket
import subprocess
import sys
import time
from collections import Counter


def send_cmd(sock: socket.socket, cmd: str) -> str:
    """Send one command over OpenOCD's Tcl RPC protocol (newline-agnostic,
    each command/response framed by a trailing 0x1a byte)."""
    sock.sendall(cmd.encode() + b"\x1a")
    resp = b""
    while not resp.endswith(b"\x1a"):
        chunk = sock.recv(4096)
        if not chunk:
            break
        resp += chunk
    return resp[:-1].decode(errors="replace").strip()


def parse_pc(reg_output: str):
    # `reg pc` prints a line like: "pc (/32): 0x10008a4c"
    for token in reg_output.split():
        if token.startswith("0x"):
            try:
                return int(token, 16)
            except ValueError:
                pass
    return None


def resolve_addrs(elf_path: str, addrs, inlines: bool = False):
    if not addrs:
        return []
    flags = ["-f", "-C", "-e", elf_path]
    if inlines:
        flags.append("-i")
    proc = subprocess.run(
        ["arm-none-eabi-addr2line", *flags],
        input="\n".join(hex(a) for a in addrs),
        capture_output=True,
        text=True,
        check=True,
    )
    if not inlines:
        lines = proc.stdout.splitlines()
        # addr2line -f prints two lines per address: function name, then file:line
        return [lines[i] for i in range(0, len(lines), 2)]

    # With -i, addr2line prints a *variable-length* block per address (the
    # full inline chain, innermost first, two lines each), with no
    # delimiter between addresses other than "this line pair repeats until
    # the next address's block starts". Since we can't tell those blocks
    # apart from stdout alone, resolve one address at a time instead --
    # slower, but correct, and only used for a handful of drill-down
    # addresses, not the whole sample set.
    raise NotImplementedError("call resolve_addr_inlines() for one address at a time instead")


def resolve_addr_inlines(elf_path: str, addr: int):
    proc = subprocess.run(
        ["arm-none-eabi-addr2line", "-f", "-C", "-i", "-e", elf_path, hex(addr)],
        capture_output=True,
        text=True,
        check=True,
    )
    lines = proc.stdout.splitlines()
    return [(lines[i], lines[i + 1]) for i in range(0, len(lines) - 1, 2)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", required=True, help="Path to the .elf being profiled (for symbol resolution)")
    ap.add_argument("--host", default="localhost", help="OpenOCD Tcl RPC host (default: localhost)")
    ap.add_argument("--port", type=int, default=6666, help="OpenOCD Tcl RPC port (default: 6666)")
    ap.add_argument("--target", default="", help="Explicit OpenOCD target name for core0 (see step 3 in the module docstring). Leave empty to use OpenOCD's current default target.")
    ap.add_argument("--samples", type=int, default=2000, help="Number of halt/resume samples to take (default: 2000)")
    ap.add_argument("--interval", type=float, default=0.003, help="Seconds to run between samples (default: 0.003)")
    ap.add_argument("--drill-down", default="", help="After the histogram, show the full inline chain (addr2line -i) for every sample whose resolved top-level function name matches this substring -- use this to check whether a surprising hot function is a real standalone call or an inlining/symbol-resolution artifact.")
    ap.add_argument("--drill-down-lr", action="store_true", help="With --drill-down, also read $lr at each matching sample and resolve it -- reveals the actual caller when the matched function is a leaf (no nested calls) that hasn't clobbered LR yet, e.g. still inside its own loop body.")
    args = ap.parse_args()

    sock = socket.create_connection((args.host, args.port))
    if args.target:
        # `targets <name>` SWITCHES OpenOCD's current target -- it's not a
        # per-command prefix (that syntax silently returns an empty
        # response, which is why samples came back unresolved at first).
        # Select once, then issue plain halt/reg/resume against whichever
        # target is now current.
        send_cmd(sock, f"targets {args.target}")

    pcs = []
    lrs = []  # only populated (non-None entries) when --drill-down-lr is set
    print(f"Sampling {args.samples} times, ~{args.interval*1000:.1f}ms apart "
          f"(~{args.samples*args.interval:.1f}s total) -- keep the target doing "
          f"the thing you want profiled now.", file=sys.stderr)
    try:
        for i in range(args.samples):
            send_cmd(sock, "halt")
            pc_line = send_cmd(sock, "reg pc")
            pc = parse_pc(pc_line)
            if pc is not None:
                pcs.append(pc)
                if args.drill_down_lr:
                    lr_line = send_cmd(sock, "reg lr")
                    lrs.append(parse_pc(lr_line))
                else:
                    lrs.append(None)
            send_cmd(sock, "resume")
            time.sleep(args.interval)
            if (i + 1) % 200 == 0:
                print(f"  {i+1}/{args.samples} samples...", file=sys.stderr)
    except KeyboardInterrupt:
        print("Interrupted, resolving what we have so far...", file=sys.stderr)
    finally:
        try:
            send_cmd(sock, "resume")
        except Exception:
            pass
        sock.close()

    funcs = resolve_addrs(args.elf, pcs)
    if not funcs:
        print("No samples resolved -- check --target and that the ELF matches what's running.", file=sys.stderr)
        return 1

    counts = Counter(funcs)
    total = sum(counts.values())
    print(f"\n{total} samples resolved\n")
    for func, n in counts.most_common(40):
        print(f"{100.0 * n / total:5.1f}%  {n:5d}  {func}")

    if args.drill_down:
        matches = [(pc, lr) for pc, lr, f in zip(pcs, lrs, funcs) if args.drill_down in f]
        print(f"\n--- drill-down: {len(matches)} sample(s) matching '{args.drill_down}' ---")
        chain_counts = Counter()
        example = {}
        for pc, lr in matches:
            chain = tuple(resolve_addr_inlines(args.elf, pc))
            lr_chain = tuple(resolve_addr_inlines(args.elf, lr)) if lr is not None else None
            key = (chain, lr_chain)
            chain_counts[key] += 1
            example.setdefault(key, (pc, lr))
        for (chain, lr_chain), n in chain_counts.most_common(20):
            pc, lr = example[(chain, lr_chain)]
            print(f"\n{n} sample(s), e.g. pc={hex(pc)}:")
            for func, loc in chain:
                print(f"    {func}  ({loc})")
            if lr_chain is not None:
                print(f"  called from (lr={hex(lr)}):")
                for func, loc in lr_chain:
                    print(f"    {func}  ({loc})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
