#!/usr/bin/env bash

#  BSD LICENSE
#
#  Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of the copyright holder nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/lvol/common.sh
source $rootdir/test/bdev/nbd_common.sh

function get_block_checksums() {
	xtrace_disable
	local dev=$1
	local size=$2
	local start=$3
	local count=$4
	local block

	for (( block=start ; block < count; block++ )); do
		dd if="$dev" bs="$size" count=1 status=none | md5sum
	done | awk '{print $1}'
	xtrace_restore
}

function compare_block() {
	local block=$1
	local a1_name=$2
	local a2_name=$3
	local -n a1=$2
	local -n a2=$3
	local ret=1

	if (( ${#a1[@]} < block )); then
		echo "ERROR: compare_block: array1 is too short"
	elif (( ${#a2[@]} < block )); then
		echo "ERROR: compare_block: array2 is too short"
	elif [[ -z ${a1[block]} && -z ${a2[block]} ]]; then
		echo "ERROR: both blocks are null"
	elif [[ ${a1[block]} != ${a2[block]} ]]; then
		echo "ERROR: $a1_name[$block] (${a1[block]}) != $a2_name[$block] (${a2[block]})"
	else
		ret=0
	fi
	return $ret
}

function compare_blocks() {
	xtrace_disable
	local a1_name=$1
	local a2_name=$2
	local -n a1=$1
	local -n a2=$2
	local start=${3:-0}
	local count=${4:-}
	local block
	local ret=0

	if [[ -z $count ]]; then
		local l1=${#a1[@]}
		local l2=${#a2[@]}
		if (( l1 != l2 )); then
			echo "ERROR: arrays have different lengths ($l1, $l2)"
			xtrace_restore
			return $ret
		fi
		(( count = l1 - start ))
	fi

	for (( block=start; block < (start + count); block++ )); do
		compare_block $block $a1_name $a2_name && continue
		ret=1
		break
	done

	xtrace_restore
	return $ret
}

function test_esnap_compare_with_lvol_bdev() {
	local block_size=4096
	local esnap_size_mb=1
	local esnap_block_count=$(( esnap_size_mb * 1024 * 1024 / block_size ))
	local lvs_size_mb=$(( esnap_size_mb * 4 ))
	local lvs_cluster_size=$(( 64 * 1024 ))
	local blocks_per_cluster=$(( lvs_cluster_size / block_size ))
	local esnap_name

	# Create an external snapshot device, fill each block with unique content,
	# then remember a hash of each block.
	esnap_name=$(rpc_cmd bdev_malloc_create $esnap_size_mb $block_size)
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$esnap_name" /dev/nbd0
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs=1024k count=$esnap_size_mb status=none
	esnap_sums=( $(get_block_checksums /dev/nbd0 $block_size 0 $esnap_block_count))
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0

	# Create the lvstore
	malloc_name=$(rpc_cmd bdev_malloc_create $lvs_size_mb $block_size)
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore -c "$lvs_cluster_size" "$malloc_name" lvs_test)

	# Create a clone of the external snapshot.
	lvol_uuid1=$(rpc_cmd bdev_lvol_clone_bdev lvs_test "$esnap_name" lvol_test1)
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$lvol_uuid1" /dev/nbd0

	# Read the clone content and verify that it matches the external snapshot.
	lvol_sums1=( $(get_block_checksums /dev/nbd0 $block_size 0 $esnap_block_count) )
	compare_blocks lvol_sums1 esnap_sums

	# Overwrite the second block of the first cluster then verify the whole first cluster.
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs=$block_size seek=1 count=1 status=none
	cluster_sums=( $(get_block_checksums /dev/nbd0 $block_size 0 $blocks_per_cluster) )
	[[ "${cluster_sums[1]}" != "${lvol_sums[1]}" ]]
	lvol_sums[1]=${cluster_sums[1]}
	compare_blocks cluster_sums esnap_sums 0 $blocks_per_cluster

	# Overwrite overwrite the two blocks that span the end of the first
	# cluster and the start of the second cluster, then check the content of
	# both clusters.
	local block=$((blocks_per_cluster - 1))
	dd if=/dev/urandom of=/dev/nbd0 oflag=direct bs=$((block_size * 2)) count=1 \
	    seek=$block status=none
	cluster_sums=( $(get_block_checksums /dev/nbd0 $block_size 0 $((blocks_per_cluster * 2)) ) )
	[[ "${cluster_sums[block]}" != "${lvol_sums[block]}" ]]
	lvol_sums[block]=${cluster_sums[block]}
	(( block++ ))
	[[ "${cluster_sums[block]}" != "${lvol_sums[block]}" ]]
	lvol_sums[block]=${cluster_sums[block]}
	compare_blocks cluster_sums esnap_sums 0 $(( blocks_per_cluster * 2 ))

	# Clean up the lvol
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	rpc_cmd bdev_lvol_delete "$lvol_uuid1"
	rpc_cmd bdev_get_bdevs -b "$lvol_uuid1" && false

	# Verify the external snapshot did not change.
	nbd_start_disks "$DEFAULT_RPC_ADDR" "$esnap_name" /dev/nbd0
	esnap_sums_again=( $(get_block_checksums /dev/nbd0 $block_size 0 $esnap_block_count))
	nbd_stop_disks "$DEFAULT_RPC_ADDR" /dev/nbd0
	compare_blocks esnap_sums esnap_sums_again

	rpc_cmd bdev_lvol_delete_lvstore -u "$lvs_uuid"
	rpc_cmd bdev_lvol_get_lvstores -u "$lvs_uuid" && false
	rpc_cmd bdev_malloc_delete "$malloc_name"
	rpc_cmd bdev_malloc_delete "$esnap_name"
	check_leftover_devices
}

function test_esnap_reload() {
	local bs_dev esnap_dev
	local block_size=512
	local esnap_size_mb=1
	local lvs_cluster_size=$(( 16 * 1024 ))
	local lvs_uuid eclone_uuid snap_uuid clone_uuid uuid
	local aio_bdev=test_esnap_reload_aio0

	# Create the lvstore on an aio device. Can't use malloc because we need to remove
	# the device and re-add it to trigger an lvstore unload and then load.
	rm -f $testdir/aio_bdev_0
	truncate -s "${AIO_SIZE_MB}M" $testdir/aio_bdev_0
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_create_lvstore -c "$lvs_cluster_size" "$bs_dev" lvs_test)

	# Create a bdev that will be the external snapshot
	esnap_dev=$(rpc_cmd bdev_malloc_create "$esnap_size_mb" "$block_size")
	eclone_uuid=$(rpc_cmd bdev_lvol_clone_bdev lvs_test "$esnap_dev" "eclone1")

	# Unload the lvstore
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test

	# Load the lvstore, expect to see eclone1 again
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_get_lvstores -l lvs_test)
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/eclone1 | jq -r '.[] | .name')
	test "$uuid"  == "$eclone_uuid"

	# Create a snapshot of the eclone, reload, and verify all is there.
	snap_uuid=$(rpc_cmd bdev_lvol_snapshot "$eclone_uuid" snap1)
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_get_lvstores -l lvs_test)
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/eclone1 | jq -r '.[] | .name')
	test "$uuid"  == "$eclone_uuid"
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/snap1 | jq -r '.[] | .name')
	test "$uuid"  == "$snap_uuid"

	rpc_cmd bdev_get_bdevs > /tmp/bdevs.0
	# Create a clone of the snapshot, reload, and verify all is there.
	clone_uuid=$(rpc_cmd bdev_lvol_clone "$snap_uuid" clone1)
	rpc_cmd bdev_get_bdevs > /tmp/bdevs.1
	rpc_cmd bdev_aio_delete "$aio_bdev"
	NOT rpc_cmd bdev_lvol_get_lvstores -l lvs_test
	bs_dev=$(rpc_cmd bdev_aio_create "$testdir/aio_bdev_0" "$aio_bdev" "$block_size")
	lvs_uuid=$(rpc_cmd bdev_lvol_get_lvstores -l lvs_test)
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/eclone1 | jq -r '.[] | .name')
	test "$uuid"  == "$eclone_uuid"
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/snap1 | jq -r '.[] | .name')
	test "$uuid"  == "$snap_uuid"
	uuid=$(rpc_cmd bdev_get_bdevs -b lvs_test/clone1 | jq -r '.[] | .name')
	test "$uuid"  == "$clone_uuid"

	rpc_cmd bdev_lvol_delete "$clone_uuid"
	rpc_cmd bdev_lvol_delete "$snap_uuid"
	rpc_cmd bdev_lvol_delete "$eclone_uuid"
	rpc_cmd bdev_aio_delete "$aio_bdev"
	rpc_cmd bdev_malloc_delete "$esnap_dev"
}

$SPDK_BIN_DIR/spdk_tgt &
spdk_pid=$!
trap 'killprocess "$spdk_pid"; rm -f "$testdir/aio_bdev_0"; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid
modprobe nbd

run_test "test_esnap_compare_with_lvol_bdev" test_esnap_compare_with_lvol_bdev
run_test "test_esnap_reload" test_esnap_reload

trap - SIGINT SIGTERM EXIT
killprocess $spdk_pid
rm -f "$testdir/aio_bdev_0"