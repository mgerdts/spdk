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
other blobs, when a snapshot is created, the blob that is being snapshotted becomes the leaf node (a
clone of the snapshot) and the newly created snapshot sits between the snapshotted blob and the
external snapshot. As with other snapshots, a snapshot of an external snapshot may be cloned,
inflated (XXX-mg verify), and deleted. The following illustrates the creation of a snapshot (snap1)
of an external clone (blob1) which starts out as an external snapshot of nvme1n42. That is followed
by illustrations of deleting snap1 and blob1.

Before creating snap1:

````
  ,--------.     ,----------.
  |  blob  |     |  vbdev   |
  | blob1  |<----| nvme1n42 |
  |  (rw)  |     |   (ro)   |
  `--------'     `----------'
      Figure 2
````

After creating snap1:

````
  ,--------.     ,--------.     ,----------.
  |  blob  |     |  blob  |     |  vbdev   |
  | blob1  |<----| snap1  |<----| nvme1n42 |
  |  (rw)  |     |  (ro)  |     |   (ro)   |
  `--------'     `--------'     `----------'
      Figure 3
````

Starting from Figure 3, if snap1 is removed, the chain reverts to what it looks like in Figure 2.

Starting from Figure 3, if blob1 is removed, the chain becomes:

````
  ,--------.     ,----------.
  |  blob  |     |  vbdev   |
  | snap1  |<----| nvme1n42 |
  |  (ro)  |     |   (ro)   |
  `--------'     `----------'
      Figure 4
````

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

### External Snapshot IO Channel Context

An external snapshot module that implements asynchronous IO most likely needs an IO channel per
thread. In such a case, the external snapshot module should define `io_channel_get` and
`io_channel_put` callbacks. The presence of these callbacks will lead to each blobstore IO channel
that performs IO on via the external snapshot module to `get` and IO channel and store it in a
per-channel RB tree. As external clones are closed or threads a destroyed, the IO channels will be
released via the `io_channel_put` callback.

The existing `struct spdk_bs_channel` is extended by adding an RB tree. It is initialized in
`bs_channel_create()`.

```c
struct spdk_bs_channel {
	/* ... (existing fields) */

	RB_HEAD(, bs_esnap_channel)	esnap_channels;
};
```

When a read is performed, `channel->esnap_channels` is searched for a channel to use for the esnap
IO. If there is no channel and `esnap_mod->io_channel_get` is defined, an IO channel is gotten and
added to the tree, indexed by the blob ID.

```c
struct bs_esnap_channel {
	RB_ENTRY(bs_esnap_channel)	link;
	struct spdk_blob_id		id;
	struct spdk_io_channel		*channel;
};
```

When a blob is closed, the blobstore will uses `spdk_for_each_channel()` to cause
`back_bs_dev->destroy_channel()` to be called on each external snapshot IO channel associated with
that blob. The external snapshot will be destroyed with `back_bs_dev->destroy()`.

When `bs_channel_destroy()` is called, `back_bs_dev->destroy_channel()` will be called on each
external snapshot IO channel found in the RB tree of the `struct spdk_bs_channel` that is being
destroyed.

## Public Blobstore Functions

```c
/**
 * Create a clone of an arbitrary device. This requires that the blobstore was
 * loaded by spdk_bs_load() with esnap_mod specified. The arbitrary device is
 * referred to as the external snapshot and so long as this newly created blob
 * remains a clone of it, it may be referred to as an external clone.
 *
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

## External Snapshot Modules

The initial use case for the blobstore external snapshot feature calls for bdevs to act as external
snapshots. It is envisioned that other use cases may call for using an object store for external
snapshots. Over time, other external snapshot modules may be of interest.

### External Snapshot Module Interface

At this time, no generic external snapshot module interface will be created. When the second
external snapshot module is created, this should be re-evaluated.

### esnap_bdev Module {#esnap_bdev_module}

The esnap_bdev module will provide the features required for managing external snapshots that are
bdevs. That is:

- `spdk_esnap_bdev_open` will be the module's `external_bs_dev_create` callback.
- It will implement `struct spdk_bs_dev` callback functions required to translate blobstore reads
  into bdev reads.
- It will establish a read-only claim on all bdevs in use as external snapshots.
- It will watch for the addition of missing bdevs notify the consumer(s) as missing bdevs appear.

#### esnap_bdev Public Interface

An esnap_bdev consumer that commits to performing IO using a specific block size should set the
`SPDK_ESNAP_BLOCK_SIZE_XATTR` XATTR on the blob.

```c
/**
 *
 * The name of an XATTR that specifies the minimum IO size and alignment of IO
 * request issued by the esnap_bdev consumer to the blobstore. The consumer is
 * responsible for setting this value when needed. The value must be set when
 * the blobstore's IO unit size (io_unit_size in struct spdk_blob_store) is
 * smaller than the bdev's blocklen.
 *
 * The following restrictions exist:
 *
 * - The value must be no smaller than the blobstore's IO unit size.
 * - The value must be no smaller than the bdev's blocklen.
 * - The value must be no larger than the blobstore's cluster size.
 *
 * The value is specified as a string. The typical value is "4096" or otherwise
 * is unset.
 */
