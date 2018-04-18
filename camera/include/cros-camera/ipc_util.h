/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef INCLUDE_CROS_CAMERA_IPC_UTIL_H_
#define INCLUDE_CROS_CAMERA_IPC_UTIL_H_

#include <string>

#include <base/files/scoped_file.h>
#include <mojo/edk/embedder/embedder.h>

namespace base {
class FilePath;
}  // namespace base

namespace cros {

bool CreateServerUnixDomainSocket(const base::FilePath& socket_path,
                                  int* server_listen_fd);

bool ServerAcceptConnection(int server_listen_fd, int* server_socket);

base::ScopedFD CreateClientUnixDomainSocket(const base::FilePath& socket_path);

MojoResult CreateMojoChannelToParentByUnixDomainSocket(
    const base::FilePath& socket_path,
    mojo::ScopedMessagePipeHandle* child_pipe);

MojoResult CreateMojoChannelToChildByUnixDomainSocket(
    const base::FilePath& socket_path,
    mojo::ScopedMessagePipeHandle* parent_pipe);

mojo::ScopedHandle WrapPlatformHandle(int handle);
int UnwrapPlatformHandle(mojo::ScopedHandle handle);

}  // namespace cros

#endif  // INCLUDE_CROS_CAMERA_IPC_UTIL_H_