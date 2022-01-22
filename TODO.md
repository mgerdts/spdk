# SPDK Logical Volume External Snapshot Support

This document records items left to implement to allow SPDK's volume manager to
support clones of external snapshots.

## Blobstore

### Unavailability of external snapshot

- Make external snapshot health visible via an RPC call.

### Misc

- Add a RB tree for faster lookups and to ensure uniqueness of bdev uuids.

## lvol

- Does lvol_get_xattr_value need to get the external snapshot name?  Since
  switching to an internal xattr this is probably broken.
- Add external snapshot tests

## vbdev_ro

### RPC

- Implement RPC calls
- Implement JSON dump for `save_config` support

### Other

- Use ext bdev API in IO path, like in [this change to
  part.c](https://review.spdk.io/gerrit/c/spdk/spdk/+/11048/1/lib/bdev/part.c).
- Implement JSON dump `bdev_get_bdevs` support
- Support resize of base bdev?


## vbev_wait

- Write unit tests
- Update documentation
- Sort out behavior when the desired bdev already exists or comes into
  existence just as the wait bdev is created.
- Consider collecting stats on number of io_submit calls.

## Blob CLI

- Update docs to suggest use of vbdev_ro.
- Verify that changes don't affect existing tests.
