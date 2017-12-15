// Copyright 2016 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "authpolicy/samba_interface.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <base/files/file.h>
#include <base/files/file_util.h>
#include <base/single_thread_task_runner.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <base/threading/platform_thread.h>
#include <base/time/time.h>

#include "authpolicy/anonymizer.h"
#include "authpolicy/platform_helper.h"
#include "authpolicy/process_executor.h"
#include "bindings/authpolicy_containers.pb.h"

namespace authpolicy {
namespace {

// Samba configuration file data.
const char kSmbConfData[] =
    "[global]\n"
    "\tnetbios name = %s\n"
    "\tsecurity = ADS\n"
    "\tworkgroup = %s\n"
    "\trealm = %s\n"
    "\tlock directory = %s\n"
    "\tcache directory = %s\n"
    "\tstate directory = %s\n"
    "\tprivate directory = %s\n"
    "\tkerberos method = secrets and keytab\n"
    "\tkerberos encryption types = strong\n"
    "\tclient signing = mandatory\n"
    "\tclient min protocol = SMB2\n"
    // TODO(ljusten): Remove this line once crbug.com/662440 is resolved.
    "\tclient max protocol = SMB3\n"
    "\tclient ipc min protocol = SMB2\n"
    "\tclient schannel = yes\n"
    "\tclient ldap sasl wrapping = sign\n";

const int kFileMode_rwr = base::FILE_PERMISSION_READ_BY_USER |
                          base::FILE_PERMISSION_WRITE_BY_USER |
                          base::FILE_PERMISSION_READ_BY_GROUP;

const int kFileMode_rwxrx = kFileMode_rwr |
                            base::FILE_PERMISSION_EXECUTE_BY_USER |
                            base::FILE_PERMISSION_EXECUTE_BY_GROUP;

const int kFileMode_rwxrwx =
    kFileMode_rwxrx | base::FILE_PERMISSION_WRITE_BY_GROUP;

// Directories with permissions to be created. AUTHPOLICY_TMP_DIR needs group rx
// access to read smb.conf and krb5.conf and to access SAMBA_DIR, but no write
// access. The Samba directories need full group rwx access since Samba reads
// and writes files there.
const std::pair<Path, int> kDirsAndMode[] = {
    {Path::TEMP_DIR, kFileMode_rwxrx},
    {Path::SAMBA_DIR, kFileMode_rwxrwx},
    {Path::SAMBA_LOCK_DIR, kFileMode_rwxrwx},
    {Path::SAMBA_CACHE_DIR, kFileMode_rwxrwx},
    {Path::SAMBA_STATE_DIR, kFileMode_rwxrwx},
    {Path::SAMBA_PRIVATE_DIR, kFileMode_rwxrwx}};

// Directory / filenames for user and device policy.
const char kPRegUserDir[] = "User";
const char kPRegDeviceDir[] = "Machine";
const char kPRegFileName[] = "registry.pol";

// Size limit when loading the config file (256 kb).
const size_t kConfigSizeLimit = 256 * 1024;

// Maximum smbclient tries.
const int kSmbClientMaxTries = 5;
// Wait interval between two smbclient tries.
const int kSmbClientRetryWaitSeconds = 1;

// Keys for interpreting net output.
const char kKeyJoinAccessDenied[] = "NT_STATUS_ACCESS_DENIED";
const char kKeyInvalidMachineName[] = "Improperly formed account name";
const char kKeyMachineNameTooLong[] = "Our netbios name can be at most";
const char kKeyUserHitJoinQuota[] =
    "Insufficient quota exists to complete the operation";
const char kKeyJoinFailedToFindDC[] = "failed to find DC";
const char kKeyNoLogonServers[] = "No logon servers";
const char kKeyJoinLogonFailure[] = "Logon failure";
const char kKeyJoinMustChangePassword[] = "Must change password";
// Setting OU during domain join failed. More specific errors below.
const char kKeyBadOuCommon[] = "failed to precreate account in ou";
// The domain join createcomputer argument specified a non-existent OU.
const char kKeyBadOuNoSuchObject[] = "No such object";
// The domain join createcomputer argument syntax was invalid. Caused by some
// special characters in OU names, e.g. 'ou=123!' or 'a"b'. Seems like a Samba
// issue since OUs allow all characters and we do escape names properly.
const char kKeyBadOuInvalidDnSyntax[] = "Invalid DN syntax";
// Domain join operation would have violated an attribute constraint.
const char kKeyBadOuConstrainViolation[] = "Constraint violation";
// Domain join required access permissions that the user does not possess.
const char kKeyBadOuInsufficientAccess[] = "Insufficient access";
// All other OU errors result in a generic ERROR_SETTING_OU_FAILED, e.g.
//  - "Referral": dc=... specification resulted in a referral to another server.
//  - "Operations error": Unspecific error.
// Keys for interpreting smbclient output.
const char kKeyConnectionReset[] = "NT_STATUS_CONNECTION_RESET";
const char kKeyNetworkTimeout[] = "NT_STATUS_IO_TIMEOUT";
const char kKeyObjectNameNotFound[] =
    "NT_STATUS_OBJECT_NAME_NOT_FOUND opening remote file ";

// Replacement strings for anonymization.
const char kMachineNamePlaceholder[] = "<MACHINE_NAME>";
const char kLogonNamePlaceholder[] = "<USER_LOGON_NAME>";
const char kGivenNamePlaceholder[] = "<USER_GIVEN_NAME>";
const char kDisplayNamePlaceholder[] = "<USER_DISPLAY_NAME>";
const char kSAMAccountNamePlaceholder[] = "<USER_SAM_ACCOUNT_NAME>";
const char kCommonNamePlaceholder[] = "<USER_COMMON_NAME>";
const char kAccountIdPlaceholder[] = "<USER_ACCOUNT_ID>";
const char kWorkgroupPlaceholder[] = "<WORKGROUP>";
const char kDeviceRealmPlaceholder[] = "<DEVICE_REALM>";
const char kUserRealmPlaceholder[] = "<USER_REALM>";
const char kForestPlaceholder[] = "<FOREST>";
const char kDomainPlaceholder[] = "<DOMAIN>";
const char kServerNamePlaceholder[] = "<SERVER_NAME>";
const char kSiteNamePlaceholder[] = "<SITE_NAME>";
const char kIpAddressPlaceholder[] = "<IP_ADDRESS>";

// Keys for net ads searches.
const char kKeyWorkgroup[] = "Workgroup";
const char kKeyAdsDnsParseRrSrv[] = "ads_dns_parse_rr_srv";
const char kKeyPdcDnsName[] = "pdc_dns_name";
const char kKeyAdsDcName[] = "ads_dc_name";
const char kKeyPdcName[] = "pdc_name";
const char kKeyServerSite[] = "server_site";
const char kKeyClientSite[] = "client_site";
const char kKeyForest[] = "Forest";
const char kKeyDomain[] = "Domain";
const char kKeyDomainController[] = "Domain Controller";
const char kKeyPreWin2kDomain[] = "Pre-Win2k Domain";
const char kKeyPreWin2kHostname[] = "Pre-Win2k Hostname";
const char kKeyServerSiteName[] = "Server Site Name";
const char kKeyClientSiteName[] = "Client Site Name";
const char kKeyKdcServer[] = "KDC server";
const char kKeyLdapServer[] = "LDAP server";
const char kKeyLdapServerName[] = "LDAP server name";

// Maximum time that logging through SetDefaultLogLevel() should stay enabled.
// The method is called through the authpolicy_debug crosh command. The time is
// limited so users don't have to remember to turn logging off.
// Keep in sync with description in crosh!
int kMaxDefaultLogLevelUptimeMinutes = 30;

ErrorType GetNetError(const ProcessExecutor& executor,
                      const std::string& net_command) {
  const std::string& net_out = executor.GetStdout();
  const std::string& net_err = executor.GetStderr();
  const std::string error_msg("net ads " + net_command + " failed: ");

  if (Contains(net_out, kKeyJoinFailedToFindDC) ||
      Contains(net_err, kKeyNoLogonServers)) {
    LOG(ERROR) << error_msg << "network problem";
    return ERROR_NETWORK_PROBLEM;
  }
  if (Contains(net_out, kKeyJoinLogonFailure)) {
    LOG(ERROR) << error_msg << "logon failure";
    return ERROR_BAD_PASSWORD;
  }
  if (Contains(net_out, kKeyJoinMustChangePassword)) {
    LOG(ERROR) << error_msg << "must change password";
    return ERROR_PASSWORD_EXPIRED;
  }
  if (Contains(net_out, kKeyJoinAccessDenied)) {
    LOG(ERROR) << error_msg << "user is not permitted to join the domain";
    return ERROR_JOIN_ACCESS_DENIED;
  }
  if (Contains(net_out, kKeyInvalidMachineName)) {
    LOG(ERROR) << error_msg << "invalid machine name";
    return ERROR_INVALID_MACHINE_NAME;
  }
  if (Contains(net_out, kKeyMachineNameTooLong)) {
    LOG(ERROR) << error_msg << "machine name is too long";
    return ERROR_MACHINE_NAME_TOO_LONG;
  }
  if (Contains(net_out, kKeyUserHitJoinQuota)) {
    LOG(ERROR) << error_msg << "user joined max number of machines";
    return ERROR_USER_HIT_JOIN_QUOTA;
  }
  if (Contains(net_out, kKeyBadOuCommon)) {
    if (Contains(net_out, kKeyBadOuNoSuchObject)) {
      LOG(ERROR) << error_msg << "computer OU does not exist";
      return ERROR_OU_DOES_NOT_EXIST;
    }
    if (Contains(net_out, kKeyBadOuInvalidDnSyntax)) {
      LOG(ERROR) << error_msg << "computer OU invalid";
      return ERROR_INVALID_OU;
    }
    if (Contains(net_out, kKeyBadOuConstrainViolation) ||
        Contains(net_out, kKeyBadOuInsufficientAccess)) {
      LOG(ERROR) << error_msg << "access denied setting computer OU";
      return ERROR_OU_ACCESS_DENIED;
    }
    LOG(ERROR) << error_msg << "setting computer OU failed, unspecified error";
    return ERROR_SETTING_OU_FAILED;
  }
  LOG(ERROR) << error_msg << "exit code " << executor.GetExitCode();
  return ERROR_NET_FAILED;
}

ErrorType GetSmbclientError(const ProcessExecutor& smb_client_cmd) {
  const std::string& smb_client_out = smb_client_cmd.GetStdout();
  if (Contains(smb_client_out, kKeyNetworkTimeout) ||
      Contains(smb_client_out, kKeyConnectionReset)) {
    LOG(ERROR) << "smbclient failed - network problem";
    return ERROR_NETWORK_PROBLEM;
  }
  LOG(ERROR) << "smbclient failed with exit code "
             << smb_client_cmd.GetExitCode();
  return ERROR_SMBCLIENT_FAILED;
}

// Creates the given directory recursively and sets error message on failure.
ErrorType CreateDirectory(const base::FilePath& dir) {
  base::File::Error ferror;
  if (!base::CreateDirectoryAndGetError(dir, &ferror)) {
    LOG(ERROR) << "Failed to create directory '" << dir.value()
               << "': " << base::File::ErrorToString(ferror);
    return ERROR_LOCAL_IO;
  }
  return ERROR_NONE;
}

// Sets file permissions for a given filepath and sets error message on failure.
ErrorType SetFilePermissions(const base::FilePath& fp, int mode) {
  if (!base::SetPosixFilePermissions(fp, mode)) {
    LOG(ERROR) << "Failed to set permissions on '" << fp.value() << "'";
    return ERROR_LOCAL_IO;
  }
  return ERROR_NONE;
}

// Similar to |SetFilePermissions|, but sets permissions recursively up the path
// to |base_fp| (not including |base_fp|). Returns false if |base_fp| is not a
// parent of |fp|.
ErrorType SetFilePermissionsRecursive(const base::FilePath& fp,
                                      const base::FilePath& base_fp,
                                      int mode) {
  if (!base_fp.IsParent(fp)) {
    LOG(ERROR) << "Base path '" << base_fp.value() << "' is not a parent of '"
               << fp.value() << "'";
    return ERROR_LOCAL_IO;
  }
  ErrorType error = ERROR_NONE;
  for (base::FilePath curr_fp = fp; curr_fp != base_fp && error == ERROR_NONE;
       curr_fp = curr_fp.DirName()) {
    error = SetFilePermissions(curr_fp, mode);
  }
  return error;
}

// Checks whether the file at |default_level_path| exists and was last modified
// in a certain time range. If not, it is deleted to prevent that a user forgets
// to disable logging.
bool CheckFlagsDefaultLevelValid(const base::FilePath& default_level_path) {
  // Having no file is the out-of-box state with no level set, so exit quietly.
  if (!base::PathExists(default_level_path))
    return false;

  base::File::Info info;
  if (!GetFileInfo(default_level_path, &info)) {
    PLOG(ERROR) << "Failed to get file info from "
                << default_level_path.value();
    return false;
  }

  // Check < -1 to prevent issues with clocks running backwards for a bit.
  int uptime_min = (base::Time::Now() - info.last_modified).InMinutes();
  if (uptime_min < -1 || uptime_min > kMaxDefaultLogLevelUptimeMinutes) {
    LOG(INFO) << "Removing flags default level file and resetting (uptime: "
              << uptime_min << " minutes).";
    PCHECK(base::DeleteFile(default_level_path, false /* recursive */))
        << "Failed to delete flags default level file "
        << default_level_path.value();
    return false;
  }

  return true;
}

// Parses |gpo_policy_data| from |gpo_policy_data_blob|. Returns ERROR_NONE on
// success. Returns ERROR_PARSE_FAILED and prints an error on failure.
ErrorType ParsePolicyData(const std::string& gpo_policy_data_blob,
                          protos::GpoPolicyData* gpo_policy_data) {
  if (!gpo_policy_data->ParseFromString(gpo_policy_data_blob)) {
    LOG(ERROR) << "Failed to parse policy data from string";
    return ERROR_PARSE_FAILED;
  }
  return ERROR_NONE;
}

}  // namespace

SambaInterface::SambaInterface(
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    AuthPolicyMetrics* metrics,
    const PathService* path_service,
    const base::Closure& user_kerberos_files_changed)
    : user_account_(Path::USER_SMB_CONF),
      device_account_(Path::DEVICE_SMB_CONF),
      metrics_(metrics),
      paths_(path_service),
      anonymizer_(std::make_unique<Anonymizer>()),
      jail_helper_(paths_, &flags_, anonymizer_.get()),
      user_tgt_manager_(task_runner,
                        paths_,
                        metrics_,
                        &flags_,
                        &jail_helper_,
                        anonymizer_.get(),
                        Path::USER_KRB5_CONF,
                        Path::USER_CREDENTIAL_CACHE),
      device_tgt_manager_(task_runner,
                          paths_,
                          metrics_,
                          &flags_,
                          &jail_helper_,
                          anonymizer_.get(),
                          Path::DEVICE_KRB5_CONF,
                          Path::DEVICE_CREDENTIAL_CACHE) {
  DCHECK(paths_);
  LoadFlagsDefaultLevel();
  user_tgt_manager_.SetKerberosFilesChangedCallback(
      user_kerberos_files_changed);
}

SambaInterface::~SambaInterface() = default;

ErrorType SambaInterface::Initialize(bool expect_config) {
  ReloadDebugFlags();

  ErrorType error = ERROR_NONE;
  for (const auto& dir_and_mode : kDirsAndMode) {
    const base::FilePath dir(paths_->Get(dir_and_mode.first));
    const int mode = dir_and_mode.second;
    error = ::authpolicy::CreateDirectory(dir);
    if (error != ERROR_NONE)
      return error;
    error = SetFilePermissions(dir, mode);
    if (error != ERROR_NONE)
      return error;
  }

  if (expect_config) {
    error = ReadConfiguration();
    if (error != ERROR_NONE)
      return error;
  }

  return ERROR_NONE;
}

// static
bool SambaInterface::CleanState(const PathService* path_service) {
  // Note: We're not permitted to delete the folder and DeleteFile apparently
  // doesn't support wildcards, so DeleteFile returns false.
  DCHECK(path_service);
  const base::FilePath state_dir(path_service->Get(Path::STATE_DIR));
  base::DeleteFile(state_dir, true /* recursive */);
  if (!base::IsDirectoryEmpty(state_dir)) {
    LOG(ERROR) << "Failed to clean state dir '" << state_dir.value() << "'";
    return false;
  }
  return true;
}

ErrorType SambaInterface::AuthenticateUser(
    const std::string& user_principal_name,
    const std::string& account_id,
    int password_fd,
    ActiveDirectoryAccountInfo* account_info) {
  ReloadDebugFlags();

  ErrorType error = AuthenticateUserInternal(user_principal_name, account_id,
                                             password_fd, account_info);

  last_auth_error_ = error;
  return error;
}

ErrorType SambaInterface::AuthenticateUserInternal(
    const std::string& user_principal_name,
    const std::string& account_id,
    int password_fd,
    ActiveDirectoryAccountInfo* account_info) {
  if (!account_id.empty())
    SetUser(account_id);

  // We need the device account here to authenticate while getting the user's
  // account info.
  ErrorType error = CheckDeviceAccountValid();
  if (error != ERROR_NONE)
    return error;

  // Split user_principal_name into parts and normalize.
  std::string user_name, user_realm, normalized_upn;
  if (!ParseUserPrincipalName(user_principal_name, &user_name, &user_realm,
                              &normalized_upn)) {
    return ERROR_PARSE_UPN_FAILED;
  }
  SetUserRealm(user_realm);

  // Get account info for the user.
  error = GetAccountInfo(user_name, normalized_upn, account_id, account_info);
  if (error != ERROR_NONE)
    return error;

  if (account_id.empty())
    SetUser(account_info->account_id());

  // Update normalized_upn. This handles the situation when the user name
  // changes on the server and the user logs in with their old user name (e.g.
  // from the pods screen in Chrome).
  normalized_upn = account_info->sam_account_name() + "@" + user_account_.realm;

  // Update IPs, server names etc. for the user account.
  error = UpdateAccountData(&user_account_);
  if (error != ERROR_NONE)
    return error;

  // Call kinit to get the Kerberos ticket-granting-ticket.
  DCHECK(!user_account_.kdc_ip.empty());
  error = user_tgt_manager_.AcquireTgtWithPassword(
      normalized_upn, password_fd, user_account_.realm, user_account_.kdc_ip);
  if (error != ERROR_NONE)
    return error;

  // Renew TGT periodically. The usual validity lifetime is about 10 hours, so
  // this won't happen too often.
  user_tgt_manager_.EnableTgtAutoRenewal(true);

  // Store sAMAccountName for policy fetch. Note that net ads gpo list always
  // wants the sAMAccountName. Also note that pwd_last_set is zero and stale
  // at this point if AcquireTgtWithPassword() set a new password, but that's
  // fine, the timestamp is updated in the next GetUserStatus() call.
  user_sam_account_name_ = account_info->sam_account_name();
  user_pwd_last_set_ = account_info->pwd_last_set();
  user_logged_in_ = true;
  return ERROR_NONE;
}

ErrorType SambaInterface::GetUserStatus(
    const std::string& user_principal_name,
    const std::string& account_id,
    ActiveDirectoryUserStatus* user_status) {
  ReloadDebugFlags();
  SetUser(account_id);

  // The device account is needed here to get the user's account info.
  ErrorType error = CheckDeviceAccountValid();
  if (error != ERROR_NONE)
    return error;

  if (!user_principal_name.empty()) {
    std::string user_name, user_realm, normalized_upn;
    if (!ParseUserPrincipalName(user_principal_name, &user_name, &user_realm,
                                &normalized_upn)) {
      return ERROR_PARSE_UPN_FAILED;
    }
    SetUserRealm(user_realm);
  } else {
    // Backwards compatibility for old interface not taking UPN: Use device
    // realm. Can be removed after interface is switched to new protobuf
    // interface.
    if (user_account_.realm.empty())
      SetUserRealm(device_account_.realm);
  }

  // Get account info for the user.
  ActiveDirectoryAccountInfo account_info;
  error =
      GetAccountInfo("" /* user_name unused */, "" /* normalized_upn unused */,
                     account_id, &account_info);
  if (error != ERROR_NONE)
    return error;

  // Determine the status of the TGT.
  ActiveDirectoryUserStatus::TgtStatus tgt_status =
      ActiveDirectoryUserStatus::TGT_VALID;
  error = GetUserTgtStatus(&tgt_status);
  if (error != ERROR_NONE)
    return error;

  // Determine the status of the password.
  ActiveDirectoryUserStatus::PasswordStatus password_status =
      GetUserPasswordStatus(account_info);

  *user_status->mutable_account_info() = account_info;
  user_status->set_tgt_status(tgt_status);
  user_status->set_password_status(password_status);
  user_status->set_last_auth_error(last_auth_error_);
  return ERROR_NONE;
}

ErrorType SambaInterface::GetUserKerberosFiles(const std::string& account_id,
                                               KerberosFiles* files) {
  ReloadDebugFlags();
  SetUser(account_id);
  return user_tgt_manager_.GetKerberosFiles(files);
}

ErrorType SambaInterface::JoinMachine(
    const std::string& machine_name,
    const std::string& machine_domain,
    const std::vector<std::string>& machine_ou,
    const std::string& user_principal_name,
    int password_fd,
    std::string* joined_domain) {
  ReloadDebugFlags();

  // Split user principal name into parts.
  std::string user_name, user_realm, normalized_upn;
  if (!ParseUserPrincipalName(user_principal_name, &user_name, &user_realm,
                              &normalized_upn)) {
    return ERROR_PARSE_UPN_FAILED;
  }
  AnonymizeRealm(user_realm, kUserRealmPlaceholder);
  anonymizer_->SetReplacement(user_name, kSAMAccountNamePlaceholder);

  std::string join_realm;
  if (!machine_domain.empty()) {
    // Join machine to the given domain (note: realm and domain is the same).
    join_realm = base::ToUpperASCII(machine_domain);
    AnonymizeRealm(join_realm, kDeviceRealmPlaceholder);
  } else {
    // By default, join machine to the user's realm.
    join_realm = user_realm;
  }

  // The netbios name in smb.conf needs to be upper-case, but there is also
  // Samba code that logs the machine name lower-case, so add both here.
  anonymizer_->SetReplacementAllCases(machine_name, kMachineNamePlaceholder);

  // Wipe and (re-)create config. Note that all session data is wiped to make
  // testing easier.
  Reset();
  device_account_.netbios_name = base::ToUpperASCII(machine_name);
  device_account_.realm = join_realm;

  // Update IPs, server names etc. for the device account.
  ErrorType error = UpdateAccountData(&device_account_);
  if (error != ERROR_NONE) {
    Reset();
    return error;
  }

  // Call net ads join to join the machine to the Active Directory domain.
  std::vector<std::string> args(
      {paths_->Get(Path::NET), "ads", "join", "-U", normalized_upn, "-s",
       paths_->Get(Path::DEVICE_SMB_CONF), "-d", flags_.net_log_level()});
  if (!machine_ou.empty()) {
    args.push_back("createcomputer=" +
                   BuildDistinguishedName(machine_ou, join_realm));
  }
  ProcessExecutor net_cmd(args);
  net_cmd.SetInputFile(password_fd);
  net_cmd.SetEnv(kKrb5KTEnvKey,  // Machine keytab file path.
                 kFilePrefix + paths_->Get(Path::MACHINE_KT_TEMP));
  if (!jail_helper_.SetupJailAndRun(&net_cmd, Path::NET_ADS_SECCOMP,
                                    TIMER_NET_ADS_JOIN)) {
    Reset();
    return GetNetError(net_cmd, "join");
  }

  // Prevent that authpolicyd-exec can make changes to the keytab file.
  error = SecureMachineKeyTab();
  if (error != ERROR_NONE) {
    Reset();
    return error;
  }

  // Store configuration for subsequent runs of the daemon.
  error = WriteConfiguration();
  if (error != ERROR_NONE) {
    Reset();
    return error;
  }

  // Only if everything worked out, keep the config.
  retry_machine_kinit_ = true;
  *joined_domain = join_realm;
  return ERROR_NONE;
}

ErrorType SambaInterface::FetchUserGpos(
    const std::string& account_id, protos::GpoPolicyData* gpo_policy_data) {
  ReloadDebugFlags();
  SetUser(account_id);

  if (!user_logged_in_) {
    LOG(ERROR) << "User not logged in. Please call AuthenticateUser first.";
    return ERROR_NOT_LOGGED_IN;
  }
  DCHECK(!user_sam_account_name_.empty());
  DCHECK(!user_account_.realm.empty());

  // Update IPs, server names etc for the user account.
  ErrorType error = UpdateAccountData(&user_account_);
  if (error != ERROR_NONE)
    return error;

  // FetchDeviceGpos writes a krb5.conf here. For user policy, there's no need
  // to do that here since we're reusing the TGT generated in AuthenticateUser.

  // Get the list of GPOs for the given user name.
  protos::GpoList gpo_list;
  error = GetGpoList(user_sam_account_name_, user_account_, PolicyScope::USER,
                     &gpo_list);
  if (error != ERROR_NONE)
    return error;

  // Download GPOs from Active Directory server.
  std::vector<base::FilePath> gpo_file_paths;
  error =
      DownloadGpos(gpo_list, user_account_, PolicyScope::USER, &gpo_file_paths);
  if (error != ERROR_NONE)
    return error;

  // Parse GPOs and store them in a user+extension policy protobuf.
  std::string gpo_policy_data_blob;
  error = ParseGposIntoProtobuf(gpo_file_paths, kCmdParseUserPreg,
                                &gpo_policy_data_blob);
  if (error != ERROR_NONE)
    return error;

  return ParsePolicyData(gpo_policy_data_blob, gpo_policy_data);
}

ErrorType SambaInterface::FetchDeviceGpos(
    protos::GpoPolicyData* gpo_policy_data) {
  ReloadDebugFlags();

  // Check if the device is domain joined.
  ErrorType error = CheckDeviceAccountValid();
  if (error != ERROR_NONE)
    return error;

  // Update IPs, server names etc.
  error = UpdateAccountData(&device_account_);
  if (error != ERROR_NONE)
    return error;

  // Call kinit to get the Kerberos ticket-granting-ticket. retry_machine_kinit_
  // is true for the first device policy fetch after joining Active Directory,
  // which can be very slow because machine credentials need to propagate
  // through the AD deployment.
  std::string machine_principal =
      device_account_.netbios_name + "$@" + device_account_.realm;
  DCHECK(!device_account_.realm.empty() && !device_account_.kdc_ip.empty());
  error = device_tgt_manager_.AcquireTgtWithKeytab(
      machine_principal, Path::MACHINE_KT_STATE, retry_machine_kinit_,
      device_account_.realm, device_account_.kdc_ip);
  retry_machine_kinit_ = false;
  if (error != ERROR_NONE)
    return error;

  // Get the list of GPOs for the machine.
  protos::GpoList gpo_list;
  error = GetGpoList(device_account_.netbios_name + "$", device_account_,
                     PolicyScope::MACHINE, &gpo_list);
  if (error != ERROR_NONE)
    return error;

  // Download GPOs from Active Directory server.
  std::vector<base::FilePath> gpo_file_paths;
  error = DownloadGpos(gpo_list, device_account_, PolicyScope::MACHINE,
                       &gpo_file_paths);
  if (error != ERROR_NONE)
    return error;

  // Parse GPOs and store them in a device+extension policy protobuf.
  std::string gpo_policy_data_blob;
  error = ParseGposIntoProtobuf(gpo_file_paths, kCmdParseDevicePreg,
                                &gpo_policy_data_blob);
  if (error != ERROR_NONE)
    return error;

  return ParsePolicyData(gpo_policy_data_blob, gpo_policy_data);
}

void SambaInterface::SetDefaultLogLevel(AuthPolicyFlags::DefaultLevel level) {
  flags_default_level_ = level;
  LOG(INFO) << "Flags default level = " << flags_default_level_;
  SaveFlagsDefaultLevel();
}

std::string SambaInterface::GetUserAndRealm() const {
  return user_sam_account_name_ + "@" + user_account_.realm;
}

ErrorType SambaInterface::UpdateKdcIp(AccountData* account) const {
  // Call net ads info to get the KDC IP.
  const std::string& smb_conf_path = paths_->Get(account->smb_conf_path);
  authpolicy::ProcessExecutor net_cmd({paths_->Get(Path::NET), "ads", "info",
                                       "-s", smb_conf_path, "-d",
                                       flags_.net_log_level()});
  // Replace a few values immediately in the net_cmd output, see
  // SearchAccountInfo for an explanation.
  anonymizer_->ReplaceSearchArg(kKeyKdcServer, kIpAddressPlaceholder);
  anonymizer_->ReplaceSearchArg(kKeyLdapServer, kIpAddressPlaceholder);
  anonymizer_->ReplaceSearchArg(kKeyLdapServerName, kServerNamePlaceholder);
  const bool net_result = jail_helper_.SetupJailAndRun(
      &net_cmd, Path::NET_ADS_SECCOMP, TIMER_NET_ADS_INFO);
  anonymizer_->ResetSearchArgReplacements();
  if (!net_result)
    return GetNetError(net_cmd, "info");
  const std::string& net_out = net_cmd.GetStdout();

  // Parse the output to find the KDC IP. Enclose in a sandbox for security
  // considerations.
  ProcessExecutor parse_cmd(
      {paths_->Get(Path::PARSER), kCmdParseKdcIp, SerializeFlags(flags_)});
  parse_cmd.SetInputString(net_out);
  if (!jail_helper_.SetupJailAndRun(&parse_cmd, Path::PARSER_SECCOMP,
                                    TIMER_NONE)) {
    // Log net output if it hasn't been done yet.
    net_cmd.LogOutputOnce();
    LOG(ERROR) << "authpolicy_parser parse_kdc_ip failed with exit code "
               << parse_cmd.GetExitCode();
    return ERROR_PARSE_FAILED;
  }
  account->kdc_ip = parse_cmd.GetStdout();

  // Explicitly set replacements again, see SearchAccountInfo for an
  // explanation.
  anonymizer_->SetReplacementAllCases(account->kdc_ip, kIpAddressPlaceholder);

  return ERROR_NONE;
}

ErrorType SambaInterface::UpdateDcName(AccountData* account) const {
  // Call net ads lookup to get the domain controller name.
  const std::string& smb_conf_path = paths_->Get(account->smb_conf_path);
  authpolicy::ProcessExecutor net_cmd({paths_->Get(Path::NET), "ads", "lookup",
                                       "-s", smb_conf_path, "-d",
                                       flags_.net_log_level()});
  // Replace a few values immediately in the net_cmd output, see
  // SearchAccountInfo for an explanation.
  anonymizer_->ReplaceSearchArg(kKeyForest, kForestPlaceholder);
  anonymizer_->ReplaceSearchArg(kKeyDomain, kDomainPlaceholder);
  anonymizer_->ReplaceSearchArg(kKeyDomainController, kServerNamePlaceholder);
  anonymizer_->ReplaceSearchArg(kKeyPreWin2kDomain, kDomainPlaceholder);
  anonymizer_->ReplaceSearchArg(kKeyPreWin2kHostname, kServerNamePlaceholder);
  anonymizer_->ReplaceSearchArg(kKeyServerSiteName, kSiteNamePlaceholder);
  anonymizer_->ReplaceSearchArg(kKeyClientSiteName, kSiteNamePlaceholder);
  const bool net_result = jail_helper_.SetupJailAndRun(
      &net_cmd, Path::NET_ADS_SECCOMP, TIMER_NET_ADS_INFO);
  anonymizer_->ResetSearchArgReplacements();
  if (!net_result)
    return GetNetError(net_cmd, "lookup");
  const std::string& net_out = net_cmd.GetStdout();

  // Parse the output to find the domain controller name. Enclose in a sandbox
  // for security considerations.
  ProcessExecutor parse_cmd(
      {paths_->Get(Path::PARSER), kCmdParseDcName, SerializeFlags(flags_)});
  parse_cmd.SetInputString(net_out);
  if (!jail_helper_.SetupJailAndRun(&parse_cmd, Path::PARSER_SECCOMP,
                                    TIMER_NONE)) {
    // Log net output if it hasn't been done yet.
    net_cmd.LogOutputOnce();
    LOG(ERROR) << "authpolicy_parser parse_dc_name failed with exit code "
               << parse_cmd.GetExitCode();
    return ERROR_PARSE_FAILED;
  }
  account->dc_name = parse_cmd.GetStdout();

  // Explicitly set replacements again, see SearchAccountInfo for an
  // explanation.
  anonymizer_->SetReplacementAllCases(account->dc_name, kServerNamePlaceholder);

  return ERROR_NONE;
}

ErrorType SambaInterface::GetUserTgtStatus(
    ActiveDirectoryUserStatus::TgtStatus* tgt_status) {
  protos::TgtLifetime lifetime;
  ErrorType error = user_tgt_manager_.GetTgtLifetime(&lifetime);
  switch (error) {
    case ERROR_NONE:
      *tgt_status = lifetime.validity_seconds() > 0
                        ? ActiveDirectoryUserStatus::TGT_VALID
                        : ActiveDirectoryUserStatus::TGT_EXPIRED;
      return ERROR_NONE;
    // Eat two errors and convert them to TgtStatus values instead.
    case ERROR_NO_CREDENTIALS_CACHE_FOUND:
      *tgt_status = ActiveDirectoryUserStatus::TGT_NOT_FOUND;
      return ERROR_NONE;
    case ERROR_KERBEROS_TICKET_EXPIRED:
      *tgt_status = ActiveDirectoryUserStatus::TGT_EXPIRED;
      return ERROR_NONE;
    default:
      return error;
  }
}

ActiveDirectoryUserStatus::PasswordStatus SambaInterface::GetUserPasswordStatus(
    const ActiveDirectoryAccountInfo& account_info) {
  // See https://msdn.microsoft.com/en-us/library/ms679430(v=vs.85).aspx.

  // Password is always valid if it never expires.
  if ((account_info.user_account_control() & UF_DONT_EXPIRE_PASSWD) != 0)
    return ActiveDirectoryUserStatus::PASSWORD_VALID;

  // Password expired, user will have to enter a new password.
  if (account_info.pwd_last_set() == 0)
    return ActiveDirectoryUserStatus::PASSWORD_EXPIRED;

  // Memorize pwd_last_set if it wasn't set yet. This happens after the password
  // expired and was reset by AuthenticateUser().
  if (user_pwd_last_set_ == 0) {
    user_pwd_last_set_ = account_info.pwd_last_set();
    return ActiveDirectoryUserStatus::PASSWORD_VALID;
  }

  // Password changed on the server. Note: Don't update pwd_last_set_ here,
  // update it in AuthenticateUser() when we know that Chrome sent the right
  // password.
  if (user_pwd_last_set_ != account_info.pwd_last_set())
    return ActiveDirectoryUserStatus::PASSWORD_CHANGED;

  // pwd_last_set did not change, password is still valid.
  return ActiveDirectoryUserStatus::PASSWORD_VALID;
}

ErrorType SambaInterface::UpdateWorkgroup(AccountData* account) {
  const std::string& smb_conf_path = paths_->Get(account->smb_conf_path);
  ProcessExecutor net_cmd({paths_->Get(Path::NET), "ads", "workgroup", "-s",
                           smb_conf_path, "-d", flags_.net_log_level()});
  // Parse workgroup from the net_cmd output immediately, see SearchAccountInfo
  // for an explanation. Also replace a bunch of other server names.
  anonymizer_->ReplaceSearchArg(kKeyWorkgroup, kWorkgroupPlaceholder);
  anonymizer_->ReplaceSearchArg(kKeyAdsDnsParseRrSrv, kServerNamePlaceholder,
                                "Parsed (.+?)\\.");
  anonymizer_->ReplaceSearchArg(kKeyPdcDnsName, kServerNamePlaceholder,
                                "'(.+)'");
  anonymizer_->ReplaceSearchArg(kKeyAdsDcName, kServerNamePlaceholder,
                                "using server='(.+?)\\.");
  anonymizer_->ReplaceSearchArg(kKeyPdcName, kServerNamePlaceholder, "'(.+)'");
  anonymizer_->ReplaceSearchArg(kKeyServerSite, kSiteNamePlaceholder, "'(.+)'");
  anonymizer_->ReplaceSearchArg(kKeyClientSite, kSiteNamePlaceholder, "'(.+)'");
  const bool net_result = jail_helper_.SetupJailAndRun(
      &net_cmd, Path::NET_ADS_SECCOMP, TIMER_NET_ADS_WORKGROUP);
  anonymizer_->ResetSearchArgReplacements();
  if (!net_result)
    return GetNetError(net_cmd, "workgroup");
  const std::string& net_out = net_cmd.GetStdout();

  // Parse the output to find the workgroup. Enclose in a sandbox for security
  // considerations.
  ProcessExecutor parse_cmd(
      {paths_->Get(Path::PARSER), kCmdParseWorkgroup, SerializeFlags(flags_)});
  parse_cmd.SetInputString(net_out);
  if (!jail_helper_.SetupJailAndRun(&parse_cmd, Path::PARSER_SECCOMP,
                                    TIMER_NONE)) {
    LOG(ERROR) << "authpolicy_parser parse_workgroup failed with exit code "
               << parse_cmd.GetExitCode();
    return ERROR_PARSE_FAILED;
  }
  account->workgroup = parse_cmd.GetStdout();

  // Explicitly set replacements again, see SearchAccountInfo for an
  // explanation.
  anonymizer_->SetReplacement(account->workgroup, kWorkgroupPlaceholder);
  return ERROR_NONE;
}

ErrorType SambaInterface::WriteSmbConf(const AccountData& account) const {
  // account.netbios_name and account.workgroup may be empty at this point.
  DCHECK(!account.realm.empty());

  std::string data = base::StringPrintf(
      kSmbConfData, account.netbios_name.c_str(), account.workgroup.c_str(),
      account.realm.c_str(), paths_->Get(Path::SAMBA_LOCK_DIR).c_str(),
      paths_->Get(Path::SAMBA_CACHE_DIR).c_str(),
      paths_->Get(Path::SAMBA_STATE_DIR).c_str(),
      paths_->Get(Path::SAMBA_PRIVATE_DIR).c_str());

  const base::FilePath smbconf_path(paths_->Get(account.smb_conf_path));
  const int data_size = static_cast<int>(data.size());
  if (base::WriteFile(smbconf_path, data.c_str(), data_size) != data_size) {
    LOG(ERROR) << "Failed to write Samba conf file '" << smbconf_path.value()
               << "'";
    return ERROR_LOCAL_IO;
  }

  return ERROR_NONE;
}

ErrorType SambaInterface::UpdateAccountData(AccountData* account) {
  // Write smb.conf for UpdateWorkgroup().
  ErrorType error = WriteSmbConf(*account);
  if (error != ERROR_NONE)
    return error;

  // Update |account|->workgroup.
  const std::string prev_workgroup = account->workgroup;
  error = UpdateWorkgroup(account);
  if (error != ERROR_NONE)
    return error;

  // Write smb.conf again for the rest in case the workgroup changed.
  if (account->workgroup != prev_workgroup) {
    error = WriteSmbConf(*account);
    if (error != ERROR_NONE)
      return error;
  }

  // Query the key distribution center IP and store it in |account|->kdc_ip.
  error = UpdateKdcIp(account);
  if (error != ERROR_NONE)
    return error;

  // Query the domain controller name and store it in |account|->dc_name.
  error = UpdateDcName(account);
  if (error != ERROR_NONE)
    return error;

  return ERROR_NONE;
}

ErrorType SambaInterface::WriteConfiguration() const {
  DCHECK(!device_account_.realm.empty());
  DCHECK(!device_account_.netbios_name.empty());

  protos::ActiveDirectoryConfig config;
  config.set_realm(device_account_.realm);
  config.set_machine_name(device_account_.netbios_name);

  std::string config_blob;
  if (!config.SerializeToString(&config_blob)) {
    LOG(ERROR) << "Failed to serialize configuration to string";
    return ERROR_LOCAL_IO;
  }

  const base::FilePath config_path(paths_->Get(Path::CONFIG_DAT));
  const int config_size = static_cast<int>(config_blob.size());
  if (base::WriteFile(config_path, config_blob.c_str(), config_size) !=
      config_size) {
    LOG(ERROR) << "Failed to write configuration file '" << config_path.value()
               << "'";
    return ERROR_LOCAL_IO;
  }

  // This file is only authpolicyd's business.
  ErrorType error =
      SetFilePermissions(config_path, base::FILE_PERMISSION_READ_BY_USER);
  if (error != ERROR_NONE)
    return error;

  LOG(INFO) << "Wrote configuration file '" << config_path.value() << "'";
  return ERROR_NONE;
}

ErrorType SambaInterface::ReadConfiguration() {
  const base::FilePath config_path(paths_->Get(Path::CONFIG_DAT));
  if (!base::PathExists(config_path)) {
    LOG(ERROR) << "Configuration file '" << config_path.value()
               << "' does not exist";
    return ERROR_LOCAL_IO;
  }

  std::string config_blob;
  if (!base::ReadFileToStringWithMaxSize(config_path, &config_blob,
                                         kConfigSizeLimit)) {
    PLOG(ERROR) << "Failed to read configuration file '" << config_path.value()
                << "'";
    return ERROR_LOCAL_IO;
  }

  auto config = std::make_unique<protos::ActiveDirectoryConfig>();
  if (!config->ParseFromString(config_blob)) {
    LOG(ERROR) << "Failed to parse configuration from string";
    return ERROR_LOCAL_IO;
  }

  // Check if the config is valid.
  if (config->machine_name().empty() || config->realm().empty()) {
    LOG(ERROR) << "Configuration is invalid";
    return ERROR_LOCAL_IO;
  }

  device_account_.realm = config->realm();
  device_account_.netbios_name = config->machine_name();
  LOG(INFO) << "Read configuration file '" << config_path.value() << "'";

  AnonymizeRealm(device_account_.realm, kDeviceRealmPlaceholder);
  anonymizer_->SetReplacementAllCases(device_account_.netbios_name,
                                      kMachineNamePlaceholder);
  return ERROR_NONE;
}

ErrorType SambaInterface::SecureMachineKeyTab() const {
  // At this point, tmp_kt_fp is rw for authpolicyd-exec only, so we, i.e.
  // user authpolicyd, cannot read it. Thus, change file permissions as
  // authpolicyd-exec user, so that the authpolicyd group can read it.
  const base::FilePath temp_kt_fp(paths_->Get(Path::MACHINE_KT_TEMP));
  const base::FilePath state_kt_fp(paths_->Get(Path::MACHINE_KT_STATE));
  ErrorType error;

  // Set group read permissions on keytab as authpolicyd-exec, so we can copy it
  // as authpolicyd (and own the copy).
  {
    ScopedSwitchToSavedUid switch_scope;
    error = SetFilePermissions(temp_kt_fp, kFileMode_rwr);
    if (error != ERROR_NONE)
      return error;
  }

  // Create empty file in destination directory. Note that it is created with
  // rw_r__r__ permissions.
  if (base::WriteFile(state_kt_fp, nullptr, 0) != 0) {
    LOG(ERROR) << "Failed to create file '" << state_kt_fp.value() << "'";
    return ERROR_LOCAL_IO;
  }

  // Revoke 'read by others' permission. We could also just copy temp_kt_fp to
  // state_kt_fp (see below) and revoke the read permission afterwards, but then
  // state_kt_fp would be readable by anyone for a split second, causing a
  // potential security risk.
  error = SetFilePermissions(state_kt_fp, kFileMode_rwr);
  if (error != ERROR_NONE)
    return error;

  // Now we may copy the file. The copy is owned by authpolicyd:authpolicyd.
  if (!base::CopyFile(temp_kt_fp, state_kt_fp)) {
    PLOG(ERROR) << "Failed to copy file '" << temp_kt_fp.value() << "' to '"
                << state_kt_fp.value() << "'";
    return ERROR_LOCAL_IO;
  }

  // Clean up temp file (must be done as authpolicyd-exec).
  {
    ScopedSwitchToSavedUid switch_scope;
    if (!base::DeleteFile(temp_kt_fp, false)) {
      LOG(ERROR) << "Failed to delete file '" << temp_kt_fp.value() << "'";
      return ERROR_LOCAL_IO;
    }
  }

  return ERROR_NONE;
}

ErrorType SambaInterface::GetAccountInfo(
    const std::string& user_name,
    const std::string& normalized_upn,
    const std::string& account_id,
    ActiveDirectoryAccountInfo* account_info) {
  // The user TGT might not be accessible at this point (we need the
  // sAMAccountName returned in |account_info| to fetch the user TGT). Thus, we
  // have to query the account info using the DEVICE account. Thus, first make
  // sure we have everything we need.
  ErrorType error = UpdateAccountData(&device_account_);
  if (error != ERROR_NONE)
    return error;

  // Refresh the device TGT used by SearchAccountInfo() below.
  std::string machine_principal =
      device_account_.netbios_name + "$@" + device_account_.realm;
  DCHECK(!device_account_.realm.empty() && !device_account_.kdc_ip.empty());
  error = device_tgt_manager_.AcquireTgtWithKeytab(
      machine_principal, Path::MACHINE_KT_STATE, false, device_account_.realm,
      device_account_.kdc_ip);
  if (error != ERROR_NONE)
    return error;

  // Write the user's smb.conf for SearchAccountInfo().
  error = WriteSmbConf(user_account_);
  if (error != ERROR_NONE)
    return error;

  // If |account_id| is provided, search by objectGUID only.
  if (!account_id.empty()) {
    // Searching by objectGUID has to use the octet string representation!
    // Note: If |account_id| is malformed, the search yields no results.
    const std::string account_id_octet = GuidToOctetString(account_id);
    anonymizer_->SetReplacement(account_id_octet, kAccountIdPlaceholder);
    std::string search_string =
        base::StringPrintf("(objectGUID=%s)", account_id_octet.c_str());
    return SearchAccountInfo(search_string, account_info);
  }

  // Otherwise, search by sAMAccountName, then by userPrincipalName.
  anonymizer_->SetReplacement(user_name, kSAMAccountNamePlaceholder);
  std::string search_string =
      base::StringPrintf("(sAMAccountName=%s)", user_name.c_str());
  error = SearchAccountInfo(search_string, account_info);
  if (error != ERROR_BAD_USER_NAME)  // ERROR_BAD_USER_NAME means there were
    return error;                    // no search results.

  LOG(WARNING) << "Account info not found by sAMAccountName. "
               << "Trying userPrincipalName.";
  anonymizer_->SetReplacement(user_name, kLogonNamePlaceholder);
  search_string =
      base::StringPrintf("(userPrincipalName=%s)", normalized_upn.c_str());
  return SearchAccountInfo(search_string, account_info);
}

ErrorType SambaInterface::SearchAccountInfo(
    const std::string& search_string,
    ActiveDirectoryAccountInfo* account_info) {
  // Call net ads search to find the user's account info. Note that we're
  // authenticating with the device account, but we're searching on the user's
  // realm!
  const std::string& smb_conf_path = paths_->Get(user_account_.smb_conf_path);
  ProcessExecutor net_cmd({paths_->Get(Path::NET), "ads", "search",
                           search_string, kSearchObjectGUID,
                           kSearchSAMAccountName, kSearchCommonName,
                           kSearchDisplayName, kSearchGivenName,
                           kSearchPwdLastSet, kSearchUserAccountControl, "-s",
                           smb_conf_path, "-d", flags_.net_log_level()});

  // Parse the search args from the net_cmd output immediately. This resolves
  // the chicken-egg-problem that replacement strings cannot be set before the
  // strings-to-replace are known, so the output of net_cmd would still contain
  // sensitive strings.
  anonymizer_->ReplaceSearchArg(kSearchObjectGUID, kAccountIdPlaceholder);
  anonymizer_->ReplaceSearchArg(kSearchDisplayName, kDisplayNamePlaceholder);
  anonymizer_->ReplaceSearchArg(kSearchGivenName, kGivenNamePlaceholder);
  anonymizer_->ReplaceSearchArg(kSearchSAMAccountName,
                                kSAMAccountNamePlaceholder);
  anonymizer_->ReplaceSearchArg(kSearchCommonName, kCommonNamePlaceholder);

  // Use the machine TGT to query the account info.
  net_cmd.SetEnv(kKrb5CCEnvKey,
                 paths_->Get(device_tgt_manager_.GetCredentialCachePath()));
  const bool net_result = jail_helper_.SetupJailAndRun(
      &net_cmd, Path::NET_ADS_SECCOMP, TIMER_NET_ADS_SEARCH);
  anonymizer_->ResetSearchArgReplacements();
  if (!net_result) {
    return GetNetError(net_cmd, "search");
  }
  const std::string& net_out = net_cmd.GetStdout();

  // Parse the output to find the account info proto blob. Enclose in a sandbox
  // for security considerations.
  ProcessExecutor parse_cmd({paths_->Get(Path::PARSER), kCmdParseAccountInfo,
                             SerializeFlags(flags_)});
  parse_cmd.SetInputString(net_out);
  if (!jail_helper_.SetupJailAndRun(&parse_cmd, Path::PARSER_SECCOMP,
                                    TIMER_NONE)) {
    LOG(ERROR) << "Failed to get user account id. Net response: " << net_out;
    return ERROR_PARSE_FAILED;
  }
  const std::string& account_info_blob = parse_cmd.GetStdout();

  // Parse account info protobuf.
  if (account_info_blob.empty()) {
    // No search results. Return ERROR_BAD_USER_NAME since it usually means that
    // the user mistyped their user name.
    LOG(WARNING) << "Search yielded no results";
    return ERROR_BAD_USER_NAME;
  } else if (!account_info->ParseFromString(account_info_blob)) {
    // Log net output if it hasn't been done yet.
    net_cmd.LogOutputOnce();
    LOG(ERROR) << "Failed to parse account info protobuf";
    return ERROR_PARSE_FAILED;
  }

  // Explicitly set replacements again in case logging is currently disabled
  // and the anonymizer has not parsed the search values above. If we didn't do
  // it here and logging would be enabled later, logs would contain sensitive
  // data.
  anonymizer_->SetReplacement(account_info->account_id(),
                              kAccountIdPlaceholder);
  anonymizer_->SetReplacement(account_info->display_name(),
                              kDisplayNamePlaceholder);
  anonymizer_->SetReplacement(account_info->given_name(),
                              kGivenNamePlaceholder);
  anonymizer_->SetReplacement(account_info->sam_account_name(),
                              kSAMAccountNamePlaceholder);
  anonymizer_->SetReplacement(account_info->common_name(),
                              kCommonNamePlaceholder);

  return ERROR_NONE;
}

ErrorType SambaInterface::GetGpoList(const std::string& user_or_machine_name,
                                     const AccountData& account,
                                     PolicyScope scope,
                                     protos::GpoList* gpo_list) const {
  DCHECK(gpo_list);
  LOG(INFO) << "Getting " << (scope == PolicyScope::USER ? "user" : "device")
            << " GPO list";

  // Machine names are names ending with $, anything else is a user name.
  authpolicy::ProcessExecutor net_cmd(
      {paths_->Get(Path::NET), "ads", "gpo", "list", user_or_machine_name, "-s",
       paths_->Get(account.smb_conf_path), "-d", flags_.net_log_level()});
  const TgtManager& tgt_manager =
      scope == PolicyScope::USER ? user_tgt_manager_ : device_tgt_manager_;
  net_cmd.SetEnv(kKrb5CCEnvKey,
                 paths_->Get(tgt_manager.GetCredentialCachePath()));
  if (!jail_helper_.SetupJailAndRun(&net_cmd, Path::NET_ADS_SECCOMP,
                                    TIMER_NET_ADS_GPO_LIST)) {
    return GetNetError(net_cmd, "gpo list");
  }

  // GPO data is written to stderr, not stdin!
  const std::string& net_out = net_cmd.GetStderr();

  // Parse the GPO list. Enclose in a sandbox for security considerations.
  const char* cmd = scope == PolicyScope::USER ? kCmdParseUserGpoList
                                               : kCmdParseDeviceGpoList;
  ProcessExecutor parse_cmd(
      {paths_->Get(Path::PARSER), cmd, SerializeFlags(flags_)});
  parse_cmd.SetInputString(net_out);
  if (!jail_helper_.SetupJailAndRun(&parse_cmd, Path::PARSER_SECCOMP,
                                    TIMER_NONE)) {
    // Log net output if it hasn't been done yet.
    net_cmd.LogOutputOnce();
    LOG(ERROR) << "Failed to parse GPO list";
    return ERROR_PARSE_FAILED;
  }
  std::string gpo_list_blob = parse_cmd.GetStdout();

  // Parse GPO list protobuf.
  if (!gpo_list->ParseFromString(gpo_list_blob)) {
    LOG(ERROR) << "Failed to read GPO list protobuf";
    return ERROR_PARSE_FAILED;
  }

  return ERROR_NONE;
}

struct GpoPaths {
  std::string server_;    // GPO file path on server (not a local file path!).
  base::FilePath local_;  // Local GPO file path.
  GpoPaths(const std::string& server, const std::string& local)
      : server_(server), local_(local) {}
};

ErrorType SambaInterface::DownloadGpos(
    const protos::GpoList& gpo_list,
    const AccountData& account,
    PolicyScope scope,
    std::vector<base::FilePath>* gpo_file_paths) const {
  metrics_->Report(METRIC_DOWNLOAD_GPO_COUNT, gpo_list.entries_size());
  if (gpo_list.entries_size() == 0) {
    LOG(INFO) << "No GPOs to download";
    return ERROR_NONE;
  }

  // Generate all smb source and linux target directories and create targets.
  ErrorType error;
  std::string smb_command = "prompt OFF;lowercase ON;";
  std::string gpo_share;
  std::vector<GpoPaths> gpo_paths;
  for (int entry_idx = 0; entry_idx < gpo_list.entries_size(); ++entry_idx) {
    const protos::GpoEntry& gpo = gpo_list.entries(entry_idx);

    // Security check, make sure nobody sneaks in smbclient commands.
    if (gpo.share().find(';') != std::string::npos ||
        gpo.directory().find(';') != std::string::npos) {
      LOG(ERROR) << "GPO paths may not contain a ';'";
      return ERROR_BAD_GPOS;
    }

    // All GPOs should have the same share, i.e. come from the same SysVol.
    if (gpo_share.empty()) {
      gpo_share = gpo.share();
    } else if (!base::EqualsCaseInsensitiveASCII(gpo_share, gpo.share())) {
      LOG(ERROR) << "Inconsistent share '" << gpo_share << "' != '"
                 << gpo.share() << "'";
      return ERROR_BAD_GPOS;
    }

    // Figure out local (Linux) and remote (smb) directories.
    const char* preg_dir =
        scope == PolicyScope::USER ? kPRegUserDir : kPRegDeviceDir;
    std::string smb_dir =
        base::StringPrintf("\\%s\\%s", gpo.directory().c_str(), preg_dir);
    std::string linux_dir = paths_->Get(Path::GPO_LOCAL_DIR) + smb_dir;
    std::replace(linux_dir.begin(), linux_dir.end(), '\\', '/');

    // Make local directory.
    const base::FilePath linux_dir_fp(linux_dir);
    error = ::authpolicy::CreateDirectory(linux_dir_fp);
    if (error != ERROR_NONE)
      return error;

    // Set group rwx permissions recursively, so that smbclient can write GPOs
    // there and the parser tool can read the GPOs later.
    error = SetFilePermissionsRecursive(
        linux_dir_fp, base::FilePath(paths_->Get(Path::SAMBA_DIR)),
        kFileMode_rwxrwx);
    if (error != ERROR_NONE)
      return error;

    // Build command for smbclient.
    smb_command += base::StringPrintf("cd %s;lcd %s;get %s;", smb_dir.c_str(),
                                      linux_dir.c_str(), kPRegFileName);

    // Record output file paths.
    gpo_paths.push_back(GpoPaths(smb_dir + "\\" + kPRegFileName,
                                 linux_dir + "/" + kPRegFileName));

    // Delete any preexisting policy file. Otherwise, if downloading the file
    // failed, we wouldn't realize it and use a stale version.
    if (base::PathExists(gpo_paths.back().local_) &&
        !base::DeleteFile(gpo_paths.back().local_, false)) {
      LOG(ERROR) << "Failed to delete old GPO file '"
                 << anonymizer_->Process(gpo_paths.back().local_.value())
                 << "'";
      return ERROR_LOCAL_IO;
    }
  }

  DCHECK(!account.dc_name.empty());
  const std::string service =
      base::StringPrintf("//%s/%s", account.dc_name.c_str(), gpo_share.c_str());

  // The exit code of smbclient corresponds to the LAST command issued. Some
  // files might be missing and fail to download, which is fine and handled
  // below. Appending 'exit' makes sure the exit code is not 1 if the last file
  // happens to be missing.
  smb_command += "exit;";

  // Download GPO into local directory. Retry a couple of times in case of
  // network errors, Kerberos authentication may be flaky in some deployments,
  // see crbug.com/684733.
  ProcessExecutor smb_client_cmd({paths_->Get(Path::SMBCLIENT), service, "-s",
                                  paths_->Get(account.smb_conf_path), "-k",
                                  "-d", flags_.net_log_level(), "-c",
                                  smb_command});
  const TgtManager& tgt_manager =
      scope == PolicyScope::USER ? user_tgt_manager_ : device_tgt_manager_;
  smb_client_cmd.SetEnv(kKrb5CCEnvKey,
                        paths_->Get(tgt_manager.GetCredentialCachePath()));
  smb_client_cmd.SetEnv(kKrb5ConfEnvKey,  // Kerberos configuration file path.
                        kFilePrefix + paths_->Get(tgt_manager.GetConfigPath()));
  int tries, failed_tries = 0;
  for (tries = 1; tries <= kSmbClientMaxTries; ++tries) {
    if (tries > 1 && smbclient_retry_sleep_enabled_) {
      base::PlatformThread::Sleep(
          base::TimeDelta::FromSeconds(kSmbClientRetryWaitSeconds));
    }
    if (jail_helper_.SetupJailAndRun(&smb_client_cmd, Path::SMBCLIENT_SECCOMP,
                                     TIMER_SMBCLIENT)) {
      error = ERROR_NONE;
      break;
    }
    failed_tries++;
    error = GetSmbclientError(smb_client_cmd);
    if (error != ERROR_NETWORK_PROBLEM)
      break;
  }
  metrics_->Report(METRIC_SMBCLIENT_FAILED_TRY_COUNT, failed_tries);
  if (error != ERROR_NONE)
    return error;

  // Note that the errors are in stdout and the output is in stderr :-/
  const std::string& smbclient_out_lower =
      base::ToLowerASCII(smb_client_cmd.GetStdout());

  // Make sure the GPO files actually downloaded.
  DCHECK(gpo_file_paths);
  for (const GpoPaths& gpo_path : gpo_paths) {
    if (base::PathExists(gpo_path.local_)) {
      gpo_file_paths->push_back(gpo_path.local_);
    } else {
      // Gracefully handle non-existing GPOs. Testing revealed these cases do
      // exist, see crbug.com/680921.
      const std::string no_file_error_key(
          base::ToLowerASCII(kKeyObjectNameNotFound + gpo_path.server_));
      if (Contains(smbclient_out_lower, no_file_error_key)) {
        LOG_IF(WARNING, flags_.log_gpo())
            << "Ignoring missing preg file '"
            << anonymizer_->Process(gpo_path.local_.value()) << "'";
      } else {
        // Log smbclient output if it hasn't been done yet.
        smb_client_cmd.LogOutputOnce();
        LOG(ERROR) << "Failed to download preg file '"
                   << anonymizer_->Process(gpo_path.local_.value()) << "'";
        return ERROR_SMBCLIENT_FAILED;
      }
    }
  }

  return ERROR_NONE;
}

ErrorType SambaInterface::ParseGposIntoProtobuf(
    const std::vector<base::FilePath>& gpo_file_paths,
    const char* parser_cmd_string,
    std::string* policy_blob) const {
  // Convert file paths to proto blob.
  std::string gpo_file_paths_blob;
  protos::FilePathList fp_proto;
  for (const auto& fp : gpo_file_paths)
    *fp_proto.add_entries() = fp.value();
  if (!fp_proto.SerializeToString(&gpo_file_paths_blob)) {
    LOG(ERROR) << "Failed to serialize policy file paths to protobuf";
    return ERROR_PARSE_PREG_FAILED;
  }

  // Load GPOs into protobuf. Enclose in a sandbox for security considerations.
  ProcessExecutor parse_cmd(
      {paths_->Get(Path::PARSER), parser_cmd_string, SerializeFlags(flags_)});
  parse_cmd.SetInputString(gpo_file_paths_blob);
  if (!jail_helper_.SetupJailAndRun(&parse_cmd, Path::PARSER_SECCOMP,
                                    TIMER_NONE)) {
    LOG(ERROR) << "Failed to parse preg files";
    return ERROR_PARSE_PREG_FAILED;
  }
  *policy_blob = parse_cmd.GetStdout();
  return ERROR_NONE;
}

void SambaInterface::SetUser(const std::string& account_id) {
  // Don't allow authenticating multiple users. Chrome should prevent that.
  CHECK(!account_id.empty());
  CHECK(user_account_id_.empty() || user_account_id_ == account_id)
      << "Multi-user not supported";
  user_account_id_ = account_id;
}

void SambaInterface::SetUserRealm(const std::string& user_realm) {
  // Allow setting the realm only once. This makes sure that nobody calls
  // AuthenticateUser() with a different realm, the call fails and we're stuck
  // with a wrong realm.
  CHECK(!user_realm.empty());
  CHECK(user_account_.realm.empty() || user_account_.realm == user_realm)
      << "Multi-user not supported";
  user_account_.realm = user_realm;
  AnonymizeRealm(user_realm, kUserRealmPlaceholder);
}

void SambaInterface::AnonymizeRealm(const std::string& realm,
                                    const char* placeholder) {
  anonymizer_->SetReplacementAllCases(realm, placeholder);

  std::vector<std::string> parts = base::SplitString(
      realm, ".", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (size_t n = 0; n < parts.size(); ++n)
    anonymizer_->SetReplacementAllCases(parts[n], placeholder);
}

ErrorType SambaInterface::CheckDeviceAccountValid() const {
  if (device_account_.realm.empty() || device_account_.netbios_name.empty()) {
    LOG(ERROR) << "Device is not joined. Must call JoinMachine first.";
    return ERROR_NOT_JOINED;
  }
  return ERROR_NONE;
}

void SambaInterface::Reset() {
  user_account_id_.clear();
  user_sam_account_name_.clear();
  user_pwd_last_set_ = 0;
  user_logged_in_ = false;
  user_account_ = AccountData(Path::USER_SMB_CONF);
  device_account_ = AccountData(Path::DEVICE_SMB_CONF);
  retry_machine_kinit_ = false;
}

void SambaInterface::LoadFlagsDefaultLevel() {
  const base::FilePath default_level_path(
      paths_->Get(Path::FLAGS_DEFAULT_LEVEL));
  if (!CheckFlagsDefaultLevelValid(default_level_path))
    return;
  std::string level_str;
  if (!base::ReadFileToStringWithMaxSize(default_level_path, &level_str, 16)) {
    PLOG(ERROR) << "Failed to read flags default level from "
                << default_level_path.value();
    return;
  }
  int level_int;
  if (!base::StringToInt(level_str, &level_int) ||
      level_int < AuthPolicyFlags::kMinLevel ||
      level_int > AuthPolicyFlags::kMaxLevel) {
    LOG(ERROR) << "Bad flags default level '" << level_str << "'";
    return;
  }
  flags_default_level_ = static_cast<AuthPolicyFlags::DefaultLevel>(level_int);
  LOG(INFO) << "Flags default level = " << flags_default_level_;
}

void SambaInterface::SaveFlagsDefaultLevel() {
  const base::FilePath default_level_path(
      paths_->Get(Path::FLAGS_DEFAULT_LEVEL));
  const std::string level_str = std::to_string(flags_default_level_);
  const int size = static_cast<int>(level_str.size());
  if (flags_default_level_ == AuthPolicyFlags::kQuiet) {
    // Remove the file, kQuiet is the default, anyway.
    if (!base::DeleteFile(default_level_path, false /* recursive */)) {
      PLOG(ERROR) << "Failed to delete flags default level file "
                  << default_level_path.value();
    }
  } else {
    // Write the file.
    if (base::WriteFile(default_level_path, level_str.data(), size) != size) {
      PLOG(ERROR) << "Failed to write flags default level to "
                  << default_level_path.value();
    }
  }
}

void SambaInterface::ReloadDebugFlags() {
  const base::FilePath default_level_path(
      paths_->Get(Path::FLAGS_DEFAULT_LEVEL));
  if (flags_default_level_ != AuthPolicyFlags::kQuiet &&
      !CheckFlagsDefaultLevelValid(default_level_path)) {
    // Default flags file expired, reset default level.
    flags_default_level_ = AuthPolicyFlags::kQuiet;
  }

  // First set defaults, then load file on top.
  AuthPolicyFlags flags_container;
  flags_container.SetDefaults(flags_default_level_);
  const base::FilePath path(paths_->Get(Path::DEBUG_FLAGS));
  if (flags_container.LoadFromJsonFile(path) ||
      flags_default_level_ != AuthPolicyFlags::kQuiet) {
    flags_container.Dump();
  }
  flags_ = flags_container.Get();
}

}  // namespace authpolicy
