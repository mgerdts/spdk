The code that opens the esnap dev needs to live in lvstore.  The following excerpt from
[blob_esnap.md](../blob_esnap.md) is inspired by
[this review comment](https://review.spdk.io/gerrit/c/spdk/spdk/+/11643/2..4/doc/blob_esnap.md#b62).

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
> The `external_bs_dev_create()` function will create a `struct spdk_bs_dev` object which it will pass
> to the blobstore via a callback that was passed to `external_bs_dev_create()`.  In the happy path,
> the `read`, `readv`, and `readv_ext` `spdk_bs_dev` functions will be able to immediately perform
> reads from the external snapshot bdev. If the external snapshot bdev cannot be opened,
> `external_bs_dev_create()` may still succeed, registering an `spdk_bs_dev` that is operating in a
> degraded mode.
>
> The degraded mode serves multiple purposes:
>
> - External clone blobs can be opened so that IO that is not dependent on the external snapshot
>   device can be serviced.
> - bdevs that depend upon the external clone blob to be opened, which reserves their name and UUID in
>   the global namespaces.
> - Visibility of bdevs dependent on external clone blobs allows extant interfaces to view and manage
>   (e.g. remove) these bdevs.

In the first stab at this, the degraded state will be deferred.  See
[watch_for_missing_esnap_bdevs.md](watch_for_missing_esnap_bdevs.md).

As part of this fix, `struct spdk_blob_opts` should be updated as:

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
