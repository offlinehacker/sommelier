// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sms_message.h"

#include <glog/logging.h>

#include "utilities.h"

static const uint8_t kMsgTypeMask          = 0x03;
static const uint8_t kMsgTypeDeliver       = 0x00;

static const uint8_t kTypeOfAddrNumMask    = 0x70;
static const uint8_t kTypeOfAddrNumIntl    = 0x10;
static const uint8_t kTypeOfAddrNumAlpha   = 0x50;
static const uint8_t kTypeOfAddrIntlE164   = 0x91;

static const uint8_t kDataCodingSchemeMask = 0xec;
static const uint8_t kDataCodingSchemeGsm7 = 0x00;
static const uint8_t kDataCodingSchemeUcs2 = 0x08;

static const uint8_t kSmscTimestampLen = 7;
static const size_t  kMinPduLen = 7 + kSmscTimestampLen;

static char NibbleToChar(uint8_t nibble) {
  switch (nibble) {
    case 0: return '0';
    case 1: return '1';
    case 2: return '2';
    case 3: return '3';
    case 4: return '4';
    case 5: return '5';
    case 6: return '6';
    case 7: return '7';
    case 8: return '8';
    case 9: return '9';
    case 10: return '*';
    case 11: return '#';
    case 12: return 'a';
    case 13: return 'b';
    case 14: return 'c';
    case 0xff: return '\0';  // padding nibble
  }
  return '\0';
}

// Convert an array of octets into a BCD string. Each octet consists
// of two nibbles which are converted to hex characters. Those hex
// characters are the digits of the BCD string. The lower nibble is
// the more significant digit.
static std::string SemiOctetsToBcdString(const uint8_t* octets,
                                         int num_octets) {
  std::string bcd;

  for (int i = 0; i < num_octets; ++i) {
    char first = NibbleToChar(octets[i] & 0xf);
    char second = NibbleToChar((octets[i] >> 4) & 0xf);

    if (first != '\0')
      bcd += first;
    if (second != '\0')
      bcd += second;
  }
  return bcd;
}

