#!/usr/bin/env bash

if [ -n "$TRACE" ]; then
    set -x
fi

TGT_HOST=$HOSTNAME
#TGT_SSH=r-dcs79
TGT_ADDR=1.1.81.1
TGT_PORT=4420
TGT_BIN_PATH=$PWD/install-$TGT_HOST/bin
TGT_SPDK_PATH=$PWD

PERF_HOST=$HOSTNAME
#PERF_SSH=ubunut@snic
PERF_BIN_PATH=$PWD/install-$PERF_HOST/bin
PERF_SPDK_PATH=$PWD


TGT_TRTYPE=rdma
INIT_TRTYPE=nvda_rdma
SOCK_IMPL=posix
TGT_MASK=0x01
PERF_TIME=${PERF_TIME:-5}
PERF_MASKS=0x10
QUEUE_DEPTHS="16"
IO_SIZES="4096 8192 16384 32768 65536 131072"
IO_SIZES="16384"
RW="randrw -M 50"
REPEAT=${REPEAT:-1}

#TGT_TRANSPORT_EXTRA_OPTS="-c 4096"
#NVME_PERF_EXTRA_OPTS="--io-queue-size 1024"
#NVME_PERF_EXTRA_OPTS="-T vma -T nvme"
#BDEV_PERF_EXTRA_OPTS="-L nvme"
#SSH_EXTRA_OPTS="-t"
#SOCK_EXTRA_OPTS="--disable-recv-pipe"

function rpc_tgt() {
    local SSH=""
    if [ -n "$TGT_SSH" ]; then
	SSH="ssh $SSH_EXTRA_OPTS $TGT_SSH"
    fi
    $SSH sudo $TGT_SPDK_PATH/scripts/rpc.py $@ >> rpc_tgt.log 2>&1
}

function rpc_perf() {
    local SSH=""
    if [ -n "$PERF_SSH" ]; then
	SSH="ssh $SSH_EXTRA_OPTS $PERF_SSH"
    fi
    $SSH sudo $PERF_SPDK_PATH/scripts/rpc.py -s /var/tmp/bdevperf.sock $@ 2>&1 | tee -a perf.log
}

function start_tgt() {
    local SSH=""
    if [ -n "$TGT_SSH" ]; then
	SSH="ssh $SSH_EXTRA_OPTS $TGT_SSH"
    fi
    $SSH sudo SPDK_NVMF_SQE_MODE=1 $TGT_BIN_PATH/spdk_tgt -m $TGT_MASK 2>&1 | tee tgt.log > /dev/null &
    sleep 7
}

function stop_tgt() {
    rpc_tgt spdk_kill_instance 15
    sleep 3
}

function config() {
    # rpc_tgt nvmf_create_transport -t $TGT_TRTYPE -q 512 -a 16
    rpc_tgt nvmf_create_transport -t $TGT_TRTYPE $TGT_TRANSPORT_EXTRA_OPTS
    rpc_tgt nvmf_create_subsystem -a nqn.2016-06.io.spdk:cnode1
    rpc_tgt nvmf_subsystem_add_listener -t $TGT_TRTYPE -a $TGT_ADDR -f ipv4 -s $TGT_PORT nqn.2016-06.io.spdk:cnode1
    rpc_tgt bdev_null_create Null0 8192 512
    rpc_tgt nvmf_subsystem_add_ns -n 1 nqn.2016-06.io.spdk:cnode1 Null0
    rpc_tgt save_config
}

function run_nvmeperf() {
    local ADDR=${ADDR:-$TGT_ADDR}
    local PORT=${PORT:-$TGT_PORT}
    local NQN=${NQN:-}
    local SSH=""

    if [ -n "$PERF_SSH" ]; then
	SSH="ssh $SSH_EXTRA_OPTS $PERF_SSH"
    fi

    if [ -n "$NQN" ]; then
	NQN=" subnqn:$NQN"
    fi

    $SSH sudo $VMA_OPTS LD_PRELOAD=$LIBVMA $PERF_BIN_PATH/spdk_nvme_perf \
	 -S $SOCK_IMPL -r "trtype:$INIT_TRTYPE adrfam:ipv4 traddr:$ADDR trsvcid:$PORT$NQN" \
	 -c $PERF_MASK -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -t $PERF_TIME \
	 $NVME_PERF_EXTRA_OPTS 2>&1 | tee perf.log&
    PERF_PID=$!
    echo "Perf PID is $PERF_PID" 2>&1 | tee -a perf.log
}

