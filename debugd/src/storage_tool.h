// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEBUGD_SRC_STORAGE_TOOL_H_
#define DEBUGD_SRC_STORAGE_TOOL_H_

#include <string>
#include <vector>

#include <base/files/file_path.h>
#include <base/macros.h>
#include <dbus/file_descriptor.h>

#include "debugd/src/subprocess_tool.h"

namespace debugd {

class StorageTool : public SubprocessTool {
 public:
  StorageTool() = default;
  ~StorageTool() override = default;

  std::string Smartctl(const std::string& option);
  std::string Start(const dbus::FileDescriptor& outfd);
  const base::FilePath GetDevice(const base::FilePath& filesystem,
                                 const base::FilePath& mountsFile);
  bool IsSupported(const base::FilePath typeFile,
                   const base::FilePath vendFile,
                   std::string* errorMsg);

 private:
  // Returns the partition of |dst| as a string. |dst| is expected
  // to be a storage device path (e.g. "/dev/sda1").
  const std::string GetPartition(const std::string& dst);

  // Removes the partition from |dstPath| which is expected
  // to be a storage device path (e.g. "/dev/mmcblk1p2").
  void StripPartition(base::FilePath* dstPath);

  DISALLOW_COPY_AND_ASSIGN(StorageTool);
};

}  // namespace debugd

#endif  // DEBUGD_SRC_STORAGE_TOOL_H_
