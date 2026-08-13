# -*- coding: utf-8 -*-
# Decompile named addresses out of the Quake II PSX executable.
#
# This is a Ghidra headless post-script, not part of the build. The project's
# own disassembler (`q2psx-inspect disasm`) answers structural questions --
# which instruction reads which field, who references an address -- and is the
# right tool for most of them. What it cannot do is recover control flow from a
# function of a few hundred instructions, and the remaining unknowns (the
# per-part transform matrices, the per-frame integrators, the SortData bit
# reader) are all that shape. Decompiled C makes those readable in one pass.
#
# Usage, from a checkout with Ghidra installed:
#
#   q2psx-inspect exe disc.cue /tmp/text.bin        # segment, header stripped
#   analyzeHeadless /tmp/proj q2psx -import /tmp/text.bin \
#       -processor MIPS:LE:32:default -loader BinaryLoader \
#       -loader-baseAddr 0x80018000 \
#       -postScript decompile.py 0x80068044 0x8006A2C4
#
# The base address matters: the segment loads at 0x80018000, and importing the
# whole file instead of the segment shifts everything by the 0x800 header, so
# every address in docs/FORMATS.md would land in the wrong place.
#
# @category Q2PSX
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


def decompile(addr_text):
    addr = currentProgram.getAddressFactory().getAddress(addr_text)
    if addr is None:
        print("!! not an address: %s" % addr_text)
        return

    func = getFunctionContaining(addr)
    if func is None:
        # Nothing has claimed this address yet, which is the normal case for a
        # raw import: create the function rather than reporting a miss.
        func = createFunction(addr, None)
    if func is None:
        print("!! no function at %s" % addr_text)
        return

    iface = DecompInterface()
    iface.openProgram(currentProgram)
    try:
        res = iface.decompileFunction(func, 120, ConsoleTaskMonitor())
        if not res.decompileCompleted():
            print("!! decompilation failed at %s: %s"
                  % (addr_text, res.getErrorMessage()))
            return
        print("/* ---- %s  %s ---- */" % (addr_text, func.getName()))
        print(res.getDecompiledFunction().getC())
    finally:
        iface.dispose()


args = getScriptArgs()
if not args:
    print("usage: decompile.py <addr> [addr ...]")
else:
    for a in args:
        decompile(a)