function run_nvmeperf_snap() {
    local ADDR=${ADDR:-$TGT_ADDR}

    sudo $PERF_BIN_PATH/spdk_nvme_perf \
	 -r "trtype:pcie traddr:$ADDR" \
	 -c $PERF_MASK -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -t $PERF_TIME \
	 $NVME_PERF_EXTRA_OPTS 2>&1 | tee perf.log&
    PERF_PID=$!
    echo "Perf PID is $PERF_PID" 2>&1 | tee -a perf.log
}

function wait_nvmeperf() {
    echo "Waiting for perf $PERF_PID"
    wait $PERF_PID
}

function report_nvmeperf() {
    local OUT=$(grep "Total" perf.log | awk '{ print $3 " | " $4 " | " $5 " | " }')
    echo "| $TEST | $PERF_MASK | $TGT_MASK | $IO_SIZE | $QUEUE_DEPTH | $OUT" >> report.log
}

function run_bdevperf() {
    local ADDR=${ADDR:-$TGT_ADDR}
    local PORT=${PORT:-$TGT_PORT}
    local SSH=""

    if [ -n "$PERF_SSH" ]; then
	SSH="ssh $SSH_EXTRA_OPTS $PERF_SSH"
    fi

    $SSH sudo $VMA_OPTS LD_PRELOAD=$LIBVMA $PERF_SPDK_PATH/test/bdev/bdevperf/bdevperf \
	 -r /var/tmp/bdevperf.sock --wait-for-rpc -m $PERF_MASK -C \
	 -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -t $PERF_TIME -z \
	 $BDEV_PERF_EXTRA_OPTS 2>&1 | tee perf.log &
    PERF_PID=$!
    echo "Perf PID is $PERF_PID" 2>&1 | tee -a perf.log
    sleep 3
    if [ -n "$SOCK_IMPL" ]; then
	rpc_perf sock_set_default_impl -i $SOCK_IMPL
	rpc_perf sock_impl_set_options -i $SOCK_IMPL --disable-zerocopy-send-client $SOCK_EXTRA_OPTS
    fi
    rpc_perf framework_start_init
    rpc_perf bdev_nvme_set_options -k 0
    rpc_perf bdev_nvme_attach_controller -b Nvme0 -t $INIT_TRTYPE -f ipv4 -a $ADDR -s $PORT \
	     -n nqn.2016-06.io.spdk:cnode1 $NVME_ATTACH_CTRLR_EXTRA_OPTS
    $SSH sudo PYTHONPATH="$PYTHONPATH:$PERF_SPDK_PATH/scripts" \
	 $PERF_SPDK_PATH/test/bdev/bdevperf/bdevperf.py \
	 -s /var/tmp/bdevperf.sock -t 3600 perform_tests 2>&1 | tee -a perf.log &
    RPC_TASK_PID=$!
    echo "RPC task PID is $RPC_TASK_PID" 2>&1 | tee -a perf.log
}

function wait_bdevperf() {
    echo "Waiting for RPC task $RPC_TASK_PID"
    wait $RPC_TASK_PID
    rpc_perf spdk_kill_instance 15
    sleep 3
    echo "Waiting for perf $PERF_PID"
    wait $PERF_PID
}

function report_bdevperf() {
    local OUT=$(grep "Total" perf.log | tail -1 | awk '{ print $4 " | " $6 " | " }')
    echo "| $TEST | $PERF_MASK | $TGT_MASK | $IO_SIZE | $QUEUE_DEPTH | $OUT" >> report.log
}

function run_nvmeidentify() {
    local ADDR=${ADDR:-$TGT_ADDR}
    local PORT=${PORT:-$TGT_PORT}
    local NQN=${NQN:-}
    local SSH=""

    if [ -n "$PERF_SSH" ]; then
	SSH="ssh $SSH_EXTRA_OPTS $PERF_SSH"
    fi

    if [ -n "$NQN" ]; then
	NQN=" subnqn:$NQN"
    fi

    $SSH sudo $VMA_OPTS LD_PRELOAD=$LIBVMA $PERF_BIN_PATH/spdk_nvme_identify \
	 -r "trtype:$INIT_TRTYPE adrfam:ipv4 traddr:$ADDR trsvcid:$PORT$NQN" \
	 $NVME_IDENTIFY_EXTRA_OPTS 2>&1 | tee perf.log&
    PERF_PID=$!
    echo "Perf PID is $PERF_PID" 2>&1 | tee -a perf.log
}

function basic_test_nvme() {
    start_tgt
    config
    for PERF_MASK in $PERF_MASKS; do
	for QUEUE_DEPTH in $QUEUE_DEPTHS; do
	    for IO_SIZE in $IO_SIZES; do
		for REP in $(seq $REPEAT); do
		    run_nvmeperf # > /dev/null 2>&1
		    wait_nvmeperf
		    report_nvmeperf
		done
	    done
	done
    done
    stop_tgt
}

