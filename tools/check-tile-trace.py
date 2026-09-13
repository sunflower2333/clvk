#!/usr/bin/env python3
"""Check actual runtime submit/retirement attribution from the semantic control."""
import argparse
import re
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("log", type=Path)
p.add_argument("--groups", type=int, required=True)
a = p.parse_args()
data = a.log.read_text(errors="replace")
assert re.search(r"PASS \d+ actual compiled-kernel", data), "semantic control did not pass"
assert "EMPTY_PASS zero global size" in data, "empty NDRange no-work regression required"
assert len(re.findall(r"^CASE_PASS ", data, re.M)) == 7, "seven actual semantic cases required"
blocks = re.split(r"^CASE (\S+)[^\n]*\n", data, flags=re.M)
total = 0
for index in range(1, len(blocks), 2):
    name, body = blocks[index:index+2]
    tiles = re.findall(r"NDRANGE_TILE_SUBMIT command=(\S+) event=(\S+) tile=(\d+) budget=(\d+) first=(\d+) last=(\d+)", body)
    active = a.groups > 0 and name.startswith("source-")
    if not active:
        assert not tiles, f"{name}: disabled/uniform/imported path was tiled"
        if a.groups:
            assert "reason=unproven_region_abi" in body, f"{name}: missing ABI bypass"
        continue
    assert len(tiles) > 1, f"{name}: multiple independently submitted tiles required"
    command, event = tiles[0][:2]
    for ordinal, row in enumerate(tiles, 1):
        cmd, evt, number, budget, first, last = row
        assert (cmd, evt) == (command, event), f"{name}: public command/event changed"
        assert int(number) == ordinal and int(budget) == a.groups
        assert int(first) == (ordinal == 1) and int(last) == (ordinal == len(tiles))
    records = re.findall(r"DISPATCH_RECORD id=(\d+) ordinal=\d+ command=" + re.escape(command) + r" event=" + re.escape(event) + r" .*region_gws=\{(\d+),(\d+),(\d+)\} region_lws=\{(\d+),(\d+),(\d+)\}", body)
    assert len(records) == len(tiles), f"{name}: one dispatch per tile required"
    ids = set()
    for record in records:
        submit_id = record[0]
        assert submit_id not in ids, f"{name}: tiles share one submission"
        ids.add(submit_id)
        sizes = tuple(map(int, record[1:]))
        product = 1
        for g, l in zip(sizes[:3], sizes[3:]):
            assert l and g % l == 0
            product *= g // l
        assert product <= a.groups, f"{name}: tile exceeds group budget"
        assert re.search(r"DISPATCH_SUBMIT_BEGIN id=" + submit_id + r" .*commands=1 dispatches=1", body)
        assert re.search(r"DISPATCH_SUBMIT_RETURN id=" + submit_id + r" .*result=0", body)
        assert re.search(r"DISPATCH_WAIT_RETURN id=" + submit_id + r" .*result=0", body), f"{name}: no successful retirement"
    assert len(re.findall(r"^EVENT " + re.escape(name) + r" ", body, re.M)) == 1
    total += len(tiles)
print(f"PASS actual semantic trace: groups={a.groups}, independent retired tiles={total}, one public event per case, uniform/import bypass intact")