#define SPDK_ESNAP_BLOCK_SIZE_XATTR "SPDK_ESNAP_BLOCK_SIZE"
```

A consumer of this module passes `spdk_esnap_bdev_open` to `spdk_bs_load` via the
`external_bs_dev_create` field in `struct spdk_bs_opts`.

```c
void spdk_esnap_bdev_open(void *ctx void *cookie, size_t cookie_len,
			  struct spdk_blob *blob, spdk_blob_op_with_dev cb, void *cb_arg);
```

To receive notifications as blobs are opened, the consumer of this module must register a callback
with `spdk_esnap_bdev_register_cb()`. This must be called once per blobstore prior to opening blobs
that are external clones.

```c
/**
 * External snapshot events are sent for a particular blobstore only after
 * spdk_esnap_bdev_register() is called. Likewise, any device that could not be
 * found before registration will not have a watch established and as such will
 * not generate an event when they are added.
 */
enum spdk_esnap_event_type {
	/**
	 * The external_bs_dev_create() callback in struct spdk_bs_dev was called.
	   This callback was unable to open the device.
	 */
	SPDK_ESNAP_EVENT_DEV_MISSING,

	/**
	 * A device that was previously missing when external_bs_dev_create() was
	 * called has become available.
	 */
	SPDK_ESNAP_EVENT_DEV_FOUND,
};

/**
 * Callback type to be used with spdk_esnap_bdev_register.
 *
 * \param ctx Callee context passed to spdk_esnap_bdev_register().
 * \param event The event that has happened.
 * \param blob The blob that experienced the event. The callback must not
 * dereference blob nor any pointer retried by querying the blob after
 * returning.
 */
typedef void (*spdk_esnap_event_cb)(void *ctx, enum spdk_esnap_event_type event,
				    struct spdk_blob *blob);

/**
 * Register interest in events for a particular blobstore.
 *
 * \param bs_uuid UUID of the blobstore to be notified about.
 * \param cb The callback to call when an event happens.
 * \param cb_arg The context to pass to cb via the ctx parameter.
 *
 * \return 0 Success.
 * \return -EBUSY A callback is already registered for this blobstore.
 * \return -ENOMEM Out of memory.
 */
int spdk_esnap_bdev_register(const struct spdk_uuid *bs_uuid,
			     spdk_esnap_event_cb cb, void *cb_arg);
```

A consumer may cancel interest in events for a blobstore. This would normally
happen while an application unloads the blobstore.

```c
/**
 * Cancel interest in events for a blobstore. This also clears all watches for
 * missing devices.
 *
 * This should be called before unloading a blobstore.
 *
 * \param bs_uuid UUID of the blobstore to cancel.
 */
void spdk_esnap_bdev_unregister(const struct spdk_uuid *bs_uuid);
```

A consumer may cancel interest in a particular missing device. This may happen
if the external clone is deleted.

```c
/**
 * Cancel interest in events for a specific blob.
 *
 * To avoid races between blob deleting and creation of a new blob that reuses
 * the blob ID, this should be called before deleting the blob.
 *
 * \param bs_uuid UUID of the blobstore containing the blob
 * \param blob_id Blob ID that is no longer interesting.
 */
void spdk_esnap_bdev_unregister_blob(const struct uuid *bs_uuid,
				     spdk_blob_id blob_id);
