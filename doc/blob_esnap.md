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

## Theory of Operation

Much like with regular clones, an external clone may read clusters that are allocated to the cluster
or read from the snapshot from which the clone is created. This is done using various callbacks
referenced by `blob->back_bs_dev`. Unlike regular clones, external clones have an external snapshot
device which uses storage that is not managed by the blobstore.

For example, the following diagram illustrates blob1 with an external snapshot named nvme1n1.

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

When an external clone is created, the callee-specific cookie provided in
`spdk_bs_create_external_clone()` is stored in the `BLOB_EXTERNAL_SNAPSHOT_COOKIE` internal XATTR.
This cookie serves as an identifier that must be usable for the life of the blob, including across
restarts of the SPDK application.

As an external snapshot blob is being loaded, the blobstore will call the aforementioned
`external_bs_dev_create()` function with the blob's external snapshot cookie, the blob, and a
callback.  See #esnap_spdk_bs_opts for the complete list of arguments.  The
`external_bs_dev_create()` function should open the implementation-specific device and pass this new
`back_bs_dev` to the blobstore via the callback. If `external_bs_dev_create()` is unable to open the
device, it must record information about this blob so that it can retry `spdk_bs_open_blob()` or
`spdk_bs_open_blob_ext()` when the device becomes available. No memory references to the blob, its
extended attributes, etc., may be retained after calling the completion callback.

### Channels

As is the case with all blobstore devices, the external snapshot's `back_bs_dev` must define
`create_channel` and `destroy_channel`.  The channel passed to the other functions defined in
`back_bs_dev` will be a channel obtained by calling `back_bs_dev->create_channel()`. These
`back_bs_dev` IO channels will be stored in an RB tree that is indexed by `blob->id` and rooted in
each `struct spdk_bs_channel`.

### Snapshots and Clones

An external clone may be snapshotted, just as any other blob may be snapshotted. As happens with
other blobs, when a snapshot is created the blob that is being snapshotted becomes the leaf node (a
clone of the snapshot) and the newly created snapshot sits between the snapshotted blob and the
external snapshot. As with other snapshots, a snapshot of an external snapshot may be cloned,
inflated (XXX-mg verify), and deleted. The following illustrates the creation of a snapshot (snap1)
of an external clone (blob1) which starts out as an external snapshot of nvme1n42. That is followed
by illustrations of deleting snap1 and blob1.

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

An external clone may be inflated or decoupled. Since `struct spdk_bs_dev` provides no means to
obtain ranges of unallocated storage, `spdk_bs_blob_decouple_parent()` is equivalent to
`spdk_bs_inflate_blob()`. Each of these operations will remove the dependence on the external
snapshot, turning it into a thick-provisioned blob. As the inflate or decouple completes,
`back_bs_dev->destroy()` will be called.

### Opening an External Snapshot {#opening_an_external_snapshot}

When an external clone blob is loaded, it needs to load its external snapshot. To do this, it uses
the blobstore's `external_bs_dev_create` function that was registered with the blobstore was
created or loaded.

Implementations of `external_bs_dev_create` should call `spdk_bdev_create_bs_dev_ro()` to open the
bdev and initialize a `struct spdk_bs_dev`. `spdk_bdev_create_bs_dev_ro()` is similar to
`spdk_bdev_create_bs_dev_ext()`, except it opens the bdev read-only and establishes a shared
read-only claim on the bdev.

The lvstore implementation of `external_bs_dev_create` will keep track of external snapshot bdevs
that fail to open on a watch list. The watch list may or may not be implemented using a list. There
may be multiple external clones of a single bdev, so each node of the watch list will maintain a
list of blobs that are awaiting a bdev. As new bdevs are created, vbdev_lvol's `examine_config`
callback will check the watch list to see if the new bdev matches one that is needed by a blob. If
it a match is found, `spdk_lvol_open()` is called for each waiting blob. While an lvol is on the
watch list, its name is reserved - no new lvol can be created with the same name.

### Blobs with Missing External Snapshot

As discussed in the previous section, a blob's external snapshot bdev may not be available when the
blobstore tries to open it. This may be a transient or persistent problem. In the case of it being a
persistent problem, it is essential that an external clone with a missing external snapshot can be
deleted.

The RPC call `bdev_lvol_get_lvols` will print information about each blob associated with an
lvstore, regardless of whether there is a functional bdev associated with the blob. This allows an
administrator to identify lvols with missing external snapshots so that they may be deleted with the
`bdev_lvol_delete` RPC call.

