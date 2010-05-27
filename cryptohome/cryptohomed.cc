// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/service.h"

#include <stdio.h>
#include <unistd.h>

#include <base/command_line.h>
#include <base/logging.h>

// TODO(wad) This is a placeholder DBus service which allows
//           chrome-login (and anything else running as chronos)
//           to request to mount, unmount, or check if a mapper
//           device is mounted. This is very temporary but should
//           serve as a baseline for moving all the shell scripts
//           into C++.
//           We will need a "CheckKey" interface as well to simplify
//           offline authentication checks.


namespace switches {
// Keeps std* open for debugging
static const char *kNoCloseOnDaemonize = "noclose";
}  // namespace switches

int main(int argc, char **argv) {
  ::g_type_init();
  CommandLine::Init(argc, argv);
  logging::InitLogging("/var/log/cryptohomed.log",
                       logging::LOG_TO_BOTH_FILE_AND_SYSTEM_DEBUG_LOG,
                       logging::DONT_LOCK_LOG_FILE,
                       logging::APPEND_TO_OLD_LOG_FILE);

  cryptohome::Service service;
  LOG_IF(FATAL, !service.Initialize()) << "Failed";

  // Allow the commands to be configurable.
  CommandLine *cl = CommandLine::ForCurrentProcess();
  int noclose = cl->HasSwitch(switches::kNoCloseOnDaemonize);
  PLOG_IF(FATAL, daemon(0, noclose) == -1) << "Failed to daemonize";

  LOG_IF(FATAL, !service.Register(chromeos::dbus::GetSystemBusConnection()))
    << "Failed";
  LOG_IF(FATAL, !service.Run()) << "Failed";
  return 0;
}
