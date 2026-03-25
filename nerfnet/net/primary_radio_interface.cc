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
      poll_fail_count_(0),
      state_(CoordinatorState::ResetSync),
      connection_reset_required_(true),
      peer_has_pending_(false),
      last_tx_was_data_(false) {
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
      if (!ConnectionReset()) {
        LOGE("[COORD] connection reset failed");
        HandleTransactionFailure();
      } else {
        LOGI("[COORD] connection reset success");
        connection_reset_required_ = false;
        state_ = CoordinatorState::Idle;
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
  }

  peer_has_pending_ = false;
  last_tx_was_data_ = false;

  MacFrame tx;
  tx.type = FrameType::Reset;
  tx.seq = 0;
  tx.ack = 0;
  tx.pending = 0;
  tx.arg = 0;

  std::vector<uint8_t> request;
  if (!EncodeMacFrame(tx, request)) {
    return false;
  }

  auto result = Send(request);
  if (result != RequestResult::Success) {
    return false;
  }

  std::vector<uint8_t> response(kMaxPacketSize, 0x00);
  result = Receive(response, /*timeout_us=*/100000);
  if (result != RequestResult::Success) {
    return false;
  }

  MacFrame rx;
  if (!DecodeMacFrame(response, rx)) {
    return false;
  }

  return rx.type == FrameType::Reset;
}

bool PrimaryRadioInterface::ChooseCoordinatorTxFrame(MacFrame& tx) {
  tx = {};
  tx.ack = last_rx_seq_.value_or(kNoSeq);

  bool local_has_data = false;
  {
    std::lock_guard<std::mutex> lock(read_buffer_mutex_);
    local_has_data = tx_in_flight_ || !read_buffer_.empty();
  }

  switch (state_) {
    case CoordinatorState::ResetSync:
      tx.type = FrameType::Reset;
      tx.pending = 0;
      tx.arg = 0;
      last_tx_was_data_ = false;
      return true;

    case CoordinatorState::IssueGrant:
      tx.type = FrameType::Grant;
      tx.pending = local_has_data ? 1 : 0;
      tx.arg = kDefaultBurstGrant;
      last_tx_was_data_ = false;
      return true;

    case CoordinatorState::Idle:
      if (local_has_data) {
        std::lock_guard<std::mutex> lock(read_buffer_mutex_);
        if (!BuildNextDataFrameLocked(tx)) {
          return false;
        }
        tx.pending = 1;
        last_tx_was_data_ = true;
        return true;
      }

      tx.type = FrameType::Pending;
      tx.pending = 0;
      tx.arg = 0;
      last_tx_was_data_ = false;
      return true;
  }

  return false;
}

bool PrimaryRadioInterface::ApplyPeerResponse(const MacFrame& rx) {
  {
    std::lock_guard<std::mutex> lock(read_buffer_mutex_);
    CommitAckLocked(rx.ack);

    if (rx.type == FrameType::Data) {
      if (!ConsumeDataFrameLocked(rx)) {
        LOGE("[COORD] failed to consume peer DATA");
        return false;
      }
    }
  }

  peer_has_pending_ = (rx.pending != 0);

  switch (rx.type) {
    case FrameType::Ack:
      state_ = peer_has_pending_ ? CoordinatorState::IssueGrant
                                 : CoordinatorState::Idle;
      return true;

    case FrameType::Pending:
      state_ = CoordinatorState::IssueGrant;
      return true;

    case FrameType::Data:
      state_ = peer_has_pending_ ? CoordinatorState::IssueGrant
                                 : CoordinatorState::Idle;
      return true;

    case FrameType::Reset:
      LOGE("[COORD] unexpected RESET from peer");
      return false;

    case FrameType::Grant:
      LOGE("[COORD] unexpected GRANT from peer");
      return false;

    default:
      return false;
  }
}

bool PrimaryRadioInterface::PerformExchange() {
  MacFrame tx;
  if (!ChooseCoordinatorTxFrame(tx)) {
    return false;
  }

  LogCoordinatorTx(tx);

  std::vector<uint8_t> request;
  if (!EncodeMacFrame(tx, request)) {
    return false;
  }

  auto result = Send(request);
  if (result != RequestResult::Success) {
    LOGE("[COORD] send failed");
    return false;
  }

  std::vector<uint8_t> response(kMaxPacketSize, 0x00);
  result = Receive(response, /*timeout_us=*/100000);
  if (result != RequestResult::Success) {
    LOGE("[COORD] receive failed");
    return false;
  }

  MacFrame rx;
  if (!DecodeMacFrame(response, rx)) {
    LOGE("[COORD] decode failed");
    return false;
  }

  LogCoordinatorRx(rx);

  return ApplyPeerResponse(rx);
}

void PrimaryRadioInterface::HandleTransactionFailure() {
  ++poll_fail_count_;
  if (poll_fail_count_ > 10) {
    connection_reset_required_ = true;
    state_ = CoordinatorState::ResetSync;
  } else {
    current_poll_interval_us_ *= 2;
    if (current_poll_interval_us_ > 1000000) {
      current_poll_interval_us_ = 1000000;
    }
  }
}

void PrimaryRadioInterface::LogCoordinatorTx(const MacFrame& tx) {
  LOGI("[COORD][TX] type=%u seq=%u ack=%u pending=%u arg=%u payload=%zu state=%d",
       static_cast<unsigned>(tx.type),
       tx.seq,
       tx.ack,
       tx.pending,
       tx.arg,
       tx.payload.size(),
       static_cast<int>(state_));
}

void PrimaryRadioInterface::LogCoordinatorRx(const MacFrame& rx) {
  LOGI("[COORD][RX] type=%u seq=%u ack=%u pending=%u arg=%u payload=%zu",
       static_cast<unsigned>(rx.type),
       rx.seq,
       rx.ack,
       rx.pending,
       rx.arg,
       rx.payload.size());
}

}  // namespace nerfnet