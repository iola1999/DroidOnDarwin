#!/usr/bin/env python3
"""将 macOS vCPU 采样中的 TCG 地址还原为客体 PC，排除采样期间变化的映射。"""
import argparse
import bisect
from collections import Counter
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 30:
        parser.error("seconds must be 1..30")
    args.output.mkdir(parents=True, exist_ok=False)
    perfmap = Path(f"/tmp/perf-{args.pid}.map")
    before_size = perfmap.stat().st_size
    sample_path = args.output / "host-sample.txt"
    subprocess.run(["sample", str(args.pid), str(args.seconds), "5", "-file", str(sample_path)],
                   check=True, capture_output=True, timeout=args.seconds + 15)
    after_size = perfmap.stat().st_size
    sections = re.split(r"(?=^    \d+ Thread_)", sample_path.read_text(), flags=re.MULTILINE)
    threads = [s for s in sections if re.search(r" (?:rr|mttcg)_cpu_thread_fn ", s)]
    if len(threads) != 1:
        raise RuntimeError("This sampler requires exactly one TCG vCPU thread")
    thread = threads[0].split("Total number in stack", 1)[0]
    counts = Counter()
    for count, address in re.findall(r"^[ +!:|]*(\d+) \?\?\? .*?\[0x([0-9a-f]+)\]", thread, re.MULTILINE):
        counts[int(address, 16)] += int(count)
    addresses = sorted(counts)
    before, after = {}, {}
    consumed = 0
    with perfmap.open("rb") as stream:
        while consumed < after_size:
            line = stream.readline()
            consumed += len(line)
            if not line or consumed > after_size or not line.endswith(b"\n"):
                break
            fields = line.decode().split()
            if len(fields) != 3 or not fields[2].startswith("guest-0x"):
                continue
            start, length = int(fields[0], 16), int(fields[1], 16)
            guest = int(fields[2][6:], 16)
            for address in addresses[bisect.bisect_left(addresses, start):bisect.bisect_left(addresses, start + length)]:
                after[address] = guest
                if consumed <= before_size:
                    before[address] = guest
    guests = Counter()
    unstable = unmapped = 0
    for address, count in counts.items():
        if address not in before or address not in after:
            unmapped += count
        elif before[address] != after[address]:
            unstable += count
        else:
            guests[before[address]] += count
    result = {"pid": args.pid, "seconds": args.seconds,
              "vcpu_samples": int(re.match(r"\s+(\d+) Thread_", thread)[1]),
              "jit_samples": sum(counts.values()), "mapped_samples": sum(guests.values()),
              "unmapped_samples": unmapped, "changed_mapping_samples": unstable,
              "perfmap_bytes_before": before_size, "perfmap_bytes_after": after_size,
              "guest_pcs": {f"0x{pc:x}": count for pc, count in guests.most_common()}}
    (args.output / "guest-pcs.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "guest_pcs"}))


if __name__ == "__main__":
    main()
