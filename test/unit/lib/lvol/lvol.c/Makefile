#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

TEST_FILE = lvol_ut.c
LDFLAGS += -Wl,--wrap,lvs_esnap_dev_create

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
