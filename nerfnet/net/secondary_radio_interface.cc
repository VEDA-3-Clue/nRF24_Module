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

namespace nerfnet {

SecondaryRadioInterface::SecondaryRadioInterface(uint16_t ce_pin,
                                                 int tunnel_fd,
                                                 uint32_t primary_addr,
                                                 uint32_t secondary_addr,
                                                 uint8_t channel)
    : RadioInterface(ce_pin, tunnel_fd, primary_addr, secondary_addr, channel),
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

    LogPeerRx(rx);

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

    LogPeerTx(tx);

    std::vector<uint8_t> response;
    if (!EncodeMacFrame(tx, response)) {
      LOGE("[PEER] encode failed");
      continue;
    }

    result = Send(response);
    if (result != RequestResult::Success) {
      LOGE("[PEER] send failed");
    }
  }
}

bool SecondaryRadioInterface::HandleReset() {
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

  return Send(response) == RequestResult::Success;
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
    local_has_data = tx_in_flight_ || !read_buffer_.empty();
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

}  // namespace nerfnet