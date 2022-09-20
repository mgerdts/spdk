This problem is described in the commit message for:

ff80eb92177 lvol: add test_esnap_partial_last_cluster

As stated in [blob_esnap.md](../docs/blob_esnap.md) there will initially be a restriction to avoid this problem:

> As part of the initial implementation, the size of the external snapshot bdev must be evenly
> divisible by the blobstore's cluster size.