```

#### esnap_bdev Implementation details

When the blobstore opens a blob that is an external clone, it will call `spdk_esnap_bdev_open` via
the `external_bs_dev_create` callback. That function will look similar to:

```c
void
spdk_esnap_bdev_open(void *cookie, size_t cookie_len, struct spdk_blob *blob,
		     spdk_blob_op_with_dev cb, void *cb_arg)
{
	char *bdev_name = cookie;
	struct lvs_esnap_dev *esnap_dev = calloc(1, sizeof(*esnap_dev));
	struct spdk_bs_dev *bs_dev;

	/* Open the external snapshot device with a read-only claim */
	rc = esnap_bdev_open(bdev_name, esnap_dev);
	if (rc != 0) {
		if (rc == -ENODEV) {
			esnap_register_watch(ctx, bdev_name);
		}
		cb(cb_arg, NULL rc);
		return;
	}

	bs_dev = &esnap_dev->bs_dev;
	bs_dev->create_channel = lvs_esnap_io_channel_get;
	bs_dev->destroy_channel = lvs_esnap_io_channel_put;
	bs_dev->read = lvs_esnap_read;
	bs_dev->readv = lvs_esnap_readv;
	bs_dev->blocklen = esnap_get_blocklen(esnap_dev)

	/* Verifies bs and bdev compatibility, sets bs_dev->block{len,cnt} */
	rc = esnap_configure_blocksize(esnap_dev, blob);
	if (rc != 0) {
		/* TODO: cleanup */
		cb(cb_arg, NULL, -EINVAL);
	}

	cb(cb_arg, bs_dev, 0);
}
```

Watches for missing bdevs are registered with `esnap_register_watch()`, which relies on a bdev
module's `examine_disk` callback. Thus, `esnap_bdev` registers itself as a bdev, but implements very
little of what a bdev module normally implements.

```c
static struct spdk_bdev_module esnap_if = {
	.name = "esnap",
	.module_init = esnap_mod_init,
	.module_fini = esnap_mod_fini,
	.examine_disk = esnap_mod_examine_disk,
}
```

The bdevs that are being watched need to be looked up in a few ways:

- By name or alias. `esnap_mod_examine_disk` is the expected consumer.
- By blobstore UUID and blob ID.  This should be relatively rare, as it is only happens when a bdev
  that was missing during blob open has the corresponding blob deleted.
- Delete all with matching blobstore UUID.

This seems to call for an RB tree indexed by name. Each tree node will have also be a member of a
per-blobstore linked list. The list heads of the per-blobstore linked lists will be stored in
another RB tree indexed by blobstore UUID.

The data structures for keeping track of watches are roughly:

```c
/*
 * One of these structures per registered blobstore. Look up via
 * g_esnap_watch_bs RB tree or follow link from any esnap_watch.
 */
struct esnap_watch_bs {
	struct spdk_uuid		bs_uuid;
	spdk_esnap_event_cb		event_cb;
	void				*event_ctx;

	RB_ENTRY(esnap_watch_bs)	bs_node;
	LIST_HEAD(, esnap_watch)	watches;
};

/*
 * One of these per blob known by a particular name that is watching for its
 * external snapshot.
 */
struct esnap_watch {
	spdk_blob_id			blob_id;
	TAILQ_ENTRY(esnap_watch)	watches;
	struct esnap_watch_bs		*bs_watches;
	struct esnap_watch_name		*watch_name;
};

/*
 * One of these per name that is being watched. Each name refers to only one
 * bdev, but that may be waited upon for any number of blobs in any number of
 * blobstores. A blob may have unique instances of this for its name and any
 * number of aliases.
 */
struct esnap_watch_name {
	char				*name;

	TAILQ_HEAD(, esnap_watch)	watches;
	RB_ENTRY(esnap_watch)		node;
	LIST_ENTRY(esnap_watch)		bs_link;
};
```

During the `examine_disk` callback, this is executed:

```c
static void
esnap_mod_exmaine_disk(struct spdk_bdev *bdev)
{
	const struct spdk_bdev_aliases_list *aliases;
	const struct spdk_bdev_alias *alias;

	esnap_examine_disk_name(bdev, spdk_bdev_get_name(bdev);

	aliases = spdk_bdev_get_aliases(bdev);
	TAILQ_FOREACH(alias, aliases, tailq) {
		esnap_examine_disk_name(bdev, alias->alias.name);
	}
}

static void
esnap_examine_disk_name(struct spdk_bdev *bdev, const char *name)
{
	struct esnap_watch_name find;
	struct esnap_watch_name *watch_name;
	struct esnap_watch *watch, *watch_tmp;

	find.name = name;

	pthread_mutex_lock(&g_esnap_watches_lock);
	watch_name = RB_FIND(esnap_name_tree, &g_esnap_watches, &find);
	if (watch == NULL) {
		pthread_mutex_unlock(&g_esnap_watches_lock);
		return;
	}

	RB_REMOVE(esnap_name_tree, &g_esnap_watches, watch_name);
	LIST_REMOVE(watch_name, bs_link);
	pthread_mutex_unlock(&g_esnap_watches_lock);

	TAILQ_FOREACH_SAFE(watch, &watch_name.watches, watches, watch_tmp) {
		TAILQ_REMOVE(&watch_name.watches, watch, watches);
		/*
		 * TODO: this will access watch->bs_watches, which could race
		 * with spdk_esnap_bdev_unregister().  Add reference count
		 * to esnap_watch_bs and be sure that esnap_notify causes it to
		 * be decremented when freeing watch.
		 */
		/* Notify the consumer and delete watch. */
		esnap_notify(watch);
	}
}
```

The examples above are illustrative enough to understand how to add a watch when a device cannot be
opened.

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

Among other things, `spdk_lvol_create_clone()` will make a call like:

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
