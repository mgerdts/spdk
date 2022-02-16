<!--
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
-->

# Blobstore External Snapshots Design {#blob_esnap_design}

## Terminology

*External snapshots* provide a way for a blob to be a clone of storage that exists outside of the
blobstore. This document describes the design of this feature.

An *external clone* is a blob that is a clone of an external snapshot.

Blobstore consumers that use blobstore's external snapshot feature may refer to their objects using
similar terminology.

The motivation for this feature is to create lvol external clones from arbitrary bdevs. While the
focus of this document is the blobstore layer, lvols are mentioned to illustrate how blobstore
changes will be used.

## Theory of Operation

Much like with regular clones, an external clone may read clusters that are allocated to the cluster
or read from the snapshot from which the clone is created. This is done using various callbacks
referenced by `blob->back_bs_dev`. With regular clones, the `back_bs_dev` references a read-only
snapshot blob in the same blobstore. With external clones, `back_bs_dev` references an arbitrary
bdev.

For example, the following diagram illustrates blob1 with an external snapshot named nvme1n42.

```text
,------------------------- SPDK ------------------------.
|                                                       |
|  ,--------- Blobstore ----------.                     |     ,--- Remote Target ---.
|  |                              |                     |     |                     |
|  |   +-----------------------+  |  +---------------+  |     |   +--------------+  |
|  |   |       blob1 (rw)      |  |  | nvme1n42 (ro) |  |     |   |   Device     |  |
|  |   |-----------------------|  |  |---------------|  |     |   |--------------|  |
|  |   | Range (MiB) | Cluster |  |  |  Range (MiB)  |  |     |   | Range (MiB)  |  |
|  |   |-------------|---------|  |  |---------------|  |     |   |--------------|  |
|  |   |   [0 - 1)   |~~~~~~~~~~~~~~>|    [0 - 1)    |~~~~~~~~~~~>|   [0 - 1)    |  |
|  |   |   [1 - 2)   |~~~~~~~~~~~~~~>|    [1 - 2)    |~~~~~~~~~~~>|   [1 - 2)    |  |
|  |   |   [2 - 3)   |~~~~~~~~~~~~~~>|    [2 - 3)    |~~~~~~~~~~~>|   [2 - 3)    |  |
|  |   +-----------------------+  |  +---------------+  |     |   +--------------+  |
|  |                              |                     |     |                     |
|  `------------------------------'                     |     `---------------------'
`-------------------------------------------------------'
    Figure 1
```

Like all snapshots, external snapshots do not support `write` or any other operation which may
mutate the data on the device. All successful writes to an external clone result in changes to the
external clone on blocks that part of the blobstore's device (`blob->bs->dev`).

### Back Blobstore Device

A blobstore consumer that intends to support external snapshots must set the
`external_bs_dev_create` function pointer in the `struct spdk_bs_opts` that is passed to
`spdk_bs_load()`.

When an external clone is created, `external_snapshot_cookie` and `external_snapshot_cookie_len` are
set in the options passed to `spdk_bs_create_blob_ext()`. This cookie is stored in the
`BLOB_EXTERNAL_SNAPSHOT_COOKIE` internal XATTR.  This cookie serves as an identifier that must be
usable for the life of the blob, including across restarts of the SPDK application.

### Opening an External Snapshot {#opening_an_external_snapshot}

As an external snapshot blob is being loaded, the blobstore will call the aforementioned
`bs->external_bs_dev_create()` function with the blob, a callback function pointer, and a callback
argument pointer.  The `external_bs_dev_create` implementation will likely use
`spdk_blob_get_external_cookie()` to get the external cookie and may use
`spdk_blob_get_xattr_value()` to get any other consumer-specific metadata that it needs.

The `external_bs_dev_create()` function will create a `struct spdk_bs_dev` object which it will pass
to the blobstore via a callback that was passed to `external_bs_dev_create()`.  In the happy path,
the `read`, `readv`, and `readv_ext` `spdk_bs_dev` functions will be able to immediately perform
reads from the external snapshot bdev. If the external snapshot bdev cannot be opened,
`external_bs_dev_create()` may still succeed, registering an `spdk_bs_dev` that is operating in a
degraded mode.

