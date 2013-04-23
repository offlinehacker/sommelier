// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "update_engine/payload_state.h"

#include <algorithm>

#include <base/logging.h>
#include "base/string_util.h"
#include <base/stringprintf.h>

#include "update_engine/clock.h"
#include "update_engine/constants.h"
#include "update_engine/prefs.h"
#include "update_engine/system_state.h"
#include "update_engine/utils.h"

using base::Time;
using base::TimeDelta;
using std::min;
using std::string;

namespace chromeos_update_engine {

const TimeDelta PayloadState::kDurationSlack = TimeDelta::FromSeconds(600);

// We want to upperbound backoffs to 16 days
static const uint32_t kMaxBackoffDays = 16;

// We want to randomize retry attempts after the backoff by +/- 6 hours.
static const uint32_t kMaxBackoffFuzzMinutes = 12 * 60;

PayloadState::PayloadState()
    : prefs_(NULL),
      payload_attempt_number_(0),
      url_index_(0),
      url_failure_count_(0),
      url_switch_count_(0) {
 for (int i = 0; i <= kNumDownloadSources; i++)
  total_bytes_downloaded_[i] = current_bytes_downloaded_[i] = 0;
}

bool PayloadState::Initialize(SystemState* system_state) {
  system_state_ = system_state;
  prefs_ = system_state_->prefs();
  LoadResponseSignature();
  LoadPayloadAttemptNumber();
  LoadUrlIndex();
  LoadUrlFailureCount();
  LoadUrlSwitchCount();
  LoadBackoffExpiryTime();
  LoadUpdateTimestampStart();
  // The LoadUpdateDurationUptime() method relies on LoadUpdateTimestampStart()
  // being called before it. Don't reorder.
  LoadUpdateDurationUptime();
  for (int i = 0; i < kNumDownloadSources; i++) {
    DownloadSource source = static_cast<DownloadSource>(i);
    LoadCurrentBytesDownloaded(source);
    LoadTotalBytesDownloaded(source);
  }
  LoadNumReboots();
  return true;
}

void PayloadState::SetResponse(const OmahaResponse& omaha_response) {
  // Always store the latest response.
  response_ = omaha_response;

  // Check if the "signature" of this response (i.e. the fields we care about)
  // has changed.
  string new_response_signature = CalculateResponseSignature();
  bool has_response_changed = (response_signature_ != new_response_signature);

  // If the response has changed, we should persist the new signature and
  // clear away all the existing state.
  if (has_response_changed) {
    LOG(INFO) << "Resetting all persisted state as this is a new response";
    SetResponseSignature(new_response_signature);
    ResetPersistedState();
    return;
  }

  // This is the earliest point at which we can validate whether the URL index
  // we loaded from the persisted state is a valid value. If the response
  // hasn't changed but the URL index is invalid, it's indicative of some
  // tampering of the persisted state.
  if (url_index_ >= GetNumUrls()) {
    LOG(INFO) << "Resetting all payload state as the url index seems to have "
                 "been tampered with";
    ResetPersistedState();
    return;
  }

  // Update the current download source which depends on the latest value of
  // the response.
  UpdateCurrentDownloadSource();
}

void PayloadState::DownloadComplete() {
  LOG(INFO) << "Payload downloaded successfully";
  IncrementPayloadAttemptNumber();
}

void PayloadState::DownloadProgress(size_t count) {
  if (count == 0)
    return;

  CalculateUpdateDurationUptime();
  UpdateBytesDownloaded(count);

  // We've received non-zero bytes from a recent download operation.  Since our
  // URL failure count is meant to penalize a URL only for consecutive
  // failures, downloading bytes successfully means we should reset the failure
  // count (as we know at least that the URL is working). In future, we can
  // design this to be more sophisticated to check for more intelligent failure
  // patterns, but right now, even 1 byte downloaded will mark the URL to be
  // good unless it hits 10 (or configured number of) consecutive failures
  // again.

  if (GetUrlFailureCount() == 0)
    return;

  LOG(INFO) << "Resetting failure count of Url" << GetUrlIndex()
            << " to 0 as we received " << count << " bytes successfully";
  SetUrlFailureCount(0);
}

void PayloadState::UpdateResumed() {
  LOG(INFO) << "Resuming an update that was previously started.";
  UpdateNumReboots();
}

void PayloadState::UpdateRestarted() {
  LOG(INFO) << "Starting a new update";
  ResetDownloadSourcesOnNewUpdate();
  SetNumReboots(0);
}

void PayloadState::UpdateSucceeded() {
  // Send the relevant metrics that are tracked in this class to UMA.
  CalculateUpdateDurationUptime();
  SetUpdateTimestampEnd(system_state_->clock()->GetWallclockTime());
  ReportBytesDownloadedMetrics();
  ReportUpdateUrlSwitchesMetric();
  ReportRebootMetrics();
  ReportDurationMetrics();
}

void PayloadState::UpdateFailed(ErrorCode error) {
  ErrorCode base_error = utils::GetBaseErrorCode(error);
  LOG(INFO) << "Updating payload state for error code: " << base_error
            << " (" << utils::CodeToString(base_error) << ")";

  if (GetNumUrls() == 0) {
    // This means we got this error even before we got a valid Omaha response.
    // So we should not advance the url_index_ in such cases.
    LOG(INFO) << "Ignoring failures until we get a valid Omaha response.";
    return;
  }

  switch (base_error) {
    // Errors which are good indicators of a problem with a particular URL or
    // the protocol used in the URL or entities in the communication channel
    // (e.g. proxies). We should try the next available URL in the next update
    // check to quickly recover from these errors.
    case kErrorCodePayloadHashMismatchError:
    case kErrorCodePayloadSizeMismatchError:
    case kErrorCodeDownloadPayloadVerificationError:
    case kErrorCodeDownloadPayloadPubKeyVerificationError:
    case kErrorCodeSignedDeltaPayloadExpectedError:
    case kErrorCodeDownloadInvalidMetadataMagicString:
    case kErrorCodeDownloadSignatureMissingInManifest:
    case kErrorCodeDownloadManifestParseError:
    case kErrorCodeDownloadMetadataSignatureError:
    case kErrorCodeDownloadMetadataSignatureVerificationError:
    case kErrorCodeDownloadMetadataSignatureMismatch:
    case kErrorCodeDownloadOperationHashVerificationError:
    case kErrorCodeDownloadOperationExecutionError:
    case kErrorCodeDownloadOperationHashMismatch:
    case kErrorCodeDownloadInvalidMetadataSize:
    case kErrorCodeDownloadInvalidMetadataSignature:
    case kErrorCodeDownloadOperationHashMissingError:
    case kErrorCodeDownloadMetadataSignatureMissingError:
      IncrementUrlIndex();
      break;

    // Errors which seem to be just transient network/communication related
    // failures and do not indicate any inherent problem with the URL itself.
    // So, we should keep the current URL but just increment the
    // failure count to give it more chances. This way, while we maximize our
    // chances of downloading from the URLs that appear earlier in the response
    // (because download from a local server URL that appears earlier in a
    // response is preferable than downloading from the next URL which could be
    // a internet URL and thus could be more expensive).
    case kErrorCodeError:
    case kErrorCodeDownloadTransferError:
    case kErrorCodeDownloadWriteError:
    case kErrorCodeDownloadStateInitializationError:
    case kErrorCodeOmahaErrorInHTTPResponse: // Aggregate code for HTTP errors.
      IncrementFailureCount();
      break;

    // Errors which are not specific to a URL and hence shouldn't result in
    // the URL being penalized. This can happen in two cases:
    // 1. We haven't started downloading anything: These errors don't cost us
    // anything in terms of actual payload bytes, so we should just do the
    // regular retries at the next update check.
    // 2. We have successfully downloaded the payload: In this case, the
    // payload attempt number would have been incremented and would take care
    // of the backoff at the next update check.
    // In either case, there's no need to update URL index or failure count.
    case kErrorCodeOmahaRequestError:
    case kErrorCodeOmahaResponseHandlerError:
    case kErrorCodePostinstallRunnerError:
    case kErrorCodeFilesystemCopierError:
    case kErrorCodeInstallDeviceOpenError:
    case kErrorCodeKernelDeviceOpenError:
    case kErrorCodeDownloadNewPartitionInfoError:
    case kErrorCodeNewRootfsVerificationError:
    case kErrorCodeNewKernelVerificationError:
    case kErrorCodePostinstallBootedFromFirmwareB:
    case kErrorCodeOmahaRequestEmptyResponseError:
    case kErrorCodeOmahaRequestXMLParseError:
    case kErrorCodeOmahaResponseInvalid:
    case kErrorCodeOmahaUpdateIgnoredPerPolicy:
    case kErrorCodeOmahaUpdateDeferredPerPolicy:
    case kErrorCodeOmahaUpdateDeferredForBackoff:
    case kErrorCodePostinstallPowerwashError:
    case kErrorCodeUpdateCanceledByChannelChange:
      LOG(INFO) << "Not incrementing URL index or failure count for this error";
      break;

    case kErrorCodeSuccess:                            // success code
    case kErrorCodeSetBootableFlagError:               // unused
    case kErrorCodeUmaReportedMax:                     // not an error code
    case kErrorCodeOmahaRequestHTTPResponseBase:       // aggregated already
    case kErrorCodeDevModeFlag:                       // not an error code
    case kErrorCodeResumedFlag:                        // not an error code
    case kErrorCodeTestImageFlag:                      // not an error code
    case kErrorCodeTestOmahaUrlFlag:                   // not an error code
    case kErrorCodeSpecialFlags:                       // not an error code
      // These shouldn't happen. Enumerating these  explicitly here so that we
      // can let the compiler warn about new error codes that are added to
      // action_processor.h but not added here.
      LOG(WARNING) << "Unexpected error code for UpdateFailed";
      break;

    // Note: Not adding a default here so as to let the compiler warn us of
    // any new enums that were added in the .h but not listed in this switch.
  }
}

bool PayloadState::ShouldBackoffDownload() {
  if (response_.disable_payload_backoff) {
    LOG(INFO) << "Payload backoff logic is disabled. "
                 "Can proceed with the download";
    return false;
  }

  if (response_.is_delta_payload) {
    // If delta payloads fail, we want to fallback quickly to full payloads as
    // they are more likely to succeed. Exponential backoffs would greatly
    // slow down the fallback to full payloads.  So we don't backoff for delta
    // payloads.
    LOG(INFO) << "No backoffs for delta payloads. "
              << "Can proceed with the download";
    return false;
  }

  if (!utils::IsOfficialBuild()) {
    // Backoffs are needed only for official builds. We do not want any delays
    // or update failures due to backoffs during testing or development.
    LOG(INFO) << "No backoffs for test/dev images. "
              << "Can proceed with the download";
    return false;
  }

  if (backoff_expiry_time_.is_null()) {
    LOG(INFO) << "No backoff expiry time has been set. "
              << "Can proceed with the download";
    return false;
  }

  if (backoff_expiry_time_ < Time::Now()) {
    LOG(INFO) << "The backoff expiry time ("
              << utils::ToString(backoff_expiry_time_)
              << ") has elapsed. Can proceed with the download";
    return false;
  }

  LOG(INFO) << "Cannot proceed with downloads as we need to backoff until "
            << utils::ToString(backoff_expiry_time_);
  return true;
}

void PayloadState::IncrementPayloadAttemptNumber() {
  if (response_.is_delta_payload) {
    LOG(INFO) << "Not incrementing payload attempt number for delta payloads";
    return;
  }

  LOG(INFO) << "Incrementing the payload attempt number";
  SetPayloadAttemptNumber(GetPayloadAttemptNumber() + 1);
  UpdateBackoffExpiryTime();
}

void PayloadState::IncrementUrlIndex() {
  uint32_t next_url_index = GetUrlIndex() + 1;
  if (next_url_index < GetNumUrls()) {
    LOG(INFO) << "Incrementing the URL index for next attempt";
    SetUrlIndex(next_url_index);
  } else {
    LOG(INFO) << "Resetting the current URL index (" << GetUrlIndex() << ") to "
              << "0 as we only have " << GetNumUrls() << " URL(s)";
    SetUrlIndex(0);
    IncrementPayloadAttemptNumber();
  }

  // If we have multiple URLs, record that we just switched to another one
  if (GetNumUrls() > 1)
    SetUrlSwitchCount(url_switch_count_ + 1);

  // Whenever we update the URL index, we should also clear the URL failure
  // count so we can start over fresh for the new URL.
  SetUrlFailureCount(0);
}

void PayloadState::IncrementFailureCount() {
  uint32_t next_url_failure_count = GetUrlFailureCount() + 1;
  if (next_url_failure_count < response_.max_failure_count_per_url) {
    LOG(INFO) << "Incrementing the URL failure count";
    SetUrlFailureCount(next_url_failure_count);
  } else {
    LOG(INFO) << "Reached max number of failures for Url" << GetUrlIndex()
              << ". Trying next available URL";
    IncrementUrlIndex();
  }
}

void PayloadState::UpdateBackoffExpiryTime() {
  if (response_.disable_payload_backoff) {
    LOG(INFO) << "Resetting backoff expiry time as payload backoff is disabled";
    SetBackoffExpiryTime(Time());
    return;
  }

  if (GetPayloadAttemptNumber() == 0) {
    SetBackoffExpiryTime(Time());
    return;
  }

  // Since we're doing left-shift below, make sure we don't shift more
  // than this. E.g. if uint32_t is 4-bytes, don't left-shift more than 30 bits,
  // since we don't expect value of kMaxBackoffDays to be more than 100 anyway.
  uint32_t num_days = 1; // the value to be shifted.
  const uint32_t kMaxShifts = (sizeof(num_days) * 8) - 2;

  // Normal backoff days is 2 raised to (payload_attempt_number - 1).
  // E.g. if payload_attempt_number is over 30, limit power to 30.
  uint32_t power = min(GetPayloadAttemptNumber() - 1, kMaxShifts);

  // The number of days is the minimum of 2 raised to (payload_attempt_number
  // - 1) or kMaxBackoffDays.
  num_days = min(num_days << power, kMaxBackoffDays);

  // We don't want all retries to happen exactly at the same time when
  // retrying after backoff. So add some random minutes to fuzz.
  int fuzz_minutes = utils::FuzzInt(0, kMaxBackoffFuzzMinutes);
  TimeDelta next_backoff_interval = TimeDelta::FromDays(num_days) +
                                    TimeDelta::FromMinutes(fuzz_minutes);
  LOG(INFO) << "Incrementing the backoff expiry time by "
            << utils::FormatTimeDelta(next_backoff_interval);
  SetBackoffExpiryTime(Time::Now() + next_backoff_interval);
}

void PayloadState::UpdateCurrentDownloadSource() {
  current_download_source_ = kNumDownloadSources;

  if (GetUrlIndex() < response_.payload_urls.size())  {
    string current_url = response_.payload_urls[GetUrlIndex()];
    if (StartsWithASCII(current_url, "https://", false))
      current_download_source_ = kDownloadSourceHttpsServer;
    else if (StartsWithASCII(current_url, "http://", false))
      current_download_source_ = kDownloadSourceHttpServer;
  }

  LOG(INFO) << "Current download source: "
            << utils::ToString(current_download_source_);
}

void PayloadState::UpdateBytesDownloaded(size_t count) {
  SetCurrentBytesDownloaded(
      current_download_source_,
      GetCurrentBytesDownloaded(current_download_source_) + count,
      false);
  SetTotalBytesDownloaded(
      current_download_source_,
      GetTotalBytesDownloaded(current_download_source_) + count,
      false);
}

void PayloadState::ReportBytesDownloadedMetrics() {
  // Report metrics collected from all known download sources to UMA.
  // The reported data is in Megabytes in order to represent a larger
  // sample range.
  int download_sources_used = 0;
  string metric;
  uint64_t successful_mbs = 0;
  uint64_t total_mbs = 0;
  for (int i = 0; i < kNumDownloadSources; i++) {
    DownloadSource source = static_cast<DownloadSource>(i);
    const int kMaxMiBs = 10240; // Anything above 10GB goes in the last bucket.

    metric = "Installer.SuccessfulMBsDownloadedFrom" + utils::ToString(source);
    uint64_t mbs = GetCurrentBytesDownloaded(source) / kNumBytesInOneMiB;

    //  Count this download source as having been used if we downloaded any
    //  bytes that contributed to the final success of the update.
    if (mbs)
      download_sources_used |= (1 << source);

    successful_mbs += mbs;
    LOG(INFO) << "Uploading " << mbs << " (MBs) for metric " << metric;
    system_state_->metrics_lib()->SendToUMA(metric,
                                            mbs,
                                            0,  // min
                                            kMaxMiBs,
                                            kNumDefaultUmaBuckets);
    SetCurrentBytesDownloaded(source, 0, true);

    metric = "Installer.TotalMBsDownloadedFrom" + utils::ToString(source);
    mbs = GetTotalBytesDownloaded(source) / kNumBytesInOneMiB;
    total_mbs += mbs;
    LOG(INFO) << "Uploading " << mbs << " (MBs) for metric " << metric;
    system_state_->metrics_lib()->SendToUMA(metric,
                                            mbs,
                                            0,  // min
                                            kMaxMiBs,
                                            kNumDefaultUmaBuckets);

    SetTotalBytesDownloaded(source, 0, true);
  }

  metric = "Installer.DownloadSourcesUsed";
  LOG(INFO) << "Uploading 0x" << std::hex << download_sources_used
            << " (bit flags) for metric " << metric;
  int num_buckets = std::min(1 << kNumDownloadSources, kNumDefaultUmaBuckets);
  system_state_->metrics_lib()->SendToUMA(metric,
                                          download_sources_used,
                                          0,  // min
                                          1 << kNumDownloadSources,
                                          num_buckets);

  if (successful_mbs) {
    metric = "Installer.DownloadOverheadPercentage";
    int percent_overhead = (total_mbs - successful_mbs) * 100 / successful_mbs;
    LOG(INFO) << "Uploading " << percent_overhead << "% for metric " << metric;
    system_state_->metrics_lib()->SendToUMA(metric,
                                            percent_overhead,
                                            0,    // min: 0% overhead
                                            1000, // max: 1000% overhead
                                            kNumDefaultUmaBuckets);
  }
}

void PayloadState::ReportUpdateUrlSwitchesMetric() {
  string metric = "Installer.UpdateURLSwitches";
  int value = static_cast<int>(url_switch_count_);

  LOG(INFO) << "Uploading " << value << " (count) for metric " <<  metric;
  system_state_->metrics_lib()->SendToUMA(
       metric,
       value,
       0,    // min value
       100,  // max value
       kNumDefaultUmaBuckets);
}

void PayloadState::ReportRebootMetrics() {
  // Report the number of num_reboots.
  string metric = "Installer.UpdateNumReboots";
  uint32_t num_reboots = GetNumReboots();
  LOG(INFO) << "Uploading reboot count of " << num_reboots << " for metric "
            <<  metric;
  system_state_->metrics_lib()->SendToUMA(
      metric,
      static_cast<int>(num_reboots),  // sample
      0,  // min = 0.
      50,  // max
      25);  // buckets
  SetNumReboots(0);
}

void PayloadState::UpdateNumReboots() {
  // We only update the reboot count when the system has been detected to have
  // been rebooted.
  if (!system_state_->system_rebooted()) {
    return;
  }

  SetNumReboots(GetNumReboots() + 1);
}

void PayloadState::SetNumReboots(uint32_t num_reboots) {
  CHECK(prefs_);
  num_reboots_ = num_reboots;
  prefs_->SetInt64(kPrefsNumReboots, num_reboots);
  LOG(INFO) << "Number of Reboots during current update attempt = "
            << num_reboots_;
}

void PayloadState::ResetPersistedState() {
  SetPayloadAttemptNumber(0);
  SetUrlIndex(0);
  SetUrlFailureCount(0);
  SetUrlSwitchCount(0);
  UpdateBackoffExpiryTime(); // This will reset the backoff expiry time.
  SetUpdateTimestampStart(system_state_->clock()->GetWallclockTime());
  SetUpdateTimestampEnd(Time()); // Set to null time
  SetUpdateDurationUptime(TimeDelta::FromSeconds(0));
  ResetDownloadSourcesOnNewUpdate();
}

void PayloadState::ResetDownloadSourcesOnNewUpdate() {
  for (int i = 0; i < kNumDownloadSources; i++) {
    DownloadSource source = static_cast<DownloadSource>(i);
    SetCurrentBytesDownloaded(source, 0, true);
    // Note: Not resetting the TotalBytesDownloaded as we want that metric
    // to count the bytes downloaded across various update attempts until
    // we have successfully applied the update.
  }
}

int64_t PayloadState::GetPersistedValue(const string& key) {
  CHECK(prefs_);
  if (!prefs_->Exists(key))
    return 0;

  int64_t stored_value;
  if (!prefs_->GetInt64(key, &stored_value))
    return 0;

  if (stored_value < 0) {
    LOG(ERROR) << key << ": Invalid value (" << stored_value
               << ") in persisted state. Defaulting to 0";
    return 0;
  }

  return stored_value;
}

string PayloadState::CalculateResponseSignature() {
  string response_sign = StringPrintf("NumURLs = %d\n",
                                      response_.payload_urls.size());

  for (size_t i = 0; i < response_.payload_urls.size(); i++)
    response_sign += StringPrintf("Url%d = %s\n",
                                  i, response_.payload_urls[i].c_str());

  response_sign += StringPrintf("Payload Size = %llu\n"
                                "Payload Sha256 Hash = %s\n"
                                "Metadata Size = %llu\n"
                                "Metadata Signature = %s\n"
                                "Is Delta Payload = %d\n"
                                "Max Failure Count Per Url = %d\n"
                                "Disable Payload Backoff = %d\n",
                                response_.size,
                                response_.hash.c_str(),
                                response_.metadata_size,
                                response_.metadata_signature.c_str(),
                                response_.is_delta_payload,
                                response_.max_failure_count_per_url,
                                response_.disable_payload_backoff);
  return response_sign;
}

void PayloadState::LoadResponseSignature() {
  CHECK(prefs_);
  string stored_value;
  if (prefs_->Exists(kPrefsCurrentResponseSignature) &&
      prefs_->GetString(kPrefsCurrentResponseSignature, &stored_value)) {
    SetResponseSignature(stored_value);
  }
}

void PayloadState::SetResponseSignature(const string& response_signature) {
  CHECK(prefs_);
  response_signature_ = response_signature;
  LOG(INFO) << "Current Response Signature = \n" << response_signature_;
  prefs_->SetString(kPrefsCurrentResponseSignature, response_signature_);
}

void PayloadState::LoadPayloadAttemptNumber() {
  SetPayloadAttemptNumber(GetPersistedValue(kPrefsPayloadAttemptNumber));
}

void PayloadState::SetPayloadAttemptNumber(uint32_t payload_attempt_number) {
  CHECK(prefs_);
  payload_attempt_number_ = payload_attempt_number;
  LOG(INFO) << "Payload Attempt Number = " << payload_attempt_number_;
  prefs_->SetInt64(kPrefsPayloadAttemptNumber, payload_attempt_number_);
}

void PayloadState::LoadUrlIndex() {
  SetUrlIndex(GetPersistedValue(kPrefsCurrentUrlIndex));
}

void PayloadState::SetUrlIndex(uint32_t url_index) {
  CHECK(prefs_);
  url_index_ = url_index;
  LOG(INFO) << "Current URL Index = " << url_index_;
  prefs_->SetInt64(kPrefsCurrentUrlIndex, url_index_);

  // Also update the download source, which is purely dependent on the
  // current URL index alone.
  UpdateCurrentDownloadSource();
}

void PayloadState::LoadUrlSwitchCount() {
  SetUrlSwitchCount(GetPersistedValue(kPrefsUrlSwitchCount));
}

void PayloadState::SetUrlSwitchCount(uint32_t url_switch_count) {
  CHECK(prefs_);
  url_switch_count_ = url_switch_count;
  LOG(INFO) << "URL Switch Count = " << url_switch_count_;
  prefs_->SetInt64(kPrefsUrlSwitchCount, url_switch_count_);
}

void PayloadState::LoadUrlFailureCount() {
  SetUrlFailureCount(GetPersistedValue(kPrefsCurrentUrlFailureCount));
}

void PayloadState::SetUrlFailureCount(uint32_t url_failure_count) {
  CHECK(prefs_);
  url_failure_count_ = url_failure_count;
  LOG(INFO) << "Current URL (Url" << GetUrlIndex()
            << ")'s Failure Count = " << url_failure_count_;
  prefs_->SetInt64(kPrefsCurrentUrlFailureCount, url_failure_count_);
}

void PayloadState::LoadBackoffExpiryTime() {
  CHECK(prefs_);
  int64_t stored_value;
  if (!prefs_->Exists(kPrefsBackoffExpiryTime))
    return;

  if (!prefs_->GetInt64(kPrefsBackoffExpiryTime, &stored_value))
    return;

  Time stored_time = Time::FromInternalValue(stored_value);
  if (stored_time > Time::Now() + TimeDelta::FromDays(kMaxBackoffDays)) {
    LOG(ERROR) << "Invalid backoff expiry time ("
               << utils::ToString(stored_time)
               << ") in persisted state. Resetting.";
    stored_time = Time();
  }
  SetBackoffExpiryTime(stored_time);
}

void PayloadState::SetBackoffExpiryTime(const Time& new_time) {
  CHECK(prefs_);
  backoff_expiry_time_ = new_time;
  LOG(INFO) << "Backoff Expiry Time = "
            << utils::ToString(backoff_expiry_time_);
  prefs_->SetInt64(kPrefsBackoffExpiryTime,
                   backoff_expiry_time_.ToInternalValue());
}

TimeDelta PayloadState::GetUpdateDuration() {
  Time end_time = update_timestamp_end_.is_null()
    ? system_state_->clock()->GetWallclockTime() :
      update_timestamp_end_;
  return end_time - update_timestamp_start_;
}

void PayloadState::LoadUpdateTimestampStart() {
  int64_t stored_value;
  Time stored_time;

  CHECK(prefs_);

  Time now = system_state_->clock()->GetWallclockTime();

  if (!prefs_->Exists(kPrefsUpdateTimestampStart)) {
    // The preference missing is not unexpected - in that case, just
    // use the current time as start time
    stored_time = now;
  } else if (!prefs_->GetInt64(kPrefsUpdateTimestampStart, &stored_value)) {
    LOG(ERROR) << "Invalid UpdateTimestampStart value. Resetting.";
    stored_time = now;
  } else {
    stored_time = Time::FromInternalValue(stored_value);
  }

  // Sanity check: If the time read from disk is in the future
  // (modulo some slack to account for possible NTP drift
  // adjustments), something is fishy and we should report and
  // reset.
  TimeDelta duration_according_to_stored_time = now - stored_time;
  if (duration_according_to_stored_time < -kDurationSlack) {
    LOG(ERROR) << "The UpdateTimestampStart value ("
               << utils::ToString(stored_time)
               << ") in persisted state is "
               << utils::FormatTimeDelta(duration_according_to_stored_time)
               << " in the future. Resetting.";
    stored_time = now;
  }

  SetUpdateTimestampStart(stored_time);
}

void PayloadState::SetUpdateTimestampStart(const Time& value) {
  CHECK(prefs_);
  update_timestamp_start_ = value;
  prefs_->SetInt64(kPrefsUpdateTimestampStart,
                   update_timestamp_start_.ToInternalValue());
  LOG(INFO) << "Update Timestamp Start = "
            << utils::ToString(update_timestamp_start_);
}

void PayloadState::SetUpdateTimestampEnd(const Time& value) {
  update_timestamp_end_ = value;
  LOG(INFO) << "Update Timestamp End = "
            << utils::ToString(update_timestamp_end_);
}

TimeDelta PayloadState::GetUpdateDurationUptime() {
  return update_duration_uptime_;
}

void PayloadState::LoadUpdateDurationUptime() {
  int64_t stored_value;
  TimeDelta stored_delta;

  CHECK(prefs_);

  if (!prefs_->Exists(kPrefsUpdateDurationUptime)) {
    // The preference missing is not unexpected - in that case, just
    // we'll use zero as the delta
  } else if (!prefs_->GetInt64(kPrefsUpdateDurationUptime, &stored_value)) {
    LOG(ERROR) << "Invalid UpdateDurationUptime value. Resetting.";
    stored_delta = TimeDelta::FromSeconds(0);
  } else {
    stored_delta = TimeDelta::FromInternalValue(stored_value);
  }

  // Sanity-check: Uptime can never be greater than the wall-clock
  // difference (modulo some slack). If it is, report and reset
  // to the wall-clock difference.
  TimeDelta diff = GetUpdateDuration() - stored_delta;
  if (diff < -kDurationSlack) {
    LOG(ERROR) << "The UpdateDurationUptime value ("
               << utils::FormatTimeDelta(stored_delta)
               << ") in persisted state is "
               << utils::FormatTimeDelta(diff)
               << " larger than the wall-clock delta. Resetting.";
    stored_delta = update_duration_current_;
  }

  SetUpdateDurationUptime(stored_delta);
}

void PayloadState::LoadNumReboots() {
  SetNumReboots(GetPersistedValue(kPrefsNumReboots));
}

void PayloadState::SetUpdateDurationUptimeExtended(const TimeDelta& value,
                                                   const Time& timestamp,
                                                   bool use_logging) {
  CHECK(prefs_);
  update_duration_uptime_ = value;
  update_duration_uptime_timestamp_ = timestamp;
  prefs_->SetInt64(kPrefsUpdateDurationUptime,
                   update_duration_uptime_.ToInternalValue());
  if (use_logging) {
    LOG(INFO) << "Update Duration Uptime = "
              << utils::FormatTimeDelta(update_duration_uptime_);
  }
}

void PayloadState::SetUpdateDurationUptime(const TimeDelta& value) {
  Time now = system_state_->clock()->GetMonotonicTime();
  SetUpdateDurationUptimeExtended(value, now, true);
}

void PayloadState::CalculateUpdateDurationUptime() {
  Time now = system_state_->clock()->GetMonotonicTime();
  TimeDelta uptime_since_last_update = now - update_duration_uptime_timestamp_;
  TimeDelta new_uptime = update_duration_uptime_ + uptime_since_last_update;
  // We're frequently called so avoid logging this write
  SetUpdateDurationUptimeExtended(new_uptime, now, false);
}

void PayloadState::ReportDurationMetrics() {
  TimeDelta duration = GetUpdateDuration();
  TimeDelta duration_uptime = GetUpdateDurationUptime();
  string metric;

  metric = "Installer.UpdateDurationMinutes";
  system_state_->metrics_lib()->SendToUMA(
       metric,
       static_cast<int>(duration.InMinutes()),
       1,             // min: 1 minute
       365*24*60,     // max: 1 year (approx)
       kNumDefaultUmaBuckets);
  LOG(INFO) << "Uploading " << utils::FormatTimeDelta(duration)
            << " for metric " <<  metric;

  metric = "Installer.UpdateDurationUptimeMinutes";
  system_state_->metrics_lib()->SendToUMA(
       metric,
       static_cast<int>(duration_uptime.InMinutes()),
       1,             // min: 1 minute
       30*24*60,      // max: 1 month (approx)
       kNumDefaultUmaBuckets);
  LOG(INFO) << "Uploading " << utils::FormatTimeDelta(duration_uptime)
            << " for metric " <<  metric;

  prefs_->Delete(kPrefsUpdateTimestampStart);
  prefs_->Delete(kPrefsUpdateDurationUptime);
}

string PayloadState::GetPrefsKey(const string& prefix, DownloadSource source) {
  return prefix + "-from-" + utils::ToString(source);
}

void PayloadState::LoadCurrentBytesDownloaded(DownloadSource source) {
  string key = GetPrefsKey(kPrefsCurrentBytesDownloaded, source);
  SetCurrentBytesDownloaded(source, GetPersistedValue(key), true);
}

void PayloadState::SetCurrentBytesDownloaded(
    DownloadSource source,
    uint64_t current_bytes_downloaded,
    bool log) {
  CHECK(prefs_);

  if (source >= kNumDownloadSources)
    return;

  // Update the in-memory value.
  current_bytes_downloaded_[source] = current_bytes_downloaded;

  string prefs_key = GetPrefsKey(kPrefsCurrentBytesDownloaded, source);
  prefs_->SetInt64(prefs_key, current_bytes_downloaded);
  LOG_IF(INFO, log) << "Current bytes downloaded for "
                    << utils::ToString(source) << " = "
                    << GetCurrentBytesDownloaded(source);
}

void PayloadState::LoadTotalBytesDownloaded(DownloadSource source) {
  string key = GetPrefsKey(kPrefsTotalBytesDownloaded, source);
  SetTotalBytesDownloaded(source, GetPersistedValue(key), true);
}

void PayloadState::SetTotalBytesDownloaded(
    DownloadSource source,
    uint64_t total_bytes_downloaded,
    bool log) {
  CHECK(prefs_);

  if (source >= kNumDownloadSources)
    return;

  // Update the in-memory value.
  total_bytes_downloaded_[source] = total_bytes_downloaded;

  // Persist.
  string prefs_key = GetPrefsKey(kPrefsTotalBytesDownloaded, source);
  prefs_->SetInt64(prefs_key, total_bytes_downloaded);
  LOG_IF(INFO, log) << "Total bytes downloaded for "
                    << utils::ToString(source) << " = "
                    << GetTotalBytesDownloaded(source);
}

}  // namespace chromeos_update_engine