SmsMessage* SmsMessage::CreateMessage(const uint8_t* pdu, size_t pdu_len) {
  // Make sure the PDU is of a valid size
  if (pdu_len < kMinPduLen) {
    LOG(INFO) << "PDU too short: " << pdu_len << " vs. " << kMinPduLen;
    return NULL;
  }

  // Format of message:
  //
  //  1 octet  - length of SMSC information in octets, including type field
  //  1 octet  - type of address of SMSC (value 0x91 is international E.164)
  //  variable - SMSC address
  //  1 octet  - first octet of SMS-DELIVER (value = 0x04)
  //  1 octet  - length of sender address in decimal digits (semi-octets)
  //  1 octet  - type of sender address (value 0x91 is international E.164)
  //  variable - sender address
  //  1 octet  - protocol identifier (value = 0)
  //  1 octet  - data coding scheme (value 0 is GSM7, 8 is UCS2)
  //  7 octets - SMSC timestamp
  //  1 octet  - user data length in septets
  //  variable - user data (body of message)

  // Do a bunch of validity tests first so we can bail out early
  // if we're not able to handle the PDU. We first check the validity
  // of all length fields and make sure the PDU length is consistent
  // with those values.

  uint8_t smsc_addr_num_octets = pdu[0];
  uint8_t variable_length_items = smsc_addr_num_octets;

  if (pdu_len < variable_length_items + kMinPduLen) {
    LOG(INFO) << "PDU too short: " << pdu_len << " vs. "
              << variable_length_items + kMinPduLen;
    return NULL;
  }

  // where in the PDU the actual SMS protocol message begins
  uint8_t msg_start_offset = 1 + smsc_addr_num_octets;
  uint8_t sender_addr_num_digits = pdu[msg_start_offset + 1];
  // round the sender address length up to an even number of semi-octets,
  // and thus an integral number of octets
  uint8_t sender_addr_num_octets = (sender_addr_num_digits + 1) >> 1;
  variable_length_items += sender_addr_num_octets;
  if (pdu_len < variable_length_items + kMinPduLen) {
    LOG(INFO) << "PDU too short: " << pdu_len << " vs. "
              << variable_length_items + kMinPduLen;
    return NULL;
  }

  uint8_t tp_pid_offset = msg_start_offset + 3 + sender_addr_num_octets;
  uint8_t user_data_offset = tp_pid_offset + 2 + kSmscTimestampLen;
  uint8_t user_data_num_septets = pdu[user_data_offset];
  variable_length_items += (7 * (user_data_num_septets + 1 )) / 8;

  if (pdu_len < variable_length_items + kMinPduLen) {
    LOG(INFO) << "PDU too short: " << pdu_len << " vs. "
              << variable_length_items + kMinPduLen;
    return NULL;
  }

  // smsc number format must be international, E.164
  if (pdu[1] != kTypeOfAddrIntlE164) {
    LOG(INFO) << "Invalid SMSC address format: "
              << std::hex << (int)pdu[1] << " vs. "
              << std::hex << kTypeOfAddrIntlE164;
    return NULL;
  }

  // we only handle SMS-DELIVER messages
  if ((pdu[msg_start_offset] & kMsgTypeMask) != kMsgTypeDeliver) {
    LOG(INFO) << "Unhandled message type: "
              << std::hex << (int)(pdu[msg_start_offset] & kMsgTypeMask)
              << " vs. " << std::hex << kMsgTypeDeliver;
    return NULL;
  }

  uint8_t sender_addr_type = pdu[msg_start_offset + 2];

  // for data coding scheme, we only handle GSM7 and UCS2
  uint8_t dcs = pdu[tp_pid_offset+1] & kDataCodingSchemeMask;
  if (dcs != kDataCodingSchemeGsm7 && dcs != kDataCodingSchemeUcs2) {
    LOG(INFO) << "Unhandled data coding scheme: " << std::hex
              << (int)pdu[tp_pid_offset+1];
    return NULL;
  }

  std::string smsc_addr = SemiOctetsToBcdString(&pdu[2],
                                                smsc_addr_num_octets-1);
  std::string sender_addr;
  if ((sender_addr_type & kTypeOfAddrNumMask) != kTypeOfAddrNumAlpha) {
    sender_addr = SemiOctetsToBcdString(&pdu[msg_start_offset+3],
                                        sender_addr_num_octets);
    if ((sender_addr_type & kTypeOfAddrNumMask) == kTypeOfAddrNumIntl) {
      sender_addr = "+" + sender_addr;
    }
  } else {
    uint8_t *bytes = new uint8_t[sender_addr_num_octets+1];
    bytes[0] = (sender_addr_num_digits * 4) / 7;
    memcpy(&bytes[1], &pdu[msg_start_offset+3], sender_addr_num_octets);
    sender_addr = utilities::Gsm7ToUtf8String(bytes);
    delete [] bytes;
  }

  std::string sc_timestamp = SemiOctetsToBcdString(&pdu[tp_pid_offset+2],
                                                   kSmscTimestampLen-1);
  std::string msg_text;
  if (dcs == kDataCodingSchemeGsm7)
    msg_text = utilities::Gsm7ToUtf8String(&pdu[user_data_offset]);
  else
    msg_text = utilities::Ucs2ToUtf8String(&pdu[user_data_offset]);

  // The last two semi-octets of the timestamp indicate an offset from
  // GMT, and are handled differently than the first 12 semi-octets.
  uint8_t toff_octet = pdu[tp_pid_offset+1+kSmscTimestampLen];
  sc_timestamp += (toff_octet & 0x8) ? '-' : '+';
  uint8_t offset_in_hours =
      ((toff_octet & 0x7) << 4 | (toff_octet & 0xf0) >> 4) / 4;
  sc_timestamp += (char)((offset_in_hours / 10) + '0');
  sc_timestamp += (char)((offset_in_hours % 10) + '0');

  return new SmsMessage(smsc_addr, sender_addr, sc_timestamp, msg_text);
}
