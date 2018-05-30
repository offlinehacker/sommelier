// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <base/at_exit.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <base/memory/ref_counted.h>
#include <base/message_loop/message_loop.h>
#include <base/run_loop.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_piece.h>
#include <base/strings/string_split.h>
#include <base/strings/stringprintf.h>
#include <base/sys_info.h>
#include <brillo/flag_helper.h>
#include <brillo/syslog_logging.h>
#include <chromeos/dbus/service_constants.h>
#include <crosvm/qcow_utils.h>
#include <dbus/bus.h>
#include <dbus/message.h>
#include <dbus/object_path.h>
#include <dbus/object_proxy.h>
#include <vm_concierge/proto_bindings/service.pb.h>

using std::string;

namespace {

constexpr int kDefaultTimeoutMs = 80 * 1000;

constexpr char kImageTypeQcow2[] = "qcow2";
constexpr char kImageTypeRaw[] = "raw";
constexpr int64_t kMinimumDiskSize = 1ll * 1024 * 1024 * 1024;  // 1 GiB
constexpr char kRemovableMediaRoot[] = "/media/removable";
constexpr char kStorageCryptohomeRoot[] = "cryptohome-root";
constexpr char kStorageCryptohomeDownloads[] = "cryptohome-downloads";

// Converts an IPv4 address in network byte order into a string.
void IPv4AddressToString(uint32_t addr, string* address) {
  CHECK(address);

  char buf[INET_ADDRSTRLEN];
  struct in_addr in = {
      .s_addr = addr,
  };
  if (inet_ntop(AF_INET, &in, buf, sizeof(buf)) == nullptr) {
    PLOG(WARNING) << "Failed to convert " << addr << " into a string";
    return;
  }

  *address = buf;
}

int StartVm(dbus::ObjectProxy* proxy,
            string owner_id,
            string name,
            string kernel,
            string rootfs,
            string extra_disks) {
  if (name.empty()) {
    LOG(ERROR) << "--name is required";
    return -1;
  }

  if (kernel.empty()) {
    LOG(ERROR) << "--kernel is required";
    return -1;
  }

  if (rootfs.empty()) {
    LOG(ERROR) << "--rootfs is required";
    return -1;
  }

  if (!base::PathExists(base::FilePath(kernel))) {
    LOG(ERROR) << kernel << " does not exist";
    return -1;
  }

  if (!base::PathExists(base::FilePath(rootfs))) {
    LOG(ERROR) << rootfs << " does not exist";
    return -1;
  }

  LOG(INFO) << "Starting VM " << name << " with kernel " << kernel
            << " and rootfs " << rootfs;

  dbus::MethodCall method_call(vm_tools::concierge::kVmConciergeInterface,
                               vm_tools::concierge::kStartVmMethod);
  dbus::MessageWriter writer(&method_call);

  vm_tools::concierge::StartVmRequest request;
  request.set_owner_id(std::move(owner_id));
  request.set_name(std::move(name));

  request.mutable_vm()->set_kernel(std::move(kernel));
  request.mutable_vm()->set_rootfs(std::move(rootfs));

  for (base::StringPiece disk :
       base::SplitStringPiece(extra_disks, ":", base::TRIM_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    std::vector<base::StringPiece> tokens = base::SplitStringPiece(
        disk, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);

    // disk path[,writable[,image type[,mount target,fstype[,flags[,data]]]]]
    if (tokens.empty()) {
      LOG(ERROR) << "Disk description is empty";
      return -1;
    }

    vm_tools::concierge::DiskImage* disk_image = request.add_disks();
    disk_image->set_path(tokens[0].data(), tokens[0].size());
    disk_image->set_do_mount(false);

    if (tokens.size() > 1) {
      int writable = 0;
      if (!base::StringToInt(tokens[1], &writable)) {
        LOG(ERROR) << "Unable to parse writable token: " << tokens[1];
        return -1;
      }

      disk_image->set_writable(writable != 0);
    }

    if (tokens.size() > 2) {
      if (tokens[2] == kImageTypeRaw) {
        disk_image->set_image_type(vm_tools::concierge::DISK_IMAGE_RAW);
      } else if (tokens[2] == kImageTypeQcow2) {
        disk_image->set_image_type(vm_tools::concierge::DISK_IMAGE_QCOW2);
      } else {
        LOG(ERROR) << "Invalid disk image type: " << tokens[2];
        return -1;
      }
    }

    if (tokens.size() > 3) {
      if (tokens.size() == 4) {
        LOG(ERROR) << "Missing fstype for " << disk;
        return -1;
      }
      disk_image->set_mount_point(tokens[3].data(), tokens[3].size());
      disk_image->set_fstype(tokens[4].data(), tokens[4].size());
      disk_image->set_do_mount(true);
    }

    if (tokens.size() > 5) {
      uint64_t flags;
      if (!base::HexStringToUInt64(tokens[5], &flags)) {
        LOG(ERROR) << "Unable to parse flags: " << tokens[5];
        return -1;
      }

      disk_image->set_flags(flags);
    }

    if (tokens.size() > 6) {
      // Unsplit the rest of the string since data is comma-separated.
      string data(tokens[6].as_string());
      for (int i = 7; i < tokens.size(); i++) {
        data += ",";
        tokens[i].AppendToString(&data);
      }

      disk_image->set_data(std::move(data));
    }

    if (!base::PathExists(base::FilePath(disk_image->path()))) {
      LOG(ERROR) << "Extra disk path does not exist: " << disk_image->path();
      return -1;
    }

    char flag_buf[20];
    snprintf(flag_buf, sizeof(flag_buf), "0x%x", disk_image->flags());

    LOG(INFO) << "Disk " << disk_image->path();
    LOG(INFO) << "    mnt point: " << disk_image->mount_point();
    LOG(INFO) << "    type:      " << disk_image->fstype();
    LOG(INFO) << "    flags:     " << flag_buf;
    LOG(INFO) << "    data:      " << disk_image->data();
    LOG(INFO) << "    writable:  " << disk_image->writable();
    LOG(INFO) << "    do_mount:  " << disk_image->do_mount();
  }

  if (!writer.AppendProtoAsArrayOfBytes(request)) {
    LOG(ERROR) << "Failed to encode StartVmRequest protobuf";
    return -1;
  }

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  dbus::MessageReader reader(dbus_response.get());
  vm_tools::concierge::StartVmResponse response;
  if (!reader.PopArrayOfBytesAsProto(&response)) {
    LOG(ERROR) << "Failed to parse response protobuf";
    return -1;
  }

  if (!response.success()) {
    LOG(ERROR) << "Failed to start VM: " << response.failure_reason();
    return -1;
  }

  vm_tools::concierge::VmInfo vm_info = response.vm_info();
  string address;
  IPv4AddressToString(vm_info.ipv4_address(), &address);

  LOG(INFO) << "Started VM with";
  LOG(INFO) << "    ip address: " << address;
  LOG(INFO) << "    vsock cid:  " << vm_info.cid();
  LOG(INFO) << "    process id: " << vm_info.pid();

  return 0;
}

int StopVm(dbus::ObjectProxy* proxy, string owner_id, string name) {
  if (name.empty()) {
    LOG(ERROR) << "--name is required";
    return -1;
  }

  LOG(INFO) << "Stopping VM " << name;

  dbus::MethodCall method_call(vm_tools::concierge::kVmConciergeInterface,
                               vm_tools::concierge::kStopVmMethod);
  dbus::MessageWriter writer(&method_call);

  vm_tools::concierge::StopVmRequest request;
  request.set_owner_id(std::move(owner_id));
  request.set_name(std::move(name));

  if (!writer.AppendProtoAsArrayOfBytes(request)) {
    LOG(ERROR) << "Failed to encode StopVmRequest protobuf";
    return -1;
  }

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  dbus::MessageReader reader(dbus_response.get());
  vm_tools::concierge::StopVmResponse response;
  if (!reader.PopArrayOfBytesAsProto(&response)) {
    LOG(ERROR) << "Failed to parse response protobuf";
    return -1;
  }

  if (!response.success()) {
    LOG(ERROR) << "Failed to stop VM: " << response.failure_reason();
    return -1;
  }

  LOG(INFO) << "Done";
  return 0;
}

int StopAllVms(dbus::ObjectProxy* proxy) {
  LOG(INFO) << "Stopping all VMs";

  dbus::MethodCall method_call(vm_tools::concierge::kVmConciergeInterface,
                               vm_tools::concierge::kStopAllVmsMethod);

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  LOG(INFO) << "Done";
  return 0;
}

int GetVmInfo(dbus::ObjectProxy* proxy, string owner_id, string name) {
  LOG(INFO) << "Getting VM info";

  dbus::MethodCall method_call(vm_tools::concierge::kVmConciergeInterface,
                               vm_tools::concierge::kGetVmInfoMethod);
  dbus::MessageWriter writer(&method_call);

  vm_tools::concierge::GetVmInfoRequest request;
  request.set_owner_id(std::move(owner_id));
  request.set_name(std::move(name));

  if (!writer.AppendProtoAsArrayOfBytes(request)) {
    LOG(ERROR) << "Failed to encode GetVmInfo protobuf";
    return -1;
  }

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  dbus::MessageReader reader(dbus_response.get());
  vm_tools::concierge::GetVmInfoResponse response;
  if (!reader.PopArrayOfBytesAsProto(&response)) {
    LOG(ERROR) << "Failed to parse response protobuf";
    return -1;
  }

  if (!response.success()) {
    LOG(ERROR) << "Failed to get VM info";
    return -1;
  }

  vm_tools::concierge::VmInfo vm_info = response.vm_info();
  string address;
  IPv4AddressToString(vm_info.ipv4_address(), &address);

  LOG(INFO) << "VM:           " << name;
  LOG(INFO) << "IPv4 address: " << address;
  LOG(INFO) << "pid:          " << vm_info.pid();
  LOG(INFO) << "vsock cid:    " << vm_info.cid();
  LOG(INFO) << "Done";
  return 0;
}

int CreateDiskImage(dbus::ObjectProxy* proxy,
                    string cryptohome_id,
                    string disk_path,
                    uint64_t disk_size,
                    string image_type,
                    string storage_location,
                    string* result_path) {
  if (cryptohome_id.empty()) {
    LOG(ERROR) << "Cryptohome id cannot be empty";
    return -1;
  } else if (disk_path.empty()) {
    LOG(ERROR) << "Disk path cannot be empty";
    return -1;
  } else if (disk_size == 0) {
    LOG(ERROR) << "Disk size cannot be 0";
    return -1;
  }

  LOG(INFO) << "Creating disk image";

  dbus::MethodCall method_call(vm_tools::concierge::kVmConciergeInterface,
                               vm_tools::concierge::kCreateDiskImageMethod);
  dbus::MessageWriter writer(&method_call);

  vm_tools::concierge::CreateDiskImageRequest request;
  request.set_cryptohome_id(std::move(cryptohome_id));
  request.set_disk_path(std::move(disk_path));
  request.set_disk_size(std::move(disk_size));

  if (image_type == kImageTypeRaw) {
    request.set_image_type(vm_tools::concierge::DISK_IMAGE_RAW);
  } else if (image_type == kImageTypeQcow2) {
    request.set_image_type(vm_tools::concierge::DISK_IMAGE_QCOW2);
  } else {
    LOG(ERROR) << "'" << image_type << "' is not a valid disk image type";
    return -1;
  }

  if (storage_location == kStorageCryptohomeRoot) {
    request.set_storage_location(vm_tools::concierge::STORAGE_CRYPTOHOME_ROOT);
  } else if (storage_location == kStorageCryptohomeDownloads) {
    request.set_storage_location(
        vm_tools::concierge::STORAGE_CRYPTOHOME_DOWNLOADS);
  } else {
    LOG(ERROR) << "'" << storage_location
               << "' is not a valid storage location";
    return -1;
  }

  if (!writer.AppendProtoAsArrayOfBytes(request)) {
    LOG(ERROR) << "Failed to encode CreateDiskImageRequest protobuf";
    return -1;
  }

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  dbus::MessageReader reader(dbus_response.get());
  vm_tools::concierge::CreateDiskImageResponse response;
  if (!reader.PopArrayOfBytesAsProto(&response)) {
    LOG(ERROR) << "Failed to parse response protobuf";
    return -1;
  }

  if (response.status() != vm_tools::concierge::DISK_STATUS_EXISTS &&
      response.status() != vm_tools::concierge::DISK_STATUS_CREATED) {
    LOG(ERROR) << "Failed to create disk image: " << response.failure_reason();
    return -1;
  }

  if (result_path)
    *result_path = response.disk_path();

  return 0;
}

int DestroyDiskImage(dbus::ObjectProxy* proxy,
                     string cryptohome_id,
                     string name,
                     string storage_location) {
  if (cryptohome_id.empty()) {
    LOG(ERROR) << "Cryptohome id cannot be empty";
    return -1;
  } else if (name.empty()) {
    LOG(ERROR) << "Name cannot be empty";
    return -1;
  }

  LOG(INFO) << "Destroying disk image";

  dbus::MethodCall method_call(vm_tools::concierge::kVmConciergeInterface,
                               vm_tools::concierge::kDestroyDiskImageMethod);
  dbus::MessageWriter writer(&method_call);

  vm_tools::concierge::DestroyDiskImageRequest request;
  request.set_cryptohome_id(std::move(cryptohome_id));
  request.set_disk_path(std::move(name));

  if (storage_location == kStorageCryptohomeRoot) {
    request.set_storage_location(vm_tools::concierge::STORAGE_CRYPTOHOME_ROOT);
  } else if (storage_location == kStorageCryptohomeDownloads) {
    request.set_storage_location(
        vm_tools::concierge::STORAGE_CRYPTOHOME_DOWNLOADS);
  } else {
    LOG(ERROR) << "'" << storage_location
               << "' is not a valid storage location";
    return -1;
  }

  if (!writer.AppendProtoAsArrayOfBytes(request)) {
    LOG(ERROR) << "Failed to encode DestroyDiskImageRequest protobuf";
    return -1;
  }

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  dbus::MessageReader reader(dbus_response.get());
  vm_tools::concierge::DestroyDiskImageResponse response;
  if (!reader.PopArrayOfBytesAsProto(&response)) {
    LOG(ERROR) << "Failed to parse response protobuf";
    return -1;
  }

  if (response.status() != vm_tools::concierge::DISK_STATUS_DESTROYED &&
      response.status() != vm_tools::concierge::DISK_STATUS_DOES_NOT_EXIST) {
    LOG(ERROR) << "Failed to destroy disk image: " << response.failure_reason();
    return -1;
  }

  return 0;
}

int ListDiskImages(dbus::ObjectProxy* proxy,
                   string cryptohome_id,
                   string storage_location) {
  if (cryptohome_id.empty()) {
    LOG(ERROR) << "Cryptohome id cannot be empty";
    return -1;
  }

  dbus::MethodCall method_call(vm_tools::concierge::kVmConciergeInterface,
                               vm_tools::concierge::kListVmDisksMethod);
  dbus::MessageWriter writer(&method_call);

  vm_tools::concierge::ListVmDisksRequest request;
  request.set_cryptohome_id(std::move(cryptohome_id));

  if (storage_location == kStorageCryptohomeRoot) {
    request.set_storage_location(vm_tools::concierge::STORAGE_CRYPTOHOME_ROOT);
  } else if (storage_location == kStorageCryptohomeDownloads) {
    request.set_storage_location(
        vm_tools::concierge::STORAGE_CRYPTOHOME_DOWNLOADS);
  } else {
    LOG(ERROR) << "'" << storage_location
               << "' is not a valid storage location";
    return -1;
  }

  if (!writer.AppendProtoAsArrayOfBytes(request)) {
    LOG(ERROR) << "Failed to encode ListVmDisksRequest protobuf";
    return -1;
  }

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  dbus::MessageReader reader(dbus_response.get());
  vm_tools::concierge::ListVmDisksResponse response;
  if (!reader.PopArrayOfBytesAsProto(&response)) {
    LOG(ERROR) << "Failed to parse response protobuf";
    return -1;
  }

  if (!response.success()) {
    LOG(ERROR) << "Failed list VM disks: " << response.failure_reason();
    return -1;
  }

  for (const auto& image : response.images()) {
    std::cout << image << std::endl;
  }
  std::cout << "Total Size (bytes): " << response.total_size() << std::endl;
  return 0;
}

int CreateExternalDiskImage(string removable_media,
                            string name,
                            uint64_t disk_size) {
  if (disk_size < kMinimumDiskSize) {
    LOG(ERROR) << "Disk size must be greater than one megabyte";
    return -1;
  }
  if (removable_media.empty() || name.empty()) {
    LOG(ERROR) << "Both --removable_media and --name are required.";
    return -1;
  }

  base::FilePath media_path =
      base::FilePath(kRemovableMediaRoot).Append(removable_media);
  base::FilePath disk_path = media_path.Append(name);

  if (disk_path.ReferencesParent() || !base::DirectoryExists(media_path)) {
    LOG(ERROR) << "Invalid Removable Media path";
    return -1;
  }

  return create_qcow_with_size(disk_path.value().c_str(), disk_size);
}

int StartTerminaVm(dbus::ObjectProxy* proxy,
                   string name,
                   string cryptohome_id,
                   string removable_media,
                   string image_name) {
  if (name.empty()) {
    LOG(ERROR) << "--name is required";
    return -1;
  }

  LOG(INFO) << "Starting Termina VM '" << name << "'";

  dbus::MethodCall method_call(vm_tools::concierge::kVmConciergeInterface,
                               vm_tools::concierge::kStartVmMethod);
  dbus::MessageWriter writer(&method_call);

  vm_tools::concierge::StartVmRequest request;
  request.set_start_termina(true);

  if (!cryptohome_id.empty()) {
    int64_t disk_size =
        base::SysInfo::AmountOfFreeDiskSpace(base::FilePath("/home"));
    disk_size = (disk_size * 9) / 10;

    if (disk_size < kMinimumDiskSize)
      disk_size = kMinimumDiskSize;

    string disk_path;
    if (CreateDiskImage(proxy, cryptohome_id, name, disk_size, kImageTypeQcow2,
                        kStorageCryptohomeRoot, &disk_path) != 0) {
      return -1;
    }

    vm_tools::concierge::DiskImage* disk_image = request.add_disks();
    disk_image->set_path(std::move(disk_path));
    disk_image->set_image_type(vm_tools::concierge::DISK_IMAGE_QCOW2);
    disk_image->set_writable(true);
    disk_image->set_do_mount(false);

    request.set_owner_id(std::move(cryptohome_id));
    request.set_name(std::move(name));
    if (!writer.AppendProtoAsArrayOfBytes(request)) {
      LOG(ERROR) << "Failed to encode StartVmRequest protobuf";
      return -1;
    }
  } else if (!removable_media.empty()) {
    if (image_name.empty()) {
      LOG(ERROR) << "start: --image_name is required with --removable_media";
      return -1;
    }
    base::FilePath disk_path = base::FilePath(kRemovableMediaRoot)
                                   .Append(removable_media)
                                   .Append(image_name);
    if (disk_path.ReferencesParent()) {
      LOG(ERROR) << "Invalid removable_vm_path";
      return -1;
    }
    base::ScopedFD disk_fd(
        HANDLE_EINTR(open(disk_path.value().c_str(), O_RDWR | O_NOFOLLOW)));
    if (!disk_fd.is_valid()) {
      LOG(ERROR) << "Failed opening VM disk state";
      return -1;
    }

    request.set_name(std::move(name));
    request.set_use_fd_for_storage(true);
    if (!writer.AppendProtoAsArrayOfBytes(request)) {
      LOG(ERROR) << "Failed to encode StartVmRequest protobuf";
      return -1;
    }
    writer.AppendFileDescriptor(disk_fd.get());
  } else {
    LOG(ERROR) << "either --removable_vm_path or --cryptohome_id is required";
    return -1;
  }

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  dbus::MessageReader reader(dbus_response.get());
  vm_tools::concierge::StartVmResponse response;
  if (!reader.PopArrayOfBytesAsProto(&response)) {
    LOG(ERROR) << "Failed to parse response protobuf";
    return -1;
  }

  if (!response.success()) {
    LOG(ERROR) << "Failed to start VM: " << response.failure_reason();
    return -1;
  }

  vm_tools::concierge::VmInfo vm_info = response.vm_info();
  string address;
  IPv4AddressToString(vm_info.ipv4_address(), &address);

  LOG(INFO) << "Started Termina VM with";
  LOG(INFO) << "    ip address: " << address;
  LOG(INFO) << "    vsock cid:  " << vm_info.cid();
  LOG(INFO) << "    process id: " << vm_info.pid();

  return 0;
}

int StartContainer(dbus::ObjectProxy* proxy,
                   string name,
                   string container_name,
                   string cryptohome_id) {
  if (name.empty()) {
    LOG(ERROR) << "--name is required";
    return -1;
  }

  if (container_name.empty()) {
    LOG(ERROR) << "--container_name is required";
    return -1;
  }

  LOG(INFO) << "Starting VM Container '" << name << ":" << container_name
            << "'";

  dbus::MethodCall method_call(vm_tools::concierge::kVmConciergeInterface,
                               vm_tools::concierge::kStartContainerMethod);
  dbus::MessageWriter writer(&method_call);

  vm_tools::concierge::StartContainerRequest request;
  request.set_vm_name(name);
  request.set_container_name(container_name);
  request.set_cryptohome_id(cryptohome_id);

  if (!writer.AppendProtoAsArrayOfBytes(request)) {
    LOG(ERROR) << "Failed to encode StartContainerRequest protobuf";
    return -1;
  }

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  dbus::MessageReader reader(dbus_response.get());
  vm_tools::concierge::StartContainerResponse response;
  if (!reader.PopArrayOfBytesAsProto(&response)) {
    LOG(ERROR) << "Failed to parse response protobuf";
    return -1;
  }

  int ret = -1;
  std::string status;
  switch (response.status()) {
    case vm_tools::concierge::CONTAINER_STATUS_RUNNING:
      status = "Running";
      ret = 0;
      break;
    case vm_tools::concierge::CONTAINER_STATUS_STARTING:
      status = "Starting";
      ret = 0;
      break;
    case vm_tools::concierge::CONTAINER_STATUS_FAILURE:
      status = "Failure";
      break;
    default:
      status = "Unknown";
      break;
  }

  LOG(INFO) << "Container state for '" << name << ":" << container_name << "'"
            << " is now " << status;

  return ret;
}

int LaunchApplication(dbus::ObjectProxy* proxy,
                      string owner_id,
                      string name,
                      string container_name,
                      string application) {
  if (name.empty()) {
    LOG(ERROR) << "--name is required";
    return -1;
  }

  if (container_name.empty()) {
    LOG(ERROR) << "--container_name is required";
    return -1;
  }

  if (application.empty()) {
    LOG(ERROR) << "--application is required";
    return -1;
  }

  LOG(INFO) << "Starting application " << application << " in '" << name << ":"
            << container_name << "'";

  dbus::MethodCall method_call(
      vm_tools::concierge::kVmConciergeInterface,
      vm_tools::concierge::kLaunchContainerApplicationMethod);
  dbus::MessageWriter writer(&method_call);

  vm_tools::concierge::LaunchContainerApplicationRequest request;
  request.set_owner_id(owner_id);
  request.set_vm_name(name);
  request.set_container_name(container_name);
  request.set_desktop_file_id(application);

  if (!writer.AppendProtoAsArrayOfBytes(request)) {
    LOG(ERROR) << "Failed to encode LaunchContainerApplicationRequest protobuf";
    return -1;
  }

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  dbus::MessageReader reader(dbus_response.get());
  vm_tools::concierge::LaunchContainerApplicationResponse response;
  if (!reader.PopArrayOfBytesAsProto(&response)) {
    LOG(ERROR) << "Failed to parse response protobuf";
    return -1;
  }

  if (!response.success()) {
    LOG(ERROR) << "Failed to launch application: " << response.failure_reason();
    return -1;
  }

  LOG(INFO) << "Launched application " << application << " in '" << name << ":"
            << container_name << "'";

  return 0;
}

void Write(const std::string& output_filepath, const std::string& content) {
  int content_size = content.size();
  if (content_size != base::WriteFile(base::FilePath(output_filepath),
                                      content.c_str(), content_size)) {
    LOG(ERROR) << "Failed to write to file " << output_filepath;
  }
}

int GetIcon(dbus::ObjectProxy* proxy,
            string owner_id,
            string name,
            string container_name,
            string application,
            int icon_size,
            int scale,
            string output_filepath) {
  if (name.empty()) {
    LOG(ERROR) << "--name is required";
    return -1;
  }

  if (container_name.empty()) {
    LOG(ERROR) << "--container_name is required";
    return -1;
  }

  if (application.empty()) {
    LOG(ERROR) << "--application is required";
    return -1;
  }

  if (output_filepath.empty()) {
    LOG(ERROR) << "--output_filepath is required";
    return -1;
  }

  LOG(INFO) << "Getting icon for " << application << " in '" << name << ":"
            << container_name << "'";

  dbus::MethodCall method_call(vm_tools::concierge::kVmConciergeInterface,
                               vm_tools::concierge::kGetContainerAppIconMethod);
  dbus::MessageWriter writer(&method_call);

  vm_tools::concierge::ContainerAppIconRequest request;
  request.set_owner_id(owner_id);
  request.set_vm_name(name);
  request.set_container_name(container_name);
  request.add_desktop_file_ids(application);
  request.set_size(icon_size);
  request.set_scale(scale);

  if (!writer.AppendProtoAsArrayOfBytes(request)) {
    LOG(ERROR) << "Failed to encode ContainerAppIconRequest protobuf";
    return -1;
  }

  std::unique_ptr<dbus::Response> dbus_response =
      proxy->CallMethodAndBlock(&method_call, kDefaultTimeoutMs);
  if (!dbus_response) {
    LOG(ERROR) << "Failed to send dbus message to concierge service";
    return -1;
  }

  dbus::MessageReader reader(dbus_response.get());
  vm_tools::concierge::ContainerAppIconResponse response;
  if (!reader.PopArrayOfBytesAsProto(&response)) {
    LOG(ERROR) << "Failed to parse response protobuf";
    return -1;
  }

  // This should have up to one icon since the input has only one application
  // file ID.
  CHECK_LE(response.icons_size(), 1);
  for (vm_tools::concierge::DesktopIcon icon : response.icons()) {
    if (!icon.icon().empty())
      Write(output_filepath, icon.icon());
  }

  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;

  // Operations.
  DEFINE_bool(start, false, "Start a VM");
  DEFINE_bool(stop, false, "Stop a running VM");
  DEFINE_bool(stop_all, false, "Stop all running VMs");
  DEFINE_bool(get_vm_info, false, "Get info for the given VM");
  DEFINE_bool(create_disk, false, "Create a disk image");
  DEFINE_bool(create_external_disk, false,
              "Create a disk image on removable media");
  DEFINE_bool(destroy_disk, false, "Destroy a disk image");
  DEFINE_bool(list_disks, false, "List disk images");
  DEFINE_bool(start_termina_vm, false,
              "Start a termina VM with a default config");
  DEFINE_bool(start_container, false, "Starts a container within a VM");
  DEFINE_bool(launch_application, false,
              "Launches an application in a container");
  DEFINE_bool(get_icon, false, "Get an app icon from a container within a VM");

  // Parameters.
  DEFINE_string(kernel, "", "Path to the VM kernel");
  DEFINE_string(rootfs, "", "Path to the VM rootfs");
  DEFINE_string(name, "", "Name to assign to the VM");
  DEFINE_string(extra_disks, "",
                "Additional disk images to be mounted inside the VM");
  DEFINE_string(container_name, "", "Name of the container within the VM");
  DEFINE_string(application, "", "Name of the application to launch");
  DEFINE_string(removable_media, "", "Name of the removable media to use");
  DEFINE_string(image_name, "", "Name of the file on removable media to use");
  DEFINE_string(output_filepath, "",
                "Filename with path to write appliction icon to");
  DEFINE_int32(icon_size, 48,
               "The size of the icon to get is this icon_size by icon_size");
  DEFINE_int32(scale, 1, "The scale that the icon is designed to use with");

  // create_disk parameters.
  DEFINE_string(cryptohome_id, "", "User cryptohome id");
  DEFINE_string(disk_path, "", "Path to the disk image to create");
  DEFINE_uint64(disk_size, 0, "Size of the disk image to create");
  DEFINE_string(image_type, "qcow2", "Disk image type");
  DEFINE_string(storage_location, "cryptohome-root",
                "Location to store the disk image");

  brillo::FlagHelper::Init(argc, argv, "vm_concierge client tool");
  brillo::InitLog(brillo::kLogToStderrIfTty);

  base::MessageLoopForIO message_loop;

  dbus::Bus::Options opts;
  opts.bus_type = dbus::Bus::SYSTEM;
  scoped_refptr<dbus::Bus> bus(new dbus::Bus(std::move(opts)));

  if (!bus->Connect()) {
    LOG(ERROR) << "Failed to connect to system bus";
    return -1;
  }

  dbus::ObjectProxy* proxy = bus->GetObjectProxy(
      vm_tools::concierge::kVmConciergeServiceName,
      dbus::ObjectPath(vm_tools::concierge::kVmConciergeServicePath));
  if (!proxy) {
    LOG(ERROR) << "Unable to get dbus proxy for "
               << vm_tools::concierge::kVmConciergeServiceName;
    return -1;
  }

  // The standard says that bool to int conversion is implicit and that
  // false => 0 and true => 1.
  // clang-format off
  if (FLAGS_start + FLAGS_stop + FLAGS_stop_all + FLAGS_get_vm_info +
      FLAGS_create_disk + FLAGS_create_external_disk + FLAGS_start_termina_vm +
      FLAGS_destroy_disk + FLAGS_list_disks + FLAGS_start_container +
      FLAGS_launch_application + FLAGS_get_icon != 1) {
    // clang-format on
    LOG(ERROR) << "Exactly one of --start, --stop, --stop_all, --get_vm_info, "
               << "--create_disk, --create_external_disk --destroy_disk, "
               << "--list_disks, --start_termina_vm, --start_container, "
               << "--launch_application, or --get_icon "
               << "must be provided";
    return -1;
  }

  if (FLAGS_start) {
    return StartVm(proxy, std::move(FLAGS_cryptohome_id), std::move(FLAGS_name),
                   std::move(FLAGS_kernel), std::move(FLAGS_rootfs),
                   std::move(FLAGS_extra_disks));
  } else if (FLAGS_stop) {
    return StopVm(proxy, std::move(FLAGS_cryptohome_id), std::move(FLAGS_name));
  } else if (FLAGS_stop_all) {
    return StopAllVms(proxy);
  } else if (FLAGS_get_vm_info) {
    return GetVmInfo(proxy, std::move(FLAGS_cryptohome_id),
                     std::move(FLAGS_name));
  } else if (FLAGS_create_disk) {
    return CreateDiskImage(proxy, std::move(FLAGS_cryptohome_id),
                           std::move(FLAGS_disk_path), FLAGS_disk_size,
                           std::move(FLAGS_image_type),
                           std::move(FLAGS_storage_location), nullptr);
  } else if (FLAGS_create_external_disk) {
    return CreateExternalDiskImage(std::move(FLAGS_removable_media),
                                   std::move(FLAGS_name),
                                   std::move(FLAGS_disk_size));
  } else if (FLAGS_destroy_disk) {
    return DestroyDiskImage(proxy, std::move(FLAGS_cryptohome_id),
                            std::move(FLAGS_name),
                            std::move(FLAGS_storage_location));
  } else if (FLAGS_list_disks) {
    return ListDiskImages(proxy, std::move(FLAGS_cryptohome_id),
                          std::move(FLAGS_storage_location));
  } else if (FLAGS_start_termina_vm) {
    return StartTerminaVm(
        proxy, std::move(FLAGS_name), std::move(FLAGS_cryptohome_id),
        std::move(FLAGS_removable_media), std::move(FLAGS_image_name));
  } else if (FLAGS_start_container) {
    return StartContainer(proxy, std::move(FLAGS_name),
                          std::move(FLAGS_container_name),
                          std::move(FLAGS_cryptohome_id));
  } else if (FLAGS_launch_application) {
    return LaunchApplication(
        proxy, std::move(FLAGS_cryptohome_id), std::move(FLAGS_name),
        std::move(FLAGS_container_name), std::move(FLAGS_application));
  } else if (FLAGS_get_icon) {
    return GetIcon(proxy, std::move(FLAGS_cryptohome_id), std::move(FLAGS_name),
                   std::move(FLAGS_container_name),
                   std::move(FLAGS_application), FLAGS_icon_size, FLAGS_scale,
                   std::move(FLAGS_output_filepath));
  }

  // Unreachable.
  return 0;
}
