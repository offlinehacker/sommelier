// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "vm_tools/concierge/virtual_machine.h"

#include <arpa/inet.h>
#include <linux/capability.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <utility>

#include <base/bind.h>
#include <base/files/file.h>
#include <base/files/file_util.h>
#include <base/guid.h>
#include <base/logging.h>
#include <base/memory/ptr_util.h>
#include <base/strings/stringprintf.h>
#include <base/sys_info.h>
#include <base/time/time.h>
#include <google/protobuf/repeated_field.h>
#include <grpc++/grpc++.h>

#include "vm_tools/common/constants.h"

using std::string;

namespace vm_tools {
namespace concierge {
namespace {

using ProcessExitBehavior = VirtualMachine::ProcessExitBehavior;
using ProcessStatus = VirtualMachine::ProcessStatus;
using Subnet = SubnetPool::Subnet;

// Path to the crosvm binary.
constexpr char kCrosvmBin[] = "/usr/bin/crosvm";

// Name of the control socket used for controlling crosvm.
constexpr char kCrosvmSocket[] = "crosvm.sock";

// Path to the wayland socket.
constexpr char kWaylandSocket[] = "/run/chrome/wayland-0";

// How long to wait before timing out on shutdown RPCs.
constexpr int64_t kShutdownTimeoutSeconds = 6;

// How long to wait before timing out on regular RPCs.
constexpr int64_t kDefaultTimeoutSeconds = 2;

// How long to wait before timing out on child process exits.
constexpr base::TimeDelta kChildExitTimeout = base::TimeDelta::FromSeconds(2);

// Offset in a subnet of the gateway/host.
constexpr size_t kHostAddressOffset = 0;

// Offset in a subnet of the client/guest.
constexpr size_t kGuestAddressOffset = 1;

// Calculates the amount of memory to give the virtual machine. Currently
// configured to provide 75% of system memory. This is deliberately over
// provisioned with the expectation that we will use the balloon driver to
// reduce the actual memory footprint.
string GetVmMemoryMiB() {
  int64_t vm_memory_mb = base::SysInfo::AmountOfPhysicalMemoryMB();
  vm_memory_mb /= 4;
  vm_memory_mb *= 3;

  return std::to_string(vm_memory_mb);
}

// Converts an EUI-48 mac address into string representation.
string MacAddressToString(const MacAddress& addr) {
  constexpr char kMacAddressFormat[] =
      "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx";
  return base::StringPrintf(kMacAddressFormat, addr[0], addr[1], addr[2],
                            addr[3], addr[4], addr[5]);
}

// Converts an IPv4 address to a string.
bool IPv4AddressToString(const uint32_t address, std::string* str) {
  CHECK(str);

  char result[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &address, result, sizeof(result)) != result) {
    return false;
  }
  *str = std::string(result);
  return true;
}

// Sets the pgid of the current process to its pid.  This is needed because
// crosvm assumes that only it and its children are in the same process group
// and indiscriminately sends a SIGKILL if it needs to shut them down.
bool SetPgid() {
  if (setpgid(0, 0) != 0) {
    PLOG(ERROR) << "Failed to change process group id";
    return false;
  }

  return true;
}

// Waits for the |pid| to exit.  Returns true if |pid| successfully exited and
// false if it did not exit in time.
bool WaitForChild(pid_t child, base::TimeDelta timeout) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGCHLD);

  const base::Time deadline = base::Time::Now() + timeout;
  while (true) {
    pid_t ret = waitpid(child, nullptr, WNOHANG);
    if (ret == child || (ret < 0 && errno == ECHILD)) {
      // Either the child exited or it doesn't exist anymore.
      return true;
    }

    // ret == 0 means that the child is still alive
    if (ret < 0) {
      PLOG(ERROR) << "Failed to wait for child process";
      return false;
    }

    base::Time now = base::Time::Now();
    if (deadline <= now) {
      // Timed out.
      return false;
    }

    const struct timespec ts = (deadline - now).ToTimeSpec();
    if (sigtimedwait(&set, nullptr, &ts) < 0 && errno == EAGAIN) {
      // Timed out.
      return false;
    }
  }
}

}  // namespace

VirtualMachine::VirtualMachine(MacAddress mac_addr,
                               std::unique_ptr<Subnet> subnet,
                               uint32_t vsock_cid,
                               base::FilePath runtime_dir)
    : mac_addr_(std::move(mac_addr)),
      subnet_(std::move(subnet)),
      vsock_cid_(vsock_cid) {
  CHECK(subnet_);
  CHECK(base::DirectoryExists(runtime_dir));

  // Take ownership of the runtime directory.
  CHECK(runtime_dir_.Set(runtime_dir));
}

