// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file contains wrapper of libusb functions.

#ifndef HAMMERD_USB_UTILS_H_
#define HAMMERD_USB_UTILS_H_

#include <libusb.h>
#include <stdint.h>

#include <string>

#include <base/macros.h>

namespace hammerd {

constexpr uint8_t kUsbSubclassGoogleUpdate = 0x53;
constexpr uint8_t kUsbProtocolGoogleUpdate = 0xff;

bool InitLibUSB();
void ExitLibUSB();
void LogUSBError(const char* func_name, int return_code);

class UsbEndpointInterface {
 public:
  virtual ~UsbEndpointInterface() = default;

  // Initializes the USB endpoint.
  virtual bool Connect() = 0;
  // Releases USB endpoint.
  virtual void Close() = 0;
  // Returns whether the USB endpoint is initialized.
  virtual bool IsConnected() const = 0;

  // Sends the data to USB endpoint and then reads the result back.
  // Returns the byte number of the received data. -1 if the process fails.
  virtual int Transfer(const void* outbuf,
                       int outlen,
                       void* inbuf,
                       int inlen,
                       bool allow_less,
                       unsigned int timeout_ms = 0) = 0;
  // Sends the data to USB endpoint.
  // Returns the byte number of the received data.
  virtual int Send(const void* outbuf, int outlen, unsigned int timeout_ms = 0)
      = 0;
  // Receives the data from USB endpoint.
  // Returns the byte number of the received data. -1 if the amount of received
  // data is not as required and `allow_less` argument is false.
  virtual int Receive(void* inbuf,
                      int inlen,
                      bool allow_less = false,
                      unsigned int timeout_ms = 0) = 0;

  // Gets the chunk length of the USB endpoint.
  virtual size_t GetChunkLength() const = 0;

  // Gets the configuration string of the USB endpoint.
  virtual std::string GetConfigurationString() const = 0;
};

class UsbEndpoint : public UsbEndpointInterface {
 public:
  UsbEndpoint(uint16_t vendor_id, uint16_t product_id, int bus, int port);

  // UsbEndpointInterface:
  ~UsbEndpoint() override;
  bool Connect() override;
  void Close() override;
  bool IsConnected() const override;
  int Transfer(const void* outbuf,
               int outlen,
               void* inbuf,
               int inlen,
               bool allow_less,
               unsigned int timeout_ms = 0) override;
  int Send(const void* outbuf, int outlen, unsigned int timeout_ms = 0)
      override;
  int Receive(void* inbuf,
              int inlen,
              bool allow_less = false,
              unsigned int timeout_ms = 0) override;
  size_t GetChunkLength() const override { return chunk_len_; }
  std::string GetConfigurationString() const override {
    return configuration_string_;
  }

 private:
  // Opens and returns a handle of device with vendor_id and product_id
  // connected to given bus and port.
  libusb_device_handle* OpenDevice(
      uint16_t vendor_id, uint16_t product_id, int bus, int port);
  // Returns the descriptor at the given index as an ASCII string.
  std::string GetStringDescriptorAscii(uint8_t index);
  // Finds the interface number. Returns -1 on error.
  int FindInterface();
  // Find the USB endpoint of the hammer EC.
  int FindEndpoint(const libusb_interface* iface);

  // Wrapper of libusb_bulk_transfer.
  // Returns the actual transfered data size. -1 if the transmission fails.
  // If the timeout is not assigned, then use default timeout value.
  int BulkTransfer(void* buf,
                   enum libusb_endpoint_direction direction_mask,
                   int len,
                   unsigned int timeout_ms = 0);

  uint16_t vendor_id_;
  uint16_t product_id_;
  int bus_;
  int port_;
  libusb_device_handle* devh_;
  std::string configuration_string_;
  int iface_num_;
  int ep_num_;
  size_t chunk_len_;
  DISALLOW_COPY_AND_ASSIGN(UsbEndpoint);
};

}  // namespace hammerd
#endif  // HAMMERD_USB_UTILS_H_
