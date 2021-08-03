#!/usr/bin/env python3

import sys
import json
from collections import defaultdict

stat_type = ''
fileName1 = ''
fileName2 = ''
argv = sys.argv[1:]

if len(argv) < 3:
    print('Usage: stat.py stat_type fileName1 fileName2')
    sys.exit()

stat_type = argv[0]
fileName1 = argv[1]
fileName2 = argv[2]

file1 = open(fileName1)
json_data1 = file1.read()
data1 = json.loads(json_data1)

file2 = open(fileName2)
json_data2 = file2.read()
data2 = json.loads(json_data2)


def diff_group_stats(groups1, groups2, stats, aggregated_stats):
    diff = dict()
    sum_diff = defaultdict(int)
    i = 0
    for g1 in groups1:
        if 'group_name' in g1:
            # compare between groups with same name
            g2 = next(filter(lambda x: x['group_name'] == g1['group_name'], groups2), None)
            if g2 is None:
                print('Error: can not find matched group_name:', g1['group_name'])
                sys.exit()
        else:
            g2 = groups2[i]
        for s in stats:
            diff[s] = g2[s] - g1[s]
            sum_diff[s] += diff[s]
        print("\ngroup : ", i)
        i += 1
        print_stats(diff)
        res = aggregate_stats(diff, aggregated_stats)
        print_stats(res)
    return sum_diff


def aggregate_stats(data, aggregated_stats):
    out = dict()
    for s, x, f in aggregated_stats:
        m = map(lambda y: data[y], x)
        out[s] = f(list(m))
    return out


def print_stats(data):
    for (k, v) in data.items():
        print(k, ": ", v)


def handle_stats(groups1, groups2, stats, aggregated_stats):
    res = diff_group_stats(groups1, groups2, stats, aggregated_stats)
    print("\n")
    print("sum results:")
    print_stats(res)
    res2 = aggregate_stats(res, aggregated_stats)
    print_stats(res2)


if stat_type == 'sock_posix':
    posix_stats = ["succ_flushs", "total_flushs", "flushed_iovcnt", "sent_bytes", "writev_async", "readv",
                   "readv_bytes", "poll_events", "busy_polls", "total_polls", "recv_from_pipe", "recv_from_pipe_bytes"]
    posix_aggregated_stats = [("idle_polls", ("total_polls", "busy_polls"), lambda x: x[0] - x[1]),
                              ("busy_polls_percent(%)", ("busy_polls", "total_polls"),
                               lambda x: 100 * x[0] / x[1] if x[1] > 0 else 0),
                              ("succ_flushs/total_flushs(%)", ("succ_flushs", "total_flushs"),
                               lambda x: 100 * x[0] / x[1] if x[1] > 0 else 0),
                              ("flushed_iovcnt/succ_flushs", ("flushed_iovcnt", "succ_flushs"),
                               lambda x: x[0] / x[1] if x[1] > 0 else 0),
                              ("sent_bytes/succ_flushs", ("sent_bytes", "succ_flushs"),
                               lambda x: x[0] / x[1] if x[1] > 0 else 0),
                              ("poll_events/busy_polls", ("poll_events", "busy_polls"),
                               lambda x: x[0] / x[1] if x[1] > 0 else 0),
                              ("recv_from_pipe_bytes/recv_from_pipe", ("recv_from_pipe_bytes", "recv_from_pipe"),
                               lambda x: x[0] / x[1] if x[1] > 0 else 0),
                              ("readv_bytes/readv", ("readv_bytes", "readv"), lambda x: x[0] / x[1] if x[1] > 0 else 0),
                              ]
    groups1 = data1['sock_groups']
    groups2 = data2['sock_groups']
    handle_stats(groups1, groups2, posix_stats, posix_aggregated_stats)
