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

#include "nerfnet/net/radio_interface.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include "nerfnet/util/log.h"
#include "nerfnet/util/time.h"

namespace nerfnet {

RadioInterface::RadioInterface(uint16_t ce_pin,
                               int tunnel_fd,
                               uint32_t primary_addr,
                               uint32_t secondary_addr,
                               uint8_t channel)
    : radio_(ce_pin, 0),
      tunnel_fd_(tunnel_fd),
      primary_addr_(primary_addr),
      secondary_addr_(secondary_addr),
      tunnel_thread_(&RadioInterface::TunnelThread, this),
      running_(true),
      next_tx_seq_(1),
      tx_in_flight_(false),
      tx_inflight_seq_(0),
      tx_inflight_bytes_left_(0),
      tunnel_logs_enabled_(false) {
  CHECK(channel < 128, "Channel must be between 0 and 127");
  CHECK(radio_.begin(), "Failed to start NRF24L01");
  radio_.setChannel(channel);
  radio_.setPALevel(RF24_PA_MAX);
  radio_.setDataRate(RF24_2MBPS);
  radio_.setAddressWidth(3);
  radio_.setAutoAck(1);
  radio_.setRetries(0, 15);
  radio_.setCRCLength(RF24_CRC_8);
  CHECK(radio_.isChipConnected(), "NRF24L01 is unavailable");
}

RadioInterface::~RadioInterface() {
  running_ = false;
  if (tunnel_thread_.joinable()) {
    tunnel_thread_.join();
  }
}

RadioInterface::RequestResult RadioInterface::Send(
    const std::vector<uint8_t>& request) {
  radio_.stopListening();

  if (request.size() > kMaxPacketSize) {
    LOGE("Request is too large (%zu vs %zu)", request.size(), kMaxPacketSize);
    return RequestResult::Malformed;
  }

  if (!radio_.write(request.data(), request.size())) {
    LOGE("Failed to write request");
    return RequestResult::TransmitError;
  }

  while (!radio_.txStandBy()) {
    SleepUs(50);
  }
  return RequestResult::Success;
}

RadioInterface::RequestResult RadioInterface::Receive(
    std::vector<uint8_t>& response,
    uint64_t timeout_us) {
  radio_.startListening();

  const uint64_t start_us = TimeNowUs();
  while (!radio_.available()) {
    if (timeout_us != 0 && (start_us + timeout_us) < TimeNowUs()) {
      LOGE("Timeout receiving response");
      return RequestResult::Timeout;
    }
    SleepUs(50);
  }

  radio_.read(response.data(), response.size());
  return RequestResult::Success;
}

size_t RadioInterface::GetReadBufferSize() {
  std::lock_guard<std::mutex> lock(read_buffer_mutex_);
  return read_buffer_.size();
}

size_t RadioInterface::GetTransferSize(const std::vector<uint8_t>& frame) {
  return std::min(frame.size(), static_cast<size_t>(kMaxPayloadSize));
}

uint8_t RadioInterface::PendingHintLocked() const {
  return static_cast<uint8_t>(std::min<size_t>(read_buffer_.size(), 255));
}

void RadioInterface::AdvanceTxSeq() {
  ++next_tx_seq_;
  if (next_tx_seq_ == 0 || next_tx_seq_ > kMaxSeq) {
    next_tx_seq_ = 1;
  }
}

bool RadioInterface::IsExpectedRxSeq(uint8_t seq) const {
  if (seq == 0) {
    return false;
  }
  if (!last_rx_seq_.has_value()) {
    return true;
  }
  if (last_rx_seq_.value() == kMaxSeq) {
    return seq == 1;
  }
  return seq == static_cast<uint8_t>(last_rx_seq_.value() + 1);
}

void RadioInterface::TunnelThread() {
  constexpr size_t kMaxBufferedFrames = 1024;

  uint8_t buffer[3200];
  while (running_) {
    const int bytes_read = read(tunnel_fd_, buffer, sizeof(buffer));
    if (bytes_read < 0) {
      LOGE("Failed to read: %s (%d)", strerror(errno), errno);
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(read_buffer_mutex_);
      read_buffer_.emplace_back(&buffer[0], &buffer[bytes_read]);
      if (tunnel_logs_enabled_) {
        LOGI("Read %zu bytes from tunnel", read_buffer_.back().size());
      }
    }

    while (GetReadBufferSize() > kMaxBufferedFrames && running_) {
      SleepUs(1000);
    }
  }
}

bool RadioInterface::EncodeMacFrame(const MacFrame& frame,
                                    std::vector<uint8_t>& packet) {
  packet.assign(kMaxPacketSize, 0x00);

  if (frame.payload.size() > kMaxPayloadSize) {
    LOGE("Payload too large (%zu > %zu)", frame.payload.size(), kMaxPayloadSize);
    return false;
  }

  packet[0] = static_cast<uint8_t>(frame.type);
  packet[1] = frame.seq;
  packet[2] = frame.ack;
  packet[3] = frame.pending;
  packet[4] = frame.arg;

  for (size_t i = 0; i < frame.payload.size(); ++i) {
    packet[kHeaderSize + i] = frame.payload[i];
  }
  return true;
}

bool RadioInterface::DecodeMacFrame(const std::vector<uint8_t>& packet,
                                    MacFrame& frame) {
  if (packet.size() != kMaxPacketSize) {
    LOGE("Received short packet");
    return false;
  }

  frame.type = static_cast<FrameType>(packet[0]);
  frame.seq = packet[1];
  frame.ack = packet[2];
  frame.pending = packet[3];
  frame.arg = packet[4];
  frame.payload.clear();

  if (frame.type == FrameType::Data && frame.arg > 0) {
    const uint8_t payload_size =
        std::min<uint8_t>(frame.arg, static_cast<uint8_t>(kMaxPayloadSize));
    frame.payload.insert(frame.payload.end(),
                         packet.begin() + kHeaderSize,
                         packet.begin() + kHeaderSize + payload_size);
  }
  return true;
}

bool RadioInterface::BuildNextDataFrameLocked(MacFrame& frame) {
  frame = {};
  frame.type = FrameType::Data;
  frame.ack = last_rx_seq_.value_or(kNoSeq);
  frame.pending = PendingHintLocked();

  if (tx_in_flight_) {
    frame.seq = tx_inflight_seq_;
    frame.arg = tx_inflight_bytes_left_;
    frame.payload = tx_inflight_payload_;
    return true;
  }

  if (read_buffer_.empty()) {
    return false;
  }

  auto& current = read_buffer_.front();
  const size_t transfer_size = GetTransferSize(current);

  tx_in_flight_ = true;
  tx_inflight_seq_ = next_tx_seq_;
  tx_inflight_bytes_left_ =
      static_cast<uint8_t>(std::min<size_t>(current.size(), 255));
  tx_inflight_payload_.assign(current.begin(), current.begin() + transfer_size);

  frame.seq = tx_inflight_seq_;
  frame.arg = tx_inflight_bytes_left_;
  frame.payload = tx_inflight_payload_;
  return true;
}

void RadioInterface::CommitAckLocked(uint8_t ack_seq) {
  if (!tx_in_flight_) {
    return;
  }
  if (ack_seq != tx_inflight_seq_) {
    return;
  }

  if (!read_buffer_.empty()) {
    auto& frame = read_buffer_.front();
    const size_t consumed = std::min(frame.size(), tx_inflight_payload_.size());
    frame.erase(frame.begin(), frame.begin() + consumed);
    if (frame.empty()) {
      read_buffer_.pop_front();
    }
  }

  tx_in_flight_ = false;
  tx_inflight_seq_ = 0;
  tx_inflight_bytes_left_ = 0;
  tx_inflight_payload_.clear();
  AdvanceTxSeq();
}

bool RadioInterface::ConsumeDataFrameLocked(const MacFrame& frame) {
  if (frame.type != FrameType::Data) {
    return true;
  }
  if (frame.seq == 0) {
    LOGE("DATA frame missing seq");
    return false;
  }
  if (!IsExpectedRxSeq(frame.seq)) {
    LOGE("Received non-sequential DATA seq=%u last=%u",
         frame.seq,
         last_rx_seq_.value_or(0));
    return false;
  }

  last_rx_seq_ = frame.seq;

  if (!frame.payload.empty()) {
    frame_buffer_.insert(frame_buffer_.end(),
                         frame.payload.begin(),
                         frame.payload.end());

    if (frame.arg <= kMaxPayloadSize) {
      WriteTunnel();
    }
  }
  return true;
}

void RadioInterface::WriteTunnel() {
  const int bytes_written =
      write(tunnel_fd_, frame_buffer_.data(), frame_buffer_.size());
  if (tunnel_logs_enabled_) {
    LOGI("Writing %zu bytes to tunnel", frame_buffer_.size());
  }
  frame_buffer_.clear();

  if (bytes_written < 0) {
    LOGE("Failed to write to tunnel %s (%d)", strerror(errno), errno);
  }
}

}  // namespace nerfnet