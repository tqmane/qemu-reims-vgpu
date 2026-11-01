#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the actual report function with an event sink; no QEMU/device runs.

Usage: python tests/unit/test-xhci-xfer-report.py --cc clang --build-dir DIR
DIR must be an explicit scratch directory outside the source tree.
"""

import argparse
from pathlib import Path
import re
import subprocess


HARNESS = r"""
typedef struct XHCIState { int unused; } XHCIState;
typedef struct XHCIEPContext {
    XHCIState *xhci;
    unsigned int slotid, epid;
} XHCIEPContext;
typedef struct XHCITransfer {
    XHCIEPContext *epctx;
    XHCITRB *trbs;
    unsigned int trb_count;
    struct { unsigned int actual_length; } packet;
    TRBCCode status;
} XHCITransfer;

static XHCIState controller;
static XHCIEPContext epctx = { &controller, 5, 11 };
static XHCIEvent events[16];
static unsigned int event_count;

static void xhci_event(XHCIState *xhci, XHCIEvent *event, int vector)
{
    assert(xhci == &controller);
    assert(vector == 1);
    assert(event_count < 16);
    events[event_count++] = *event;
}

@REPORT@

static XHCITRB trb(TRBType type, unsigned int length, unsigned int flags,
                   uint64_t addr)
{
    XHCITRB result = { 0 };
    result.status = length | (1U << TRB_INTR_SHIFT);
    result.control = (type << TRB_TYPE_SHIFT) | flags;
    result.addr = addr;
    return result;
}

static void report(XHCITRB *trbs, unsigned int count, unsigned int actual,
                   TRBCCode status)
{
    XHCITransfer xfer = { &epctx, trbs, count, { actual }, status };
    event_count = 0;
    xhci_xfer_report(&xfer);
}

static void event(unsigned int index, uint64_t ptr, TRBCCode code,
                  unsigned int length, unsigned int flags)
{
    assert(index < event_count);
    assert(events[index].type == ER_TRANSFER);
    assert(events[index].slotid == 5 && events[index].epid == 11);
    assert(events[index].ptr == ptr);
    assert(events[index].ccode == code);
    assert(events[index].length == length);
    assert(events[index].flags == flags);
}