**XXX-mg degraded mode not implemented.**

The degraded mode serves multiple purposes:

- External clone blobs can be opened so that IO that is not dependent on the external snapshot
  device can be serviced.
- bdevs that depend upon the external clone blob to be opened, which reserves their name and UUID in
  the global namespaces.
- Visibility of bdevs dependent on external clone blobs allows extant interfaces to view and manage
  (e.g. remove) these bdevs.

After calling the callback, The `external_bs_dev_create()` function must not use `blob` or any
memory references it obtained from `spdk_blob_get_external_cookie()`, `spdk_blob_get_xattr_value()`,
or any other references to memory associated with the blob.

### lvstore Handling of Blobs with Missing External Snapshot

The lvstore implementation of `external_bs_dev_create` will allow external clones to be opened in a
degraded mode and will keep track of external snapshot bdevs that fail to open. As new bdevs are
created, `vbdev_lvol`'s `examine_config` callback will determine if the new bdev matches one that is
needed by one or more lvols. If so, a `blob_bdev` will be created for each dependent lvol registered
with the appropriate blob. See {#spdk_blob_set_esnap_dev}.

While an external clone lvol bdev does not have its external snapshot bdev opened, the driver
specific information returned by the `bdev_get_bdevs` RPC call will show `external_clone: true` and
`degraded: true`.

An lvol that is in degraded mode can be deleted, renamed, cloned, etc., just as any other lvol can
be.

### XXX {#spdk_blob_set_esnap_dev}

**XXX-mg move this - probably documenting in a header is sufficient**
**XXX-mg reparenting ode not implemented.**

When transitioning from degraded mode, an external clone will need to update `blob->back_bs_dev`.
This is accomplished via `spdk_blob_set_esnap_dev()`. This function will freeze the blob, destroy
the current `back_bs_dev`, install the new `back_bs_dev`, then unfreeze the blob.

### Channels

As always, there is one channel per thread that performs IO. The channel used to perform IO on the
bdev that backs the blobstore is different from the channel used to perform IO on the various
external snapshot devices. Thus, each blobstore channel must keep track of a channel per esnap
device. To support this, a tree of external snapshot channels indexed by `blob_id` is stored in each
`spdk_bs_channel`.

### Snapshots and Clones

An external clone may be snapshotted, just as any other blob may be snapshotted. As happens with
other blobs, when a snapshot is created the blob that is being snapshotted becomes the leaf node (a
clone of the snapshot) and the newly created snapshot sits between the snapshotted blob and the
external snapshot. As with other snapshots, a snapshot of an external snapshot may be cloned,
inflated, and deleted. The following illustrates the creation of a snapshot (snap1) of an external
clone (blob1) which starts out as an external snapshot of nvme1n42. That is followed by an
illustrations of deleting snap1 and blob1.

Before creating snap1:

```
  ,--------.     ,----------.
  |  blob  |     |   bdev   |
  | blob1  |<----| nvme1n42 |
  |  (rw)  |     |   (ro)   |
  `--------'     `----------'
      Figure 2
```

After creating snap1:

```
  ,--------.     ,--------.     ,----------.
  |  blob  |     |  blob  |     |   bdev   |
  | blob1  |<----| snap1  |<----| nvme1n42 |
  |  (rw)  |     |  (ro)  |     |   (ro)   |
  `--------'     `--------'     `----------'
      Figure 3
```

Starting from Figure 3, if snap1 is removed, the chain reverts to what it looks like in Figure 2.

Starting from Figure 3, if blob1 is removed, the chain becomes:

```
  ,--------.     ,----------.
  |  blob  |     |   bdev   |
  | snap1  |<----| nvme1n42 |
  |  (ro)  |     |   (ro)   |
  `--------'     `----------'
      Figure 4
```

In each case, the blob pointed to by the nvme bdev is considered the *external clone*.  The
external clone always has the attributes described in #external_snapshot_ondisk_blob and
`blob->parent_id` is always `SPDK_BLOBID_EXTERNAL_SNAPSHOT`. No other blob that descends from the
external clone may have any of those set.

### Inflation and Decoupling

An external clone may be inflated or decoupled. Since there is no `is_zeroes` or similar callback
for bdevs, a clone of a bdev-based external snapshot will see mostly the same results from inflate
and decouple operations. The exception is that if an external clone has been grown, decouple will
preserve the sparseness of any unallocated blocks beyond the end of the external snapshot.

Scenario                                   | Inflate | Decouple
------------------------------------------ | ------- | --------
Clean clusters copied from `back_bs_dev`   | Yes     | Yes
Allocate space for end clusters after grow | Yes     | No
Retains dependency on `back_bs_dev`        | No      | No

**XXX-mg be sure a unit test covers the grow case**

## On-Disk Representation {#external_snapshot_on_disk}

### Blobstore Superblock

The blobstore's superblock has no changes for this feature.

### Blob {#external_snapshot_on_disk_blob}

A blob that is an external clone has the following characteristics:

1. The `SPDK_BLOB_EXTERNAL_SNAPSHOT` (0x8) bit is set in `invalid_flags`.
1. An internal XATTR with name `BLOB_EXTERNAL_SNAPSHOT_COOKIE` ("EXTSNAP") exists.

The blobstore does not interpret the value of the internal XATTR, it only passes it and its size to
`external_bs_dev_create()` which will handle it in an implementation-specific manner.

## Blobstore updates

### Public Interfaces

#### External Clone Creation

Clones of external snapshots are created by setting `external_snapshot_cookie` and
`external_snapshot_cookie_len` in `struct spdk_blob_opts` while calling `spdk_bs_create_blob_ext()`.

```c
struct spdk_blob_opts {
	...
        /**
	 * If set, pass this cookie to bs->external_bs_dev_create() while creating an external
	 * clone. The value passed in this option will be stored in the blobstore and used to find
	 * this same bdev when the blob is loaded.
         */
        void *external_snapshot_cookie;

	/**
	 * The size of external_snapshot_cookie, in bytes.
	 */
	uint32_t external_snapshot_cookie_len;
};
```

#### Blobstore load {#esnap_spdk_bs_opts}

As a blobstore is being loaded or create, `external_bs_dev_create` must be set on any blobstore that
is to be used with external snapshots.

```c

struct spdk_bs_opts {
	/* existing fields */

	/**
	 * A blobstore consumer that supports external snapshots must define external_bs_dev_create,
	 * which is responsible for opening the external snapshot and providing the blobstore with
	 * `struct spdk_bs_dev` that may be used for performing reads from the external snapshot
	 * device.
	 *
	 * Open the external snapshot device. On success, calls:
	 *
	 *    cb(cb_arg, esnap_bs_dev, 0);
	 *
	 * On failure, calls:
	 *
	 *    cb(cb_arg, NULL, -<errno>);
	 *
	 * Before calling cb(), blob may be inspected with spdk_blob_get_xattr_value(),
	 * spdk_blob_get_external_cookie(), spdk_blob_get_id(), etc. After calling cb(), it is not
	 * safe to reference blob or any memory references obtained from these functions.
	 */
	void (*external_bs_dev_create)(struct spdk_blob *blob, spdk_blob_op_with_bs_dev cb,
				       void *cb_arg);
};
```

The callback function passed to `external_bs_dev_create` is defined as:

```c
/**
 * Blob device open completion callback with blobstore device.
 *
 * \param cb_arg Callback argument.
 * \param bs_dev Blobstore device.
 * \param bserrno 0 if it completed successfully, or negative errno if it failed.
 */
typedef void (*spdk_blob_op_with_bs_dev)(void *cb_arg, struct spdk_bs_dev *bs_dev, int bserrno);

struct spdk_bs_dev_cb_args {
	spdk_bs_dev_cpl		cb_fn;
	struct spdk_io_channel	*channel;
	void			*cb_arg;
};
```

### Internal interfaces

#### External Snapshot IO Channel Context

An external snapshot device requires an IO channel per thread per bdev and as such must define
`io_channel_get` and `io_channel_put` callbacks. As reads occur, a per thread cache of IO channels
will be maintained so that IO channels for external snapshots may be obtained without taking locks.

The existing `struct spdk_bs_channel` is extended by adding an RB tree. It is initialized in
`bs_channel_create()`.

**XXX-mg Maybe a splay tree would be better: it is optimized for quick access of recently used
items.**

```c
struct spdk_bs_channel {
	/* ... (existing fields) */

	RB_HEAD(blob_esnap_channel_tree, blob_esnap_channel) esnap_channels;
};
```

Each node in the tree is:

```c
struct blob_esnap_channel {
	RB_ENTRY(blob_esnap_channel)	node;
	spdk_blob_id			blob_id;
	struct spdk_io_channel		*channel;
};
```

Reads from an external snapshot only come through `bs_{batch,sequence}_{read,readv}_bs_dev()`
functions (4 total). Each of them is passed a `struct spdk_bs_request_set` (as an `spdk_bs_batch_t`
or `spdk_bs_sequence_t`). These same functions may be used for reading from blobs. Within these
functions, there is no way to determine whether which channel should be used. Thus, it will be up to
the callers to provide the right channel for IO on the back device.

When the sequence or batch is allocated, it is trivial to determine whether an external snapshot
channel is needed. They can then set a new field in `struct spdk_bs_request_set` to the appropriate
handle. As shown below, `back_channel` is added and comments are added to explain the difference
between `channel` and `back_channel`.

```
struct spdk_bs_request_set {
	/* ... existing */
	/*
	 * The blobstore's channel, obtained by blobstore consumers via
	 * spdk_bs_alloc_io_channel(). Used for IO to the blobstore.
	 */
	struct spdk_bs_channel		*channel;
	/*
	 * The channel used by the blobstore to perform IO on back_bs_dev. Unless the blob
	 * is an external clone, back_channel == spdk_io_channel_get_ctx(set->channel).
	 */
	struct spdk_io_channel		*back_channel;
```

When a blob is closed, the blobstore will use `spdk_for_each_channel()` to cause
`back_bs_dev->destroy_channel()` to be called on each external snapshot IO channel associated with
that blob. The external snapshot will be destroyed with `back_bs_dev->destroy()`.

When `bs_channel_destroy()` is called, `back_bs_dev->destroy_channel()` will be called on each
external snapshot IO channel found in the RB tree of the `struct spdk_bs_channel` that is being
destroyed.

## Blob bdev Updates

The blob bdev module will be updated as follows:

- `spdk_bdev_create_bs_dev_ro()` is added to be used when opening an external snapshot.
- Shared read-only claims are implemented and used by `spdk_bdev_create_bs_dev_ro()`. Shared
  read-only claims are automatically released through the `destroy()` callback of `struct
  spdk_bs_dev`s returned from `spdk_bdev_create_bs_dev_ro()`.

**XXX-mg add shared read-only claims**

### Public interface

As mentioned in #opening_an_external_snapshot, `spdk_bdev_creaet_bs_dev_ro()` is called when a
module opens an external snapshot on behalf of a blobstore.

```c
/**
 * Create a read-only blobstore block device from a bdev.
 *
 * This is typically done as part of opening an external snapshot. On successful
 * return, the bdev is open read-only.
 *
 * \param bdev_name The bdev to use.
 * \param event_cb Called when the bdev triggers asynchronous event.
 * \param event_ctx Argument passed to function event_cb.
 * \param bs_dev Output parameter for a pointer to the blobstore block device.
 * \return 0 if operation is successful, or suitable errno value otherwise.
 */
int spdk_bdev_create_bs_dev_ro(const char *bdev_name, spdk_bdev_event_cb_t event_cb,
				void *event_ctx, struct spdk_bs_dev **bs_dev);

/**
 * Establish a shared read-only claim owned by blob bdev.
 *
 * This is intended to be called during an examine_confg() callback before a
 * bdev module calls spdk_bs_open_blob_ext(). The claim needs to be established
 * before the examine_config() callback returns to prevent other modules from
 * claiming it or opening it read-write. spdk_bs_open_blob_ext() performs its
 * work asynchronously and as such any shared claims that result from
 * spdk_bs_open_blob_ext() will come after the return of examine_config().
 *
 * Each successful call must be paired with a call to spdk_bdev_blob_ro_release().
 *
 * \param bdev The bdev to claim.
 * \param module The bdev module making the claim.
 * \return 0 on success
 * \return -ENOMEM if out of memory
 * \return other non-zero return value from spdk_bdev_module_claim_bdev().
 */
int spdk_bdev_blob_ro_claim(struct spdk_bdev *bdev, struct spdk_bdev_module *module);

/**
 * Release a shared read-only claim
 *
 * \param bdev The previously claimed bdev to release.
 */
void spdk_bdev_blob_ro_release(struct spdk_bdev *bdev);
```

### Internal interfaces

The implementation details in this section are internal to bdev_blob.

#### Shared read-only claims

The same external snapshot may be used by multiple external clone blobs in one or more blobstores.
The bdev layer allows each bdev to be claimed once by one bdev module at a time. The first time that
a bdev is opened for an external snapshot, a read-only claim is established via a call to
`spdk_bdev_module_claim_bdev()` using the module passed to `spdk_bdev_blob_ro_claim()`..

Read-only claims for bdevs for all blobstores are tracked in an RB tree. Each node of the tree
contains the bdev's UUID and the number of shared claims. When the number of claims drops to 0, the
tree node is removed.

Shared read-only claims are taken with `bdev_blob_ro_claim()`. This may fail due to memory
allocation for the first claim on a bdev or for any of the reasons that
`spdk_bdev_module_claim_bdev()` mail fail. Claims are released via `bdev_blob_ro_release()`.

```c
static int bdev_blob_ro_claim(struct spdk_bdev *bdev);
static void bdev_blob_ro_release(struct spdk_bdev *bdev);
```

## Logical Volume Updates

### Public Interfaces

#### Create and open external clone logical volume

A new function, `spdk_lvol_create_bdev_clone()` is created. It is based on
`spdk_lvol_create_clone()`.

```c
/**
 * Create clone of given non-lvol bdev.
 *
 * \param lvs Handle to lvolstore.
 * \param esnap_name Name or UUID of the bdev to use as the external snapshot.
 * \param clone_name Name of created clone.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
int spdk_lvol_create_bdev_clone(struct spdk_lvol_store *lvs,
				const char *esnap_name, const char *clone_name,
				spdk_lvol_op_with_handle_complete cb_fn, void *cb_arg);
```

The external snapshot name must be either the bdev's name or its stringified UUID. An alias must not
be used because aliases are registered after a new bdev's `examine_config` and `examine_disk`
callbacks are executed. If the external snapshot is known by an alias it will not be hotplugged when
it is registered after the lvstore loads.

As part of the initial implementation, the size of the external snapshot bdev must be evenly
divisible by the blobstore's cluster size.

#### RPC: bdev_lvol_clone_bdev

Create a clone of an arbitrary read-only bdev. If the bdev is an lvol in the same lvstore,
use `bdev_lvol_clone` instead.

##### Parameters

Name                    | Optional | Type        | Description
----------------------- | -------- | ----------- | -----------
lvs_name                | Required | string      | Name of logical volume store to create logical volume on
bdev_name               | Required | string      | Name or UUID of existing bdev to clone. An alias must not be used.
clone_name              | Required | string      | Name of logical volume to create

Either lvs_uuid or lvs_name may be specified.

##### Example

TODO
