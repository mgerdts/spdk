# SPDK Logical Volume External Snapshot Support

This document records items left to implement to allow SPDK's volume manager to
support clones of external snapshots.

## Blobstore

### Consistent naming

- seed --> external snapshot
- external clone - simplified version of "clone of an external snapshot"

### Unavailability of external snapshot

- Handle surprise removal of external snapshot, transitioning the blob to
  degraded mode.
- Handle surprise add of external snapshot, transitioning from degraded to
  healthy.

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

## Blob CLI

- Update docs to suggest use of vbdev_ro.
- Verify that changes don't affect existing tests.
