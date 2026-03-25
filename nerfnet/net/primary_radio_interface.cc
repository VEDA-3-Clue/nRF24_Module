/*
 * Copyright 2020 Andrew Rossignol andrew.rossignol@gmail.com
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "nerfnet/net/primary_radio_interface.h"

#include <algorithm>
#include <vector>

#include "nerfnet/util/log.h"
#include "nerfnet/util/time.h"

namespace nerfnet {

PrimaryRadioInterface::PrimaryRadioInterface(uint16_t ce_pin,
                                             int tunnel_fd,
                                             uint32_t primary_addr,
                                             uint32_t secondary_addr,
                                             uint8_t channel,
                                             uint64_t poll_interval_us)
    : RadioInterface(ce_pin, tunnel_fd, primary_addr, secondary_addr, channel),
      poll_interval_us_(poll_interval_us),
      current_poll_interval_us_(poll_interval_us),
      connection_reset_required_(true) {
  uint8_t writing_addr[5] = {
      static_cast<uint8_t>(primary_addr),
      static_cast<uint8_t>(primary_addr >> 8),
      static_cast<uint8_t>(primary_addr >> 16),
      static_cast<uint8_t>(primary_addr >> 24),
      0,
  };
  uint8_t reading_addr[5] = {
      static_cast<uint8_t>(secondary_addr),
      static_cast<uint8_t>(secondary_addr >> 8),
      static_cast<uint8_t>(secondary_addr >> 16),
      static_cast<uint8_t>(secondary_addr >> 24),
      0,
  };
  radio_.openWritingPipe(writing_addr);
  radio_.openReadingPipe(kPipeId, reading_addr);
}

void PrimaryRadioInterface::Run() {
  while (true) {
    SleepUs(current_poll_interval_us_);

    if (connection_reset_required_) {
      LOGI("Resetting coordinator/peer session");
      if (!ConnectionReset()) {
        LOGE("Connection reset failed");
        HandleTransactionFailure();
      } else {
        LOGI("Connection reset success");
        connection_reset_required_ = false;
        poll_fail_count_ = 0;
        current_poll_interval_us_ = poll_interval_us_;
      }
      continue;
    }

    if (PerformExchange()) {
      poll_fail_count_ = 0;
      current_poll_interval_us_ = poll_interval_us_;
    } else {
      HandleTransactionFailure();
    }
  }
}

bool PrimaryRadioInterface::ConnectionReset() {
  {
    std::lock_guard<std::mutex> lock(read_buffer_mutex_);
    next_tx_seq_ = 1;
    tx_in_flight_ = false;
    tx_inflight_seq_ = 0;
    tx_inflight_bytes_left_ = 0;
    tx_inflight_payload_.clear();
    last_rx_seq_.reset();
    frame_buffer_.clear();
    peer_pending_hint_ = 0;
    peer_grant_remaining_ = 0;
  }

  MacFrame reset;
  reset.type = FrameType::Reset;

  std::vector<uint8_t> request;
  if (!EncodeMacFrame(reset, request)) {
    return false;
  }

  auto result = Send(request);
  if (result != RequestResult::Success) {
    LOGE("Failed to send RESET");
    return false;
  }

  std::vector<uint8_t> response(kMaxPacketSize, 0x00);
  result = Receive(response, /*timeout_us=*/100000);
  if (result != RequestResult::Success) {
    LOGE("Failed to receive RESET ACK");
    return false;
  }

  MacFrame rx;
  if (!DecodeMacFrame(response, rx)) {
    return false;
  }

  return rx.type == FrameType::Reset;
}

bool PrimaryRadioInterface::PerformExchange() {
  MacFrame tx;

  {
    std::lock_guard<std::mutex> lock(read_buffer_mutex_);

    if (peer_grant_remaining_ > 0) {
      tx.type = FrameType::Grant;
      tx.ack = last_rx_seq_.value_or(kNoSeq);
      tx.pending = PendingHintLocked();
      tx.arg = peer_grant_remaining_;
    } else if (tx_in_flight_ || !read_buffer_.empty()) {
      if (!BuildNextDataFrameLocked(tx)) {
        return false;
      }
    } else if (peer_pending_hint_ > 0) {
      peer_grant_remaining_ =
          std::min<uint8_t>(kDefaultBurstGrant, peer_pending_hint_);

      tx.type = FrameType::Grant;
      tx.ack = last_rx_seq_.value_or(kNoSeq);
      tx.pending = PendingHintLocked();
      tx.arg = peer_grant_remaining_;
    } else {
      tx.type = FrameType::Pending;
      tx.ack = last_rx_seq_.value_or(kNoSeq);
      tx.pending = PendingHintLocked();
      tx.arg = 0;
    }
  }

  std::vector<uint8_t> request;
  if (!EncodeMacFrame(tx, request)) {
    return false;
  }

  auto result = Send(request);
  if (result != RequestResult::Success) {
    LOGE("Failed to send coordinator frame");
    return false;
  }

  std::vector<uint8_t> response(kMaxPacketSize, 0x00);
  result = Receive(response, /*timeout_us=*/100000);
  if (result != RequestResult::Success) {
    LOGE("Failed to receive peer frame");
    return false;
  }

  MacFrame rx;
  if (!DecodeMacFrame(response, rx)) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(read_buffer_mutex_);

    CommitAckLocked(rx.ack);
    peer_pending_hint_ = rx.pending;

    switch (rx.type) {
      case FrameType::Data:
        if (!ConsumeDataFrameLocked(rx)) {
          return false;
        }
        if (peer_grant_remaining_ > 0) {
          --peer_grant_remaining_;
        }
        break;

      case FrameType::Pending:
        // Peer has data, but no grant yet.
        break;

      case FrameType::Ack:
        // Pure ack; no extra action needed.
        peer_grant_remaining_ = 0;
        break;

      case FrameType::Grant:
        // Peer should not grant coordinator in this minimal protocol.
        LOGE("Unexpected GRANT from peer");
        return false;

      case FrameType::Reset:
        LOGE("Unexpected RESET from peer");
        return false;

      default:
        return false;
    }
  }

  return true;
}

void PrimaryRadioInterface::HandleTransactionFailure() {
  ++poll_fail_count_;
  if (poll_fail_count_ > 10) {
    if (current_poll_interval_us_ < 1000000) {
      current_poll_interval_us_ *= 2;
    } else {
      connection_reset_required_ = true;
    }
  }
}

}  // namespace nerfnet
