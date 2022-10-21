# Shared Claims

This document is written to facilitate discussion of a proposal. It is not expected that this file
will be merged, but the information contained in it should find its way into appropriate API
documentation and other documentation.

## Problem Statement

To support [external snapshots](https://review.spdk.io/gerrit/c/spdk/spdk/+/11643), vbdev_lvol needs
read-only claims on the other bdevs that act as external snapshots. It is expected that there may be
many clones of any external snapshot bdev. Claims need to be managed in such a way that writers are
blocked while a bdev is an external snapshot.

The bdev layer currently has no means to support shared claims, leaving it up to a vbdev module to
do its own reference counting in situations such as this. Consumers of
[lib/bdev/part.c](../lib/bdev/part.c), such as vbdev_gpt, vbdev_opal, and vbdev_split rely on the
vbdev module to reference count multiple vbdevs that may each need a claim on a shared underlying
bdev.

The bdev layer assumes that any vbdev that requires a claim wants exclusive write access. When a
bdev descriptor with `write` set to `false` is passed to `spdk_bdev_module_claim_bdev()`, the bdev
is "promoted to write-exclusive" by setting `write` to `true`.

While a claim prevents future writers, an existing writer without a claim does not prevent the claim
from being granted.

## Proposal

The module claims interface is extended to allow shared claims, as follows:

 * Shared claims must be explicitly requested. Traditional claims will not automatically become
   shared.
 * A shared claim is granted so long as there are no conflicting descriptors open for writing and no
   conflicting claims.
   - read-only shared claim requests:
     * Must be passed a descriptor that is opened without write access
     * If there are any open descriptors with write access is a conflict.
     * Any traditional claim or shared read-write claim is a conflict.
   - read-write shared claim requests:
     * Must be passed a descriptor that that is opened with write access
     * Must include a shared claim key (pointer)
     * If there is an open read-write descriptor aside from the one passed with the request, it is a
       conflict.
     * Any traditional claim or shared read-only claim is a conflict.
     * Any shared read-write claim with a mismatched shared claim key is a conflict.
     * A read-only descriptor passed with an otherwise valid claim request is promoted to
       read-write.
 * Closing a descriptor releases all claims associated with that descriptor.
 * New API functions for requesting claims will take an options structure to allow future
   extensions.

Notably absent from the checks above is any limitation that all claims come from the same bdev
module. Shared read-only claims are generally safe across bdev modules. Shared read-write claims can
be safe if the claimants coordinate with each other. For instance, if vbdev_gpt and vbdev_split
agree to write to separate regions of the same underlying bdev they should be able to share write
access to that bdev. Each bdev module making a claim must use its own descriptor.

To verify the sufficiency of the API the following changes will be made:

 - vbdev_lvol will use `SPDK_BDEV_MOD_CLAIM_WRITE_ONCE` while claiming the bdev that contains the
   blobstore. This claim will be handled directly by vbdev_lvol rather than asking blob_bdev to take
   a claim on its behalf.
 - vbdev_lvol will use `SPDK_BDEV_MOD_CLAIM_READ_ONLY_MANY` to claim external snapshot bdevs.
 - `lib/bdev/part.c` will use `SPDK_BDEV_MOD_CLAIM_READ_WRITE_MANY`.

Over time the remaining in-tree consumers of newly deprecated claims will be refactored to use the
new API.

## New API

```c
/**
 * Indicates which type of claim is being requested.
 */
enum spdk_bdev_module_claim_type {
	/* Not an actual claim mode. Reserves 0 so intent can be distinguished from calloc(). */
	SPDK_BDEV_MOD_CLAIM_NONE = 0,

	/*
	 * Exclusive writer, with allowances for legacy behavior.  This matches the behavior of
	 * `spdk_bdev_module_claim_bdev()` as of SPDK 22.09, which is deprecated.  This claim type
	 * is deprecated.
	 */
	SPDK_BDEV_MOD_CLAIM_EXCL_WRITE,

	/**
	 * The descriptor passed with this claim request is the only writer. Other claimless readers
	 * are allowed.
	 */
	SPDK_BDEV_MOD_CLAIM_WRITE_ONCE,

	/**
	 * Any number of readers, no writers. Readers without a claim are allowed.
	 */
	SPDK_BDEV_MOD_CLAIM_READ_ONLY_MANY,

	/**
	 * Any number of writers with matching shared_claim_key. After the first writer establishes
	 * a claim, future aspiring writers should open read-only and pass the read-only descriptor.
	 * If the shared claim is granted to the aspiring writer, the descriptor will be upgraded to
	 * read-write.
	 */
	SPDK_BDEV_MOD_CLAIM_READ_WRITE_MANY
};

/**
 * Options that may be passed to spdk_bdev_module_claim_bdev_ext().
 */
struct spdk_bdev_module_claim_opts {
	/* For API compatibility, like other _opts structures */
	size_t size;
	/**
	 * Used with SPDK_BDEV_MOD_CLAIM_READ_WRITE_MANY claims.
	 */
	void *shared_claim_key;
};

/**
 * Initialize bdev module claim options structure.
 *
 * \param opts The structure to initialize.
 * \param size The size of *opts.
 */
void spdk_bdev_module_claim_opts_init(struct spdk_bdev_module_claim_opts *opts, size_t size);

/**
 * Claim the bdev referenced by the open descriptor. This interface is typically used for
 * establishing shared claims.
 *
 * \param desc An open bdev descriptor. Some claim types may upgrade this from read-only to
 * read-write.
 * \param type The type of claim to establish.
 * \param opts NULL or options required by the particular claim type.
 * \param module The bdev module making this claim.
 * \return 0 on success
 * \return -ENOMEM if insufficient memory to track the claim
 * \return -EBUSY if the claim cannot be granted due to a conflict
 * \return -EINVAL if the claim type required options that were not passed or required parameters
 * were NULL.
 */
int spdk_bdev_module_claim_bdev_desc(struct spdk_bdev_desc *desc,
				     enum spdk_bdev_module_claim_type type,
				     struct spdk_bdev_module_claim_opts *opts,
				     struct spdk_bdev_module *module);

/**
 * Release a claim obtained with the specified descriptor. Each call to
 * spdk_bdev_module_claim_bdev_desc() should be matched with a call to this function. Alternatively,
 * spdk_bdev_close() will release all claims associated with the descriptor.
 *
 * \param desc A descriptor previously passed to spdk_bdev_module_claim_desc().
 */
void spdk_bdev_module_release_bdev_desc(struct spdk_bdev_desc *desc);
```

## Implementation details

Within `struct spdk_bdev`, `internal.claim_module` will continue to be replaced with:

```c
	enum spdk_bdev_module_claim_type	claim_type;
	union {
		/* Use only with deprecated SPDK_BDEV_MOD_CLAIM_EXCL_WRITE */
		struct {
			/* used as internal.claim_module before */
			struct spdk_bdev_module		*claim_module;
			/* used for generating deprecation warnings */
			struct spdk_bdev_desc		*desc;
		} v1;
		/* Used with all other claim types. */
		struct {
			TAILQ_HEAD(, bdev_claim)	claims;
			/* See spdk_bdev_module_claim_opts.shared_claim_key */
			void				*key;
		} v2;
	} claim;
```

As before, claim manipulation happens while holding `internal.mutex`.

Each item on `internal.v2.claims` is:

```c
struct bdev_claim {
	struct spdk_bdev_module *module;
	struct spdk_bdev_desc *desc;
	uint32_t count;
	TAILQ_ENTRY(bdev_claim) link;
};
```

So that claims can easily be found using the descriptor, `struct spdk_bdev_desc` will gain a pointer
to any `struct bdev_claim` that is associated with the descriptor.

## Deprecations

`spdk_bdev_module_claim_bdev()` is deprecated. `spdk_bdev_module_claim_bdev_desc()` should be used
instead. The following problematic behavior will be allowed but result in log messages at the NOTICE
level when encountered.

 - Claiming a bdev that has an open read-write descriptor aside from the one passed in.
 - Performing writes using a descriptor that was not passed to `spdk_bdev_module_claim_bdev()` nor
   `spdk_bdev_module_claim_bdev_desc()`. To avoid IO path overhead and excessive log entries, this
   will only be enabled on debug builds and will only log the first time it happens on each
   descriptor.
 - Closing a descriptor that retains a claim obtained by `spdk_bdev_module_claim_bdev()`.

As a side effect of these changes, taking and/or holding a claim without an associated open bdev
descriptor is deprecated. Rather than claiming a bdev and storing a value in `bool claimed`,
consumers may open the bdev, establish a claim, then store a pointer to the descriptor with the
claim.

`SPDK_BDEV_MOD_CLAIM_EXCL_WRITE` is introduced deprecated for internal use until the removal of
`spdk_bdev_module_claim_bdev`.
