/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef INCLUDE_ARC_CAMERA_ALGORITHM_OPS_IMPL_H_
#define INCLUDE_ARC_CAMERA_ALGORITHM_OPS_IMPL_H_

#include "base/threading/thread.h"
#include "mojo/camera_algorithm.mojom.h"
#include "mojo/public/cpp/bindings/binding.h"

#include "arc/camera_algorithm.h"

namespace arc {

// This is the implementation of CameraAlgorithmOps mojo interface. It is used
// by the sandboxed camera algorithm library process.

class CameraAlgorithmOpsImpl : public mojom::CameraAlgorithmOps,
                               private camera_algorithm_callback_ops_t {
 public:
  // Get singleton instance
  static CameraAlgorithmOpsImpl* GetInstance();

  // Completes a binding by removing the message pipe endpoint from |request|
  // and binding it to the interface implementation.
  bool Bind(mojom::CameraAlgorithmOpsRequest request,
            camera_algorithm_ops_t* cam_algo,
            scoped_refptr<base::SingleThreadTaskRunner> ipc_task_runner,
            base::Closure& ipc_lost_handler);

  // Unbinds the underlying pipe.
  void Unbind();

  // Implementation of mojom::CameraAlgorithmOps::Initialize interface
  void Initialize(mojom::CameraAlgorithmCallbackOpsPtr callbacks,
                  const InitializeCallback& callback) override;

  // Implementation of mojom::CameraAlgorithmOps::RegisterBuffer interface
  void RegisterBuffer(mojo::ScopedHandle buffer_fd,
                      const RegisterBufferCallback& callback) override;

  // Implementation of mojom::CameraAlgorithmOps::Request interface
  void Request(mojo::Array<uint8_t> req_headers,
               int32_t buffer_handle,
               const RequestCallback& callback) override;

  // Implementation of mojom::CameraAlgorithmOps::DeregisterBuffers interface
  void DeregisterBuffers(mojo::Array<int32_t> buffer_handles) override;

 private:
  CameraAlgorithmOpsImpl();

  ~CameraAlgorithmOpsImpl() override {}

  static int32_t ReturnCallbackForwarder(
      const camera_algorithm_callback_ops_t* callback_ops,
      int32_t buffer_handle);

  int32_t ReturnCallback(int32_t buffer_handle);

  void ReturnCallbackOnIPCThread(int32_t buffer_handle,
                                 base::Callback<void(int32_t)> cb);

  // Binding of CameraAlgorithmOps interface to message pipe
  mojo::Binding<mojom::CameraAlgorithmOps> binding_;

  // Interface of camera algorithm library
  camera_algorithm_ops_t* cam_algo_;

  // Pointer to self for ReturnCallbackForwarder to get the singleton instance
  static CameraAlgorithmOpsImpl* singleton_;

  // Task runner of |CameraAlgorithmAdapter::ipc_thread_|
  scoped_refptr<base::SingleThreadTaskRunner> ipc_task_runner_;

  // Pointer to local proxy of remote CameraAlgorithmCallback interface
  // implementation
  mojom::CameraAlgorithmCallbackOpsPtr cb_ptr_;

  DISALLOW_COPY_AND_ASSIGN(CameraAlgorithmOpsImpl);
};

}  // namespace arc

#endif  // INCLUDE_ARC_CAMERA_ALGORITHM_OPS_IMPL_H_