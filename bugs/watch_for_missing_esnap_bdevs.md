If an lvstore loads before an esnap bdev is loaded (due to order or transient
problem), the esnap bdev never loads. As stated in [blob_esnap.md](../docs/blob_esnap.md),

> ### lvstore Handling of Blobs with Missing External Snapshot
>
> The lvstore implementation of `external_bs_dev_create` will allow external clones to be opened in a
> degraded mode and will keep track of external snapshot bdevs that fail to open. As new bdevs are
> created, `vbdev_lvol`'s `examine_config` callback will determine if the new bdev matches one that is
> needed by one or more lvols. If so, the bdev will be opened for each dependent lvol, allowing the
> associated `spdk_bs_dev` to service future reads.
>
> While an external clone lvol bdev does not have its external snapshot bdev opened, it the driver
> specific information returned by the `bdev_get_bdevs` RPC call will show `external_clone: true` and
> `degraded: true`.
