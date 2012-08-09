// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/manager_dbus_adaptor.h"

#include <map>
#include <string>
#include <vector>

#include <dbus-c++/dbus.h>

#include "shill/device.h"
#include "shill/error.h"
#include "shill/key_value_store.h"
#include "shill/manager.h"
#include "shill/scope_logger.h"
#include "shill/wifi_service.h"

using std::map;
using std::string;
using std::vector;

namespace shill {

// static
const char ManagerDBusAdaptor::kPath[] = "/";

ManagerDBusAdaptor::ManagerDBusAdaptor(DBus::Connection* conn, Manager *manager)
    : DBusAdaptor(conn, kPath),
      manager_(manager) {
}

ManagerDBusAdaptor::~ManagerDBusAdaptor() {
  manager_ = NULL;
}

void ManagerDBusAdaptor::UpdateRunning() {}

void ManagerDBusAdaptor::EmitBoolChanged(const string &name, bool value) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  PropertyChanged(name, DBusAdaptor::BoolToVariant(value));
}

void ManagerDBusAdaptor::EmitUintChanged(const string &name,
                                         uint32 value) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  PropertyChanged(name, DBusAdaptor::Uint32ToVariant(value));
}

void ManagerDBusAdaptor::EmitIntChanged(const string &name, int value) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  PropertyChanged(name, DBusAdaptor::Int32ToVariant(value));
}

void ManagerDBusAdaptor::EmitStringChanged(const string &name,
                                           const string &value) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  PropertyChanged(name, DBusAdaptor::StringToVariant(value));
}

void ManagerDBusAdaptor::EmitStringsChanged(const string &name,
                                            const vector<string> &value) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  PropertyChanged(name, DBusAdaptor::StringsToVariant(value));
}

void ManagerDBusAdaptor::EmitRpcIdentifierChanged(
    const string &name,
    const string &value) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  PropertyChanged(name, DBusAdaptor::PathToVariant(value));
}

void ManagerDBusAdaptor::EmitRpcIdentifierArrayChanged(
    const string &name,
    const vector<string> &value) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  vector< ::DBus::Path> paths;
  vector<string>::const_iterator it;
  for (it = value.begin(); it != value.end(); ++it) {
    paths.push_back(*it);
  }

  PropertyChanged(name, DBusAdaptor::PathsToVariant(paths));
}

void ManagerDBusAdaptor::EmitStateChanged(const string &new_state) {
  SLOG(DBus, 2) << __func__;
  StateChanged(new_state);
}

map<string, ::DBus::Variant> ManagerDBusAdaptor::GetProperties(
    ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__;
  map<string, ::DBus::Variant> properties;
  DBusAdaptor::GetProperties(manager_->store(), &properties, &error);
  return properties;
}

void ManagerDBusAdaptor::SetProperty(const string &name,
                                     const ::DBus::Variant &value,
                                     ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  if (DBusAdaptor::SetProperty(manager_->mutable_store(),
                               name,
                               value,
                               &error)) {
    PropertyChanged(name, value);
  }
}

string ManagerDBusAdaptor::GetState(::DBus::Error &/*error*/) {
  SLOG(DBus, 2) << __func__;
  return manager_->CalculateState(NULL);
}

::DBus::Path ManagerDBusAdaptor::CreateProfile(const string &name,
                                               ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  Error e;
  string path;
  manager_->CreateProfile(name, &path, &e);
  e.ToDBusError(&error);
  return ::DBus::Path(path);
}

void ManagerDBusAdaptor::RemoveProfile(const string &name,
                                       ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  Error e;
  manager_->RemoveProfile(name, &e);
  e.ToDBusError(&error);
}

::DBus::Path ManagerDBusAdaptor::PushProfile(const std::string &name,
                                             ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  Error e;
  string path;
  manager_->PushProfile(name, &path, &e);
  e.ToDBusError(&error);
  return ::DBus::Path(path);
}

void ManagerDBusAdaptor::PopProfile(const std::string &name,
                                    ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__ << ": " << name;
  Error e;
  manager_->PopProfile(name, &e);
  e.ToDBusError(&error);
}

void ManagerDBusAdaptor::PopAnyProfile(::DBus::Error &error) {
  SLOG(DBus, 2) << __func__;
  Error e;
  manager_->PopAnyProfile(&e);
  e.ToDBusError(&error);
}

void ManagerDBusAdaptor::RecheckPortal(::DBus::Error &error) {
  SLOG(DBus, 2) << __func__;
  Error e;
  manager_->RecheckPortal(&e);
  e.ToDBusError(&error);
}

