// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef POWER_MANAGER_POWER_CONSTANTS_H_
#define POWER_MANAGER_POWER_CONSTANTS_H_

namespace power_manager {

extern const char kPluggedBrightnessOffset[];
extern const char kUnpluggedBrightnessOffset[];
extern const char kAlsBrightnessLevel[];
extern const char kLowBatterySuspendPercent[];
extern const char kCleanShutdownTimeoutMs[];
extern const char kPluggedDimMs[];
extern const char kPluggedOffMs[];
extern const char kUnpluggedDimMs[];
extern const char kUnpluggedOffMs[];
extern const char kUnpluggedSuspendMs[];
extern const char kEnforceLock[];
extern const char kDisableIdleSuspend[];
extern const char kLockOnIdleSuspend[];
extern const char kLockMs[];
extern const char kRetrySuspendMs[];
extern const char kRetrySuspendAttempts[];
extern const char kUseXScreenSaver[];
extern const char kPluggedSuspendMs[];

}  // namespace power_manager

#endif  // POWER_MANAGER_POWER_CONSTANTS_H_