`blobcli` displays internal XATTRs and may be used to debug blobstores that are not open by another
SPDK application. Additionally, it provides an option to delete a blob. Deleting an lvstore's blob
with `blobcli` fully deletes a volume.

## On-Disk Representation {#external_snapshot_on_disk}

### Blobstore Superblock

The blobstore's superblock has no changes for this feature.

### Blob {#external_snapshot_on_disk_blob}

A blob that is an external clone has the following characteristics:

1. The `SPDK_BLOB_EXTERNAL_SNAPSHOT` (0x8) bit is set in `invalid_flags`.
1. An internal XATTR with name `BLOB_EXTERNAL_SNAPSHOT_COOKIE` ("EXTSNAP_COOKIE") exists.

The blobstore does not interpret the value of the internal XATTR, it only passes it and its size to
`external_bs_dev_create()` which will handle it in an implementation-specific manner.

## Data Structures

### `spkd_bs_opts` {#esnap_spdk_bs_opts}

```c

struct spdk_bs_opts {
	/* existing fields */

	/**
	 * A blobstore consumer that supports external snapshots must define
	 * external_bs_dev_create, which is responsible for opening the external
	 * snapshot and providing the blobstore with `struct spdk_bs_dev` that
	 * may be used for performing reads from the external snapshot device.
	 *
	 * Open the external snapshot device. On success, calls:
	 *
	 *    cb(cb_arg, back_bs_dev, 0);
	 *
	 * On failure, calls:
	 *
	 *    cb(cb_arg, NULL, -<errno>);
	 *
	 * Before calling cb(), blob may be inspected with functions like
	 * spdk_blob_get_id() and spdk_blob_get_xattr_value(). After calling fn(),
	 * none of cookie, blob, nor any pointers obtained from spdk_blob_*() calls
	 * may be dereferenced.
	 */
	void (*external_bs_dev_create)(const void *cookie, size_t cookie_sz,
		    struct spdk_blob *blob, spdk_blob_op_with_dev cb, void *cb_arg);
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
typedef void (*spdk_blob_op_with_dev)(void *cb_arg, struct spdk_bs_dev *bs_dev, int bserrno);
```

### External Snapshot IO Channel Context

An external snapshot device requires an IO channel per thread and as such must define
`io_channel_get` and `io_channel_put` callbacks. The presence of these callbacks will lead to each
blobstore IO channel that performs IO on the external snapshot to `get` an IO channel and store it
in a per-channel RB tree. As external clones are closed or threads are destroyed, the IO channels
will be released via the `io_channel_put` callback.

The existing `struct spdk_bs_channel` is extended by adding an RB tree. It is initialized in
`bs_channel_create()`.

```c
struct spdk_bs_channel {
	/* ... (existing fields) */

	RB_HEAD(, bs_esnap_channel)	esnap_channels;
};
```

Each node in the tree is:

```c
struct bs_esnap_channel {
	RB_ENTRY(bs_esnap_channel)	link;
	struct spdk_blob_id		id;
	struct spdk_io_channel		*channel;
};
```

Reads from an external snapshot device come only through `blob_request_submit_op_single()` or
`blob_request_submit_rw_iov()`, which will call new functions `bs_batch_read_esnap_dev()` and
`bs_sequence_readv_esnap_dev()`, respectively. They will each call `bs_esnap_io_channel_from_ctx()`
to obtain a channel. `bs_esnap_io_channel_from_ctx()` will search `channel->esnap_channels` is
searched for a channel to use for the external snapshot bdev. If there is no channel
`back_bs_dev->io_channel_get` is used to get an IO channel which is then added to the
`channel->esnap_channels` tree.

When a blob is closed, the blobstore will use `spdk_for_each_channel()` to cause
`back_bs_dev->destroy_channel()` to be called on each external snapshot IO channel associated with
that blob. The external snapshot will be destroyed with `back_bs_dev->destroy()`.

When `bs_channel_destroy()` is called, `back_bs_dev->destroy_channel()` will be called on each
external snapshot IO channel found in the RB tree of the `struct spdk_bs_channel` that is being
destroyed.

## Public Blobstore Functions

