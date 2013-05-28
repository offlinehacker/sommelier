// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mist/usb_config_descriptor.h"

#include <libusb.h>

#include <base/logging.h>
#include <base/stringprintf.h>

#include "mist/usb_device.h"
#include "mist/usb_interface.h"

using base::StringPrintf;
using std::ostream;
using std::string;

namespace mist {

UsbConfigDescriptor::UsbConfigDescriptor(
    const base::WeakPtr<UsbDevice>& device,
    libusb_config_descriptor* config_descriptor,
    bool own_config_descriptor)
    : device_(device),
      config_descriptor_(config_descriptor),
      own_config_descriptor_(own_config_descriptor) {
  CHECK(config_descriptor_);
}

UsbConfigDescriptor::~UsbConfigDescriptor() {
  if (own_config_descriptor_) {
    libusb_free_config_descriptor(config_descriptor_);
    config_descriptor_ = NULL;
  }
}

uint8 UsbConfigDescriptor::GetLength() const {
  return config_descriptor_->bLength;
}

uint8 UsbConfigDescriptor::GetDescriptorType() const {
  return config_descriptor_->bDescriptorType;
}

uint16 UsbConfigDescriptor::GetTotalLength() const {
  return config_descriptor_->wTotalLength;
}

uint8 UsbConfigDescriptor::GetNumInterfaces() const {
  return config_descriptor_->bNumInterfaces;
}

uint8 UsbConfigDescriptor::GetConfigurationValue() const {
  return config_descriptor_->bConfigurationValue;
}

string UsbConfigDescriptor::GetConfigurationDescription() const {
  return device_ ?
      device_->GetStringDescriptorAscii(config_descriptor_->iConfiguration) :
      string();
}

uint8 UsbConfigDescriptor::GetAttributes() const {
  return config_descriptor_->bmAttributes;
}

uint8 UsbConfigDescriptor::GetMaxPower() const {
  return config_descriptor_->MaxPower;
}

scoped_ptr<UsbInterface> UsbConfigDescriptor::GetInterface(uint8 index) const {
  if (index >= GetNumInterfaces()) {
    LOG(ERROR) << StringPrintf("Invalid interface index %d. "
                               "Must be less than %d.",
                               index, GetNumInterfaces());
    return scoped_ptr<UsbInterface>();
  }

  return scoped_ptr<UsbInterface>(
      new UsbInterface(device_, &config_descriptor_->interface[index]));
}

string UsbConfigDescriptor::ToString() const {
  return StringPrintf("Configuration (Length=%u, "
                      "DescriptorType=%u, "
                      "TotalLength=%u, "
                      "NumInterfaces=%u, "
                      "ConfigurationValue=%u, "
                      "Configuration='%s', "
                      "Attributes=0x%02x, "
                      "MaxPower=%u)",
                      GetLength(),
                      GetDescriptorType(),
                      GetTotalLength(),
                      GetNumInterfaces(),
                      GetConfigurationValue(),
                      GetConfigurationDescription().c_str(),
                      GetAttributes(),
                      GetMaxPower());
}

}  // namespace mist

ostream& operator<<(ostream& stream,
                    const mist::UsbConfigDescriptor& config_descriptor) {
  stream << config_descriptor.ToString();
  return stream;
}
