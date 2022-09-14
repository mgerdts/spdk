A typical gpt bdev looks like:

```json
  {
    "name": "hostn2p10",
    "aliases": [
      "57c44049-2315-4b82-9483-f546951dcb3b"
    ],
    "product_name": "GPT Disk",
    "block_size": 512,
    "num_blocks": 20971520,
    "uuid": "57c44049-2315-4b82-9483-f546951dcb3b",
    ...,
    "driver_specific": {
      "gpt": {
        "base_bdev": "hostn2",
        "offset_blocks": 2099200,
        "partition_type_guid": "7c5222bd-8f5d-4087-9c00-bf9843c7b58c",
        "unique_partition_guid": "5c0d6c97-2ef6-48f0-8fd3-fa109188136f",
        "partition_name": "Unknown"
      }
    }
  }
```

Every time a gpt bdev is discovered it gets a new uuid. Rather than having
ephemeral uuids, it should use the unique partition guid as the uuid.

Benefits:

1. For base bdevs (such as nvme) where names are partially at the whim of
   enumeration order, this will provide a durable alias to use with
   `spdk_bdev_open()`.
2. For base bdevs (such as aio) that have no stable storage for a uuid, an SPDK
   partition can be created to provide a durable alias.
