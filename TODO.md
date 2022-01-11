# SPDK Logical Volume External Snapshot Support

This document records items left to implement to allow SPDK's volume manager to
support clones of external snapshots.

## Blobstore

### Consistent naming

- seed --> external snapshot
- external clone - simplified version of "clone of an external snapshot"

### Unavailability of external snapshot

- Allow blob to be opened in a degraded mode when the external snapshot is
  missing. Reads from and writes to blobstore-resident clusters can be serviced
  without problems.  IO (read, CoW) that requires access to the external
  snapshot should return `EIO` or similar.  Maybe it should go through retries
  with delays before returning an error.
- Handle surprise removal of external snapshot, transitioning the blob to
  degraded mode.
- Handle surprise add of external snapshot, transitioning from degraded to
  healthy.

### Misc

- Verify that when a snapshot of an external clone is created that the XATTR
  moves to the snapshot.  And then reverse it when the snapshot is deleted.
- Verify that when an external clone is inflated or decoupled that the XATTR is
  removed.
- Verify that storing the UUID in the blobstore is OK.  This seems better than
  a name like "spdk1n1", but maybe another scheme is needed to have names that
  are more consistent than which namespace on which target a particular thing
  exists.
- Assuming storing UUIDs is OK, add a RB tree for faster lookups and to ensure
  uniqueness.

## lvol

- Does lvol_get_xattr_value need to get the external snapshot name?  Since
  switching to an internal xattr this is probably broken.
- Add external snapshot tests

## vbdev_ro

### Surprise removal of base bdev

- The removal notice should be propagated to those that have the bdev open.
- The read-only bdev should be deleted.

### RPC

- Implement RPC calls
- Implement JSON dump for `save_config` support

### Other

- Implement JSON dump `bdev_get_bdevs` support
- Support resize of base bdev?

## Blob CLI

- Update docs to suggest use of vbdev_ro.
- Verify that changes don't affect existing tests.
