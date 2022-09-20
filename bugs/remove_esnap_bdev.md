After the completion of [esnap_io_channels.md](esnap_io_channels.md), the
shared read-only claims can be moved into blob_bdev and esnap_bdev can be
removed.

> ## Blob bdev Updates
>
> The blob bdev module will be updated as follows:
>
> - `spdk_bdev_create_bs_dev_ro()` is added to be used when opening an external snapshot.
> - Shared read-only claims are implemented and used by `spdk_bdev_create_bs_dev_ro()`. Shared
>   read-only claims are automatically released through the `destroy()` callback of `struct
>   spdk_bs_dev`s returned from `spdk_bdev_create_bs_dev_ro()`.
>
> ### Public interface
>
> As mentioned in #opening_an_external_snapshot, `spdk_bdev_creaet_bs_dev_ro()` is called when a
> module opens an external snapshot on behalf of a blobstore.
>
> ```c
> /**
>  * Create a read-only blobstore block device from a bdev.
>  *
>  * This is typically done as part of opening an external snapshot. On successful
>  * return, the bdev is open read-only with a claim that blocks writers.
>  *
>  * \param bdev_name The bdev to use.
>  * \param event_cb Called when the bdev triggers asynchronous event.
>  * \param event_ctx Argument passed to function event_cb.
>  * \param bs_dev Output parameter for a pointer to the blobstore block device.
>  * \return 0 if operation is successful, or suitable errno value otherwise.
>  */
> int spdk_bdev_create_bs_dev_ro(const char *bdev_name, spdk_bdev_event_cb_t event_cb,
>                                 void *event_ctx, struct spdk_bs_dev **bs_dev);
>
> /**
>  * Establish a shared read-only claim owned by blob bdev.
>  *
>  * This is intended to be called during an examine_confg() callback before a
>  * bdev module calls spdk_bs_open_blob_ext(). The claim needs to be established
>  * before the examine_config() callback returns to prevent other modules from
>  * claiming it or opening it read-write. spdk_bs_open_blob_ext() performs its
>  * work asynchronously and as such any shared claims that result from
>  * spdk_bs_open_blob_ext() will come after the return of examine_config().
>  *
>  * Each successful call must be paired with a call to spdk_bdev_blob_ro_release().
>  *
>  * \param bdev The bdev to claim.
>  * \param module The bdev module making the claim.
>  * \return 0 on success
>  * \return -ENOMEM if out of memory
>  * \return other non-zero return value from spdk_bdev_module_claim_bdev().
>  */
> int spdk_bdev_blob_ro_claim(struct spdk_bdev *bdev, struct spdk_bdev_module *module);
>
> /**
>  * Release a shared read-only claim
>  *
>  * \param bdev The previously claimed bdev to release.
>  */
> void spdk_bdev_blob_ro_release(struct spdk_bdev *bdev);
> ```
>
> ### Internal interfaces
>
> The implementation details in this section are internal to bdev_blob.
>
> #### Shared read-only claims
>
> The same external snapshot may be used by multiple external clone blobs in one or more blobstores.
> The bdev layer allows each bdev to be claimed once by one bdev module at a time. The first time that
> a bdev is opened for an external snapshot, a read-only claim is established via a call to
> `spdk_bdev_module_claim_bdev()` using the module passed to `spdk_bdev_blob_ro_claim()`..
>
> Read-only claims for bdevs for all blobstores are tracked in an RB tree. Each node of the tree
> contains the bdev's UUID and the number of shared claims. When the number of claims drops to 0, the
> tree node is removed.
>
> Shared read-only claims are taken with `bdev_blob_ro_claim()`. This may fail due to memory
> allocation for the first claim on a bdev or for any of the reasons that
> `spdk_bdev_module_claim_bdev()` mail fail. Claims are released via `bdev_blob_ro_release()`.
>
> ```
> static int bdev_blob_ro_claim(struct spdk_bdev *bdev);
> static void bdev_blob_ro_release(struct spdk_bdev *bdev);
> ```
