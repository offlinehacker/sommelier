// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_PROXY_FACTORY_
#define SHILL_PROXY_FACTORY_

#include <string>

#include <base/basictypes.h>
#include <base/lazy_instance.h>
#include <base/memory/scoped_ptr.h>
#include <dbus-c++/dbus.h>

#include "shill/refptr_types.h"

namespace shill {

class DBusObjectManagerProxyInterface;
class DBusPropertiesProxyDelegate;
class DBusPropertiesProxyInterface;
class DHCPProxyInterface;
class ModemCDMAProxyInterface;
class ModemGSMCardProxyInterface;
class ModemGSMNetworkProxyInterface;
class ModemManagerClassic;
class ModemManagerProxyInterface;
class ModemProxyInterface;
class ModemSimpleProxyInterface;
class PowerManagerProxyDelegate;
class PowerManagerProxyInterface;
class SupplicantBSSProxyInterface;
class SupplicantInterfaceProxyInterface;
class SupplicantProcessProxyInterface;

// Global DBus proxy factory that can be mocked out in tests.
class ProxyFactory {
 public:
  virtual ~ProxyFactory();

  // Since this is a singleton, use ProxyFactory::GetInstance()->Foo()
  static ProxyFactory *GetInstance();

  virtual void Init();

  virtual DBusObjectManagerProxyInterface *CreateDBusObjectManagerProxy(
      const std::string &path,
      const std::string &service);

  virtual DBusPropertiesProxyInterface *CreateDBusPropertiesProxy(
      DBusPropertiesProxyDelegate *delegate,
      const std::string &path,
      const std::string &service);

  virtual ModemManagerProxyInterface *CreateModemManagerProxy(
      ModemManagerClassic *manager,
      const std::string &path,
      const std::string &service);

  virtual ModemProxyInterface *CreateModemProxy(const std::string &path,
                                                const std::string &service);

  virtual ModemSimpleProxyInterface *CreateModemSimpleProxy(
      const std::string &path,
      const std::string &service);

  virtual ModemCDMAProxyInterface *CreateModemCDMAProxy(
      const std::string &path,
      const std::string &service);

  virtual ModemGSMCardProxyInterface *CreateModemGSMCardProxy(
      const std::string &path,
      const std::string &service);

  virtual ModemGSMNetworkProxyInterface *CreateModemGSMNetworkProxy(
      const std::string &path,
      const std::string &service);

  // The caller retains ownership of 'delegate'.  It must not be deleted before
  // the proxy.
  virtual PowerManagerProxyInterface *CreatePowerManagerProxy(
      PowerManagerProxyDelegate *delegate);

  virtual SupplicantProcessProxyInterface *CreateSupplicantProcessProxy(
      const char *dbus_path,
      const char *dbus_addr);

  virtual SupplicantInterfaceProxyInterface *CreateSupplicantInterfaceProxy(
      const WiFiRefPtr &wifi,
      const DBus::Path &object_path,
      const char *dbus_addr);

  // See comment in supplicant_bss_proxy.h, about bare pointer.
  virtual SupplicantBSSProxyInterface *CreateSupplicantBSSProxy(
      WiFiEndpoint *wifi_endpoint,
      const DBus::Path &object_path,
      const char *dbus_addr);

  virtual DHCPProxyInterface *CreateDHCPProxy(const std::string &service);

  DBus::Connection *connection() const { return connection_.get(); }

 protected:
  ProxyFactory();

 private:
  friend struct base::DefaultLazyInstanceTraits<ProxyFactory>;

  scoped_ptr<DBus::Connection> connection_;

  DISALLOW_COPY_AND_ASSIGN(ProxyFactory);
};

}  // namespace shill

#endif  // SHILL_PROXY_FACTORY_