VirtualMachine::~VirtualMachine() {
  Shutdown();
}

std::unique_ptr<VirtualMachine> VirtualMachine::Create(
    base::FilePath kernel,
    base::FilePath rootfs,
    std::vector<VirtualMachine::Disk> disks,
    MacAddress mac_addr,
    std::unique_ptr<Subnet> subnet,
    uint32_t vsock_cid,
    base::FilePath runtime_dir) {
  auto vm = base::WrapUnique(new VirtualMachine(std::move(mac_addr),
                                                std::move(subnet), vsock_cid,
                                                std::move(runtime_dir)));

  if (!vm->Start(std::move(kernel), std::move(rootfs), std::move(disks))) {
    vm.reset();
  }

  return vm;
}

bool VirtualMachine::Start(base::FilePath kernel,
                           base::FilePath rootfs,
                           std::vector<VirtualMachine::Disk> disks) {
  std::string host_ip;
  std::string netmask;

  if (!IPv4AddressToString(subnet_->AddressAtOffset(kHostAddressOffset),
                           &host_ip)) {
    LOG(ERROR) << "Failed to convert host IP to string";
    return false;
  }
  if (!IPv4AddressToString(subnet_->Netmask(), &netmask)) {
    LOG(ERROR) << "Failed to convert netmask to string";
    return false;
  }

  // Build up the process arguments.
  // clang-format off
  std::vector<string> args = {
      kCrosvmBin,       "run",
      "--cpus",         std::to_string(base::SysInfo::NumberOfProcessors()),
      "--mem",          GetVmMemoryMiB(),
      "--root",         rootfs.value(),
      "--mac",          MacAddressToString(mac_addr_),
      "--host_ip",      std::move(host_ip),
      "--netmask",      std::move(netmask),
      "--cid",          std::to_string(vsock_cid_),
      "--socket",       runtime_dir_.path().Append(kCrosvmSocket).value(),
      "--wayland-sock", kWaylandSocket,
  };
  // clang-format on

  // Add any extra disks.
  for (const auto& disk : disks) {
    if (disk.writable) {
      if (disk.image_type == VirtualMachine::DiskImageType::RAW)
        args.emplace_back("--rwdisk");
      else
        args.emplace_back("--rwqcow");
    } else {
      if (disk.image_type == VirtualMachine::DiskImageType::RAW)
        args.emplace_back("--disk");
      else
        args.emplace_back("--rwqcow");
    }

    args.emplace_back(disk.path.value());
  }

  // Finally list the path to the kernel.
  args.emplace_back(kernel.value());

  // Put everything into the brillo::ProcessImpl.
  for (string& arg : args) {
    process_.AddArg(std::move(arg));
  }

  // Change the process group before exec so that crosvm sending SIGKILL to the
  // whole process group doesn't kill us as well.
  process_.SetPreExecCallback(base::Bind(&SetPgid));

  if (!process_.Start()) {
    LOG(ERROR) << "Failed to start VM process";
    return false;
  }

  // Create a stub for talking to the maitre'd instance inside the VM.
  stub_ = std::make_unique<vm_tools::Maitred::Stub>(grpc::CreateChannel(
      base::StringPrintf("vsock:%u:%u", vsock_cid_, vm_tools::kMaitredPort),
      grpc::InsecureChannelCredentials()));

  return true;
}

bool VirtualMachine::Shutdown() {
  // Do a sanity check here to make sure the process is still around.  It may
  // have crashed and we don't want to be waiting around for an RPC response
  // that's never going to come.  kill with a signal value of 0 is explicitly
  // documented as a way to check for the existence of a process.
  if (process_.pid() == 0 || (kill(process_.pid(), 0) < 0 && errno == ESRCH)) {
    // The process is already gone.
    process_.Release();
    return true;
  }

  grpc::ClientContext ctx;
  ctx.set_deadline(gpr_time_add(
      gpr_now(GPR_CLOCK_MONOTONIC),
      gpr_time_from_seconds(kShutdownTimeoutSeconds, GPR_TIMESPAN)));

  vm_tools::EmptyMessage empty;
  grpc::Status status = stub_->Shutdown(&ctx, empty, &empty);

  // brillo::ProcessImpl doesn't provide a timed wait function and while the
  // Shutdown RPC may have been successful we can't really trust crosvm to
  // actually exit.  This may result in an untimed wait() blocking indefinitely.
  // Instead, do a timed wait here and only return success if the process
  // _actually_ exited as reported by the kernel, which is really the only
  // thing we can trust here.
  if (status.ok() && WaitForChild(process_.pid(), kChildExitTimeout)) {
    process_.Release();
    return true;
  }

  LOG(WARNING) << "Shutdown RPC failed for VM " << vsock_cid_ << " with error "
               << "code " << status.error_code() << ": "
               << status.error_message();

  // Try to shut it down via the crosvm socket.
  brillo::ProcessImpl crosvm;
  crosvm.AddArg(kCrosvmBin);
  crosvm.AddArg("stop");
  crosvm.AddArg(runtime_dir_.path().Append(kCrosvmSocket).value());
  crosvm.Run();

  // We can't actually trust the exit codes that crosvm gives us so just see if
  // it exited.
  if (WaitForChild(process_.pid(), kChildExitTimeout)) {
    process_.Release();
    return true;
  }

  LOG(WARNING) << "Failed to stop VM " << vsock_cid_ << " via crosvm socket";

  // Kill the process with SIGTERM.
  if (process_.Kill(SIGTERM, kChildExitTimeout.InSeconds())) {
    return true;
  }

  LOG(WARNING) << "Failed to kill VM " << vsock_cid_ << " with SIGTERM";

  // Kill it with fire.
  if (process_.Kill(SIGKILL, kChildExitTimeout.InSeconds())) {
    return true;
  }

  LOG(ERROR) << "Failed to kill VM " << vsock_cid_ << " with SIGKILL";
  return false;
}

