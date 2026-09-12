#!/usr/bin/env python3
"""Build docs/stats.json from .info files for the statistics page.

usage: tools/make_stats.py [--chesstb ./chesstb] [--info INFO_DIR] [--out docs/stats.json]

INFO_DIR follows the bucket's layout: <metric>/*.info and castling/<metric>/*.info,
where <metric> is one of dtz, dtm, dtm50, dtc.
"""
import argparse
import datetime
import glob
import json
import os
import re
import subprocess
import sys

METRICS = ["dtz", "dtm", "dtm50", "dtc"]

FRAME = re.compile(r"^\s+(WHITE|BLACK)\s+win=(\d+)\s+draw=(\d+)\s+lose=(\d+)\s+illegal=(\d+)\s+legal=(\d+)")
LONGEST = re.compile(r'^\s+longest_win=(\d+)\s+idx=\d+\s+fen="([^"]*)"')
HEADER = re.compile(r"^(\S+\.info) \(\d+ bytes\)")


def read_infos(chesstb, files):
    """Parse `chesstb --info` output into {material: {"w": frame, "b": frame}}."""
    out = {}
    for i in range(0, len(files), 500):
        text = subprocess.run([chesstb, "--info", *files[i:i + 500]],
                              capture_output=True, text=True, check=True).stdout
        material = color = None
        for line in text.splitlines():
            if m := HEADER.match(line):
                material = os.path.basename(m.group(1))[:-len(".info")]
                out[material] = {}
            elif m := FRAME.match(line):
                color = "w" if m.group(1) == "WHITE" else "b"
                win, draw, lose, _, legal = map(int, m.groups()[1:])
                out[material][color] = {"win": win, "draw": draw, "lose": lose, "legal": legal}
            elif m := LONGEST.match(line):
                out[material][color]["longest"] = int(m.group(1))
                out[material][color]["fen"] = m.group(2)
    return out


def mirror_fen(fen):
    """Color-flip a FEN: mirror the ranks, swap piece colors, side to move and castling rights."""
    parts = fen.split()
    board = "/".join(reversed(parts[0].split("/"))).swapcase()
    rest = []
    if len(parts) > 1:
        rest.append("b" if parts[1] == "w" else "w")
    if len(parts) > 2:
        rest.append(parts[2] if parts[2] == "-" else "".join(sorted(parts[2].swapcase(), key="KQkq".find)))
    return " ".join([board, *rest, *parts[3:]])


def complete(frames):
    """Fill a symmetric material's empty Black frame from the White one."""
    w, b = frames.get("w"), frames.get("b")
    if w and w["legal"] and b is not None and b["legal"] == 0:
        frames["b"] = dict(w, fen=mirror_fen(w["fen"]) if w.get("fen") else "")
    return frames


def order(chesstb, names, castling):
    """Canonical generator order; castling tables follow their right-free counterparts."""
    enum = subprocess.run([chesstb, "--enumerate", "7"], capture_output=True, text=True).stdout.split()
    rank = {n: i for i, n in enumerate(enum)}

    def key(name):
        base = name.replace("r", "R") if castling else name
        return (len(name), rank.get(base, len(rank)), name)
    return sorted(names, key=key)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chesstb", default="./chesstb")
    ap.add_argument("--info", default="info")
    ap.add_argument("--out", default="docs/stats.json")
    args = ap.parse_args()

    sets = []
    for castling in (False, True):
        root = os.path.join(args.info, "castling") if castling else args.info
        per_metric = {}
        for metric in METRICS:
            files = sorted(glob.glob(os.path.join(root, metric, "*.info")))
            if files:
                per_metric[metric] = {k: complete(v) for k, v in read_infos(args.chesstb, files).items()}
        names = set().union(*(d.keys() for d in per_metric.values())) if per_metric else set()
        rows = []
        for name in order(args.chesstb, names, castling):
            # Every metric's .info carries the same clock-free W/D/L counts; take the first present.
            src = next(per_metric[m][name] for m in METRICS if m in per_metric and name in per_metric[m])
            w, b = src["w"], src["b"]
            row = {
                "m": name,
                "n": len(name),
                # Side-to-move counts: [win, draw, lose, legal] for White to move, then Black to move.
                "c": [w["win"], w["draw"], w["lose"], w["legal"], b["win"], b["draw"], b["lose"], b["legal"]],
                "l": {},
            }
            for metric in METRICS:
                frames = per_metric.get(metric, {}).get(name)
                if frames:
                    row["l"][metric] = [[frames[c].get("longest", 0), frames[c].get("fen", "")] for c in ("w", "b")]
            rows.append(row)
        sets.append({"castling": castling, "metrics": [m for m in METRICS if m in per_metric], "rows": rows})

    data = {"generated": datetime.date.today().isoformat(), "sets": sets}
    with open(args.out, "w") as f:
        json.dump(data, f, separators=(",", ":"))
    total = sum(len(s["rows"]) for s in sets)
    print(f"wrote {args.out}: {total} materials", file=sys.stderr)


if __name__ == "__main__":
    main()