int main(void)
{
    XHCITRB normal[9], control[3], ed[2];
    unsigned int total = 0;

    for (unsigned int i = 0; i < 9; i++) {
        unsigned int length = i == 0 ? 2560 : i == 8 ? 1536 : 4096;
        unsigned int flags = i == 8 ? TRB_TR_IOC : TRB_TR_CH | TRB_TR_ISP;
        normal[i] = trb(TR_NORMAL, length, flags, 0x1000 + i * 16);
        total += length;
    }
    assert(total == 32768);

    report(normal, 9, 20, CC_SUCCESS);
    assert(event_count == 2);
    event(0, normal[0].addr, CC_SHORT_PACKET, 2540, 0);
    event(1, normal[8].addr, CC_SHORT_PACKET, 2540, 0);
    puts("PASS nine-normal-short: first ISP and tail IOC, no duplicate ISP");

    report(normal, 9, total, CC_SUCCESS);
    assert(event_count == 1);
    event(0, normal[8].addr, CC_SUCCESS, 0, 0);
    puts("PASS nine-normal-full: tail success");

    normal[3].control |= TRB_TR_IOC;
    report(normal, 9, 20, CC_SUCCESS);
    assert(event_count == 3);
    event(0, normal[0].addr, CC_SHORT_PACKET, 2540, 0);
    event(1, normal[3].addr, CC_SHORT_PACKET, 2540, 0);
    event(2, normal[8].addr, CC_SHORT_PACKET, 2540, 0);
    puts("PASS intermediate-ioc: every IOC retains first short residue");
    normal[3].control &= ~TRB_TR_IOC;

    report(normal, 9, 20, CC_STALL_ERROR);
    assert(event_count == 1);
    event(0, normal[0].addr, CC_STALL_ERROR, 2540, 0);
    puts("PASS error: one event then stop");

    control[0] = trb(TR_SETUP, 8, TRB_TR_IOC, 0x2000);
    control[1] = trb(TR_DATA, 64, TRB_TR_ISP, 0x2010);
    control[2] = trb(TR_STATUS, 0, TRB_TR_IOC, 0x2020);
    report(control, 3, 20, CC_SUCCESS);
    assert(event_count == 3);
    event(0, control[0].addr, CC_SUCCESS, 0, 0);
    event(1, control[1].addr, CC_SHORT_PACKET, 44, 0);
    event(2, control[2].addr, CC_SUCCESS, 0, 0);
    puts("PASS control: setup/status reset short reporting");

    ed[0] = trb(TR_NORMAL, 32768, TRB_TR_CH | TRB_TR_ISP, 0x3000);
    ed[1] = trb(TR_EVDATA, 0, TRB_TR_IOC, 0x3010);
    ed[1].parameter = 0x12345678;
    report(ed, 2, 20, CC_SUCCESS);
    assert(event_count == 2);
    event(0, ed[0].addr, CC_SHORT_PACKET, 32748, 0);
    event(1, ed[1].parameter, CC_SHORT_PACKET, 20, TRB_EV_ED);
    puts("PASS event-data: EDTLA stays actual bytes, not short residue");

    ed[0].control &= ~TRB_TR_ISP;
    report(ed, 2, 20, CC_SUCCESS);
    assert(event_count == 1);
    event(0, ed[1].parameter, CC_SHORT_PACKET, 20, TRB_EV_ED);
    puts("PASS event-data-only: one completion with actual bytes");
    return 0;
}
"""


def declaration(text, kind, name):
    return re.search(
        rf"typedef {kind} {name} \{{.*?\}} {name};", text, re.S
    ).group(0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument(
        "--report-source", type=Path,
        help="Optional baseline hcd-xhci.c for regression proof"
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.build_dir.resolve()
    if output == root or root in output.parents:
        parser.error("build-dir must be outside the source tree")
    source = (args.report_source or root / "hw/usb/hcd-xhci.c").read_text(
        encoding="utf-8"
    )
    header = (root / "hw/usb/hcd-xhci.h").read_text(encoding="utf-8")
    start = source.index("static void xhci_xfer_report(XHCITransfer *xfer)\n{")
    end = source.index("static void xhci_stall_ep(XHCITransfer *xfer)\n{")
    report = source[start:end]
    macro_names = ("TRB_TYPE_SHIFT", "TRB_TYPE_MASK", "TRB_TYPE",
                   "TRB_TR_CH", "TRB_TR_IOC", "TRB_TR_ISP", "TRB_EV_ED",
                   "TRB_INTR_SHIFT", "TRB_INTR_MASK", "TRB_INTR")
    macros = "\n".join(re.search(
        rf"^#define {name}(?:\(|\s).*?$", source, re.M
    ).group(0) for name in macro_names)
    prefix = """#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t dma_addr_t;
#define DPRINTF(...) do { } while (0)
"""
    declarations = "\n".join((
        declaration(header, "enum", "TRBType"),
        declaration(header, "enum", "TRBCCode"),
        declaration(header, "struct", "XHCIEvent"),
        declaration(source, "struct", "XHCITRB"),
    ))
    output.mkdir(parents=True, exist_ok=True)
    unit = output / "xhci-xfer-report.c"
    exe = output / "xhci-xfer-report.exe"
    unit.write_text(prefix + declarations + "\n" + macros + "\n" +
                    HARNESS.replace("@REPORT@", report), encoding="utf-8",
                    newline="\n")
    subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-missing-field-initializers", "-Wno-sign-compare",
                    str(unit), "-o", str(exe)],
                   check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
