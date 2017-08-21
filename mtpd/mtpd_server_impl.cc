// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mtpd/mtpd_server_impl.h"

#include <base/logging.h>
#include <base/rand_util.h>
#include <base/stl_util.h>
#include <base/strings/string_number_conversions.h>
#include <chromeos/dbus/service_constants.h>

namespace mtpd {

namespace {

// Maximum number of bytes to read from the device at one time. This is set low
// enough such that a reasonable device can read this much data before D-Bus
// times out.
const uint32_t kMaxReadCount = 1024 * 1024;

const char kInvalidHandleErrorMessage[] = "Invalid handle ";

void SetInvalidHandleError(const std::string& handle, DBus::Error* error) {
  std::string error_msg = kInvalidHandleErrorMessage + handle;
  error->set(kMtpdServiceError, error_msg.c_str());
}

template <typename ReturnType>
ReturnType InvalidHandle(const std::string& handle, DBus::Error* error) {
  SetInvalidHandleError(handle, error);
  return ReturnType();
}

}  // namespace

MtpdServer::MtpdServer(DBus::Connection& connection)
    : DBus::ObjectAdaptor(connection, kMtpdServicePath),
      device_manager_(this) {}

MtpdServer::~MtpdServer() {}

std::vector<std::string> MtpdServer::EnumerateStorages(DBus::Error& error) {
  return device_manager_.EnumerateStorages();
}

std::vector<uint8_t> MtpdServer::GetStorageInfo(const std::string& storageName,
                                                DBus::Error& error) {
  const StorageInfo* info = device_manager_.GetStorageInfo(storageName);
  return info ? info->ToDBusFormat() : StorageInfo().ToDBusFormat();
}

std::vector<uint8_t> MtpdServer::GetStorageInfoFromDevice(
    const std::string& storageName,
    DBus::Error& error) {
  const StorageInfo* info =
      device_manager_.GetStorageInfoFromDevice(storageName);
  return info ? info->ToDBusFormat() : StorageInfo().ToDBusFormat();
}

std::string MtpdServer::OpenStorage(const std::string& storageName,
                                    const std::string& mode,
                                    DBus::Error& error) {
  if (!(mode == kReadOnlyMode || mode == kReadWriteMode)) {
    std::string error_msg = "Cannot open " + storageName + " in mode: " + mode;
    error.set(kMtpdServiceError, error_msg.c_str());
    return std::string();
  }

  if (!device_manager_.HasStorage(storageName)) {
    std::string error_msg = "Cannot open unknown storage " + storageName;
    error.set(kMtpdServiceError, error_msg.c_str());
    return std::string();
  }

  std::string id;
  uint32_t random_data[4];
  do {
    base::RandBytes(random_data, sizeof(random_data));
    id = base::HexEncode(random_data, sizeof(random_data));
  } while (ContainsKey(handle_map_, id));

  handle_map_.insert(std::make_pair(id, std::make_pair(storageName, mode)));
  return id;
}

void MtpdServer::CloseStorage(const std::string& handle, DBus::Error& error) {
  if (handle_map_.erase(handle) == 0)
    SetInvalidHandleError(handle, &error);
}

std::vector<uint32_t> MtpdServer::ReadDirectoryEntryIds(
    const std::string& handle,
    const uint32_t& fileId,
    DBus::Error& error) {
  std::vector<uint32_t> directory_listing;
  std::string storage_name = LookupHandle(handle);
  if (storage_name.empty()) {
    SetInvalidHandleError(handle, &error);
    return directory_listing;
  }

  if (!device_manager_.ReadDirectoryEntryIds(storage_name, fileId,
                                             &directory_listing)) {
    error.set(kMtpdServiceError, "ReadDirectoryEntryIds failed");
  }
  return directory_listing;
}

std::vector<uint8_t> MtpdServer::GetFileInfo(
    const std::string& handle,
    const std::vector<uint32_t>& fileIds,
    DBus::Error& error) {
  if (fileIds.empty()) {
    error.set(kMtpdServiceError, "GetFileInfo called with no file ids");
    return FileEntry::EmptyFileEntriesToDBusFormat();
  }

  std::string storage_name = LookupHandle(handle);
  if (storage_name.empty()) {
    SetInvalidHandleError(handle, &error);
    return FileEntry::EmptyFileEntriesToDBusFormat();
  }

  std::vector<FileEntry> file_info;
  if (!device_manager_.GetFileInfo(storage_name, fileIds, &file_info)) {
    error.set(kMtpdServiceError, "GetFileInfo failed");
    return FileEntry::EmptyFileEntriesToDBusFormat();
  }
  return FileEntry::FileEntriesToDBusFormat(file_info);
}

std::vector<uint8_t> MtpdServer::ReadFileChunk(const std::string& handle,
                                               const uint32_t& fileId,
                                               const uint32_t& offset,
                                               const uint32_t& count,
                                               DBus::Error& error) {
  if (count > kMaxReadCount || count == 0) {
    error.set(kMtpdServiceError, "Invalid count for ReadFileChunk");
    return std::vector<uint8_t>();
  }
  std::string storage_name = LookupHandle(handle);
  if (storage_name.empty())
    return InvalidHandle<std::vector<uint8_t> >(handle, &error);

  std::vector<uint8_t> file_contents;
  if (!device_manager_.ReadFileChunk(storage_name, fileId, offset, count,
                                     &file_contents)) {
    error.set(kMtpdServiceError, "ReadFileChunk failed");
    return std::vector<uint8_t>();
  }
  return file_contents;
}

void MtpdServer::CopyFileFromLocal(const std::string& handle,
                                   const DBus::FileDescriptor& fileDescriptor,
                                   const uint32_t& parentId,
                                   const std::string& fileName,
                                   DBus::Error& error) {
  const std::string storage_name = LookupHandle(handle);
  if (storage_name.empty() || !IsOpenedWithWrite(handle))
    return InvalidHandle<void>(handle, &error);

  if (!device_manager_.CopyFileFromLocal(storage_name, fileDescriptor.get(),
                                         parentId, fileName)) {
    error.set(kMtpdServiceError, "CopyFileFromLocal failed");
  }
}

void MtpdServer::DeleteObject(const std::string& handle,
                              const uint32_t& objectId,
                              DBus::Error& error) {
  const std::string storage_name = LookupHandle(handle);
  if (storage_name.empty() || !IsOpenedWithWrite(handle))
    return InvalidHandle<void>(handle, &error);

  if (!device_manager_.DeleteObject(storage_name, objectId)) {
    error.set(kMtpdServiceError, "DeleteObject failed");
  }
}

void MtpdServer::RenameObject(const std::string& handle,
                              const uint32_t& objectId,
                              const std::string& newName,
                              DBus::Error& error) {
  const std::string storage_name = LookupHandle(handle);
  if (storage_name.empty() || !IsOpenedWithWrite(handle))
    return InvalidHandle<void>(handle, &error);

  if (!device_manager_.RenameObject(storage_name, objectId, newName)) {
    error.set(kMtpdServiceError, "RenameObject failed");
  }
}

void MtpdServer::CreateDirectory(const std::string& handle,
                                 const uint32_t& parentId,
                                 const std::string& directoryName,
                                 DBus::Error& error) {
  const std::string storage_name = LookupHandle(handle);
  if (storage_name.empty() || !IsOpenedWithWrite(handle))
    return InvalidHandle<void>(handle, &error);

  if (!device_manager_.CreateDirectory(storage_name, parentId, directoryName)) {
    error.set(kMtpdServiceError, "CreateDirectory failed.");
  }
}

bool MtpdServer::IsAlive(DBus::Error& error) {
  return true;
}

void MtpdServer::StorageAttached(const std::string& storage_name) {
  // Fire DBus signal.
  MTPStorageAttached(storage_name);
}

void MtpdServer::StorageDetached(const std::string& storage_name) {
  // Fire DBus signal.
  MTPStorageDetached(storage_name);
}

int MtpdServer::GetDeviceEventDescriptor() const {
  return device_manager_.GetDeviceEventDescriptor();
}

void MtpdServer::ProcessDeviceEvents() {
  device_manager_.ProcessDeviceEvents();
}

std::string MtpdServer::LookupHandle(const std::string& handle) {
  HandleMap::const_iterator it = handle_map_.find(handle);
  return (it == handle_map_.end()) ? std::string() : it->second.first;
}

bool MtpdServer::IsOpenedWithWrite(const std::string& handle) {
  HandleMap::const_iterator it = handle_map_.find(handle);
  return (it == handle_map_.end()) ? false
                                   : it->second.second == kReadWriteMode;
}

}  // namespace mtpd
