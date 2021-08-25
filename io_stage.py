#!/usr/bin/env python3

import sys
import json
from collections import defaultdict

argv = sys.argv[1:]
#print(argv)


def print_avg_counts(data, group_num):
    for (k, v) in data.items():
        print(k, ": ", v / group_num)


def sum_group(sum_counts, group_nums, groups, stages):
    #skip counts in the first core which involves some non-snap IO
    #for g in groups:
    for g in groups[1:]:
        group_nums += 1
        for s in stages:
            sum_counts[s] += g[s]
    return group_nums



stages = ["NO_STAGE", "PROCESS_SQE", "SOCK_BATCH_QUEUE", "WAIT_FOR_TARGET", "PROCESS_C2H_PDU", "WAIT_FOR_DMA"]

sum_counts = defaultdict(int)
group_num = 0

for fileName in argv :
    #print(fileName)
    with open(fileName) as f:
        data = json.load(f)
        groups = list(map(lambda pg: {**pg['counts']}, data['cores']))
        group_num = sum_group(sum_counts, group_num, groups, stages)


print_avg_counts(sum_counts, group_num)
