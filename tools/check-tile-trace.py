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
group_counts = {"source-1d-tail": 5, "source-2d-tails": 20,
                "source-3d-tails": 36, "source-3d-ready": 36}
for index in range(1, len(blocks), 2):
    name, body = blocks[index:index+2]
    tiles = re.findall(r"NDRANGE_TILE_SUBMIT command=(\S+) event=(\S+) tile=(\d+) budget=(\d+) first=(\d+) last=(\d+)", body)
    active = a.groups > 0 and group_counts.get(name, 0) > a.groups
    if not active:
        assert not tiles, f"{name}: disabled/fitting/uniform/imported path was tiled"
        if a.groups and name not in group_counts:
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

batch_blocks = re.findall(r"^BATCH_CASE (\S+)[^\n]*\n(.*?)^BATCH_PASS \1 groups=(\d+) commands=(\d+)$", data, re.M | re.S)
assert len(batch_blocks) == 3, "under/exact/over-budget ready batch controls required"
shared = 0
for name, body, groups, commands in batch_blocks:
    active = a.groups > 0 and int(groups) > a.groups
    tiles = re.findall(r"NDRANGE_TILE_SUBMIT command=(\S+) event=(\S+) tile=(\d+) budget=(\d+) first=(\d+) last=(\d+)", body)
    records = re.findall(r"DISPATCH_RECORD id=(\d+) ordinal=\d+ command=(\S+) event=(\S+) .*name=batch_order .*region_gws=\{(\d+),(\d+),(\d+)\} region_lws=\{(\d+),(\d+),(\d+)\}", body)
    by_submit = {}
    by_command = {}
    for record in records:
        submit, command, event = record[:3]
        by_submit.setdefault(submit, set()).add((command, event))
        by_command.setdefault((command, event), []).append(submit)
        if active:
            sizes = tuple(map(int, record[3:]))
            product = 1
            for g, l in zip(sizes[:3], sizes[3:]):
                assert l and g % l == 0
                product *= g // l
            assert product <= a.groups, f"{name}: over-budget tile"
    assert len(by_command) == int(commands), f"{name}: missing warmup/ready commands"
    for submit in by_submit:
        assert re.search(r"DISPATCH_SUBMIT_RETURN id=" + submit + r" .*result=0", body)
        assert re.search(r"DISPATCH_WAIT_RETURN id=" + submit + r" .*result=0", body)
    events = re.findall(r"^BATCH_EVENT " + re.escape(name) + r" ordinal=(\d+) event=(\S+)", body, re.M)
    assert len(events) == int(commands) - 1 and len({row[1] for row in events}) == len(events)
    # The trace holds cvk_event_command*, whereas OpenCL returns its _cl_event
    # base subobject. Multiple inheritance adjusts the public pointer; comparing
    # their raw addresses would reject valid executions (and is ABI-dependent).
    assert len({event for _, event in by_command}) == int(commands)
    assert [int(row[0]) for row in events] == list(range(1, int(commands)))
    if active:
        assert len(tiles) == len(records) == len(by_submit), f"{name}: independent tiles required"
        for command, submissions in by_command.items():
            own = [row for row in tiles if row[:2] == command]
            assert len(own) == len(submissions) > 1
            for i, row in enumerate(own, 1):
                assert tuple(map(int, row[2:])) == (i, a.groups, int(i == 1), int(i == len(own)))
        assert "BATCH_DURATION " not in body, f"{name}: tiled samples must not train batch cost"
        total += len(tiles)
    else:
        assert not tiles, f"{name}: whole NDRange fits budget"
        assert all(len(set(ids)) == 1 for ids in by_command.values()), f"{name}: one ordinary submission per command"
        multiple = [submit for submit, cmds in by_submit.items() if len(cmds) > 1]
        assert multiple, f"{name}: ready commands did not actually share a submission"
        for submit in multiple:
            count = len(by_submit[submit])
            assert re.search(r"BATCH_DURATION id=" + submit + r" .*commands=" + str(count) + r" .*api_success=1 eligible=1 homogeneous=1", body), f"{name}: shared duration training missing"
        shared += len(multiple)
print(f"PASS actual semantic trace: groups={a.groups}, independent retired tiles={total}, shared trained batches={shared}, public events and uniform/import bypass intact")
