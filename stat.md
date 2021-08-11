# Statistics User Guide {#stat}

# Introduction {#stat_ug_introduction}

Many statistics have been added in different layer of SPDK. The statistics can be queried by RPC commands.
This guide will explain how to get the statistics, and how to analyze it by a script.

# Socket layer {#stat_socket}

## posix {#stat_socket_posix}

POSIX sockets is the common application environment for accessing networking.

Example command

`./scripts/rpc.py sock_impl_get_stats -i posix`

Because current statistics are accumulating, i.e. never decremented, the suggested usage is to
make two measurements during the test and then check difference.

Example command

`./scripts/rpc.py sock_impl_get_stats -i posix > posix-9000-8-1.json && sleep 20 && ./scripts/rpc.py sock_impl_get_stats -i posix > posix-9000-8-2.json`

Script stat.py can help to diff two statistics with the following command:

`./stat.py sock_posix posix-9000-8-1.json posix-9000-8-2.json`

Note: The json file accumulated with more statistics must be put at the end of above command.

Example results 
~~~
succ_flushs :  3715067
total_flushs :  566206037
flushed_iovcnt :  35623816
sent_bytes :  73385060960
writev_async :  17811917
readv :  3860072
readv_bytes :  1282458168
poll_events :  3715069
busy_polls :  3715069
total_polls :  509192758
recv_from_pipe :  35623838
recv_from_pipe_bytes :  1282458168
idle_polls :  505477689
busy_polls_percent(%) :  0.729599732445527
succ_flushs/total_flushs(%) :  0.656133413851255
flushed_iovcnt/succ_flushs :  9.589010373164198
sent_bytes/succ_flushs :  19753.361368718248
poll_events/busy_polls :  1.0
recv_from_pipe_bytes/recv_from_pipe :  36.0
readv_bytes/readv :  332.236851540593
~~~


## VMA {#stat_socket_VMA}

The Mellanox Messaging Accelerator (VMA) library is a network-traffic offload, dynamically-linked user-space
Linux library which serves to transparently enhance the performance of socket-based networking-heavy
applications over an InfiniBand or Ethernet network.

Example command

`./scripts/rpc.py sock_impl_get_stats -i vma > vma.json`

Because current statistics are accumulating, i.e. never decremented, the suggested usage is to
make two measurements during the test and then check difference.

Example command

`./scripts/rpc.py sock_impl_get_stats -i vma > vma-9000-8-1.json && sleep 20 && sudo ./scripts/rpc.py sock_impl_get_stats -i vma > vma-9000-8-2.json`

Script stat.py can help to diff two statistics with the following command

`./stat.py sock_vma vma-9000-8-1.json vma-9000-8-2.json`

Note: The json file accumulated with more statistics must be put at the end of above command.

Example results 
~~~
succ_flushs :  4248649
total_flushs :  53027205
flushed_iovcnt :  18220786
sent_bytes :  1311896016
submitted_requests :  18220784
total_recv_zcopy :  7602271
succ_recv_zcopy :  4172324
recv_zcopy_unfinished :  482076
recv_zcopy_packets :  4174128
recv_zcopy_iovs :  9862245
recv_zcopy_bytes :  75069605360
poll_events :  3912026
busy_polls :  3912026
total_polls :  53027199
idle_polls :  49115173
succ_flushs/total_flushs(%) :  8.01220618737118
flushed_iovcnt/succ_flushs :  4.288607037201708
sent_bytes/succ_flushs :  308.7795711060151
poll_events/busy_polls :  1.0
busy_polls_percent(%) :  7.377395136409147
sent_bytes/submitted_requests :  71.99997629081163
succ_recv_zcopy/total_recv_zcopy(%) :  54.88260021248914
recv_zcopy_unfinished/succ_recv_zcopy :  0.11554136255957112
recv_zcopy_bytes/succ_recv_zcopy :  17992.276093611137
recv_zcopy_iovs/succ_recv_zcopy :  2.363729422738982
recv_zcopy_iovs/recv_zcopy_packets :  2.3627078517956326
~~~

# nvme layer {#stat_nvme}

## nvme tcp on initiator {#stat_nvme_tcp}

NVMe/TCP enables efficient end-to-end NVMe operations between NVMe-oF host(s) and NVMe-oF controller devices interconnected by
standard IP network with excellent performance and latency characteristics.

Example command

`./scripts/rpc.py bdev_nvme_get_transport_statistics`

Because current statistics are accumulating, i.e. never decremented, the suggested usage is to
make two measurements during the test and then check difference.

Example command

`./scripts/rpc.py bdev_nvme_get_transport_statistics > initiator-9000-8-1.json && sleep 20 && ./scripts/rpc.py bdev_nvme_get_transport_statistics > initiator-9000-8-2.json`

Script stat.py can help to diff two statistics with the following command

`./stat.py nvme initiator-9000-8-1.json initiator-9000-8-2.json`

Note: The json file accumulated with more statistics must be put at the end of above command.

Example results 
~~~
sock_completions :  2151409
nvme_completions :  25429359
num_nvme_idle_completion :  6567330
total_nvme_idle_tsc :  1990277898
total_poll_tsc :  21099372015
nvme_busy_tsc :  19109094117
sock_completions/polls(%) :  24.67568991341523
nvme_completions/polls(%) :  291.663266901326
nvme_idle_tsc/nvme_idle_completions (ticks) :  303.05739135995907
nvme_idle_tsc/nvme_idle_completions (us) :  1.5152869567997953
nvme_busy_tsc/nvme_completions (ticks) :  751.457955231982
nvme_busy_tsc/nvme_completions (us) :  3.75728977615991
~~~

