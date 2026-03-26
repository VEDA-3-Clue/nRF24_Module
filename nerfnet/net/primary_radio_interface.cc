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
                                             uint64_t poll_interval_us,
                                             const RadioConfig& radio_config,
                                             int irq_pin)
    : RadioInterface(ce_pin,
                     tunnel_fd,
                     primary_addr,
                     secondary_addr,
                     channel,
                     radio_config,
                     irq_pin),
      poll_interval_us_(poll_interval_us),
      current_poll_interval_us_(poll_interval_us),
      poll_fail_count_(0),
      state_(CoordinatorState::ResetSync),
      connection_reset_required_(true),
      peer_has_pending_(false),
      last_tx_was_data_(false),
      peer_grant_budget_(0),
      last_stat_print_us_ (0) {
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
    const uint64_t now_us = TimeNowUs();

    if (disconnected_) {
      if (now_us < next_retry_time_us_) {
        SleepUs(1000);  // 1ms 정도로 짧게 자면서 대기
        continue;
      }

      if (!ConnectionReset()) {
        ++reset_fail_log_counter_;
        if ((reset_fail_log_counter_ % 100) == 1) {
          LOGE("[COORD] connection reset failed (count=%llu, backoff_us=%llu)",
               static_cast<unsigned long long>(reset_fail_log_counter_),
               static_cast<unsigned long long>(disconnect_backoff_us_));
        }

        next_retry_time_us_ = TimeNowUs() + disconnect_backoff_us_;
        disconnect_backoff_us_ *= 2;
        if (disconnect_backoff_us_ > kMaxDisconnectBackoffUs) {
          disconnect_backoff_us_ = kMaxDisconnectBackoffUs;
        }
      } else {
        LOGI("[COORD] connection reset success");
        disconnected_ = false;
        connection_reset_required_ = false;
        poll_fail_count_ = 0;
        send_fail_streak_ = 0;
        timeout_fail_streak_ = 0;
        disconnect_backoff_us_ = kInitialDisconnectBackoffUs;
        next_retry_time_us_ = 0;
        state_ = CoordinatorState::Idle;
      }
      continue;
    }

    if (state_ == CoordinatorState::Idle && !peer_has_pending_) {
      current_poll_interval_us_ = kIdlePollIntervalUs;
    } else {
      current_poll_interval_us_ = kActivePollIntervalUs;
    }

    SleepUs(current_poll_interval_us_);

    if (connection_reset_required_) {
      if (!ConnectionReset()) {
        ++reset_fail_log_counter_;
        if ((reset_fail_log_counter_ % 100) == 1) {
          LOGE("[COORD] connection reset failed (count=%llu)",
               static_cast<unsigned long long>(reset_fail_log_counter_));
        }
        HandleTransactionFailure(FailureType::Protocol);
      } else {
        LOGI("[COORD] connection reset success");
        connection_reset_required_ = false;
        state_ = CoordinatorState::Idle;
        poll_fail_count_ = 0;
        send_fail_streak_ = 0;
        timeout_fail_streak_ = 0;
        disconnected_ = false;
        disconnect_backoff_us_ = kInitialDisconnectBackoffUs;
        next_retry_time_us_ = 0;
      }
      continue;
    }

    switch (PerformExchange()) {
      case ExchangeResult::Success:
        poll_fail_count_ = 0;
        send_fail_streak_ = 0;
        timeout_fail_streak_ = 0;
        disconnected_ = false;
        disconnect_backoff_us_ = kInitialDisconnectBackoffUs;
        next_retry_time_us_ = 0;
        break;
      case ExchangeResult::SendFailure:
        HandleTransactionFailure(FailureType::Send);
        break;
      case ExchangeResult::TimeoutFailure:
        HandleTransactionFailure(FailureType::Timeout);
        break;
      case ExchangeResult::ProtocolFailure:
        HandleTransactionFailure(FailureType::Protocol);
        break;
    }

    const uint64_t stat_now_us = TimeNowUs();
    if (last_stat_print_us_ == 0) {
      last_stat_print_us_ = stat_now_us;
    }

    if (stat_now_us - last_stat_print_us_ >= 1000000) {
      LOGI("[COORD][STAT] pend_tx=%llu grant_tx=%llu data_tx=%llu "
           "ack_rx=%llu pend_rx=%llu data_rx=%llu "
           "send_fail=%llu sf_pend=%llu sf_grant=%llu sf_data=%llu sf_ack=%llu sf_reset=%llu "
           "rx_timeout=%llu disconnected=%u fail_streak=%d send_streak=%d timeout_streak=%d backoff_us=%llu",
           static_cast<unsigned long long>(tx_pending_count_),
           static_cast<unsigned long long>(tx_grant_count_),
           static_cast<unsigned long long>(tx_data_count_),
           static_cast<unsigned long long>(rx_ack_count_),
           static_cast<unsigned long long>(rx_pending_count_),
           static_cast<unsigned long long>(rx_data_count_),
           static_cast<unsigned long long>(tx_send_fail_count_),
           static_cast<unsigned long long>(tx_send_fail_pending_count_),
           static_cast<unsigned long long>(tx_send_fail_grant_count_),
           static_cast<unsigned long long>(tx_send_fail_data_count_),
           static_cast<unsigned long long>(tx_send_fail_ack_count_),
           static_cast<unsigned long long>(tx_send_fail_reset_count_),
           static_cast<unsigned long long>(rx_timeout_count_),
           disconnected_ ? 1u : 0u,
           poll_fail_count_,
           send_fail_streak_,
           timeout_fail_streak_,
           static_cast<unsigned long long>(disconnect_backoff_us_));

      last_stat_print_us_ = stat_now_us;
    }
  }
}

