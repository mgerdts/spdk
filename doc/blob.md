# Blobstore Programmer's Guide {#blob}

## In this document {#blob_pg_toc}

* @ref blob_pg_audience
* @ref blob_pg_intro
* @ref blob_pg_theory
* @ref blob_pg_design
* @ref blob_pg_examples
* @ref blob_pg_config
* @ref blob_pg_component

## Target Audience {#blob_pg_audience}

The programmer's guide is intended for developers authoring applications that utilize the SPDK Blobstore. It is
intended to supplement the source code in providing an overall understanding of how to integrate Blobstore into
an application as well as provide some high level insight into how Blobstore works behind the scenes. It is not
intended to serve as a design document or an API reference and in some cases source code snippets and high level
sequences will be discussed; for the latest source code reference refer to the [repo](https://github.com/spdk).

## Introduction {#blob_pg_intro}

Blobstore is a persistent, power-fail safe block allocator designed to be used as the local storage system
backing a higher level storage service, typically in lieu of a traditional filesystem. These higher level services
can be local databases or key/value stores (MySQL, RocksDB), they can be dedicated appliances (SAN, NAS), or
distributed storage systems (ex. Ceph, Cassandra). It is not designed to be a general purpose filesystem, however,
and it is intentionally not POSIX compliant. To avoid confusion, we avoid references to files or objects instead
using the term 'blob'. The Blobstore is designed to allow asynchronous, uncached, parallel reads and writes to
groups of blocks on a block device called 'blobs'. Blobs are typically large, measured in at least hundreds of
kilobytes, and are always a multiple of the underlying block size.

The Blobstore is designed primarily to run on "next generation" media, which means the device supports fast random
reads and writes, with no required background garbage collection. However, in practice the design will run well on
NAND too.

## Theory of Operation {#blob_pg_theory}

### Abstractions

The Blobstore defines a hierarchy of storage abstractions as follows.

* **Logical Block**: Logical blocks are exposed by the disk itself, which are numbered from 0 to N, where N is the
  number of blocks in the disk. A logical block is typically either 512B or 4KiB.
* **Page**: A page is defined to be a fixed number of logical blocks defined at Blobstore creation time. The logical
  blocks that compose a page are always contiguous. Pages are also numbered from the beginning of the disk such
  that the first page worth of blocks is page 0, the second page is page 1, etc. A page is typically 4KiB in size,
  so this is either 8 or 1 logical blocks in practice. The SSD must be able to perform atomic reads and writes of
  at least the page size.
* **Cluster**: A cluster is a fixed number of pages defined at Blobstore creation time. The pages that compose a cluster
  are always contiguous. Clusters are also numbered from the beginning of the disk, where cluster 0 is the first cluster
  worth of pages, cluster 1 is the second grouping of pages, etc. A cluster is typically 4MiB in size, or 1024 pages.
* **Blob**: A blob is an ordered list of clusters. Blobs are manipulated (created, sized, deleted, etc.) by the application
  and persist across power failures and reboots. Applications use a Blobstore provided identifier to access a particular blob.
  Blobs are read and written in units of pages by specifying an offset from the start of the blob. Applications can also
  store metadata in the form of key/value pairs with each blob which we'll refer to as xattrs (extended attributes).
* **Blobstore**: An SSD which has been initialized by a Blobstore-based application is referred to as "a Blobstore." A
  Blobstore owns the entire underlying device which is made up of a private Blobstore metadata region and the collection of
  blobs as managed by the application.

```text
+-----------------------------------------------------------------+
|                              Blob                               |
| +-----------------------------+ +-----------------------------+ |
| |           Cluster           | |           Cluster           | |
| | +----+ +----+ +----+ +----+ | | +----+ +----+ +----+ +----+ | |
| | |Page| |Page| |Page| |Page| | | |Page| |Page| |Page| |Page| | |
| | +----+ +----+ +----+ +----+ | | +----+ +----+ +----+ +----+ | |
| +-----------------------------+ +-----------------------------+ |
+-----------------------------------------------------------------+
```

### Atomicity

For all Blobstore operations regarding atomicity, there is a dependency on the underlying device to guarantee atomic
operations of at least one page size. Atomicity here can refer to multiple operations:

* **Data Writes**: For the case of data writes, the unit of atomicity is one page. Therefore if a write operation of
  greater than one page is underway and the system suffers a power failure, the data on media will be consistent at a page
  size granularity (if a single page were in the middle of being updated when power was lost, the data at that page location
  will be as it was prior to the start of the write operation following power restoration.)
* **Blob Metadata Updates**: Each blob has its own set of metadata (xattrs, size, etc). For performance reasons, a copy of
  this metadata is kept in RAM and only synchronized with the on-disk version when the application makes an explicit call to
  do so, or when the Blobstore is unloaded. Therefore, setting of an xattr, for example is not consistent until the call to
  synchronize it (covered later) which is, however, performed atomically.
* **Blobstore Metadata Updates**: Blobstore itself has its own metadata which, like per blob metadata, has a copy in both
  RAM and on-disk. Unlike the per blob metadata, however, the Blobstore metadata region is not made consistent via a blob
  synchronization call, it is only synchronized when the Blobstore is properly unloaded via API. Therefore, if the Blobstore
  metadata is updated (blob creation, deletion, resize, etc.) and not unloaded properly, it will need to perform some extra
  steps the next time it is loaded which will take a bit more time than it would have if shutdown cleanly, but there will be
  no inconsistencies.

### Callbacks

Blobstore is callback driven; in the event that any Blobstore API is unable to make forward progress it will
not block but instead return control at that point and make a call to the callback function provided in the API, along with
arguments, when the original call is completed. The callback will be made on the same thread that the call was made from, more on
threads later. Some API, however, offer no callback arguments; in these cases the calls are fully synchronous. Examples of
asynchronous calls that utilize callbacks include those that involve disk IO, for example, where some amount of polling
is required before the IO is completed.

### Backend Support

Blobstore requires a backing storage device that can be integrated using the `bdev` layer, or by directly integrating a
device driver to Blobstore. The blobstore performs operations on a backing block device by calling function pointers
supplied to it at initialization time. For convenience, an implementation of these function pointers that route I/O
to the bdev layer is available in `bdev_blob.c`.  Alternatively, for example, the SPDK NVMe driver may be directly integrated
bypassing a small amount of `bdev` layer overhead. These options will be discussed further in the upcoming section on examples.

### Metadata Operations

Because Blobstore is designed to be lock-free, metadata operations need to be isolated to a single
thread to avoid taking locks on in memory data structures that maintain data on the layout of definitions of blobs (along
with other data). In Blobstore this is implemented as `the metadata thread` and is defined to be the thread on which the
application makes metadata related calls on. It is up to the application to setup a separate thread to make these calls on
and to assure that it does not mix relevant IO operations with metadata operations even if they are on separate threads.
This will be discussed further in the Design Considerations section.

### Threads

An application using Blobstore with the SPDK NVMe driver, for example, can support a variety of thread scenarios.
The simplest would be a single threaded application where the application, the Blobstore code and the NVMe driver share a
single core. In this case, the single thread would be used to submit both metadata operations as well as IO operations and
it would be up to the application to assure that only one metadata operation is issued at a time and not intermingled with
affected IO operations.

### Channels

Channels are an SPDK-wide abstraction and with Blobstore the best way to think about them is that they are
required in order to do IO.  The application will perform IO to the channel and channels are best thought of as being
associated 1:1 with a thread.

### Blob Identifiers

When an application creates a blob, it does not provide a name as is the case with many other similar
storage systems, instead it is returned a unique identifier by the Blobstore that it needs to use on subsequent APIs to
perform operations on the Blobstore.

## Design Considerations {#blob_pg_design}

### Initialization Options

When the Blobstore is initialized, there are multiple configuration options to consider. The
options and their defaults are:

* **Cluster Size**: By default, this value is 4MiB. The cluster size is required to be a multiple of page size and should be
  selected based on the application’s usage model in terms of allocation. Recall that blobs are made up of clusters so when
  a blob is allocated/deallocated or changes in size, disk LBAs will be manipulated in groups of cluster size.  If the
  application is expecting to deal with mainly very large (always multiple GB) blobs then it may make sense to change the
  cluster size to 1GB for example.
* **Number of Metadata Pages**: By default, Blobstore will assume there can be as many clusters as there are metadata pages
  which is the worst case scenario in terms of metadata usage and can be overridden here however the space efficiency is
  not significant.
* **Maximum Simultaneous Metadata Operations**: Determines how many internally pre-allocated memory structures are set
  aside for performing metadata operations. It is unlikely that changes to this value (default 32) would be desirable.
* **Maximum Simultaneous Operations Per Channel**: Determines how many internally pre-allocated memory structures are set
  aside for channel operations. Changes to this value would be application dependent and best determined by both a knowledge
  of the typical usage model, an understanding of the types of SSDs being used and empirical data. The default is 512.
* **Blobstore Type**: This field is a character array to be used by applications that need to identify whether the
  Blobstore found here is appropriate to claim or not. The default is NULL and unless the application is being deployed in
  an environment where multiple applications using the same disks are at risk of inadvertently using the wrong Blobstore, there
  is no need to set this value. It can, however, be set to any valid set of characters.

### Sub-page Sized Operations

Blobstore is only capable of doing page sized read/write operations. If the application
requires finer granularity it will have to accommodate that itself.

### Threads

As mentioned earlier, Blobstore can share a single thread with an application or the application
can define any number of threads, within resource constraints, that makes sense.  The basic considerations that must be
followed are:

* Metadata operations (API with MD in the name) should be isolated from each other as there is no internal locking on the
   memory structures affected by these API.
* Metadata operations should be isolated from conflicting IO operations (an example of a conflicting IO would be one that is
  reading/writing to an area of a blob that a metadata operation is deallocating).
* Asynchronous callbacks will always take place on the calling thread.
* No assumptions about IO ordering can be made regardless of how many or which threads were involved in the issuing.

### Data Buffer Memory

As with all SPDK based applications, Blobstore requires memory used for data buffers to be allocated
with SPDK API.

### Error Handling

Asynchronous Blobstore callbacks all include an error number that should be checked; non-zero values
indicate an error. Synchronous calls will typically return an error value if applicable.

### Asynchronous API

Asynchronous callbacks will return control not immediately, but at the point in execution where no
more forward progress can be made without blocking.  Therefore, no assumptions can be made about the progress of
an asynchronous call until the callback has completed.

### Xattrs

Setting and removing of xattrs in Blobstore is a metadata operation, xattrs are stored in per blob metadata.
Therefore, xattrs are not persisted until a blob synchronization call is made and completed. Having a step process for
persisting per blob metadata allows for applications to perform batches of xattr updates, for example, with only one
more expensive call to synchronize and persist the values.

### Synchronizing Metadata

As described earlier, there are two types of metadata in Blobstore, per blob and one global
metadata for the Blobstore itself.  Only the per blob metadata can be explicitly synchronized via API. The global
metadata will be inconsistent during run-time and only synchronized on proper shutdown. The implication, however, of
an improper shutdown is only a performance penalty on the next startup as the global metadata will need to be rebuilt
based on a parsing of the per blob metadata. For consistent start times, it is important to always close down the Blobstore
properly via API.

### Iterating Blobs

Multiple examples of how to iterate through the blobs are included in the sample code and tools.
Worthy to note, however, if walking through the existing blobs via the iter API, if your application finds the blob its
looking for it will either need to explicitly close it (because was opened internally by the Blobstore) or complete walking
the full list.

### The Super Blob

The super blob is simply a single blob ID that can be stored as part of the global metadata to act
as sort of a "root" blob. The application may choose to use this blob to store any information that it needs or finds
relevant in understanding any kind of structure for what is on the Blobstore.

## Examples {#blob_pg_examples}

There are multiple examples of Blobstore usage in the [repo](https://github.com/spdk/spdk):

* **Hello World**: Actually named `hello_blob.c` this is a very basic example of a single threaded application that
  does nothing more than demonstrate the very basic API. Although Blobstore is optimized for NVMe, this example uses
  a RAM disk (malloc) back-end so that it can be executed easily in any development environment. The malloc back-end
  is a `bdev` module thus this example uses not only the SPDK Framework but the `bdev` layer as well.

* **CLI**: The `blobcli.c` example is command line utility intended to not only serve as example code but as a test
  and development tool for Blobstore itself. It is also a simple single threaded application that relies on both the
  SPDK Framework and the `bdev` layer but offers multiple modes of operation to accomplish some real-world tasks. In
  command mode, it accepts single-shot commands which can be a little time consuming if there are many commands to
  get through as each one will take a few seconds waiting for DPDK initialization. It therefore has a shell mode that
  allows the developer to get to a `blob>` prompt and then very quickly interact with Blobstore with simple commands
  that include the ability to import/export blobs from/to regular files. Lastly there is a scripting mode to automate
  a series of tasks, again, handy for development and/or test type activities.

## Configuration {#blob_pg_config}

Blobstore configuration options are described in the initialization options section under @ref blob_pg_design.

## Component Detail {#blob_pg_component}

The information in this section is not necessarily relevant to designing an application for use with Blobstore, but
understanding a little more about the internals may be interesting and is also included here for those wanting to
contribute to the Blobstore effort itself.

### Media Format

The Blobstore owns the entire storage device. The device is divided into clusters starting from the beginning, such
that cluster 0 begins at the first logical block.

```text
LBA 0                                   LBA N
+-----------+-----------+-----+-----------+
| Cluster 0 | Cluster 1 | ... | Cluster N |
+-----------+-----------+-----+-----------+
```

Cluster 0 is special and has the following format, where page 0 is the first page of the cluster:

```text
+--------+-------------------+
| Page 0 | Page 1 ... Page N |
+--------+-------------------+
| Super  |  Metadata Region  |
| Block  |                   |
+--------+-------------------+
```

The super block is a single page located at the beginning of the partition. It contains basic information about
the Blobstore. The metadata region is the remainder of cluster 0 and may extend to additional clusters. Refer
to the latest source code for complete structural details of the super block and metadata region.

Each blob is allocated a non-contiguous set of pages inside the metadata region for its metadata. These pages
form a linked list. The first page in the list will be written in place on update, while all other pages will
be written to fresh locations. This requires the backing device to support an atomic write size greater than
or equal to the page size to guarantee that the operation is atomic. See the section on atomicity for details.

### Blob cluster layout {#blob_pg_cluster_layout}

Each blob is an ordered list of clusters, where starting LBA of a cluster is called extent. A blob can be
thin provisioned, resulting in no extent for some of the clusters. When first write operation occurs
to the unallocated cluster - new extent is chosen. This information is stored in RAM and on-disk.

There are two extent representations on-disk, dependent on `use_extent_table` (default:true) opts used
when creating a blob.

* **use_extent_table=true**: EXTENT_PAGE descriptor is not part of linked list of pages. It contains extents
  that are not run-length encoded. Each extent page is referenced by EXTENT_TABLE descriptor, which is serialized
  as part of linked list of pages.  Extent table is run-length encoding all unallocated extent pages.
  Every new cluster allocation updates a single extent page, in case when extent page was previously allocated.
  Otherwise additionally incurs serializing whole linked list of pages for the blob.

* **use_extent_table=false**: EXTENT_RLE descriptor is serialized as part of linked list of pages.
  Extents pointing to contiguous LBA are run-length encoded, including unallocated extents represented by 0.
  Every new cluster allocation incurs serializing whole linked list of pages for the blob.

### Thin Blobs, Snapshots, and Clones

Each in-use cluster is allocated to blobstore metadata or to a particular blob. Once a cluster is allocated to
a blob it is considered owned by that blob and that particular blob's metadata maintains a reference to the
cluster as a record of ownership. Cluster ownership is transferred during snapshot operations described later
in @ref blob_pg_snapshots.

Through the use of thin provisioning, snapshots, and/or clones, a blob may be backed by mixture of clusters it
owns and those owned by one or more other blobs. The behavior of reads and writes depend on whether the
operation targets pages that are backed by a cluster owned by the blob or not.

* **read from owned cluster**: The read is serviced by reading directly from the appropriate cluster.
* **read from other clusters**: The read is passed on to the blob's *back device* and the back device services
  the read.
* **write to an owned cluster**: The write is serviced by writing directly to the appropriate cluster.
* **wrote to other clusters**: A copy-on-write operation is triggered. This causes allocation of a cluster to
  the blob, resulting in ownership of a cluster into which the write can happen.

#### Copy-on-write {#blob_pg_copy_on_write}

A copy-on-write operation is somewhat expensive. The cost of the operation is affected more by the cluster
size, than the number of pages in the write operation that triggered it.  Each copy-on-write involves the
following steps:

1. Allocate a cluster. This requires an update to an on-disk bit array that follows the blobstore super block.
2. Allocate a cluster-sized buffer into which data can be read.
3. Trigger a full-cluster read from the back device into the cluster-sized buffer.
4. Write from the cluster-sized buffer into the newly allocated cluster.
5. Update the blob's on-disk metadata to record ownership of the newly allocated cluster. This involves at
   least one page-sized writes.
6. Write the new data to the just allocated and copied cluster.

### Thin Provisioning {#blob_pg_thin_provisioning}

As mentioned in @ref blob_pg_cluster_layout, a blob may be thin provisioned. A thin provisioned blob starts
out with no allocated clusters. Clusters are allocated as writes occur. A thin provisioned blob's back
device is a *zeroes device*. A read from a zeroes device fills the read buffer with zeroes.

Because all unallocated clusters are zeroed and the zeroes device will only write zeroes into the requested
buffer, thin provisioning takes a short cut during copy-on-write.  That is:

1. Allocate a cluster. This requires an update to an on-disk bit array that follows the blobstore super block.
2. Update the blob's on-disk metadata to record ownership of the newly allocated cluster. This involves at
   least one page-sized writes.
3. Write the new data to the just allocated cluster.

As an example, consider a 3 MiB blob on a bob store that uses 1 MiB clusters. When it is first created, all
clusters are backed by the zeroes device.

```text
+-----------------------+    +---------------+
|      Blob 1 (rw)      |    | Zeroes Device |
|-----------------------|    |---------------|
| Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|    |---------------|
|   [0 - 1)   |~~~~~~~~~~~~~>| 0000000000... |
|   [1 - 2)   |~~~~~~~~~~~~~>| 0000000000... |
|   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+    +---------------+

  Free clusters: 42, 53, 68 - 1000
```

When a 4 KiB write occurs at offset 1280 KiB, a cluster is allocated for the second 1 MiB chunk of blob.

```text
+-----------------------+    +---------------+
|      Blob 1 (rw)      |    | Zeroes Device |
|-----------------------|    |---------------|
| Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|    |---------------|
|   [0 - 1)   |~~~~~~~~~~~~~>| 0000000000... |
|   [1 - 2)   |   42    |    | 0000000000... |
|   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+    +---------------+

  Free clusters: 53, 68 - 1000
```

Now when any read or write is performed in the range [1 MiB - 2 MiB), it is performed within the confines of
cluster 42, which lies at offset [42 MiB - 43 MiB) on the device that contains the blobstore. All other reads
are still serviced by the zeroes device.

### Snapshots {#blob_pg_snapshots}

A snapshot is a special blob that is a read-only point-in-time representation of another non-snapshot blob.
There may be many snapshots of a blob. When a snapshot is created, ownership of the blob's clusters is
transferred to the snapshot.

Starting from the thin provisioned blob from the previous example, before a snapshot the blob looks like:

```text
+-----------------------+    +---------------+
|      Blob 1 (rw)      |    | Zeroes Device |
|-----------------------|    |---------------|
| Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|    |---------------|
|   [0 - 1)   |~~~~~~~~~~~~~>| 0000000000... |
|   [1 - 2)   |   42    |    | 0000000000... |
|   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+    +---------------+

  Free clusters: 53, 68 - 1000
```

When a snapshot is taken the blobstore now has:

```text
+-----------------------+    +-----------------------+    +---------------+
|      Blob 1 (rw)      |    |    Snapshot 1 (ro)    |    | Zeroes Device |
|-----------------------|    |-----------------------|    |---------------|
| Range (MiB) | Cluster |    | Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|    |-------------|---------|    |---------------|
|   [0 - 1)   |~~~~~~~~~~~~~>|   [0 - 1)   |~~~~~~~~~~~~~>| 0000000000... |
|   [1 - 2)   |~~~~~~~~~~~~~>|   [1 - 2)   |   42    |    | 0000000000... |
|   [2 - 3)   |~~~~~~~~~~~~~>|   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+    +-----------------------+    +---------------+

  Free clusters: 53, 68 - 1000
```

The diagram above shows the following behavior for reads:

* A read from Blob 1 in the [0 MiB - 1 MiB) or [2 MiB - 3 MiB) ranges triggers a read from the same range in
  Snapshot 1, described in the next bullet.
* A read from Snapshot 1 in the [0 MiB - 1 MiB) or [2 MiB - 3 MiB) ranges triggers a read from the zeroes
  device.
* Any write to Snapshot 1 fails with `EPERM`.
* Any write to Blob 1 triggers copy-on-write, even when the snapshot is backed by the zeroes device. See
  @ref blob_pg_copy_on_write.

At this point, Blob 1 could be removed and Snapshot 1 will survive.
   XXX-mg verify this is true.

If the first 1.5 MiB of the blob were overwritten, two new clusters would be allocated, copied, and written
across a series of metadata and data operations. After these complete, Blob 1 would look like:

```text
+-----------------------+    +-----------------------+    +---------------+
|      Blob 1 (rw)      |    |    Snapshot 1 (ro)    |    | Zeroes Device |
|-----------------------|    |-----------------------|    |---------------|
| Range (MiB) | Cluster |    | Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|    |-------------|---------|    |---------------|
|   [0 - 1)   |    53   |    |   [0 - 1)   |~~~~~~~~~~~~~>| 0000000000... |
|   [1 - 2)   |    68   |    |   [1 - 2)   |   42    |    | 0000000000... |
|   [2 - 3)   |~~~~~~~~~~~~~>|   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+    +-----------------------+    +---------------+

  Free clusters: 69 - 1000
```

If Snapshot 1 is deleted, the last chunk of Blob 1 references the zeroes device and cluster 42 is returned to
the free list.

```text
+-----------------------+    +---------------+
|      Blob 1 (rw)      |    | Zeroes Device |
|-----------------------|    |---------------|
| Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|    |---------------|
|   [0 - 1)   |    53   |    | 0000000000... |
|   [1 - 2)   |    68   |    | 0000000000... |
|   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+    +---------------+

  Free clusters: 42, 69 - 1000
```

If instead of deleting Snapshot 1, Blob 1 were deleted the snapshot would survive.

```text
+-----------------------+    +---------------+
|    Snapshot 1 (ro)    |    | Zeroes Device |
|-----------------------|    |---------------|
| Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|    |---------------|
|   [0 - 1)   |~~~~~~~~~~~~~>| 0000000000... |
|   [1 - 2)   |   42    |    | 0000000000... |
|   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+    +---------------+

  Free clusters: 53, 68 - 1000
```

### Clones {#blob_pg_clones}

Through the process of discussing snapshots, clones have largely already been covered. This is because the
snapshot of Blob 1 turned Blob 1 into a clone. The key concept that has not been covered is having many clones
of a single snapshot.

This diagram describes the state just after Snapshot 1 was created:

```text
+-----------------------+    +-----------------------+    +---------------+
|      Blob 1 (rw)      |    |    Snapshot 1 (ro)    |    | Zeroes Device |
|-----------------------|    |-----------------------|    |---------------|
| Range (MiB) | Cluster |    | Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|    |-------------|---------|    |---------------|
|   [0 - 1)   |~~~~~~~~~~~~~>|   [0 - 1)   |~~~~~~~~~~~~~>| 0000000000... |
|   [1 - 2)   |~~~~~~~~~~~~~>|   [1 - 2)   |   42    |    | 0000000000... |
|   [2 - 3)   |~~~~~~~~~~~~~>|   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+    +-----------------------+    +---------------+

  Free clusters: 53, 68 - 1000
```

Another blob can be created as a clone of Snapshot 1, resulting in:

```text
+-----------------------+           +-----------------------+    +---------------+
|      Blob 1 (rw)      |           |    Snapshot 1 (ro)    |    | Zeroes Device |
|-----------------------|           |-----------------------|    |---------------|
| Range (MiB) | Cluster |           | Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|           |-------------|---------|    |---------------|
|   [0 - 1)   |~~~~~~~~~~~~~+~~~~~~>|   [0 - 1)   |~~~~~~~~~~~~~>| 0000000000... |
|   [1 - 2)   |~~~~~~~~~~~~~|~+~~~~>|   [1 - 2)   |   42    |    | 0000000000... |
|   [2 - 3)   |~~~~~~~~~~~~~|~|~+~~>|   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+   | | |   +-----------------------+    +---------------+
                            | | |
                            | | |
+-----------------------+   | | |
|      Blob 2 (rw)      |   | | |
|-----------------------|   | | |
| Range (MiB) | Cluster |   | | |
|-------------|---------|   | | |
|   [0 - 1)   |~~~~~~~~~~~~~' | |
|   [1 - 2)   |~~~~~~~~~~~~~~~' |
|   [2 - 3)   |~~~~~~~~~~~~~~~~~'
+-----------------------+

  Free clusters: 53, 68 - 1000
```

While both Blob 1 and Blob 2 reference data in Snapshot 1, writes to each of these blobs will be contained
within the blob that was written.

Clone creation could be repeated an arbitrary number of times to create as many clones of Snapshot 1 as
needed. Notice how each clone directly references Snapshot 1. If instead of cloning Snapshot 1 repeatedly, a
new snapshot of Blob 1 were created for each clone, the chain from the most recent clone to the data contained
in Snapshot 1 and the zeroes device would grow arbitrarily long.

### Inflation and Decoupling

Inflation and decoupling are two similar mechanisms for dissociating a clone from its snapshot. Other systems
refer to similar concepts as *clone hydration*.

Operation | Dissociate From Snapshot | Thin Provisioned
--------- | ------------------------ | -----------------------------------
Inflate   | Yes                      | No
Decouple  | Yes                      | If snapshot referenced zeroes device

Starting with the state from the previous example, suppose Blob 1 is inflated and Blob 2 is decoupled.  The
result would be:

```text
+-----------------------+       +-----------------------+    +---------------+
|      Blob 1 (rw)      |       |    Snapshot 1 (ro)    |    | Zeroes Device |
|-----------------------|       |-----------------------|    |---------------|
| Range (MiB) | Cluster |       | Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|       |-------------|---------|    |---------------|
|   [0 - 1)   |   53    |       |   [0 - 1)   |~~~~~~~~~~~~~>| 0000000000... |
|   [1 - 2)   |   68    |       |   [1 - 2)   |   42    |    | 0000000000... |
|   [2 - 3)   |   69    |       |   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+       +-----------------------+    +---------------+


+-----------------------+    +---------------+
|      Blob 2 (rw)      |    | Zeroes Device |
|-----------------------|    |---------------|
| Range (MiB) | Cluster |    | Virtual Data  |
|-------------|---------|    |---------------|
|   [0 - 1)   |~~~~~~~~~~~~~>| 0000000000... |
|   [1 - 2)   |   70    |    | 0000000000... |
|   [2 - 3)   |~~~~~~~~~~~~~>| 0000000000... |
+-----------------------+    +---------------+

  Free clusters: 71 - 1000
```

### External snapshots

The previous sections described scenarios in which all data exists within the blobstore. In some situations,
there may be read-only data that resides outside of the blobstore that needs to be presented as read-write
within the blobstore. An example use case is when the SPDK volume manager is used to create an ephemeral clone
of a virtual machine disk image. A golden disk image that resides on an NVMe-oF target can be cloned to allow
writes to stay local to the SPDK instance.

The diagram below illustrates Blob 1 as a clone of an NVMe-oF bdev, nvme1n1.

```text
,------------------------- SPDK -----------------------.
|                                                      |
|  ,--------- Blobstore ----------.                    |     ,--- Remote Target ---.
|  |                              |                    |     |                     |
|  |   +-----------------------+  |  +--------------+  |     |   +--------------+  |
|  |   |      Blob 1 (rw)      |  |  | nvme1n1 (ro) |  |     |   |   Device     |  |
|  |   |-----------------------|  |  |--------------|  |     |   |--------------|  |
|  |   | Range (MiB) | Cluster |  |  |  Range (MiB) |  |     |   | Range (MiB)  |  |
|  |   |-------------|---------|  |  |--------------|  |     |   |--------------|  |
|  |   |   [0 - 1)   |~~~~~~~~~~~~~~>|    [0 - 1)   |~~~~~~~~~~~>|   [0 - 1)    |  |
|  |   |   [1 - 2)   |~~~~~~~~~~~~~~>|    [1 - 2)   |~~~~~~~~~~~>|   [1 - 2)    |  |
|  |   |   [2 - 3)   |~~~~~~~~~~~~~~>|    [2 - 3)   |~~~~~~~~~~~>|   [2 - 3)    |  |
|  |   +-----------------------+  |  +--------------+  |     |   +--------------+  |
|  |                              |                    |     |                     |
|  `------------------------------'                    |     `---------------------'
`---------------------------------=--------------------'
```

When Blob 1 is read before a write to that cluster, the read will be serviced by nvme1n1, which will make the
invoke the appropriate methods to perform a read from the remote device. Writes are handled like writes to any
other clone. Likewise, inflate and decouple work as they would for any other clone based on a snapshot that
has no clusters backed by a zeroes device.

Of course, any device known to SPDK may be used as an external snapshot.

### Sequences and Batches

Internally Blobstore uses the concepts of sequences and batches to submit IO to the underlying device in either
a serial fashion or in parallel, respectively. Both are defined using the following structure:

~~~{.sh}
struct spdk_bs_request_set;
~~~

These requests sets are basically bookkeeping mechanisms to help Blobstore efficiently deal with related groups
of IO. They are an internal construct only and are pre-allocated on a per channel basis (channels were discussed
earlier). They are removed from a channel associated linked list when the set (sequence or batch) is started and
then returned to the list when completed.

### Key Internal Structures

`blobstore.h` contains many of the key structures for the internal workings of Blobstore. Only a few notable ones
are reviewed here.  Note that `blobstore.h` is an internal header file, the header file for Blobstore that defines
the public API is `blob.h`.

~~~{.sh}
struct spdk_blob
~~~
This is an in-memory data structure that contains key elements like the blob identifier, its current state and two
copies of the mutable metadata for the blob; one copy is the current metadata and the other is the last copy written
to disk.

~~~{.sh}
struct spdk_blob_mut_data
~~~
This is a per blob structure, included the `struct spdk_blob` struct that actually defines the blob itself. It has the
specific information on size and makeup of the blob (ie how many clusters are allocated for this blob and which ones.)

~~~{.sh}
struct spdk_blob_store
~~~
This is the main in-memory structure for the entire Blobstore. It defines the global on disk metadata region and maintains
information relevant to the entire system - initialization options such as cluster size, etc.

~~~{.sh}
struct spdk_bs_super_block
~~~
The super block is an on-disk structure that contains all of the relevant information that's in the in-memory Blobstore
structure just discussed along with other elements one would expect to see here such as signature, version, checksum, etc.

### Code Layout and Common Conventions

In general, `Blobstore.c` is laid out with groups of related functions blocked together with descriptive comments. For
example,

~~~{.sh}
/* START spdk_bs_md_delete_blob */
< relevant functions to accomplish the deletion of a blob >
/* END spdk_bs_md_delete_blob */
~~~

And for the most part the following conventions are followed throughout:

* functions beginning with an underscore are called internally only
* functions or variables with the letters `cpl` are related to set or callback completions
