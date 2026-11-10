#!/usr/bin/env python3
"""Summarises the GTA2_3DS_PROF lines of a gta2_log.txt into tables.

  python3 prof_summary.py gta2_log.txt [--keep-first] [--blocks] [--slow]

Each block of lines (one per GTA2_3DS_PROF frames) goes to a group: walk or drive (the player in a car on
more than 90 % of the block's steps, mixed in between), day or night (lit primitives). The first block after
a game start (its load) is left out unless --keep-first. A group's first line splits the frame's wall time
(logic: a step's time by the steps per frame; timers: what the timers added). Per part: the average per step
(logic) or per frame (the rest), the largest block average, the largest single step or frame, the average
without the parts listed under it, and the calls per step or frame. --blocks lists each block's main figures,
with the player's place at its end (blocks), his vehicle model, speed (blocks per second) and the camera's height.
--slow lists the frames over GTA2_3DS_PROF_SLOW ms (45 by default) the log has a line for, with their largest parts.
--options groups the blocks by the logic options, the map cache state and where the sound pass ran
(GTA2_3DS_PROF_AB=logic, =mapcache, =soundcore1 or a comma separated list of them alternates them).
With GTA2_3DS_PROF_PMU=1 each block counted one pair of CPU events too: the step parts' events per step, then
the same without the parts under them, averaged over the blocks that counted each event.
"""
import argparse
import re
import sys

VALUE = re.compile(r"(\S+)=([-\d.]+)/([-\d.]+)(?:/([-\d.]+)/([-\d.]+))?")
PLACE = re.compile(r"prof place: from=([-\d.]+),([-\d.]+) to=([-\d.]+),([-\d.]+) z=([-\d.]+) model=(-?\d+) "
                   r"speed=([\d.]+)/([\d.]+) cam=([\d.]+)/([\d.]+)/([\d.]+) slow=(\d+)/(\d+)")
STEPS_PER_S = 30.0
PMU = re.compile(r"(\S+)=([-\d.]+)/([-\d.]+)/([-\d.]+)/([-\d.]+)")

# Shown beside the rows whose place in the tree does not tell what they hold.
NOTES = {
    "frame": "wall time between frames",
    "idle": "sleeps until the next step or frame",
    "loop.next": "the frame loop's pacing, idle included",
    "log": "this log's lines, and its writes to the SD card",
    "wait.vsync": "the top screen's refresh",
    "wait.bottom": "then the bottom screen's",
    "wait.gpu": "C3D_FrameBegin's wait for the GPU",
    "gpu": "C3D_GetDrawingTime, previous frame",
    "c3d.cpu": "C3D_GetProcessingTime, previous frame",
    "tex.convert": "palette to RGBA, outside the parts",
    "tex.upload": "tiling and mip levels, outside the parts",
    "collide": "sprite grid collisions, inside cars and peds",
    "prim": "1 in N timed, inside the draw.* parts",
    "flush": "inside prim.c3d and present",
    "prim.uv": "gbh_DrawTile's UV turns and flips",
    "prim.turn": "the view's turn of the corners",
    "prim.clip": "the window test, soft shadows",
    "prim.resolve": "the texture's lookup",
    "prim.setup": "fit to texture, depth, diffuse",
    "prim.verts": "render state, RVertex and triangles",
    "cars.car.parked": "no physics, the visit only",
    "cars.car.driven": "AI driver, its ai and physics parts aside",
    "cars.car.moving": "physics without an AI driver",
    "cars.car.other": "trailers and trains",
    "cars.car.grid": "out of and back into the sprite grid",
    "cars.car.despawn": "despawn status, off-screen timer, wreck",
    "cars.car.doors": "doors, engine, lights, horn",
    "cars.car.misc": "crushing, attachments, drowning, hitch, taxi",
    "cars.car.physics.move": "movement against the map and sprites",
    "peds.ped.idle": "inactive: lifetime only",
    "peds.ped.car": "in a car",
    "peds.ped.down": "dead or immobilised",
    "peds.ped.move": "the Char's state and movement",
    "peds.ped.map": "the block under the ped",
    "draw.map.cache": "GTA2_3DS_MAPCACHE: per layer, before and after the walk",
    "draw.map.cache.check": "tile mappings and texture versions",
    "draw.map.cache.light": "the lights past the shader's",
    "draw.map.cache.draw.switch": "to the mesh program, its upload",
    "draw.map.cache.draw.back": "to the game's program, its upload",
    "mc.drawn": "chunk layers the meshes draw",
    "mc.cpulit": "chunk layers left to the game for the lights",
    "mc.farrows": "chunk rows left to the game for a light",
    "mc.quads": "lids the meshes draw, off screen columns included",
    "mc.lids": "lids the walk left to the meshes",
    "mc.skipped": "cells left out of the walk",
    "map.lookups": "map column reads, the draw's included",
    "grid.cells": "sprite grid cells walked to reach a sprite's",
}

