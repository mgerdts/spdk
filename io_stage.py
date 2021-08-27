#!/usr/bin/env python3

import sys
import json

files = sys.argv[1:]

if len(files) == 0:
    print('Usage: io_stage.py file1 [file2...]')
    exit(0)

def file2data(filename):
    with open(filename) as f:
        return json.load(f)

data = [ file2data(f) for f in files ]
counts = [ core['counts'] for slice in data for core in slice['cores']]
stage_counts = { stage: [c[stage] for c in counts] for stage in counts[0].keys()}
num_counts = len(counts)
avg = { k: sum(v)/num_counts for k,v in stage_counts.items() }
print(avg)
