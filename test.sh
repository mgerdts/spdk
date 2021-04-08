#!/usr/bin/env bash
TGT_MASK=0x0F
PERF_MASK=0xF0
PERF_TIME=10

# Looks like it is not possible to run initiator with VMA and target without on the same host.
# So, we run it on a different host
TGT_HOST=spdk04.swx.labs.mlnx
TGT_ADDR=1.1.4.1
TGT_PORT=4420

# It is expected that SPDK was configured with --prefix=$PWD/install-$HOSTNAME
BIN_PATH=$PWD/install-$HOSTNAME/bin
if [ -n "$TGT_HOST" ]; then
    TGT_BIN_PATH=$PWD/install-$TGT_HOST/bin
else
    TGT_BIN_PATH=$PWD/install-$HOSTNAME/bin
fi

# It is expected that VMA was configured with --prefix=$PWD/install-$HOSTNAME
# Just comment the line if you want to run without VMA
LIBVMA=$PWD/../libvma-zcopy-fix/install-$HOSTNAME/lib/libvma.so
VMA_OPTS="
xVMA_INTERNAL_THREAD_AFFINITY=0x80
VMA_RING_ALLOCATION_LOGIC_TX=30
VMA_RING_ALLOCATION_LOGIC_RX=30
xVMA_TSO=0
xVMA_TX_BUF_SIZE=8000
xVMA_RX_POLL_ON_TX_TCP=1"

QUEUE_DEPTH=128
IO_SIZES="4096 8192 16384 32768 65536 131072"
RW=randread
#NVME_PERF_EXTRA_OPTS="-T vma -T nvme"
#BDEV_PERF_EXTRA_OPTS="-L vma -L nvme"
#SSH_EXTRA_OPTS="-t"

function rpc_tgt() {
    local SSH=""
    if [ -n "$TGT_HOST" ]; then
	SSH="ssh $SSH_EXTRA_OPTS $TGT_HOST"
    fi
    $SSH sudo $PWD/scripts/rpc.py $@ >> rpc_tgt.log 2>&1
}

function rpc_perf() {
    sudo ./scripts/rpc.py -s /var/tmp/bdevperf.sock $@ 2>&1 | tee -a perf.log
}

function start_tgt() {
    local SSH=""
    if [ -n "$TGT_HOST" ]; then
	SSH="ssh $SSH_EXTRA_OPTS $TGT_HOST"
    fi
    $SSH sudo $TGT_BIN_PATH/spdk_tgt -m $TGT_MASK 2>&1 | tee tgt.log > /dev/null &
    sleep 7
}

function stop_tgt() {
    rpc_tgt spdk_kill_instance 15
    sleep 3
}

function config() {
    rpc_tgt nvmf_create_transport -t tcp -q 512
    rpc_tgt nvmf_create_subsystem -a nqn.2016-06.io.spdk:cnode1
    rpc_tgt nvmf_subsystem_add_listener -t tcp -a $TGT_ADDR -f ipv4 -s $TGT_PORT nqn.2016-06.io.spdk:cnode1
    rpc_tgt bdev_null_create Null0 8192 512
    rpc_tgt nvmf_subsystem_add_ns -n 1 nqn.2016-06.io.spdk:cnode1 Null0
    rpc_tgt save_config
}

function run_nvmeperf() {
    local ADDR=${ADDR:-$TGT_ADDR}
    local PORT=${PORT:-$TGT_PORT}

    sudo $VMA_OPTS LD_PRELOAD=$LIBVMA $BIN_PATH/spdk_nvme_perf \
	 -S $SOCK_IMPL -r "trtype:tcp adrfam:ipv4 traddr:$ADDR trsvcid:$PORT" \
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

    sudo $VMA_OPTS LD_PRELOAD=$LIBVMA ./test/bdev/bdevperf/bdevperf \
	 -r /var/tmp/bdevperf.sock --wait-for-rpc -m $PERF_MASK -C \
	 -q $QUEUE_DEPTH -o $IO_SIZE -w $RW -t $PERF_TIME \
	 $BDEV_PERF_EXTRA_OPTS 2>&1 | tee perf.log &
    PERF_PID=$!
    echo "Perf PID is $PERF_PID" 2>&1 | tee -a perf.log
    sleep 3
    rpc_perf sock_set_default_impl -i $SOCK_IMPL
    rpc_perf sock_impl_set_options -i $SOCK_IMPL --enable-zerocopy-recv --disable-zerocopy-send --disable-recv-pipe
    rpc_perf framework_start_init
    rpc_perf bdev_nvme_attach_controller -b Nvme0 -t tcp -f ipv4 -a $ADDR -s $PORT \
	     -n nqn.2016-06.io.spdk:cnode1
    sudo PYTHONPATH="$PYTHONPATH:$PWD/scripts" ./test/bdev/bdevperf/bdevperf.py \
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

function basic_test_nvme() {
    start_tgt
    config
    for IO_SIZE in $IO_SIZES; do
	run_nvmeperf # > /dev/null 2>&1
	wait_nvmeperf
	report_nvmeperf
    done
    stop_tgt
}

function basic_test_bdev() {
    start_tgt
    config
    for IO_SIZE in $IO_SIZES; do
	run_bdevperf # > /dev/null 2>&1
	wait_bdevperf
	report_bdevperf
    done
    stop_tgt
}

# Test with spdk_nvme_perf VMA non zcopy
function test1() {
    SOCK_IMPL=vma NVME_PERF_EXTRA_OPTS="-P 2 $NVME_PERF_EXTRA_OPTS" basic_test_nvme
}

# Test with bdevperf VMA non zcopy
function test2() {
    SOCK_IMPL=vma basic_test_bdev
}

# Test with spdk_nvme_perf VMA zcopy
function test3() {
    SOCK_IMPL=vma NVME_PERF_EXTRA_OPTS="-n-Z vma -P 2 $NVME_PERF_EXTRA_OPTS" basic_test_nvme
}

# Test with bdevperf VMA zcopy
function test4() {
    SOCK_IMPL=vma BDEV_PERF_EXTRA_OPTS="-Z $BDEV_PERF_EXTRA_OPTS" basic_test_bdev
}

# Test with spdk_nvme_perf POSIX-VMA non zcopy
function test5() {
    SOCK_IMPL=posix NVME_PERF_EXTRA_OPTS="-z posix -P 2 $NVME_PERF_EXTRA_OPTS" basic_test_nvme
}

# Test with bdevperf POSIX-VMA non zcopy
function test6() {
    SOCK_IMPL=posix basic_test_bdev
}

# Test with spdk_nvme_perf POSIX-Kernel non zcopy
function test7() {
    LIBVMA= SOCK_IMPL=posix NVME_PERF_EXTRA_OPTS="-P 2 $NVME_PERF_EXTRA_OPTS" basic_test_nvme
}

# Test with bdevperf POSIX-Kernel non zcopy
function test8() {
    LIBVMA= SOCK_IMPL=posix basic_test_bdev
}

if [ -n "$1" ]; then
    tests="$@"
else
    tests="test1 \
	   test2 \
	   test3 \
	   test4 \
	   test5 \
	   test6 \
	   test7 \
	   test8"
fi

rm -rf rpc.log rpc_tgt.log perf.log tgt.log report.log
echo "| Test | Perf CPU | TGT CPU | IO size | QD | IOPS | BW | Lat |" >> report.log
echo "Running tests: $tests"
for t in $tests; do
    TEST="$t"
    echo "===== Start $t ====="
    $t | tee test.log
    echo "===== End $t ====="
    echo ""
done

cat report.log
