/* Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef HAL_USB_CAPTURE_REQUEST_H_
#define HAL_USB_CAPTURE_REQUEST_H_

#include <vector>

#include <hardware/camera3.h>

#include "hal/usb/camera_metadata.h"

namespace arc {

class CaptureRequest {
 public:
  explicit CaptureRequest(const camera3_capture_request& request);
  ~CaptureRequest();

  const int GetFrameNumber() const { return frame_number_; }
  CameraMetadata* GetMetadata() { return &metadata_; }
  std::vector<camera3_stream_buffer_t>* GetStreamBuffers() {
    return &output_stream_buffers_;
  }

 private:
  // Store all necessary information from capture request.
  // - CaptureRequest do not need to free the buffers from Framework.
  // - We can use the buffers after waiting on
  //   camera3_stream_buffer_t.acquire_fence.
  // - If the buffers are finished by the hal, set release_fence to -1.
  //   We cannot access the buffers after calling process_capture_result.
  // - If we need to access the buffers after calling process_capture_result
  //   (e.g. jpeg encoding is slow), set release_fence to a fence we create.
  //   We cannot use the buffers after signaling the fence.
  const int frame_number_;
  CameraMetadata metadata_;
  std::vector<camera3_stream_buffer_t> output_stream_buffers_;
  std::vector<buffer_handle_t> buffer_handles_;
};

}  // namespace arc

#endif  // HAL_USB_CAPTURE_REQUEST_H_
