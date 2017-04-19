// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "authpolicy/stub_common.h"

#include <base/files/file_util.h>
#include <base/strings/string_util.h>

#include "authpolicy/constants.h"

namespace authpolicy {

const int kExitCodeOk = 0;
const int kExitCodeError = 1;

const char kRealm[] = "REALM.COM";

const char kUserName[] = "user";
const char kUserPrincipal[] = "user@REALM.COM";
const char kInvalidUserPrincipal[] = "user.REALM.COM";
const char kNonExistingUserPrincipal[] = "non_existing_user@REALM.COM";
const char kNetworkErrorUserPrincipal[] = "network_error_user@REALM.COM";
const char kAccessDeniedUserPrincipal[] = "access_denied_user@REALM.COM";
const char kKdcRetryUserPrincipal[] = "kdc_retry_user@REALM.COM";
const char kInsufficientQuotaUserPrincipal[] =
    "insufficient_quota_user@REALM.COM";
const char kExpiredTgtUserPrincipal[] = "tgt_expired@REALM.COM";

const char kDisplayName[] = "John Doe";
const char kGivenName[] = "John";

// Should still be valid GUIDs, so GuidToOctetString() works.
const char kAccountId[] = "f892eb9d-9e11-4a74-b894-0647e218c4df";
const char kBadAccountId[] = "88adef4f-74ec-420d-b0a5-3726dbe711eb";

const char kValidKrb5CCData[] = "valid";
const char kExpiredKrb5CCData[] = "expired";

const char kPassword[] = "p4zzw!5d";
const char kWrongPassword[] = "pAzzwI5d";
const char kExpiredPassword[] = "rootpw";

const char kMachineName[] = "testcomp";
const char kTooLongMachineName[] = "too_long_machine_name";
const char kInvalidMachineName[] = "invalid?na:me";
const char kNonExistingMachineName[] = "nonexisting";
const char kEmptyGpoMachineName[] = "emptygpo";
const char kGpoDownloadErrorMachineName[] = "gpodownloaderr";
const char kOneGpoMachineName[] = "onegpo";
const char kTwoGposMachineName[] = "twogpos";
const char kZeroUserVersionMachineName[] = "zerouserversion";
const char kDisableUserFlagMachineName[] = "disableuserflag";
const char kPropagationRetryMachineName[] = "propagat.nretry";

extern const char kGpo1Guid[] = "{11111111-1111-1111-1111-111111111111}";
extern const char kGpo2Guid[] = "{22222222-2222-2222-2222-222222222222}";
extern const char kErrorGpoGuid[] = "{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}";

const char kGpo1Filename[] = "stub_registry_1.pol";
const char kGpo2Filename[] = "stub_registry_2.pol";

namespace {

// Looks up the environment variable with key |env_key|. If |remove_prefix| is
// false, returns its value. If |remove_prefix| is true, the value is expected
// to be 'FILE:<path>' and only <path> is returned. Returns an empty string if
// the variable does not exist or does not have the expected prefix.
std::string GetPathFromEnv(const char* env_key, bool remove_prefix) {
  const char* env_value = getenv(env_key);
  if (!env_value)
    return std::string();
  if (!remove_prefix)
    return env_value;

  // Remove FILE: prefix.
  std::string prefixed_path = env_value;
  if (!StartsWithCaseSensitive(prefixed_path, kFilePrefix))
    return std::string();

  return prefixed_path.substr(strlen(kFilePrefix));
}

}  // namespace

std::string GetCommandLine(int argc, const char* const* argv) {
  CHECK_GE(argc, 2);
  std::string command_line = argv[1];
  for (int n = 2; n < argc; ++n) {
    command_line += " ";
    command_line += argv[n];
  }
  return command_line;
}

std::string GetArgValue(int argc, const char* const* argv, const char* name) {
  for (int n = 1; n + 1 < argc; ++n) {
    if (strcmp(argv[n], name) == 0)
      return argv[n + 1];
  }
  return std::string();
}

bool StartsWithCaseSensitive(const std::string& str, const char* search_for) {
  return base::StartsWith(str, search_for, base::CompareCase::SENSITIVE);
}

// Writes |str| to |file_descriptor|.
void WriteFileDescriptor(int file_descriptor, const std::string& str) {
  if (!str.empty()) {
    CHECK(base::WriteFileDescriptor(file_descriptor, str.c_str(), str.size()));
  }
}

// Writes |stdout_str| and |stderr_str| to stdout and stderr, resp.
void WriteOutput(const std::string& stdout_str, const std::string& stderr_str) {
  WriteFileDescriptor(STDOUT_FILENO, stdout_str);
  WriteFileDescriptor(STDERR_FILENO, stderr_str);
}

std::string GetKeytabFilePath() {
  return GetPathFromEnv(kKrb5KTEnvKey, true /* remove_prefix */);
}

std::string GetKrb5ConfFilePath() {
  return GetPathFromEnv(kKrb5ConfEnvKey, true /* remove_prefix */);
}

std::string GetKrb5CCFilePath() {
  return GetPathFromEnv(kKrb5CCEnvKey, false /* remove_prefix */);
}

}  // namespace authpolicy