bool VirtualMachine::LaunchProcess(std::vector<string> args,
                                   std::map<string, string> env,
                                   bool respawn,
                                   bool wait_for_exit,
                                   int64_t timeout_seconds) {
  CHECK(!args.empty());
  DCHECK(!(respawn && wait_for_exit));

  LOG(INFO) << "Launching " << args[0] << " inside VM " << vsock_cid_;

  vm_tools::LaunchProcessRequest request;
  vm_tools::LaunchProcessResponse response;

  google::protobuf::RepeatedPtrField<string> argv(args.begin(), args.end());
  request.mutable_argv()->Swap(&argv);

  google::protobuf::Map<string, string> environ(env.begin(), env.end());
  request.mutable_env()->swap(environ);

  request.set_respawn(respawn);
  request.set_wait_for_exit(wait_for_exit);

  grpc::ClientContext ctx;
  ctx.set_deadline(
      gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                   gpr_time_from_seconds(timeout_seconds, GPR_TIMESPAN)));

  grpc::Status status = stub_->LaunchProcess(&ctx, request, &response);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to launch " << args[0] << ": "
               << status.error_message();
    return false;
  }

  // If waiting for the process to exit, a success means the process had to
  // return 0. Otherwise, just check that the process launched successfully.
  if (response.status() == vm_tools::EXITED && wait_for_exit) {
    return response.code() == 0;
  } else if (response.status() == vm_tools::LAUNCHED && !wait_for_exit) {
    return true;
  }

  return false;
}

bool VirtualMachine::StartProcess(std::vector<string> args,
                                  std::map<string, string> env,
                                  ProcessExitBehavior exit_behavior) {
  return LaunchProcess(std::move(args), std::move(env),
                       exit_behavior == ProcessExitBehavior::RESPAWN_ON_EXIT,
                       false, kDefaultTimeoutSeconds);
}

bool VirtualMachine::RunProcess(std::vector<string> args,
                                std::map<string, string> env) {
  return LaunchProcess(std::move(args), std::move(env), false, true,
                       kDefaultTimeoutSeconds);
}

bool VirtualMachine::RunProcessWithTimeout(std::vector<string> args,
                                           std::map<string, string> env,
                                           base::TimeDelta timeout) {
  return LaunchProcess(std::move(args), std::move(env), false, true,
                       timeout.InSeconds());
}

bool VirtualMachine::ConfigureNetwork() {
  LOG(INFO) << "Configuring network for VM " << vsock_cid_;

  vm_tools::NetworkConfigRequest request;
  vm_tools::EmptyMessage response;

  vm_tools::IPv4Config* config = request.mutable_ipv4_config();
  config->set_address(IPv4Address());
  config->set_gateway(GatewayAddress());
  config->set_netmask(Netmask());

  grpc::ClientContext ctx;
  ctx.set_deadline(gpr_time_add(
      gpr_now(GPR_CLOCK_MONOTONIC),
      gpr_time_from_seconds(kDefaultTimeoutSeconds, GPR_TIMESPAN)));

  grpc::Status status = stub_->ConfigureNetwork(&ctx, request, &response);
  if (!status.ok()) {
    LOG(ERROR) << "Failed to configure network for VM " << vsock_cid_ << ": "
               << status.error_message();
    return false;
  }

  return true;
}

