When a blob's esnap bdev is missing, the blob exists but is invisible and not removable.

We need:

- `bdev_lvol_get_lvols` needs to be added, listing healthy and unhealthy lvols.
- `bdev_lvol_delete` needs to be able to delete unhealthy lvols.
