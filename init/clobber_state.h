// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef INIT_CLOBBER_STATE_H_
#define INIT_CLOBBER_STATE_H_

#include <sys/stat.h>

#include <memory>
#include <string>
#include <vector>

#include <base/files/file_path.h>

#include "init/crossystem.h"

class ClobberState {
 public:
  struct Arguments {
    // Run in the context of a factory flow, do not reboot when done.
    bool factory_wipe = false;
    // Less thorough data destruction.
    bool fast_wipe = false;
    // Don't delete the non-active set of kernel/root partitions.
    bool keepimg = false;
    // Preserve some files and VPD keys.
    bool safe_wipe = false;
    // Preserve rollback data, attestation DB, and don't clear TPM
    bool rollback_wipe = false;
  };

  // The index of each partition within the gpt partition table.
  struct PartitionNumbers {
    int stateful = -1;
    int root_a = -1;
    int root_b = -1;
    int kernel_a = -1;
    int kernel_b = -1;
  };

  struct DeviceWipeInfo {
    // Paths under /dev for the various devices to wipe.
    base::FilePath stateful_device;
    base::FilePath inactive_root_device;
    base::FilePath inactive_kernel_device;

    // Is the stateful device backed by an MTD flash device.
    bool is_mtd_flash = false;

    // The partition number for the currently booted kernel partition.
    int active_kernel_partition = -1;
  };

  // Extracts ClobberState's arguments from argv.
  static Arguments ParseArgv(int argc, char const* const argv[]);

  // Attempts to increment the contents of |path| by 1. If the contents cannot
  // be read, or if the contents are not an integer, writes '1' to the file.
  static bool IncrementFileCounter(const base::FilePath& path);

  // Given a list of files to preserve (relative to |preserved_files_root|),
  // creates a tar file containing those files at |tar_file_path|.
  // The directory structure of the preserved files is preserved.
  static int PreserveFiles(const base::FilePath& preserved_files_root,
                           const std::vector<base::FilePath>& preserved_files,
                           const base::FilePath& tar_file_path);

  // Splits a device path, for example /dev/mmcblk0p1, /dev/sda3,
  // /dev/ubiblock9_0 into the base device and partition numbers,
  // which would be respectively /dev/mmcblk0p, 1; /dev/sda, 3; and
  // /dev/ubiblock, 9.
  // Returns true on success.
  static bool GetDevicePathComponents(const base::FilePath& device,
                                      std::string* base_device_out,
                                      int* partition_out);

  // Determine the devices to be wiped and their properties, and populate
  // |wipe_info_out| with the results. Returns true if successful.
  static bool GetDevicesToWipe(const base::FilePath& root_disk,
                               const base::FilePath& root_device,
                               const PartitionNumbers& partitions,
                               DeviceWipeInfo* wipe_info_out);

  ClobberState(const Arguments& args, std::unique_ptr<CrosSystem> cros_system);

  // Run the clobber state routine.
  int Run();
  bool MarkDeveloperMode();

  // Returns vector of files to be preserved. All FilePaths are relative to
  // stateful_.
  std::vector<base::FilePath> GetPreservedFilesList();

  // Determines if the given device (under |dev_|) is backed by a rotational
  // hard drive.
  // Returns true if it can conclusively determine it's rotational,
  // otherwise false.
  bool IsRotational(const base::FilePath& device_path);

  void SetArgsForTest(const Arguments& args);
  void SetStatefulForTest(base::FilePath stateful_path);
  void SetDevForTest(base::FilePath dev_path);
  void SetSysForTest(base::FilePath sys_path);

 protected:
  // Wrapper around stat(2). Protected so that it can be overridden for tests.
  virtual int Stat(const base::FilePath& path, struct stat* st);

 private:
  bool ClearBiometricSensorEntropy();
  int RunClobberStateShell(bool is_rotational);
  int Reboot();

  Arguments args_;
  std::unique_ptr<CrosSystem> cros_system_;
  base::FilePath stateful_;
  base::FilePath dev_;
  base::FilePath sys_;
  PartitionNumbers partitions_;
  base::FilePath root_disk_;
  DeviceWipeInfo wipe_info_;
};

#endif  // INIT_CLOBBER_STATE_H_
