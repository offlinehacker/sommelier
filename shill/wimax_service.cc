// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/wimax_service.h"

#include <algorithm>

#include <base/string_util.h>
#include <base/stringprintf.h>
#include <chromeos/dbus/service_constants.h>

#include "shill/scope_logger.h"
#include "shill/technology.h"
#include "shill/wimax.h"
#include "shill/wimax_network_proxy_interface.h"

using std::replace_if;
using std::string;

namespace shill {

WiMaxService::WiMaxService(ControlInterface *control,
                           EventDispatcher *dispatcher,
                           Metrics *metrics,
                           Manager *manager,
                           const WiMaxRefPtr &wimax)
    : Service(control, dispatcher, metrics, manager, Technology::kWiMax),
      wimax_(wimax),
      network_identifier_(0),
      need_passphrase_(true) {
  PropertyStore *store = this->mutable_store();
  // TODO(benchan): Support networks that require no user credentials or
  // implicitly defined credentials.
  store->RegisterBool(flimflam::kPassphraseRequiredProperty, &need_passphrase_);
}

WiMaxService::~WiMaxService() {}

void WiMaxService::GetConnectParameters(DBusPropertiesMap *parameters) const {
  CHECK(parameters);

  (*parameters)[wimax_manager::kEAPAnonymousIdentity].writer()
      .append_string(eap().anonymous_identity.c_str());
  (*parameters)[wimax_manager::kEAPUserIdentity].writer()
      .append_string(eap().identity.c_str());
  (*parameters)[wimax_manager::kEAPUserPassword].writer()
      .append_string(eap().password.c_str());
}

RpcIdentifier WiMaxService::GetNetworkObjectPath() const {
  CHECK(proxy_.get());
  return proxy_->path();
}

bool WiMaxService::Start(WiMaxNetworkProxyInterface *proxy) {
  SLOG(WiMax, 2) << __func__;
  CHECK(proxy);
  proxy_.reset(proxy);

  Error error;
  network_name_ = proxy_->Name(&error);
  if (!error.IsSuccess()) {
    return false;
  }
  network_identifier_ = proxy_->Identifier(&error);
  if (!error.IsSuccess()) {
    return false;
  }

  int signal_strength = proxy_->SignalStrength(&error);
  if (!error.IsSuccess()) {
    return false;
  }
  SetStrength(signal_strength);
  proxy_->set_signal_strength_changed_callback(
      Bind(&WiMaxService::OnSignalStrengthChanged, Unretained(this)));

  set_friendly_name(network_name_);
  storage_id_ =
      StringToLowerASCII(base::StringPrintf("%s_%s_%08x_%s",
                                            flimflam::kTypeWimax,
                                            network_name_.c_str(),
                                            network_identifier_,
                                            wimax_->address().c_str()));
  replace_if(
      storage_id_.begin(), storage_id_.end(), &Service::IllegalChar, '_');
  set_connectable(true);
  return true;
}

bool WiMaxService::TechnologyIs(const Technology::Identifier type) const {
  return type == Technology::kWiMax;
}

void WiMaxService::Connect(Error *error) {
  Service::Connect(error);
  wimax_->ConnectTo(this, error);
}

void WiMaxService::Disconnect(Error *error) {
  Service::Disconnect(error);
  wimax_->DisconnectFrom(this, error);
}

string WiMaxService::GetStorageIdentifier() const {
  return storage_id_;
}

string WiMaxService::GetDeviceRpcId(Error *error) {
  return wimax_->GetRpcIdentifier();
}

void WiMaxService::OnSignalStrengthChanged(int strength) {
  SLOG(WiMax, 2) << __func__ << "(" << strength << ")";
  SetStrength(strength);
}

}  // namespace shill
