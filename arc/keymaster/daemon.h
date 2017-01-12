// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ARC_KEYMASTER_DAEMON_H_
#define ARC_KEYMASTER_DAEMON_H_

#include "arc/keymaster/keymaster_server.h"

#include <base/memory/weak_ptr.h>
#include <brillo/daemons/dbus_daemon.h>
#include <dbus/exported_object.h>

namespace dbus {

class MethodCall;

}  // namespace dbus

namespace arc {
namespace keymaster {

class Daemon : public brillo::DBusDaemon {
 public:
  Daemon();
  ~Daemon() override;

 protected:
  int OnInit() override;

 private:
  // Initializes the D-Bus service. This D-Bus interface waits for the FD in a
  // BootstrapMojoConnection call incoming from Chrome, which we can use to
  // setup the Mojo IPC channel.
  void InitDBus();

  // Handles BootstrapMojoConnection D-Bus method calls.
  void BootstrapMojoConnection(
      dbus::MethodCall* method_call,
      dbus::ExportedObject::ResponseSender response_sender);

  void AcceptProxyConnection(base::ScopedFD fd);

  bool is_bound_ = false;

  base::WeakPtrFactory<Daemon> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(Daemon);
};

}  // namespace keymaster
}  // namespace arc

#endif  // ARC_KEYMASTER_DAEMON_H_