```c
/**
 * Create a clone of an arbitrary device. This requires that the blobstore was
 * loaded by spdk_bs_load() with the external_bs_dev_create function pointer
 * defined. The arbitrary device is referred to as the external snapshot and so
 * long as this newly created blob remains a clone of it, it may be referred to
 * as an external clone.
 *
 * XXX-mg fix this next paragraph.
 * Reads beyond esnap_size will not be sent to the external snapshot. If this
 * offset does not fall on a cluster boundary reads of the blob beyond this
 * offset to the end of the cluster will return zeroes. Any data written beyond
 * this size to the end of the cluster will be stored and will be returned on
 * subsequent reads.
 *
 * The memory referenced by cookie, the xattrs structure, and all memory
 * referenced by the xattrs structure must remain valid until the completion is
 * called.
 *
 * If cloning a blob in the same blobstore, use spdk_bs_create_clone instead.
 *
 * \param bs blobstore.
 * \param cookie A unique identifier for the external snapshot. The data
 * referenced by cookie will be stored in the blob's metadata and serves as a
 * primary key for external snapshot opens via esnap_mod->open().
 * \param cookie_size The size in bytes of the cookie. This value must be no
 * more than 4058.
 * \param esnap_size Size in bytes of the external snapshot.
 * \param xattrs xattrs specified for the clone
 * \param cb_fn Called when the operation is complete.
 * \param cb_arg Argument passed to function cb_fn.
 */
void spdk_bs_create_external_clone(struct spdk_blob_store *bs,
				   const void *cookie, size_t cookie_size,
				   uint64_t esnap_size,
				   const struct spdk_blob_xattr_opts *xattrs,
				   spdk_blob_op_with_id_complete_ cb_fn,
				   void *cb_arg);
```

## Blob bdev Updates

The blob bdev module will be updated as follows:

- `spdk_bdev_create_bs_dev_ro()` is added to be used when opening an external snapshot.
- Shared read-only claims are implemented and used by `spdk_bdev_create_bs_dev_ro()`. Shared
  read-only claims are automatically released through the `destroy()` callback of `struct
  spdk_bs_dev`s returned from `spdk-bdev_create_bs_dev_ro()`.

### Public interface

As mentioned in #opening_an_external_snapshot, `spdk_bdev_creaet_bs_dev_ro()` is called when a
module opens an external snapshot on behalf of a blobstore.

```c
/**
 * Create a read-only blobstore block device from a bdev.
 *
 * This is typically done as part of opening an external snapshot. On successful
 * return, the bdev is open read-only with a claim that blocks writers.
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
 * \return 0 on success
 * \return -ENOMEM if out of memory
 * \return other non-zero return value from spdk_bdev_module_claim_bdev().
 */
int spdk_bdev_blob_ro_claim(struct spdk_bdev *bdev);

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
`spdk_bdev_module_claim_bdev(bdev, NULL, &blob_bdev_if)`.

To support shared read-only claims, blob bdev will get a minimal `struct spdk_bdev_module` that can
be passed to `spdk_bdev_module_claim_bdev()`.

```c
static int
blob_bdev_init(void) {
	return 0;
}

