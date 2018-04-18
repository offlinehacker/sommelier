/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef INCLUDE_CROS_CAMERA_JPEG_DECODE_ACCELERATOR_H_
#define INCLUDE_CROS_CAMERA_JPEG_DECODE_ACCELERATOR_H_

#include <stdint.h>
#include <memory>

#include <base/bind.h>

namespace cros {

using DecodeCallback = base::Callback<void(int buffer_id, int error)>;

// Encapsulates a converter from JPEG to YU12 format. This class is not
// thread-safe.
// Before using this class, make sure mojo is initialized first.
class JpegDecodeAccelerator {
 public:
  // Enumeration of decode errors.
  enum class Error {
    // No error. Decode succeeded.
    NO_ERRORS,
    // Invalid argument was passed to an API method, e.g. the output buffer is
    // too small, JPEG width/height are too big for JDA.
    INVALID_ARGUMENT,
    // Encoded input is unreadable, e.g. failed to map on another process.
    UNREADABLE_INPUT,
    // Failed to parse compressed JPEG picture.
    PARSE_JPEG_FAILED,
    // Failed to decode JPEG due to unsupported JPEG features, such as
    // profiles, coding mode, or color formats.
    UNSUPPORTED_JPEG,
    // A fatal failure occurred in the GPU process layer or one of its
    // dependencies. Examples of such failures include hardware failures,
    // driver failures, library failures, browser programming errors, and so
    // on. Client is responsible for destroying JDA after receiving this.
    PLATFORM_FAILURE,
    // Largest used enum. This should be adjusted when new errors are added.
    LARGEST_MOJO_ERROR_ENUM = PLATFORM_FAILURE,
    // The Mojo channel is corrupted. User can call Start() again to establish
    // the channel.
    TRY_START_AGAIN,
    // Create shared memory for input buffer failed.
    CREATE_SHARED_MEMORY_FAILED,
    // mmap() for input failed.
    MMAP_FAILED,
    // No decode response from Mojo channel after timeout.
    NO_DECODE_RESPONSE,
  };

  static std::unique_ptr<JpegDecodeAccelerator> CreateInstance();

  virtual ~JpegDecodeAccelerator() {}

  // Starts the Jpeg decoder.
  // This method must be called before all the other methods are called.
  //
  // Returns:
  //    Returns true on success otherwise false.
  virtual bool Start() = 0;

  // Decodes the given buffer that contains one JPEG image.
  // The image is decoded from memory of |input_fd| with size
  // |input_buffer_size|. The size of JPEG image is |coded_size_width| and
  // |coded_size_height|.
  // Decoded I420 frame data will be put onto memory associated with |output_fd|
  // with allocated size |output_buffer_size|.
  // Note: This API doesn't close the |input_fd| and |output_fd|. Caller doesn't
  // need to dup file descriptor.
  //
  // Args:
  //    |input_fd|: A file descriptor of DMA buffer.
  //    |input_buffer_size|: Size of input buffer.
  //    |coded_size_width|: Width of JPEG image.
  //    |coded_size_height|: Height of JPEG image.
  //    |output_fd|: A file descriptor of shared memory.
  //    |output_buffer_size|: Size of output buffer.
  //
  // Returns:
  //    Returns enum Error to notify the decode status.
  //    If the return code is TRY_START_AGAIN, user can call Start() again and
  //    use this API.
  virtual Error DecodeSync(int input_fd,
                           uint32_t input_buffer_size,
                           int32_t coded_size_width,
                           int32_t coded_size_height,
                           int output_fd,
                           uint32_t output_buffer_size) = 0;

  // Asynchronous version of DecodeSync.
  //
  // Args:
  //    |input_fd|: A file descriptor of DMA buffer.
  //    |input_buffer_size|: Size of input buffer.
  //    |coded_size_width|: Width of JPEG image.
  //    |coded_size_height|: Height of JPEG image.
  //    |output_fd|: A file descriptor of shared memory.
  //    |output_buffer_size|: Size of output buffer.
  //    |callback|: callback function after finish decoding.
  //
  // Returns:
  //    Returns buffer_id of this Decode.
  virtual int32_t Decode(int input_fd,
                         uint32_t input_buffer_size,
                         int32_t coded_size_width,
                         int32_t coded_size_height,
                         int output_fd,
                         uint32_t output_buffer_size,
                         DecodeCallback callback) = 0;
};

}  // namespace cros

#endif  // INCLUDE_CROS_CAMERA_JPEG_DECODE_ACCELERATOR_H_