AB = False
EVENTS = {0x0: "i miss", 0x1: "i stall", 0x2: "d stall", 0x7: "mispred", 0x8: "insts", 0xB: "d rmiss",
          0xD: "d wmiss", 0x10: "tlb miss"}


def parse(path, keep_first, writes, slow):
    blocks = []
    block = None
    skip = False
    pending = []
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("prof slow:"):
                pending.append(line[len("prof slow: "):])
                continue
            if line.startswith("prof: every"):
                skip = not keep_first
                continue
            m = re.match(r"prof: (\d+) frames, (\d+) steps", line)
            if m:
                timers = re.search(r"\(([\d.]+) ms\)", line)
                write = re.search(r"last log write (\d+) bytes in ([\d.]+) ms", line)
                if write and int(write.group(1)):
                    writes.append((int(write.group(1)), float(write.group(2))))
                options = re.search(r"options (\d+)", line)
                mapcache = re.search(r"mapcache (\d)", line)
                soundcore1 = re.search(r"soundcore1 (\d)", line)
                block = {"frames": int(m.group(1)), "steps": int(m.group(2)), "step": {}, "frame": {}, "count": {},
                         "timers": float(timers.group(1)) if timers else 0.0, "pmu": {},
                         "options": int(options.group(1)) if options else None,
                         "mapcache": int(mapcache.group(1)) if mapcache else None,
                         "soundcore1": int(soundcore1.group(1)) if soundcore1 else None, "place": None}
                slow.extend((len(blocks), l) for l in pending)
                pending = []
                continue
            if block is None:
                continue
            m = PLACE.match(line)
            if m:
                v = [float(x) for x in m.groups()]
                block["place"] = {"x": v[2], "y": v[3], "model": int(v[5]), "speed": v[6] * STEPS_PER_S,
                                  "maxspeed": v[7] * STEPS_PER_S, "cam": v[8], "cammax": v[10], "slow": int(v[11])}
                continue
            m = re.match(r"prof pmu step (\d+),(\d+):", line)
            if m:
                ev = (int(m.group(1)), int(m.group(2)))
                for v in PMU.finditer(line):
                    a, b, sa, sb = (float(x) for x in v.groups()[1:])
                    block["pmu"].setdefault(v.group(1), {})
                    block["pmu"][v.group(1)][ev[0]] = (a, sa)
                    block["pmu"][v.group(1)][ev[1]] = (b, sb)
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
    g = "%s %s" % (d, "night" if lit > 0 else "day")
    if AB and b["options"] is not None:
        g += ", options %d" % b["options"]
    if AB and b["mapcache"] is not None:
        g += ", mapcache %s" % ("on" if b["mapcache"] else "off")
    if AB and b["soundcore1"] is not None:
        g += ", soundcore1 %s" % ("on" if b["soundcore1"] else "off")
    return g


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


