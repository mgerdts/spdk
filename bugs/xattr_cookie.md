`BLOB_EXTERNAL_SNAPSHOT_BDEV` needs to be renamed `BLOB_EXTERNAL_SNAPSHOT_COOKIE` ("EXTSNAP_COOKIE").

This becomes an opaque value that is passed to `external_bs_dev_create` while loading a blob.
