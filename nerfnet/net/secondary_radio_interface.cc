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

#include "nerfnet/net/secondary_radio_interface.h"

#include <vector>

#include "nerfnet/util/log.h"
#include "nerfnet/util/time.h"

#define read_buffer_mutex_ mac_state_->read_buffer_mutex
#define read_buffer_ mac_state_->read_buffer
#define frame_buffer_ mac_state_->frame_buffer
#define next_tx_seq_ mac_state_->next_tx_seq
#define last_rx_seq_ mac_state_->last_rx_seq

namespace nerfnet {

SecondaryRadioInterface::SecondaryRadioInterface(uint16_t ce_pin,
                                                 uint16_t csn_pin,
                                                 int tunnel_fd,
                                                 uint32_t primary_addr,
                                                 uint32_t secondary_addr,
                                                 uint8_t channel,
                                                 const RadioConfig& radio_config,
                                                 int irq_pin,
                                                 std::shared_ptr<RadioInterface::SharedMacState> shared_mac_state)
    : RadioInterface(ce_pin,
                     csn_pin,
                     tunnel_fd,
                     primary_addr,
                     secondary_addr,
                     channel,
                     radio_config,
                     irq_pin,
                     std::move(shared_mac_state)),
      granted_to_send_(false), grant_budget_(0) {
  uint8_t writing_addr[5] = {
      static_cast<uint8_t>(secondary_addr),
      static_cast<uint8_t>(secondary_addr >> 8),
      static_cast<uint8_t>(secondary_addr >> 16),
      static_cast<uint8_t>(secondary_addr >> 24),
      0,
  };
  uint8_t reading_addr[5] = {
      static_cast<uint8_t>(primary_addr),
      static_cast<uint8_t>(primary_addr >> 8),
      static_cast<uint8_t>(primary_addr >> 16),
      static_cast<uint8_t>(primary_addr >> 24),
      0,
  };

  radio_.openWritingPipe(writing_addr);
  radio_.openReadingPipe(kPipeId, reading_addr);
}

void SecondaryRadioInterface::Run() {
  while (true) {
    std::vector<uint8_t> request(kMaxPacketSize, 0x00);
    auto result = Receive(request);
    if (result != RequestResult::Success) {
      continue;
    }

    MacFrame rx;
    if (!DecodeMacFrame(request, rx)) {
      continue;
    }

    RecordRx(rx);

    if (rx.type == FrameType::Reset) {
      if (!HandleReset()) {
        LOGE("[PEER] reset handling failed");
      }
      continue;
    }

    if (!ApplyCoordinatorRequest(rx)) {
      LOGE("[PEER] failed to apply coordinator request");
      continue;
    }

    MacFrame tx;
    if (!ChoosePeerResponse(tx)) {
      LOGE("[PEER] failed to choose response");
      continue;
    }

    std::vector<uint8_t> response;
    if (!EncodeMacFrame(tx, response)) {
      LOGE("[PEER] encode failed");
      continue;
    }

    result = Send(response);
    if (result != RequestResult::Success) {
      ++tx_send_fail_count_;
      RecordSendFailure(tx.type);
      LOGE("[PEER] send failed");
    }

    RecordTx(tx);

    bool is_idle_pair =
    (rx.type == FrameType::Pending && tx.type == FrameType::Ack);

    if (!is_idle_pair || (idle_log_counter_++ % 100 == 0)) {
      LogPeerRx(rx);
      LogPeerTx(tx);
    }

    const uint64_t stat_now_us = TimeNowUs();
    if (last_stat_print_us_ == 0) {
      last_stat_print_us_ = stat_now_us;
    }
    if (stat_now_us - last_stat_print_us_ >= 1000000) {
      size_t tx_window_size = 0;
      size_t rx_reorder_size = 0;
      int link0_score = 1000;
      int link1_score = 1000;
      {
        std::lock_guard<std::mutex> lock(mac_state_->read_buffer_mutex);
        tx_window_size = mac_state_->tx_window.size();
        rx_reorder_size = mac_state_->rx_reorder_buffer.size();
        if (!mac_state_->link_states.empty()) {
          link0_score = mac_state_->link_states[0].score;
        }
        if (mac_state_->link_states.size() > 1) {
          link1_score = mac_state_->link_states[1].score;
        }
      }
      LOGI("[PEER][STAT] pend_tx=%llu data_tx=%llu ack_tx=%llu reset_tx=%llu "
           "grant_rx=%llu pend_rx=%llu ack_rx=%llu data_rx=%llu reset_rx=%llu "
           "send_fail=%llu sf_pend=%llu sf_grant=%llu sf_data=%llu sf_ack=%llu sf_reset=%llu "
           "tx_window=%zu rx_reorder=%zu link0_score=%d link1_score=%d",
           static_cast<unsigned long long>(tx_pending_count_),
           static_cast<unsigned long long>(tx_data_count_),
           static_cast<unsigned long long>(tx_ack_count_),
           static_cast<unsigned long long>(tx_reset_count_),
           static_cast<unsigned long long>(rx_grant_count_),
           static_cast<unsigned long long>(rx_pending_count_),
           static_cast<unsigned long long>(rx_ack_count_),
           static_cast<unsigned long long>(rx_data_count_),
           static_cast<unsigned long long>(rx_reset_count_),
           static_cast<unsigned long long>(tx_send_fail_count_),
           static_cast<unsigned long long>(tx_send_fail_pending_count_),
           static_cast<unsigned long long>(tx_send_fail_grant_count_),
           static_cast<unsigned long long>(tx_send_fail_data_count_),
           static_cast<unsigned long long>(tx_send_fail_ack_count_),
           static_cast<unsigned long long>(tx_send_fail_reset_count_),
           tx_window_size,
           rx_reorder_size,
           link0_score,
           link1_score);
      last_stat_print_us_ = stat_now_us;
    }
  }
}

bool SecondaryRadioInterface::HandleReset() {
  {
    std::lock_guard<std::mutex> lock(read_buffer_mutex_);
    next_tx_seq_ = 1;
    mac_state_->tx_window.clear();
    last_rx_seq_.reset();
    mac_state_->rx_reorder_buffer.clear();
    frame_buffer_.clear();
  }

  granted_to_send_ = false;
  grant_budget_ = 0;

  MacFrame tx;
  tx.type = FrameType::Reset;
  tx.seq = 0;
  tx.ack = 0;
  tx.pending = 0;
  tx.arg = 0;

  std::vector<uint8_t> response;
  if (!EncodeMacFrame(tx, response)) {
    return false;
  }

  const auto result = Send(response);
  if (result != RequestResult::Success) {
    ++tx_send_fail_count_;
    RecordSendFailure(tx.type);
    return false;
  }
  RecordTx(tx);
  return true;
}

bool SecondaryRadioInterface::ApplyCoordinatorRequest(const MacFrame& request) {
  std::lock_guard<std::mutex> lock(read_buffer_mutex_);

  CommitAckLocked(request.ack);

  switch (request.type) {
    case FrameType::Data:
      if (!ConsumeDataFrameLocked(request)) {
        return false;
      }
      granted_to_send_ = false;
      grant_budget_ = 0;
      return true;

    case FrameType::Grant:
      grant_budget_ = request.arg;
      granted_to_send_ = (grant_budget_ > 0);
      return true;

    case FrameType::Pending:
      granted_to_send_ = false;
      grant_budget_ = 0;
      return true;

    case FrameType::Ack:
      granted_to_send_ = false;
      grant_budget_ = 0;
      return true;

    default:
      return false;
  }
}

bool SecondaryRadioInterface::ChoosePeerResponse(MacFrame& tx) {
  tx = {};
  tx.ack = last_rx_seq_.value_or(kNoSeq);

  bool local_has_data = false;
  {
    std::lock_guard<std::mutex> lock(read_buffer_mutex_);
    local_has_data = !mac_state_->tx_window.empty() || !read_buffer_.empty();
  }

  tx.pending = local_has_data ? 1 : 0;

  if (granted_to_send_ && grant_budget_ > 0 && local_has_data) {
    std::lock_guard<std::mutex> lock(read_buffer_mutex_);
    if (!BuildNextDataFrameLocked(tx)) {
      return false;
    }
    tx.pending = 1;
    --grant_budget_;
    if (grant_budget_ == 0) {
      granted_to_send_ = false;
    }
    return true;
  }

  if (local_has_data) {
    tx.type = FrameType::Pending;
    tx.seq = 0;
    tx.arg = 0;
    return true;
  }

  tx.type = FrameType::Ack;
  tx.seq = 0;
  tx.arg = 0;
  return true;
}

void SecondaryRadioInterface::LogPeerRx(const MacFrame& rx) {
  LOGI("[PEER][RX] type=%u seq=%u ack=%u pending=%u arg=%u payload=%zu",
       static_cast<unsigned>(rx.type),
       rx.seq,
       rx.ack,
       rx.pending,
       rx.arg,
       rx.payload.size());
}

void SecondaryRadioInterface::LogPeerTx(const MacFrame& tx) {
  LOGI("[PEER][TX] type=%u seq=%u ack=%u pending=%u arg=%u payload=%zu",
       static_cast<unsigned>(tx.type),
       tx.seq,
       tx.ack,
       tx.pending,
       tx.arg,
       tx.payload.size());
}


void SecondaryRadioInterface::RecordRx(const MacFrame& rx) {
  switch (rx.type) {
    case FrameType::Pending:
      ++rx_pending_count_;
      break;
    case FrameType::Grant:
      ++rx_grant_count_;
      break;
    case FrameType::Data:
      ++rx_data_count_;
      break;
    case FrameType::Ack:
      ++rx_ack_count_;
      break;
    case FrameType::Reset:
      ++rx_reset_count_;
      break;
    default:
      break;
  }
}

void SecondaryRadioInterface::RecordTx(const MacFrame& tx) {
  switch (tx.type) {
    case FrameType::Pending:
      ++tx_pending_count_;
      break;
    case FrameType::Data:
      ++tx_data_count_;
      break;
    case FrameType::Ack:
      ++tx_ack_count_;
      break;
    case FrameType::Reset:
      ++tx_reset_count_;
      break;
    default:
      break;
  }
}

void SecondaryRadioInterface::RecordSendFailure(FrameType frame_type) {
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

}  // namespace nerfnet
