If an lvstore loads before an esnap bdev is loaded (due to order or transient
problem), the esnap bdev never loads.

There are two potential paths to a fix:

1. Keep the lvol closed until the esnap bdev appears.  No reads or writes are
   allowed until the esnap device is available.
2. Open the lvol in a degraded mode.  Only those IOs that require reads from the
   esnap device fail with `EIO`.

In either case, the blobstore needs to communicate the missing device to the
lvstore so that the lvstore knows that it needs to watch for the device so that
it can retry the open.  The notification needs to happen in a way that is
resistant to races.