static struct spdk_bdev_module blob_bdev_if = {
	name = "blob",
	module_init = blob_bdev_init,
};
```

Read-only claims for bdevs for all blobstores are tracked in an RB tree. Each node of the tree
contains the bdev's UUID and the number of shared claims. When the number of claims drops to 0, the
tree node is removed.

```c
struct bdev_blob_ro_claim_node {
	struct spdk_uuid	uuid;
	uint32_t		count;
	RB_ENTRY(bdev_blob_ro_claim) node;
};
```

Shared read-only claims are taken with `bdev_blob_ro_claim()`. This may fail due to memory
allocation for the first claim on a bdev or for any of the reasons that
`spdk_bdev_module_claim_bdev()` mail fail. Claims are released via `bdev_blob_ro_release()`.

```
static int bdev_blob_ro_claim(struct spdk_bdev *bdev);
static void bdev_blob_ro_release(struct spdk_bdev *bdev);
```

## Logical Volume Updates

### Create an lvstore

`vbdev_lvs_create()` will set `opts.external_bs_dev_create` to `vbdev_lvs_esnap_open`, which is:

```c
static void
vbdev_lvs_esnap_open(const void *cookie, size_t cookie_sz, struct spdk_blob *blob,
		     spdk_blob_op_with_dev cb, void *cb_arg)
{
	const char *name = cookie;

	/* Verify name[cookie_sz - 1] == '\0' */

	/* Simple case: the open succeeds and the external clone is just about ready. */
	rc = spdk_bdev_create_bs_dev_ro(name, ..., &bs_dev);
	if (rc == 0) {
		cb(cb_arg, bs_dev, 0);
		return;
	}

	if (rc == -ENODEV) {
		/* bdev not currently present. Add it to the watch list. */
	}

	cb(cb_arg, NULL, rc);
}
```

### Watch for missing external snapshots

See #opening_an_external_snapshot for background.

As new bdevs are added, each bdev module's `examine_config` and `examine_disk` callback is called.
While the  vbev_lvol `examine_disk` callback already exists, checking whether a new bdev is a
missing external snapshot is best performed in the `examine_config` callback because it does not
need to perform IO.

```c
static void
vbdev_lvs_examine_config(struct spdk_bdev *bdev)
{
	/* Look for bdev in wait list and remove if found */

	/*
	 * Iterate through blobs waiting on this bdev:
	 * - Call spdk_bdev_blob_ro_claim(bdev)
	 * - Call spdk_lvol_open() with a callback that calls
	 *   spdk_bdev_blob_ro_release().
	 */

	/*
	 * If there were any waiters, a shared claim has been established and
	 * the count will not drop to 0 until the blobs that are just starting
	 * to open are eventually closed.
	 */
	spdk_bdev_module_examine_done(&g_lvol_if);
}
```

#######################################

### esnap_bdev IO

The first `read`, `readv`, or `readv_ext` on each thread will lead to `esnap_bdev_io_channel_get()`
to be called. It will look something like:

```c
static struct spdk_io_channel *
esnap_bdev_io_channel_get(void *ctx)
{
	struct lvs_esnap_dev *esnap_dev = ctx;

	return spdk_bdev_get_io_channel(esnap_dev->bdev_desc);
}
```

The IO channel returned from the above function will be passed into `esnap_bdev_read()`, which will
look something like:

```c
static void
esnap_bdev_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	       uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct esnap_dev	*esnap_dev = back_bs_dev_to_esnap_ctx(dev);

	spdk_bdev_read_blocks(ctx->bdev_desc, ch, payload, lba, lba_count,
			      esnap_bdev_complete, cb_args);
}
```

The completion callback is:

```c
static void
esnap_bdev_complete(struct spdk_bdev_io *bdev_io, bool success, void *args)
{
	struct spdk_bs_dev_cb_args *cb_args = args;

	spdk_bdev_free_io(bdev_io);
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, success ? 0 : -EIO);
}
```

Being a read-only device, `write` and `writev` callbacks result in `-EPERM` (or `abort()` on debug builds).

## Example: Logical Volume External Clones

This example is presented to illustrate how a consumer may use the interfaces described above.

### Load the lvstore

Aside from initial creation, an lvstore is usually loaded in response to `vbdev_lvs_examine()`,
which is invoked via the `struct spdk_bdev_module`'s `examine_disk` callback. `vbdev_lvs_examine()`
calls `spdk_lvs_load()`, which calls `spdk_bs_load()`. To enable external clones, `spdk_bs_load()`
needs to be called with:

```c
	opts.external_bs_dev_create = spdk_esnap_bdev_open;
```

Since there is no way to pass `vbdev_lvs_esnap_open` to `spdk_lvs_load` a new function is required.
This will follow the model of other `_ext` functions that take an options structure.

```c
void
spdk_lvs_load_ext(struct spdk_bs_dev *bs_dev, struct lvs_load_opts *opts,
		  spdk_lvs_op_with_handle_complete cb_fn, void *cb_arg)
{
	struct spdk_bs_opts bs_opts = {};

	/* ... */

	bs_opts.external_bs_dev_create = opts.back_bs_dev_create;
	spdk_bs_load(bs_dev, &opts, lvs_load_cb, req);
}
```

The implementation of `struct lvs_load_opts` looks like the following. The comments for these fields
are elided from this document as they are the same as they are for `struct spdk_bs_opts`.

```c
struct lvs_load_opts {
	size_t opts_size;
	void (*external_bs_dev_create)(const void *cookie, size_t cookie_sz,
		    struct spdk_blob *blob, spdk_blob_op_with_dev cb, void *cb_arg);
};

void lvs_load_opts_init(struct lvs_load_opts *opts);
```

### Load the Logical Volumes

The lvstore is considered opened once `close_super_cb` is called with `lvolerrno` equal to 0.  It is
at that time that blobs being to be loaded by iteratively calling `load_next_lvol()`.  Prior to
loading the lvols, the lvstore needs to register with the esnap_bdev module.

```c
	spdk_esnap_bdev_register(&bs->uuid, lvs_esnap_event, lvs);