bool VirtualMachine::Mount(string source,
                           string target,
                           string fstype,
                           uint64_t mountflags,
                           string options) {
  LOG(INFO) << "Mounting " << source << " on " << target << " inside VM "
            << vsock_cid_;

  vm_tools::MountRequest request;
  vm_tools::MountResponse response;

  request.mutable_source()->swap(source);
  request.mutable_target()->swap(target);
  request.mutable_fstype()->swap(fstype);
  request.set_mountflags(mountflags);
  request.mutable_options()->swap(options);

  grpc::ClientContext ctx;
  ctx.set_deadline(gpr_time_add(
      gpr_now(GPR_CLOCK_MONOTONIC),
      gpr_time_from_seconds(kDefaultTimeoutSeconds, GPR_TIMESPAN)));

  grpc::Status status = stub_->Mount(&ctx, request, &response);
  if (!status.ok() || response.error() != 0) {
    LOG(ERROR) << "Failed to mount " << request.source() << " on "
               << request.target() << " inside VM " << vsock_cid_ << ": "
               << (status.ok() ? strerror(response.error())
                               : status.error_message());
    return false;
  }

  return true;
}

void VirtualMachine::SetContainerSubnet(
    std::unique_ptr<SubnetPool::Subnet> subnet) {
  container_subnet_ = std::move(subnet);
}

uint32_t VirtualMachine::GatewayAddress() const {
  return subnet_->AddressAtOffset(kHostAddressOffset);
}

uint32_t VirtualMachine::IPv4Address() const {
  return subnet_->AddressAtOffset(kGuestAddressOffset);
}

uint32_t VirtualMachine::Netmask() const {
  return subnet_->Netmask();
}

uint32_t VirtualMachine::ContainerNetmask() const {
  if (container_subnet_)
    return container_subnet_->Netmask();

  return INADDR_ANY;
}

size_t VirtualMachine::ContainerPrefix() const {
  if (container_subnet_)
    return container_subnet_->Prefix();

  return 0;
}

uint32_t VirtualMachine::ContainerSubnet() const {
  if (container_subnet_)
    return container_subnet_->AddressAtOffset(0);

  return INADDR_ANY;
}

bool VirtualMachine::RegisterContainerIp(const std::string& container_token,
                                         const std::string& container_ip) {
  // The token will be in the pending map if this is the first start of the
  // container. It will be in the main map if this is from a crash/restart of
  // the garcon process in the container.
  auto iter = pending_container_token_to_name_.find(container_token);
  if (iter != pending_container_token_to_name_.end()) {
    container_name_to_ip_[iter->second] = container_ip;
    container_token_to_name_[container_token] = iter->second;
    pending_container_token_to_name_.erase(iter);
    return true;
  }
  std::string existing_name = GetContainerNameForToken(container_token);
  if (existing_name.empty()) {
    return false;
  }
  container_name_to_ip_[existing_name] = std::move(container_ip);
  return true;
}

bool VirtualMachine::UnregisterContainerIp(const std::string& container_token) {
  auto token_iter = container_token_to_name_.find(container_token);
  if (token_iter == container_token_to_name_.end()) {
    return false;
  }
  auto name_iter = container_name_to_ip_.find(token_iter->second);
  DCHECK(name_iter != container_name_to_ip_.end());
  container_token_to_name_.erase(token_iter);
  container_name_to_ip_.erase(name_iter);
  return true;
}

std::string VirtualMachine::GenerateContainerToken(
    const std::string& container_name) {
  std::string token = base::GenerateGUID();
  pending_container_token_to_name_[token] = container_name;
  return token;
}

std::string VirtualMachine::GetContainerNameForToken(
    const std::string& container_token) {
  auto iter = container_token_to_name_.find(container_token);
  if (iter == container_token_to_name_.end()) {
    return "";
  }
  return iter->second;
}

std::string VirtualMachine::GetContainerIpForName(
    const std::string& container_name) {
  auto iter = container_name_to_ip_.find(container_name);
  if (iter == container_name_to_ip_.end()) {
    return "";
  }
  return iter->second;
}

void VirtualMachine::set_stub_for_testing(
    std::unique_ptr<vm_tools::Maitred::Stub> stub) {
  stub_ = std::move(stub);
}

std::unique_ptr<VirtualMachine> VirtualMachine::CreateForTesting(
    MacAddress mac_addr,
    std::unique_ptr<SubnetPool::Subnet> subnet,
    uint32_t vsock_cid,
    base::FilePath runtime_dir,
    std::unique_ptr<vm_tools::Maitred::Stub> stub) {
  auto vm = base::WrapUnique(new VirtualMachine(std::move(mac_addr),
                                                std::move(subnet), vsock_cid,
                                                std::move(runtime_dir)));

  vm->set_stub_for_testing(std::move(stub));

  return vm;
}

}  // namespace concierge
}  // namespace vm_tools
