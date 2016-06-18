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

#include "shill/binder/binder_adaptor.h"

#include <string>

#include <binder/Status.h>
#include <utils/String8.h>

#include "android/system/connectivity/shill/IPropertyChangedCallback.h"
#include "shill/binder/binder_control.h"
#include "shill/logging.h"

using android::binder::Status;
using android::sp;
using android::String8;
using android::system::connectivity::shill::IPropertyChangedCallback;
using std::string;

namespace {
const int kShillObjectNotAliveErrorCode = -2;
const char kShillObjectNotAliveErrorMessage[] =
    "shill object is no longer alive.";
}

namespace shill {

namespace Logging {
static auto kModuleLogScope = ScopeLogger::kBinder;
static string ObjectID(BinderAdaptor* b) {
  return "(binder_adaptor)";
}
}  // namespace Logging

BinderAdaptor::BinderAdaptor(BinderControl* control, const string& rpc_id)
    : control_(control), rpc_id_(rpc_id) {
  SLOG(this, 2) << "BinderAdaptor: " << rpc_id;
}

BinderAdaptor::~BinderAdaptor() {
    control_->OnAdaptorDestructed(rpc_id());
}

Status BinderAdaptor::GenerateShillObjectNotAliveErrorStatus() {
  return Status::fromServiceSpecificError(
      kShillObjectNotAliveErrorCode, String8(kShillObjectNotAliveErrorMessage));
}

void BinderAdaptor::AddPropertyChangedSignalHandler(
    const sp<IPropertyChangedCallback>& property_changed_callback) {
  property_changed_callbacks_.push_back(property_changed_callback);
}

void BinderAdaptor::SendPropertyChangedSignal(const string& name) {
  for (const auto& callback : property_changed_callbacks_) {
    callback->OnPropertyChanged(name);
  }
}

}  // namespace shill