```

When a external snapshot device lookup fails, `lvs_esnap_event()` will be called.  It looks like:

```c
static void
lvs_esnap_event(void *ctx, enum spdk_esnap_event_type event, struct spdk_blob *blob)
{
	struct spdk_lvol_store *lvs = ctx;

	switch (event) {
	case SPDK_ESNAP_EVENT_DEV_MISSING:
		/*
		 * TODO: Store copies of the blob id and the name and UUID
		 * XATTRs in an RB tree (by blob id) or three (blob id, name, UUID).
		 */
		 break;
	case SPDK_ESNAP_EVENT_DEV_FOUND:
		/*
		 * TODO: look up the blob id in the RB tree and unlink it from
		 * the tree(s). Call code that is factored out of
		 * load_next_lvol() to load the lvol then free the node.
		 */
		 break;
	default:
		break;
	}
}
```

{#esnap_missing_snapshot_delete}
The name and UUID are stored so that:

- RPC or other calls provide visibility into lvols that could not be opened.
- The blob ID can be found to delete lvols that have a missing external
  snapshot.
- To prevent creating new lvols or existing new ones to have names that will
  collide with a sick lvol.

### Unload Logical Volume Store

As an lvstore is unloaded, the following will be called.

```
	spdk_esnap_bdev_register(&bs->uuid);
```

### Create and open external clone logical volume

A new function, `spdk_lvol_create_external_clone()` is created. It is based on
`spdk_lvol_create_clone()`.

```c
/**
 * Create clone of and arbitrary bdev.
 *
 * The named bdev must have a block size that is greater than or equal to the
 * block size of the lvstore.
 *
 * The named bdev must remain read-only for the life of the clone and must not
 * be resized. Changes to this bdev are likely to impact the content and/or
 * functionality of external clones.
 *
 * \param lvol_store Logical volume store that will contain the clone.
 * \param cookie Identified to be passed to external_bs_dev_create callback that was
 * registered with spdk_lvs_load_ext().
 * \param cookie_len Size of cookie in bytes.
 * \param esnap_size Size of the external snapshot in bytes.
 * \param clone_name Name of created clone.
 * \param cb_fn Completion callback.
 * \param cb_arg Completion callback custom arguments.
 */
void spdk_lvol_create_external_clone(struct spdk_lvol_store *lvol_store,
				     void *cookie, size_t cookie_len,
				     uint64_t esnap_size,
				     const char *clone_name,
				     spdk_lvol_op_with_handle_complete cb_fn,
				     void *cb_arg);
```

Among other things, `spdk_lvol_create_external_clone()` will make a call like:

```c
	/* TODO: copy SPDK_ESNAP_BLOCK_SIZE_XATTR if needed */
	spdk_bs_create_external_clone(lvs->blobstore, cookie, cookie_len,
				      esnap_size, clone_xattrs,
				      lvol_create_cb, req);
```

When `lvol_create_cb()` is called, it will call `spdk_bs_open_blob_ext()`, which will lead to
`blob_load_backing_dev()` to call:

```c
	/* spdk_esnap_bdev_open("nvme1n42", 9, ...) */
	blob->bs->external_bs_dev_create(cookie, cookie_len, blob, blob_load_final, ctx);
```

See #esnap_bdev_module for details on `spdk_esnap_bdev_open()`.

### Delete a logical volume

If a logical volume is healthy no changes are required.

If a logical volume could not load its external snapshot, it will have no bdev attached.
The existing `spdk_lvol_destroy` can be used. As described in #esnap_lvol_rpc, a corresponding RPC
interface will exist.

### IO involving the external snapshot

All IO involving the external snapshot is handled by the #esnap_bdev_module.

### Snapshot an external clone

No changes are needed at the lvol layer.

### Inflate or decouple an external clone

No changes are needed at the lvol layer.

### RPC changes {#esanp_lvol_rpc}

The following RPC changes are needed:

- Include external snapshot information in `bdev_get_bdevs` output.
- Create `bdev_lvol_external_clone`, modeled after `bdev_lvol_clone`
- Create `lvstore_get_lvols` to list all lvols. This will provide visibility into lvols that lack a
  corresponding bdev due to a missing external snapshot.
- Create `lvstore_delete_lvol` to delete an lvol that does not have an attached bdev.