elif stat_type == 'sock_vma':
    vma_stats = ["succ_flushs", "total_flushs", "flushed_iovcnt", "sent_bytes", "submitted_requests",
                 "total_recv_zcopy", "succ_recv_zcopy", "recv_zcopy_unfinished", "recv_zcopy_packets",
                 "recv_zcopy_iovs", "recv_zcopy_bytes", "poll_events", "busy_polls", "total_polls"]
    vma_aggregated_stats = [("idle_polls", ("total_polls", "busy_polls"), lambda x: x[0] - x[1]),
                            ("succ_flushs/total_flushs(%)", ("succ_flushs", "total_flushs"),
                             lambda x: 100 * x[0] / x[1] if x[1] > 0 else 0),
                            ("flushed_iovcnt/succ_flushs", ("flushed_iovcnt", "succ_flushs"),
                             lambda x: x[0] / x[1] if x[1] > 0 else 0),
                            ("sent_bytes/succ_flushs", ("sent_bytes", "succ_flushs"),
                             lambda x: x[0] / x[1] if x[1] > 0 else 0),
                            ("poll_events/busy_polls", ("poll_events", "busy_polls"),
                             lambda x: x[0] / x[1] if x[1] > 0 else 0),
                            ("busy_polls_percent(%)", ("busy_polls", "total_polls"),
                             lambda x: 100 * x[0] / x[1] if x[1] > 0 else 0),
                            ("sent_bytes/submitted_requests", ("sent_bytes", "submitted_requests"),
                             lambda x: x[0] / x[1] if x[1] > 0 else 0),
                            ("succ_recv_zcopy/total_recv_zcopy(%)", ("succ_recv_zcopy", "total_recv_zcopy"),
                             lambda x: 100 * x[0] / x[1] if x[1] > 0 else 0),
                            ("recv_zcopy_unfinished/succ_recv_zcopy", ("recv_zcopy_unfinished", "succ_recv_zcopy"),
                             lambda x: x[0] / x[1] if x[1] > 0 else 0),
                            ("recv_zcopy_bytes/succ_recv_zcopy", ("recv_zcopy_bytes", "succ_recv_zcopy"),
                             lambda x: x[0] / x[1] if x[1] > 0 else 0),
                            ("recv_zcopy_iovs/succ_recv_zcopy", ("recv_zcopy_iovs", "succ_recv_zcopy"),
                             lambda x: x[0] / x[1] if x[1] > 0 else 0),
                            ("recv_zcopy_iovs/recv_zcopy_packets", ("recv_zcopy_iovs", "recv_zcopy_packets"),
                             lambda x: x[0] / x[1] if x[1] > 0 else 0)]
    groups1 = data1['sock_groups']
    groups2 = data2['sock_groups']
    handle_stats(groups1, groups2, vma_stats, vma_aggregated_stats)
elif stat_type == 'nvme':
    tsc_rate = data1['tick_rate']
    ticks_to_us = 1000 * 1000 / tsc_rate
    print(ticks_to_us)
    initiator_stats = ["polls", "sock_completions", "nvme_completions", "num_sock_idle_completion",
                       "total_sock_idle_tsc", "num_nvme_idle_completion", "total_nvme_idle_tsc", "total_poll_tsc"]
    initiator_aggregated_stats = [("sock_busy_tsc", ("total_poll_tsc", "total_sock_idle_tsc"), lambda x: x[0] - x[1]),
                                  ("nvme_busy_tsc", ("total_poll_tsc", "total_nvme_idle_tsc"), lambda x: x[0] - x[1]),
                                  ("sock_completions/polls(%)", ("sock_completions", "polls"),
                                   lambda x: 100 * x[0] / x[1] if x[1] > 0 else 0),
                                  ("nvme_completions/polls(%)", ("nvme_completions", "polls"),
                                   lambda x: 100 * x[0] / x[1] if x[1] > 0 else 0),
                                  ("sock_idle_tsc/sock_idle_completions (ticks)", ("total_sock_idle_tsc",
                                   "num_sock_idle_completion"), lambda x: x[0] / x[1] if x[1] > 0 else 0),
                                  ("sock_idle_tsc/sock_idle_completions (us)", ("total_sock_idle_tsc",
                                   "num_sock_idle_completion"), lambda x: x[0] / x[1] * ticks_to_us if x[1] > 0 else 0),
                                  ("nvme_idle_tsc/nvme_idle_completions (ticks)", ("total_nvme_idle_tsc",
                                   "num_nvme_idle_completion"), lambda x: x[0] / x[1] if x[1] > 0 else 0),
                                  ("nvme_idle_tsc/nvme_idle_completions (us)", ("total_nvme_idle_tsc",
                                   "num_nvme_idle_completion"), lambda x: x[0] / x[1] * ticks_to_us if x[1] > 0 else 0),
                                  ("sock_busy_tsc/sock_completions (ticks)", ("total_poll_tsc", "total_sock_idle_tsc",
                                   "sock_completions"), lambda x: (x[0] - x[1]) / x[2] if x[2] > 0 else 0),
                                  ("sock_busy_tsc/sock_completions (us)", ("total_poll_tsc", "total_sock_idle_tsc",
                                   "sock_completions"), lambda x: (x[0] - x[1]) / x[2] * ticks_to_us if x[2] > 0 else 0),
                                  ("nvme_busy_tsc/nvme_completions (ticks)", ("total_poll_tsc", "total_nvme_idle_tsc",
                                   "nvme_completions"), lambda x: (x[0] - x[1]) / x[2] if x[2] > 0 else 0),
                                  ("nvme_busy_tsc/nvme_completions (us)", ("total_poll_tsc", "total_nvme_idle_tsc",
                                   "nvme_completions"), lambda x: (x[0] - x[1]) / x[2] * ticks_to_us if x[2] > 0 else 0)]

    groups1 = list(map(lambda pg: {**pg['transports'][0], 'group_name': pg['thread']}, data1['poll_groups']))
    groups2 = list(map(lambda pg: {**pg['transports'][0], 'group_name': pg['thread']}, data2['poll_groups']))
    handle_stats(groups1, groups2, initiator_stats, initiator_aggregated_stats)
else:
    print('Error: not support stat_type:', stat_type)
