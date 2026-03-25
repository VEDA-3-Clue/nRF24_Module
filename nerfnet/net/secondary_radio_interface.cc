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
    : RadioInterface(ce_pin, tunnel_fd, primary_addr, secondary_addr, channel) {
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

    if (rx.type == FrameType::Reset) {
      if (!HandleReset()) {
        LOGE("Failed to handle RESET");
      }
      continue;
    }

    MacFrame tx;
    if (!HandleCoordinatorFrame(rx, tx)) {
      LOGE("Failed to handle coordinator frame");
      continue;
    }

    std::vector<uint8_t> response;
    if (!EncodeMacFrame(tx, response)) {
      LOGE("Failed to encode peer response");
      continue;
    }

    result = Send(response);
    if (result != RequestResult::Success) {
      LOGE("Failed to send peer response");
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

  MacFrame reset;
  reset.type = FrameType::Reset;

  std::vector<uint8_t> response;
  if (!EncodeMacFrame(reset, response)) {
    return false;
  }

  return Send(response) == RequestResult::Success;
}

bool SecondaryRadioInterface::HandleCoordinatorFrame(const MacFrame& request,
                                                     MacFrame& response) {
  std::lock_guard<std::mutex> lock(read_buffer_mutex_);

  // 1) First apply ACK carried by coordinator frame.
  CommitAckLocked(request.ack);

  // 2) Then consume incoming DATA if present.
  if (request.type == FrameType::Data) {
    if (!ConsumeDataFrameLocked(request)) {
      return false;
    }
  }

  // 3) Build response according to coordinator instruction.
  response = {};
  response.ack = last_rx_seq_.value_or(kNoSeq);
  response.pending = PendingHintLocked();

  switch (request.type) {
    case FrameType::Grant:
      if ((tx_in_flight_ || !read_buffer_.empty()) && request.arg > 0) {
        if (!BuildNextDataFrameLocked(response)) {
          return false;
        }
      } else {
        response.type = FrameType::Ack;
      }
      return true;

    case FrameType::Data:
      // Coordinator sent DATA. Peer does not send DATA back immediately
      // unless it has explicit grant, so advertise pending only.
      if (tx_in_flight_ || !read_buffer_.empty()) {
        response.type = FrameType::Pending;
      } else {
        response.type = FrameType::Ack;
      }
      return true;

    case FrameType::Pending:
      // Coordinator has no immediate DATA for us; advertise our queue.
      if (tx_in_flight_ || !read_buffer_.empty()) {
        response.type = FrameType::Pending;
      } else {
        response.type = FrameType::Ack;
      }
      return true;

    case FrameType::Ack:
      // Not normally expected as coordinator request, but tolerate it.
      if (tx_in_flight_ || !read_buffer_.empty()) {
        response.type = FrameType::Pending;
      } else {
        response.type = FrameType::Ack;
      }
      return true;

    case FrameType::Reset:
      // handled by caller
      return false;

    default:
      return false;
  }
}

}  // namespace nerfnet