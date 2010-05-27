// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CRYPTOHOME_MOCK_MOUNT_H_
#define CRYPTOHOME_MOCK_MOUNT_H_

#include "cryptohome/mount.h"

#include <gmock/gmock.h>

namespace cryptohome {
class Credentials;

class MockMount : public Mount {
 public:
  MockMount() {}
  ~MockMount() {}
  MOCK_METHOD0(Init, bool());
  MOCK_METHOD1(TestCredentials, bool(const Credentials&));
};
}  // namespace cryptohome

#endif  // CRYPTOHOME_MOCK_MOUNT_H_
