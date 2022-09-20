The lvol json from `bdev_get_bdevs` needs to be updated as:

> #### Lookup of lvols
>
> An lvol that is missing its external snapshot is known as a degraded external clone.  It is found
> the same way that any other lvol is found. When viewed through the `bdev_get_bdevs` RPC call, the
> `lvol` `driver_specific` data will resemble:
>
> ```json
>     "driver_specific": {
>       "lvol": {
>         ...
>         "clone": false,
>         "external_clone": true,
>         "external_clone_degraded": true,
>         "external_snapshot_name": "some_name_or_uuid"
>       }
>     }
> ```
