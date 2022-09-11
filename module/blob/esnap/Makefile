#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

C_SRCS = blob_esnap.c
LIBNAME = blob_esnap

SPDK_MAP_FILE = $(abspath $(CURDIR)/spdk_blob_esnap.map)

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