function basic_test_nvme_snap() {
    for PERF_MASK in $PERF_MASKS; do
	for QUEUE_DEPTH in $QUEUE_DEPTHS; do
	    for IO_SIZE in $IO_SIZES; do
		for REP in $(seq $REPEAT); do
		    run_nvmeperf_snap # > /dev/null 2>&1
		    wait_nvmeperf
		    report_nvmeperf
		done
	    done
	done
    done
}

function basic_test_bdev() {
    start_tgt
    config
    for PERF_MASK in $PERF_MASKS; do
	for QUEUE_DEPTH in $QUEUE_DEPTHS; do
	    for IO_SIZE in $IO_SIZES; do
		for REP in $(seq $REPEAT); do
		    run_bdevperf # > /dev/null 2>&1
		    wait_bdevperf
		    report_bdevperf
		done
	    done
	done
    done
    stop_tgt
}

# Identify target
function test0() {
    start_tgt
    config
    INIT_TRTYPE=rdma NQN="nqn.2016-06.io.spdk:cnode1" run_nvmeidentify
    wait_nvmeperf
    stop_tgt
}

# Test with nvme_perf
function test1() {
    NQN="nqn.2016-06.io.spdk:cnode1" basic_test_nvme
    # basic_test_nvme
}

# Test without memory domains
function test2() {
    basic_test_bdev
}

# Test with host memory domain and contig payload
function test3() {
    BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D host" basic_test_bdev
}

# Test with host memory domain and fragmented payload
function test4() {
    IO_SIZES=131072 \
	    BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D host -O 16384" \
	    basic_test_bdev
}

# Test with host memory domain and wrong host ID
function test5() {
    PERF_TIME=1 \
	     IO_SIZES=4096 \
	     BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D host -H 256" \
	     basic_test_bdev
}

# Test with host memory domain and custom host ID
function test6() {
    IO_SIZES=4096 \
	    NVME_ATTACH_CTRLR_EXTRA_OPTS="--host-memory-domain-id=256" \
	    BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D host -H 256" \
	    basic_test_bdev
}

# Test with rdma memory domain
function test7() {
    IO_SIZES=4096 \
	    BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D rdma" \
	    basic_test_bdev
}

# Test with rdma memory domain and wrong host ID
function test8() {
    PERF_TIME=1 \
	     IO_SIZES=4096 \
	     BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D rdma -H 256" \
	     basic_test_bdev
}

# Test with host memory domain and unaligned 2M payload
function test9() {
    IO_SIZES=2097152 \
	    TGT_TRANSPORT_EXTRA_OPTS="--max-io-size 2097152 --io-unit-size=131072"
	    BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D host" \
	    basic_test_bdev
}

# Test with host memory domain and page aligned 2M payload
function test10() {
    IO_SIZES=2097152 \
	    TGT_TRANSPORT_EXTRA_OPTS="--max-io-size 2097152 --io-unit-size=131072"
	    BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D host -a 4096" \
	    basic_test_bdev
}

# Test with host memory domain and page aligned 2M fragmented payload
function test11() {
    IO_SIZES=2097152 \
	    TGT_TRANSPORT_EXTRA_OPTS="--max-io-size 2097152 --io-unit-size=131072"
	    BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D host -O 131072 -a 4096" \
	    basic_test_bdev
}

# Test with host memory domain, contig payload and multi-iov translation
function test12() {
    IO_SIZES=16384 \
	    BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D host -I 4096" basic_test_bdev
}

# Test with host memory domain, fragmented payload and multi-iov translation
function test13() {
    IO_SIZES=131072 \
	    BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D host -O 16384 -I 4096" \
	    basic_test_bdev
}

# Test with RDMA memory domain, wrong host ID, contig payload and multi-iov translation
function test14() {
    IO_SIZES=16384 \
	    BDEV_PERF_EXTRA_OPTS="$BDEV_PERF_EXTRA_OPTS -D rdma -H 256 -I 4096" basic_test_bdev
}

function test_ngn_all() {
    run_test test3
    run_test test4
    run_test test5
    run_test test6
    run_test test7
    run_test test8
    run_test test9
    run_test test10
    run_test test11
    run_test test12
    run_test test13
    run_test test14
}

function run_test() {
    TEST="$1"
    echo "===== Start $TEST ====="
    $TEST | tee test.log
    echo "===== End $TEST ====="
    echo ""
}

if [ -n "$1" ]; then
    tests="$@"
else
    tests="test1"
fi

rm -rf rpc.log rpc_tgt.log perf.log tgt.log report.log
echo "| Test | Perf CPU | TGT CPU | IO size | QD | IOPS | BW | Lat |" >> report.log
echo "Running tests: $tests"
for t in $tests; do
    run_test $t
done

cat report.log