def pmu_table(blocks):
    events = sorted({e for b in blocks for p in b["pmu"].values() for e in p})
    if not events:
        return
    names = []
    for b in blocks:
        for n in b["step"]:
            if n not in names:
                names.append(n)
    print("\nevents per step (logic parts), total / without the parts under it")
    print("  %-26s" % "part" + "".join(" %17s" % EVENTS.get(e, hex(e)) for e in events))
    for n in names:
        cells = []
        for e in events:
            have = [b["pmu"][n][e] for b in blocks if n in b["pmu"] and e in b["pmu"][n]]
            if have:
                cells.append(" %8.0f/%8.0f" % (sum(h[0] for h in have) / len(have), sum(h[1] for h in have) / len(have)))
            else:
                cells.append(" %17s" % "-")
        print("  %-26s" % ("  " * depth(n, names) + n) + "".join(cells))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--keep-first", action="store_true", help="keep the first block after a game start")
    ap.add_argument("--blocks", action="store_true", help="list each block too")
    ap.add_argument("--slow", action="store_true", help="list the slow frames the log has a line for")
    ap.add_argument("--options", action="store_true", help="group the blocks by their logic options, map cache state and sound pass core too")
    args = ap.parse_args()
    global AB
    AB = args.options
    writes = []
    slow = []
    blocks = parse(args.log, args.keep_first, writes, slow)
    if not blocks:
        sys.exit("no GTA2_3DS_PROF blocks in %s" % args.log)
    for line in open(args.log, errors="replace"):
        if line.startswith("prof: every"):
            print(line.strip())
            break
    if writes:
        print("log writes: %s" % ", ".join("%d bytes in %.1f ms" % w for w in sorted(set(writes))))
    g = lambda d, k: d.get(k, [0])[0]
    if args.blocks:
        cols = ["step", "draw", "draw.map", "prim", "prim.light", "pump.pad", "wait.vsync", "wait.gpu", "gpu"]
        print("\n  %3s %-12s %5s %5s %6s" % ("#", "group", "steps", "fps", "max") + "".join(" %10s" % c for c in cols) +
              "  %13s %5s %5s %4s %4s" % ("at", "model", "speed", "cam", "slow"))
        for i, b in enumerate(blocks):
            st, fr, c = b["step"], b["frame"], b["count"]
            fps = 1000.0 / g(fr, "frame") if g(fr, "frame") else 0
            vals = [g(st, k) if k in st else g(fr, k) for k in cols]
            p = b["place"]
            where = "  %6.1f,%6.1f %5s %5.1f %4.1f %4d" % (p["x"], p["y"], p["model"] if p["model"] >= 0 else "-", p["speed"],
                                                       p["cam"], p["slow"]) if p else ""
            print("  %3d %-12s %5.2f %5.1f %6.1f" % (i, group_of(b), g(c, "steps"), fps, fr.get("frame", [0, 0])[1]) +
                  "".join(" %10.2f" % v for v in vals) + where)
    if args.slow:
        print("\nslow frames (block #: ms, steps, place, model, blocks per step, camera height: parts in ms, counts)")
        for i, line in slow:
            print("  %3d %s" % (i, line))
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
                  ("wait.vsync", avg("frame", "wait.vsync")), ("wait.bottom", avg("frame", "wait.bottom")), ("wait.gpu", avg("frame", "wait.gpu")),
                  ("tex", avg("frame", "tex.convert") + avg("frame", "tex.upload")), ("pump", avg("frame", "pump")),
                  ("inputs", avg("frame", "inputs"))]
        if any("loop.next" in b["frame"] for b in bs):
            # loop.next holds the idle sleeps.
            budget += [(k[5:], avg("frame", k)) for k in ("loop.next", "loop.beforestep", "loop.afterstep",
                                                           "loop.begindraw", "loop.enddraw", "loop.fps")]
            budget += [("log", avg("frame", "log"))]
        else:
            budget += [("idle", avg("frame", "idle"))]
        budget += [("timers", sum(b["timers"] for b in bs) / len(bs))]
        rest = avg("frame", "frame") - sum(v for _, v in budget)
        print("frame %.2f ms = %s + rest %.2f" % (avg("frame", "frame"), " + ".join("%s %.2f" % x for x in budget), rest))
        table("logic, per step", bs, "step")
        pmu_table(bs)
        table("frame, per frame", bs, "frame")
        table("counts (step ones per step, the others per frame)", bs, "count")


if __name__ == "__main__":
    main()
