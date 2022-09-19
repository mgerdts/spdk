The code that opens the esnap dev needs to live in lvstore.  As described after
[this review comment](https://review.spdk.io/gerrit/c/spdk/spdk/+/11643/2..4/doc/blob_esnap.md#b62):

> ### Back Blobstore Device
> 
> A blobstore consumer that intends to support external snapshots must set the
> `external_bs_dev_create` function pointer in the `struct spdk_bs_opts` that is passed to
> `spdk_bs_load()`.
> 
> When an external clone is created, the callee-specific cookie provided in
> `spdk_bs_create_external_clone()` is stored in the `BLOB_EXTERNAL_SNAPSHOT_COOKIE` internal XATTR.
> This cookie serves as an identifier that must be usable for the life of the blob, including across
> restarts of the SPDK application.
> 
> ### Opening an External Snapshot {#opening_an_external_snapshot}
> 
> As an external snapshot blob is being loaded, the blobstore will call the aforementioned
> `external_bs_dev_create()` function with the blob's external snapshot cookie, the blob, and a
> callback.  See #esnap_spdk_bs_opts for the complete list of arguments.
> 
> Implementations of `external_bs_dev_create` should call `spdk_bdev_create_bs_dev_ro()` to open the
> bdev and initialize a `struct spdk_bs_dev`. `spdk_bdev_create_bs_dev_ro()` is similar to
> `spdk_bdev_create_bs_dev_ext()`, except it opens the bdev read-only and establishes a shared
> read-only claim on the bdev.
> 
> The lvstore implementation of `external_bs_dev_create` will keep track of external snapshot bdevs
> that fail to open on a watch list. The watch list may or may not be implemented using a list. There
> may be multiple external clones of a single bdev, so each node of the watch list will maintain a
> list of blobs that are awaiting a bdev. As new bdevs are created, vbdev_lvol's `examine_config`
> callback will check the watch list to see if the new bdev matches one that is needed by a blob. If
> it a match is found, `spdk_lvol_open()` is called for each waiting blob. While an lvol is on the
> watch list, its name is reserved - no new lvol can be created with the same name.
