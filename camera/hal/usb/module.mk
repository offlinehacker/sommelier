# Copyright 2016 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include common.mk
include pc_utils.mk

hal_usb_PC_DEPS = libexif libyuv
hal_usb_CPPFLAGS := $(call get_pc_cflags,$(hal_usb_PC_DEPS))
hal_usb_LDLIBS := $(call get_pc_libs,$(hal_usb_PC_DEPS)) -ljpeg

CXX_LIBRARY(hal/usb/camera_hal.so): CPPFLAGS += $(hal_usb_CPPFLAGS)
CXX_LIBRARY(hal/usb/camera_hal.so): LDLIBS += $(hal_usb_LDLIBS)
CXX_LIBRARY(hal/usb/camera_hal.so): \
	$(ANDROID_OBJECTS) \
	$(COMMON_OBJECTS) \
	$(hal_usb_CXX_OBJECTS)

hal/usb/camera_hal: CXX_LIBRARY(hal/usb/camera_hal.so)

clean: CLEAN(hal/usb/camera_hal.so)

.PHONY: hal/usb/camera_hal