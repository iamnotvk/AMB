#!/usr/bin/env python3

import argparse
import pathlib


def generate_edges(vertex_count: int, fanout: int):
    for src in range(vertex_count):
        # A few deterministic hub destinations concentrate updates and create contention.
        for hop in range(1, fanout + 1):
            dst = (src + hop * hop + 7 * hop) % vertex_count
            if dst != src:
                yield src, dst

        hub_window = max(8, vertex_count // 64)
        for hub in range(min(hub_window, fanout)):
            dst = (hub * 17 + src // max(1, vertex_count // 256)) % vertex_count
            if dst != src:
                yield src, dst


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--vertices", type=int, default=4096)
    parser.add_argument("--fanout", type=int, default=12)
    args = parser.parse_args()

    output_path = pathlib.Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    seen = set()
    with output_path.open("w", encoding="utf-8") as handle:
        handle.write("# Deterministic power-law style benchmark graph\n")
        for src, dst in generate_edges(args.vertices, args.fanout):
            if (src, dst) in seen:
                continue
            seen.add((src, dst))
            handle.write(f"{src} {dst}\n")


if __name__ == "__main__":
    main()
