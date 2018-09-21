// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_PERCEPTION_MOJO_CONNECTOR_H_
#define MEDIA_PERCEPTION_MOJO_CONNECTOR_H_

#include <base/bind.h>
#include <base/single_thread_task_runner.h>
#include <base/threading/thread.h>
#include <mojo/edk/embedder/embedder.h>
#include <mojo/edk/embedder/process_delegate.h>
#include <mojo/public/cpp/bindings/binding.h>
#include <memory>
#include <string>

#include "media_perception/producer_impl.h"
#include "media_perception/receiver_impl.h"
#include "media_perception/video_capture_service_client.h"
#include "mojom/connector.mojom.h"


namespace mri {

class MojoConnector : public mojo::edk::ProcessDelegate {
 public:
  MojoConnector();
  ~MojoConnector() {}
  // Uses a file descriptor to establish a Mojo connection.
  void ReceiveMojoInvitationFileDescriptor(int fd_int);

  // Use the Mojo connector to ensure the video capture servicec is started in
  // Chrome and get access to the video capture service Mojo API.
  void ConnectToVideoCaptureService();

  // Get the list of video devices from the video capture service.
  void GetDevices(
      const VideoCaptureServiceClient::GetDevicesCallback& callback);

  // Attempts to acquire exclusive access to a video device. Note that this does
  // not block another client of the video capture service from taking over
  // access on this device, which would disconnect this client.
  void SetActiveDevice(
      std::string device_id,
      const VideoCaptureServiceClient::SetActiveDeviceCallback& callback);

  // Starts video capture on the active device.
  void StartVideoCapture(const CaptureFormat& capture_format,
                         std::function<void(uint64_t timestamp_in_microseconds,
                                            const uint8_t* data, int data_size)>
                             frame_handler);

  // Stops video capture on the active device.
  void StopVideoCapture();

  // Creates a new virtual device that frames can be fed into.
  // producer_impl is owned by the caller.
  void CreateVirtualDevice(
      const VideoDevice& video_device, ProducerImpl* producer_impl,
      const VideoCaptureServiceClient::VirtualDeviceCallback& callback);

  // producer_impl is owned by the caller.
  void PushFrameToVirtualDevice(ProducerImpl* producer_impl,
                                base::TimeDelta timestamp,
                                std::unique_ptr<const uint8_t[]> data,
                                int data_size, PixelFormat pixel_format,
                                int frame_width, int frame_height);

 private:
  // Handler for when the Mojo connection is closed or errors out.
  void OnConnectionErrorOrClosed();

  // mojo::edk::ProcessDelegate:
  void OnShutdownComplete() override;

  void AcceptConnectionOnIpcThread(base::ScopedFD fd);

  void ConnectToVideoCaptureServiceOnIpcThread();

  void GetDevicesOnIpcThread(
      const VideoCaptureServiceClient::GetDevicesCallback& callback);

  void OnDeviceInfosReceived(
      const VideoCaptureServiceClient::GetDevicesCallback& callback,
      mojo::Array<media::mojom::VideoCaptureDeviceInfoPtr> infos);

  void SetActiveDeviceOnIpcThread(
      std::string device_id,
      const VideoCaptureServiceClient::SetActiveDeviceCallback& callback);

  void OnSetActiveDeviceCallback(
      const VideoCaptureServiceClient::SetActiveDeviceCallback& callback,
      video_capture::mojom::DeviceAccessResultCode code);

  void StartVideoCaptureOnIpcThread(const CaptureFormat& capture_format);

  void StopVideoCaptureOnIpcThread();

  // producer_impl is owned by the caller.
  void CreateVirtualDeviceOnIpcThread(
      const VideoDevice& video_device, ProducerImpl* producer_impl,
      const VideoCaptureServiceClient::VirtualDeviceCallback& callback);

  // producer_impl is owned by the caller.
  void PushFrameToVirtualDeviceOnIpcThread(
      ProducerImpl* producer_impl, base::TimeDelta timestamp,
      std::unique_ptr<const uint8_t[]> data, int data_size,
      PixelFormat pixel_format, int frame_width, int frame_height);

  // Separate thread for doing IPC via Mojo because Mojo is asynchronous
  // by default.
  base::Thread ipc_thread_;

  // Mojo object for connecting to the video capture service.
  ::chromeos::media_perception::mojom::ConnectorPtr connector_;

  // Entry point Mojo object for talking to the video capture service API.
  video_capture::mojom::DeviceFactoryPtr device_factory_;

  // Provides interface to an open device.
  video_capture::mojom::DevicePtr active_device_;

  // Provides interface for receiving frames from the video capture service.
  ReceiverImpl receiver_impl_;
};

}  // namespace mri

#endif  // MEDIA_PERCEPTION_MOJO_CONNECTOR_H_