void ManagerDBusAdaptor::RequestScan(const string &technology,
                                     ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__ << ": " << technology;
  Error e;
  manager_->RequestScan(technology, &e);
  e.ToDBusError(&error);
}

void ManagerDBusAdaptor::EnableTechnology(const string &technology_name,
                                          ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__ << ": " << technology_name;
  Error e(Error::kOperationInitiated);
  DBus::Tag *tag = new DBus::Tag();
  manager_->EnableTechnology(technology_name, &e, GetMethodReplyCallback(tag));
  ReturnResultOrDefer(tag, e, &error);
  // TODO(ers): A reply will be sent to the client as soon as the first
  // device of a given technology has finished being enabled. It would
  // seem arguably more correct to wait until all the devices were enabled.
}

void ManagerDBusAdaptor::DisableTechnology(const string &technology_name,
                                           ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__ << ": " << technology_name;
  Error e(Error::kOperationInitiated);
  DBus::Tag *tag = new DBus::Tag();
  manager_->DisableTechnology(technology_name, &e, GetMethodReplyCallback(tag));
  ReturnResultOrDefer(tag, e, &error);
  // TODO(ers): A reply will be sent to the client as soon as the first
  // device of a given technology has finished being disabled. It would
  // seem arguably more correct to wait until all the devices were enabled.
}

// Called, e.g., to get WiFiService handle for a hidden SSID.
::DBus::Path ManagerDBusAdaptor::GetService(
    const map<string, ::DBus::Variant> &args,
    ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__;
  ServiceRefPtr service;
  KeyValueStore args_store;
  Error e;
  DBusAdaptor::ArgsToKeyValueStore(args, &args_store, &e);
  if (e.IsSuccess()) {
    service = manager_->GetService(args_store, &e);
  }
  if (e.ToDBusError(&error)) {
    return "/";  // ensure return is syntactically valid
  }
  return service->GetRpcIdentifier();
}

// Obsolete, use GetService instead.
::DBus::Path ManagerDBusAdaptor::GetVPNService(
    const map<string, ::DBus::Variant> &args,
    ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__;
  return GetService(args, error);
}

// Obsolete, use GetService instead.
::DBus::Path ManagerDBusAdaptor::GetWifiService(
    const map<string, ::DBus::Variant> &args,
    ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__;
  return GetService(args, error);
}


void ManagerDBusAdaptor::ConfigureService(
    const map<string, ::DBus::Variant> &args,
    ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__;
  KeyValueStore args_store;
  Error e;
  DBusAdaptor::ArgsToKeyValueStore(args, &args_store, &e);
  if (e.IsSuccess()) {
    manager_->ConfigureService(args_store, &e);
  }
  e.ToDBusError(&error);
}

int32_t ManagerDBusAdaptor::GetDebugLevel(::DBus::Error &/*error*/) {
  SLOG(DBus, 2) << __func__;
  return logging::GetMinLogLevel();
}

void ManagerDBusAdaptor::SetDebugLevel(const int32_t &level,
                                       ::DBus::Error &/*error*/) {
  SLOG(DBus, 2) << __func__ << ": " << level;
  if (level < logging::LOG_NUM_SEVERITIES) {
    logging::SetMinLogLevel(level);
    // Like VLOG, SLOG uses negative verbose level.
    ScopeLogger::GetInstance()->set_verbose_level(-level);
  } else {
    LOG(WARNING) << "Ignoring attempt to set log level to " << level;
  }
}

string ManagerDBusAdaptor::GetServiceOrder(::DBus::Error &/*error*/) {
  SLOG(DBus, 2) << __func__;
  return manager_->GetTechnologyOrder();
}

void ManagerDBusAdaptor::SetServiceOrder(const string &order,
                                         ::DBus::Error &error) {
  SLOG(DBus, 2) << __func__ << ": " << order;
  Error e;
  manager_->SetTechnologyOrder(order, &e);
  e.ToDBusError(&error);
}

std::string ManagerDBusAdaptor::GetDebugTags(::DBus::Error &/*error*/) {
  SLOG(DBus, 2) << __func__;
  return ScopeLogger::GetInstance()->GetEnabledScopeNames();
}

void ManagerDBusAdaptor::SetDebugTags(const std::string &tags,
                                      ::DBus::Error &/*error*/) {
  SLOG(DBus, 2) << __func__ << ": " << tags;
  ScopeLogger::GetInstance()->EnableScopesByName(tags);
}

std::string ManagerDBusAdaptor::ListDebugTags(::DBus::Error &/*error*/) {
  SLOG(DBus, 2) << __func__;
  return ScopeLogger::GetInstance()->GetAllScopeNames();
}

}  // namespace shill
