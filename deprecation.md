# Deprecation

## ABI and API Deprecation {#deprecation}

This document details the policy for maintaining stability of SPDK ABI and API.

Major ABI version can change at most once for each quarterly SPDK release.
ABI versions are managed separately for each library and follow [Semantic Versioning](https://semver.org/).

API and ABI deprecation notices shall be posted in the next section.
Each entry must describe what will be removed and can suggest the future use or alternative.
Specific future SPDK release for the removal must be provided.
ABI cannot be removed without providing deprecation notice for at least single SPDK release.

Deprecated code paths must be registered with `SPDK_DEPRECATION_REGISTER()` and logged with
`SPDK_LOG_DEPRECATED()`. The tag used with these macros will appear in the SPDK
log at the warn level when `SPDK_LOG_DEPRECATED()` is called, subject to rate limits.
The tags can be matched with the level 4 headers below.

## Deprecation Notices {#deprecation-notices}

### nvme

#### `nvme_ctrlr_prepare_for_reset``

Deprecated `spdk_nvme_ctrlr_prepare_for_reset` API, which will be removed in SPDK 22.01.
For PCIe transport, `spdk_nvme_ctrlr_disconnect` should be used before freeing I/O qpairs.

### bdev

#### `bdev_mgmt_wrong_thread``

Using a thread other than the SPDK app thread for bdev management operations is deprecated and will
be removed in SPDK 23.04. This deprecation should have no impact for applications that are configure
via a JSON configuration file and/or through the RPC interface. Applications that make bdev
management operations must perform them from the SPDK app thread, which may be accomplished using
`spdk_thread_send_msg(spdk_thread_get_app_thread(), ...)`.
