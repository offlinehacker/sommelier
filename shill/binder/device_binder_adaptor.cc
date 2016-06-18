//
// Copyright (C) 2016 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "shill/binder/device_binder_adaptor.h"

#include <binder/Status.h>

#include "shill/binder/binder_control.h"
#include "shill/binder/device_binder_service.h"
#include "shill/binder/service_binder_service.h"
#include "shill/device.h"
#include "shill/logging.h"
#include "shill/refptr_types.h"

using android::binder::Status;
using android::IBinder;
using android::sp;
using android::system::connectivity::shill::IPropertyChangedCallback;
using std::string;
using std::vector;

namespace shill {

namespace Logging {
static auto kModuleLogScope = ScopeLogger::kBinder;
static string ObjectID(DeviceBinderAdaptor* d) {
  return "Device binder adaptor (id " + d->GetRpcIdentifier() + ", " +
         d->device()->UniqueName() + ")";
}
}  // namespace Logging

DeviceBinderAdaptor::DeviceBinderAdaptor(BinderControl* control, Device* device,
                                         const string& id)
    : BinderAdaptor(control, id), device_(device), weak_ptr_factory_(this) {
  set_binder_service(
      new DeviceBinderService(weak_ptr_factory_.GetWeakPtr(), id));
}

void DeviceBinderAdaptor::EmitBoolChanged(const string& name, bool /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

void DeviceBinderAdaptor::EmitUintChanged(const string& name,
                                          uint32_t /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

void DeviceBinderAdaptor::EmitUint16Changed(const string& name,
                                            uint16_t /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

void DeviceBinderAdaptor::EmitIntChanged(const string& name, int /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

void DeviceBinderAdaptor::EmitStringChanged(const string& name,
                                            const string& /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

void DeviceBinderAdaptor::EmitStringmapChanged(const string& name,
                                               const Stringmap& /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

void DeviceBinderAdaptor::EmitStringmapsChanged(const string& name,
                                                const Stringmaps& /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

void DeviceBinderAdaptor::EmitStringsChanged(const string& name,
                                             const Strings& /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

void DeviceBinderAdaptor::EmitKeyValueStoreChanged(
    const string& name, const KeyValueStore& /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

void DeviceBinderAdaptor::EmitRpcIdentifierChanged(
    const std::string& name, const std::string& /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

void DeviceBinderAdaptor::EmitRpcIdentifierArrayChanged(
    const string& name, const vector<string>& /*value*/) {
  SLOG(this, 2) << __func__ << ": " << name;
  SendPropertyChangedSignal(name);
}

Status DeviceBinderAdaptor::GetInterface(string* _aidl_return) {
  SLOG(this, 2) << __func__;
  *_aidl_return = device_->link_name();
  return Status::ok();
}

Status DeviceBinderAdaptor::GetSelectedService(sp<IBinder>* _aidl_return) {
  SLOG(this, 2) << __func__;
  ServiceRefPtr selected_service = device_->selected_service();
  if (!selected_service) {
    *_aidl_return = NULL;
  } else {
    *_aidl_return = static_cast<ServiceBinderService*>(
        control()->GetBinderServiceForRpcIdentifier(
            selected_service->GetRpcIdentifier()).get());
  }
  return Status::ok();
}

Status DeviceBinderAdaptor::RegisterPropertyChangedSignalHandler(
    const sp<IPropertyChangedCallback>& callback) {
  AddPropertyChangedSignalHandler(callback);
  return Status::ok();
}

}  // namespace shill