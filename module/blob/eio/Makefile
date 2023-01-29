#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = blob_eio.c
LIBNAME = blob_eio

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_blob_eio.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
