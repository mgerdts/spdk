The esnap IO channel tree needs to be moved into blobstore.c.  As described in
[blob_esnap.md](../doc/blob_esnap.md):

> #### External Snapshot IO Channel Context
>
> An external snapshot device requires an IO channel per thread per bdev and as such must define
> `io_channel_get` and `io_channel_put` callbacks. As reads occur, a per thread cache of IO channels
> will be maintained so that IO channels for external snapshots may be obtained without taking locks.
>
> The existing `struct spdk_bs_channel` is extended by adding an RB tree. It is initialized in
> `bs_channel_create()`.
>
> XXX-mg Maybe a splay tree would be better: it is optimized for quick access of recently used items.
>
> ```c
> struct spdk_bs_channel {
>         /* ... (existing fields) */
>
>         RB_HEAD(, bs_esnap_channel)     esnap_channels;
> };
> ```
>
> Each node in the tree is:
>
> ```c
> struct bs_esnap_channel {
>         RB_ENTRY(bs_esnap_channel)      link;
>         struct spdk_blob_id             id;
>         struct spdk_io_channel          *channel;
> };
> ```
>
> Reads from an external snapshot device come only through `blob_request_submit_op_single()` or
> `blob_request_submit_rw_iov()`. In each case, the channel that will be used while reading from the
> external snapshot is found via `set->channel`, where `set` is a `struct bs_request_set` (aka
> `bs_sequence_t`, `spdk_bs_batch_t`).  In each case, the `set` is used to for a single read operation
> that is constrained to a range that falls within a blobstore cluster boundary.  Thus, it will be
> safe to simply update `set->channel` to reference the appropriate esnap channel prior to passing it
> to the esnap `read`, `readv`, or `readvext` method.
>
> When a blob is closed, the blobstore will use `spdk_for_each_channel()` to cause
> `back_bs_dev->destroy_channel()` to be called on each external snapshot IO channel associated with
> that blob. The external snapshot will be destroyed with `back_bs_dev->destroy()`.
>
> When `bs_channel_destroy()` is called, `back_bs_dev->destroy_channel()` will be called on each
> external snapshot IO channel found in the RB tree of the `struct spdk_bs_channel` that is being
> destroyed.