bool PrimaryRadioInterface::ConnectionReset() {
  radio_.flush_rx();
  radio_.flush_tx();

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
  peer_grant_budget_ = 0;
  last_tx_was_data_ = false;
  consecutive_local_data_frames_ = 0;

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

  if (rx.type != FrameType::Reset) {
    return false;
  }

  radio_.flush_rx();
  radio_.flush_tx();
  return true;
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
    case CoordinatorState::ActiveRx:
      tx.type = FrameType::Grant;
      tx.pending = local_has_data ? 1 : 0;
      tx.arg = kDefaultBurstGrant;
      last_tx_was_data_ = false;
      ++tx_grant_count_;
      return true;

    case CoordinatorState::Idle:
      if (local_has_data) {
        if (peer_has_pending_ && consecutive_local_data_frames_ >= kFairDataBurstLimit) {
          tx.type = FrameType::Grant;
          tx.pending = 1;
          tx.arg = kDefaultBurstGrant;
          last_tx_was_data_ = false;
          ++tx_grant_count_;
          return true;
        }

        std::lock_guard<std::mutex> lock(read_buffer_mutex_);
        if (!BuildNextDataFrameLocked(tx)) {
          return false;
        }
        tx.pending = 1;
        last_tx_was_data_ = true;
        ++tx_data_count_;
        return true;
      }

      tx.type = FrameType::Pending;
      tx.pending = 0;
      tx.arg = 0;
      last_tx_was_data_ = false;
      ++tx_pending_count_;
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
      ++rx_ack_count_;
      if (peer_has_pending_) {
        state_ = CoordinatorState::IssueGrant;
      } else {
        state_ = CoordinatorState::Idle;
      }
      return true;

    case FrameType::Pending:
      ++rx_pending_count_;
      state_ = CoordinatorState::IssueGrant;
      return true;

    case FrameType::Data:
      ++rx_data_count_;
      if (peer_has_pending_) {
        state_ = CoordinatorState::ActiveRx;
      } else {
        state_ = CoordinatorState::Idle;
      }
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

PrimaryRadioInterface::ExchangeResult PrimaryRadioInterface::PerformExchange() {
  MacFrame tx;
  if (!ChooseCoordinatorTxFrame(tx)) {
    return ExchangeResult::ProtocolFailure;
  }

  std::vector<uint8_t> request;
  if (!EncodeMacFrame(tx, request)) {
    return ExchangeResult::ProtocolFailure;
  }

  auto result = Send(request);
  if (result != RequestResult::Success) {
    ++tx_send_fail_count_;
    RecordSendFailure(tx.type);
    ++fail_log_counter_;
    if ((fail_log_counter_ % 100) == 1) {
      LOGE("[COORD] send failed");
    }
    return ExchangeResult::SendFailure;
  }

  std::vector<uint8_t> response(kMaxPacketSize, 0x00);
  result = Receive(response, /*timeout_us=*/100000);
  if (result != RequestResult::Success) {
    ++rx_timeout_count_;
    ++fail_log_counter_;
    if ((fail_log_counter_ % 100) == 1) {
      LOGE("[COORD] receive failed");
    }
    return ExchangeResult::TimeoutFailure;
  }

  MacFrame rx;
  if (!DecodeMacFrame(response, rx)) {
    LOGE("[COORD] decode failed");
    return ExchangeResult::ProtocolFailure;
  }

  bool is_idle_pair =
    (tx.type == FrameType::Pending && rx.type == FrameType::Ack);

  if (!is_idle_pair || (idle_log_counter_++ % 100 == 0)) {
    LOGI("[COORD][PAIR] TX(type=%u seq=%u ack=%u pending=%u arg=%u payload=%zu) "
        "RX(type=%u seq=%u ack=%u pending=%u arg=%u payload=%zu) state=%d",
        static_cast<unsigned>(tx.type),
        tx.seq,
        tx.ack,
        tx.pending,
        tx.arg,
        tx.payload.size(),
        static_cast<unsigned>(rx.type),
        rx.seq,
        rx.ack,
        rx.pending,
        rx.arg,
        rx.payload.size(),
        static_cast<int>(state_));
  }

  const bool applied = ApplyPeerResponse(rx);
  if (applied) {
    if (tx.type == FrameType::Data && rx.type != FrameType::Data) {
      consecutive_local_data_frames_ =
          static_cast<uint8_t>(std::min<unsigned>(consecutive_local_data_frames_ + 1, 255));
    } else if (tx.type != FrameType::Data || rx.type == FrameType::Data) {
      consecutive_local_data_frames_ = 0;
    }
  }

  if (!applied) {
    consecutive_local_data_frames_ = 0;
  }

  if (applied && (tx.type == FrameType::Data || rx.type == FrameType::Data)) {
    SleepUs(kPostDataExchangeGapUs);
  }

  return applied ? ExchangeResult::Success
                 : ExchangeResult::ProtocolFailure;
}

void PrimaryRadioInterface::RecordSendFailure(FrameType frame_type) {
  switch (frame_type) {
    case FrameType::Pending:
      ++tx_send_fail_pending_count_;
      break;
    case FrameType::Grant:
      ++tx_send_fail_grant_count_;
      break;
    case FrameType::Data:
      ++tx_send_fail_data_count_;
      break;
    case FrameType::Ack:
      ++tx_send_fail_ack_count_;
      break;
    case FrameType::Reset:
      ++tx_send_fail_reset_count_;
      break;
    default:
      break;
  }
}

void PrimaryRadioInterface::HandleTransactionFailure(FailureType failure_type) {
  switch (failure_type) {
    case FailureType::Send:
      ++send_fail_streak_;
      timeout_fail_streak_ = 0;
      break;
    case FailureType::Timeout:
      ++timeout_fail_streak_;
      send_fail_streak_ = 0;
      break;
    case FailureType::Protocol:
      ++send_fail_streak_;
      ++timeout_fail_streak_;
      break;
  }

  const int combined_fail_streak = send_fail_streak_ + timeout_fail_streak_;
  poll_fail_count_ = combined_fail_streak;

  const bool disconnect_now =
      send_fail_streak_ >= kDisconnectFailureThreshold ||
      timeout_fail_streak_ >= (kDisconnectFailureThreshold + 2) ||
      combined_fail_streak >= (kDisconnectFailureThreshold + 3);

  if (disconnect_now) {
    disconnected_ = true;
    connection_reset_required_ = true;
    state_ = CoordinatorState::ResetSync;
    next_retry_time_us_ = TimeNowUs() + disconnect_backoff_us_;
    return;
  }

  current_poll_interval_us_ *= 2;
  if (current_poll_interval_us_ > 1000000) {
    current_poll_interval_us_ = 1000000;
  }
}

}  // namespace nerfnet
