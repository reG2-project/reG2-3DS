#!/usr/bin/env python3
"""Summarises the GTA2_3DS_PROF lines of a gta2_log.txt into tables.

  python3 prof_summary.py gta2_log.txt [--keep-first] [--blocks]

Each block of lines (one per GTA2_3DS_PROF frames) goes to a group: walk or drive (the player in a car on
more than 90 % of the block's steps, mixed in between), day or night (lit primitives). The first block after
a game start (its load) is left out unless --keep-first. A group's first line splits the frame's wall time
(logic: a step's time by the steps per frame; timers: what the timers added). Per part: the average per step
(logic) or per frame (the rest), the largest block average, the largest single step or frame, the average
without the parts listed under it, and the calls per step or frame. --blocks lists each block's main figures.
"""
import argparse
import re
import sys

VALUE = re.compile(r"(\S+)=([-\d.]+)/([-\d.]+)(?:/([-\d.]+)/([-\d.]+))?")

# Shown beside the rows whose place in the tree does not tell what they hold.
NOTES = {
    "frame": "wall time between frames",
    "idle": "sleeps until the next step or frame",
    "wait.vsync": "C3D_FrameSync, in the frame's first draw",
    "wait.gpu": "C3D_FrameBegin's wait for the GPU",
    "gpu": "C3D_GetDrawingTime, previous frame",
    "c3d.cpu": "C3D_GetProcessingTime, previous frame",
    "tex.convert": "palette to RGBA, outside the parts",
    "tex.upload": "tiling and mip levels, outside the parts",
    "collide": "sprite grid collisions, inside cars and peds",
    "prim": "1 in N timed, inside the draw.* parts",
    "flush": "inside prim.c3d and present",
}


def parse(path, keep_first):
    blocks = []
    block = None
    skip = False
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("prof: every"):
                skip = not keep_first
                continue
            m = re.match(r"prof: (\d+) frames, (\d+) steps", line)
            if m:
                timers = re.search(r"\(([\d.]+) ms\)", line)
                block = {"frames": int(m.group(1)), "steps": int(m.group(2)), "step": {}, "frame": {}, "count": {},
                         "timers": float(timers.group(1)) if timers else 0.0}
                continue
            if block is None:
                continue
            for kind in ("step", "frame", "count"):
                if line.startswith("prof %s:" % kind):
                    for v in VALUE.finditer(line):
                        nums = [float(x) for x in v.groups()[1:] if x is not None]
                        block[kind][v.group(1)] = nums
                    if kind == "count":
                        if skip:
                            skip = False
                        elif block["steps"]:
                            blocks.append(block)
                        block = None
    return blocks


def group_of(b):
    drive = b["count"].get("drive", [0])[0]
    lit = b["count"].get("litprims", [0])[0]
    d = "drive" if drive > 0.9 else ("walk" if drive < 0.1 else "mixed")
    return "%s %s" % (d, "night" if lit > 0 else "day")


def depth(name, names):
    parts = name.split(".")
    return sum(1 for i in range(1, len(parts)) if ".".join(parts[:i]) in names)


def table(title, blocks, kind, extra_order=()):
    names = []
    for b in blocks:
        for n in b[kind]:
            if n not in names:
                names.append(n)
    rows = []
    for n in names:
        vals = [b[kind].get(n) for b in blocks]
        have = [v for v in vals if v]
        avg = sum(v[0] for v in have) / len(blocks)
        top_block = max(v[0] for v in have)
        top = max(v[1] for v in have)
        if kind == "count":
            rows.append((n, avg, top_block, top, None, None))
        else:
            self_ = sum(v[2] for v in have) / len(blocks)
            calls = sum(v[3] for v in have) / len(blocks)
            rows.append((n, avg, top_block, top, self_, calls))
    print("\n%s" % title)
    if kind == "count":
        print("  %-22s %10s %10s %10s" % ("count", "avg", "max blk", "max"))
        for n, avg, tb, top, _, _ in rows:
            print("  %-22s %10.2f %10.2f %10.0f" % (n, avg, tb, top))
        return
    print("  %-26s %8s %8s %8s %8s %8s" % ("part (ms)", "avg", "max blk", "max", "self", "calls"))
    for n, avg, tb, top, self_, calls in rows:
        if avg < 0.0005 and top < 0.0005:
            continue
        label = "  " * depth(n, names) + n
        note = NOTES.get(n, "")
        print("  %-26s %8.3f %8.3f %8.3f %8.3f %8.1f  %s" % (label, avg, tb, top, self_, calls, note))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--keep-first", action="store_true", help="keep the first block after a game start")
    ap.add_argument("--blocks", action="store_true", help="list each block too")
    args = ap.parse_args()
    blocks = parse(args.log, args.keep_first)
    if not blocks:
        sys.exit("no GTA2_3DS_PROF blocks in %s" % args.log)
    for line in open(args.log, errors="replace"):
        if line.startswith("prof: every"):
            print(line.strip())
            break
    g = lambda d, k: d.get(k, [0])[0]
    if args.blocks:
        cols = ["step", "draw", "draw.map", "prim", "prim.light", "pump.pad", "wait.vsync", "wait.gpu", "gpu"]
        print("\n  %3s %-12s %5s %5s %6s" % ("#", "group", "steps", "fps", "max") + "".join(" %10s" % c for c in cols))
        for i, b in enumerate(blocks):
            st, fr, c = b["step"], b["frame"], b["count"]
            fps = 1000.0 / g(fr, "frame") if g(fr, "frame") else 0
            vals = [g(st, k) if k in st else g(fr, k) for k in cols]
            print("  %3d %-12s %5.2f %5.1f %6.1f" % (i, group_of(b), g(c, "steps"), fps, fr.get("frame", [0, 0])[1]) +
                  "".join(" %10.2f" % v for v in vals))
    groups = {}
    for b in blocks:
        groups.setdefault(group_of(b), []).append(b)
    for name, bs in sorted(groups.items()):
        frames = sum(b["frames"] for b in bs)
        fps = 1000.0 * len(bs) / sum(b["frame"].get("frame", [1])[0] for b in bs)
        print("\n==== %s: %d blocks, %d frames, %.1f fps" % (name, len(bs), frames, fps))
        avg = lambda kind, k: sum(g(b[kind], k) for b in bs) / len(bs)
        logic = sum(g(b["step"], "step") * g(b["count"], "steps") for b in bs) / len(bs)
        budget = [("logic", logic), ("draw", avg("frame", "draw")),
                  ("wait.vsync", avg("frame", "wait.vsync")), ("wait.gpu", avg("frame", "wait.gpu")),
                  ("tex", avg("frame", "tex.convert") + avg("frame", "tex.upload")), ("pump", avg("frame", "pump")),
                  ("inputs", avg("frame", "inputs")), ("idle", avg("frame", "idle")),
                  ("timers", sum(b["timers"] for b in bs) / len(bs))]
        rest = avg("frame", "frame") - sum(v for _, v in budget)
        print("frame %.2f ms = %s + rest %.2f" % (avg("frame", "frame"), " + ".join("%s %.2f" % x for x in budget), rest))
        table("logic, per step", bs, "step")
        table("frame, per frame", bs, "frame")
        table("counts (step ones per step, the others per frame)", bs, "count")


if __name__ == "__main__":
    main()
