// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mtpd/string_helpers.h"

#include <base/strings/string_util.h>

namespace mtpd {

std::string EnsureUTF8String(const std::string& str) {
  return base::IsStringUTF8(str) ? str : "";
}

}  // namespace
