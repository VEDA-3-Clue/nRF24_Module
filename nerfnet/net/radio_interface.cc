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
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "nerfnet/util/log.h"
#include "nerfnet/util/time.h"

namespace nerfnet {

namespace {

std::optional<int> ReadIntFile(const std::string& path) {
  std::ifstream stream(path);
  int value = 0;
  if (!(stream >> value)) {
    return std::nullopt;
  }
  return value;
}

std::string ReadTextFile(const std::string& path) {
  std::ifstream stream(path);
  std::string value;
  std::getline(stream, value);
  return value;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool IsPreferredGpioChipLabel(const std::string& label) {
  const std::string lower = ToLower(label);
  return lower.find("pinctrl") != std::string::npos ||
         lower.find("raspberry") != std::string::npos ||
         lower.find("bcm") != std::string::npos ||
         lower.find("gpio") != std::string::npos;
}

}  // namespace

RadioInterface::RadioInterface(uint16_t ce_pin,
                               int tunnel_fd,
                               uint32_t primary_addr,
                               uint32_t secondary_addr,
                               uint8_t channel,
                               const RadioConfig& radio_config,
                               int irq_pin)
    : radio_(ce_pin, 0),
      tunnel_fd_(tunnel_fd),
      primary_addr_(primary_addr),
      secondary_addr_(secondary_addr),
      radio_config_(radio_config),
      running_(true),
      next_tx_seq_(1),
      tx_in_flight_(false),
      tx_inflight_seq_(0),
      tx_inflight_bytes_left_(0),
      tunnel_logs_enabled_(false),
      irq_pin_(irq_pin),
      irq_fd_(-1),
      resolved_irq_gpio_(-1) {
  CHECK(channel < 128, "Channel must be between 0 and 127");
  CHECK(radio_.begin(), "Failed to start NRF24L01");
  radio_.setChannel(channel);
  radio_.setPALevel(radio_config_.pa_level);
  radio_.setDataRate(radio_config_.data_rate);
  radio_.setAddressWidth(3);
  radio_.setAutoAck(1);
  radio_.setRetries(radio_config_.retry_delay, radio_config_.retry_count);
  radio_.setCRCLength(radio_config_.crc_length);
  CHECK(radio_.isChipConnected(), "NRF24L01 is unavailable");
  if (irq_pin_ >= 0 && !InitializeIrq()) {
    LOGE("Falling back to RX polling because IRQ setup failed for GPIO %d", irq_pin_);
  }

  tunnel_thread_ = std::thread(&RadioInterface::TunnelThread, this);
}

RadioInterface::~RadioInterface() {
  running_ = false;
  if (tunnel_thread_.joinable()) {
    tunnel_thread_.join();
  }
  CleanupIrq();
}

bool RadioInterface::WriteSysfsFile(const std::string& path, const std::string& value) {
  const int fd = open(path.c_str(), O_WRONLY);
  if (fd < 0) {
    return false;
  }

  const ssize_t bytes_written = write(fd, value.data(), value.size());
  close(fd);
  return bytes_written == static_cast<ssize_t>(value.size());
}

bool RadioInterface::ExportIrqGpio(int gpio) {
  errno = 0;
  return WriteSysfsFile("/sys/class/gpio/export", std::to_string(gpio)) || errno == EBUSY;
}

std::optional<int> RadioInterface::ResolveIrqGpio() const {
  if (irq_pin_ < 0) {
    return std::nullopt;
  }

  std::optional<int> fallback;
  for (const auto& entry : std::filesystem::directory_iterator("/sys/class/gpio")) {
    if (!entry.is_directory()) {
      continue;
    }

    const std::string name = entry.path().filename().string();
    if (name.rfind("gpiochip", 0) != 0) {
      continue;
    }

    const auto base = ReadIntFile((entry.path() / "base").string());
    const auto ngpio = ReadIntFile((entry.path() / "ngpio").string());
    if (!base.has_value() || !ngpio.has_value()) {
      continue;
    }

    if (irq_pin_ < 0 || irq_pin_ >= *ngpio) {
      continue;
    }

    const int resolved = *base + irq_pin_;
    const std::string label = ReadTextFile((entry.path() / "label").string());
    if (IsPreferredGpioChipLabel(label)) {
      LOGI("Resolved IRQ line offset %d to sysfs GPIO %d via %s",
           irq_pin_,
           resolved,
           name.c_str());
      return resolved;
    }

    if (!fallback.has_value()) {
      fallback = resolved;
    }
  }

  if (fallback.has_value()) {
    LOGI("Resolved IRQ line offset %d to sysfs GPIO %d", irq_pin_, *fallback);
  }
  return fallback;
}

bool RadioInterface::InitializeIrq() {
  if (irq_pin_ < 0) {
    return false;
  }

  int gpio = irq_pin_;
  if (!ExportIrqGpio(gpio)) {
    const auto resolved = ResolveIrqGpio();
    if (!resolved.has_value()) {
      LOGE("Failed to export IRQ GPIO %d: %s (%d)", irq_pin_, strerror(errno), errno);
      return false;
    }

    gpio = *resolved;
    if (!ExportIrqGpio(gpio)) {
      LOGE("Failed to export resolved IRQ GPIO %d for requested pin %d: %s (%d)",
           gpio,
           irq_pin_,
           strerror(errno),
           errno);
      return false;
    }
  }

  resolved_irq_gpio_ = gpio;
  const std::string gpio_dir = "/sys/class/gpio/gpio" + std::to_string(resolved_irq_gpio_);
  if (!WriteSysfsFile(gpio_dir + "/direction", "in")) {
    LOGE("Failed to set IRQ GPIO %d direction", resolved_irq_gpio_);
    return false;
  }
  if (!WriteSysfsFile(gpio_dir + "/edge", "falling")) {
    LOGE("Failed to set IRQ GPIO %d edge", resolved_irq_gpio_);
    return false;
  }

  irq_fd_ = open((gpio_dir + "/value").c_str(), O_RDONLY | O_NONBLOCK);
  if (irq_fd_ < 0) {
    LOGE("Failed to open IRQ GPIO %d value: %s (%d)", resolved_irq_gpio_, strerror(errno), errno);
    return false;
  }

  char value = 0;
  lseek(irq_fd_, 0, SEEK_SET);
  const ssize_t initial_read = read(irq_fd_, &value, 1);
  (void)initial_read;
  LOGI("RX IRQ enabled on GPIO %d (requested %d)", resolved_irq_gpio_, irq_pin_);
  return true;
}

void RadioInterface::CleanupIrq() {
  if (irq_fd_ >= 0) {
    close(irq_fd_);
    irq_fd_ = -1;
  }

  if (resolved_irq_gpio_ >= 0) {
    WriteSysfsFile("/sys/class/gpio/unexport", std::to_string(resolved_irq_gpio_));
    resolved_irq_gpio_ = -1;
  }
}

RadioInterface::RequestResult RadioInterface::WaitForRxReady(uint64_t timeout_us) {
  if (irq_fd_ < 0) {
    const uint64_t start_us = TimeNowUs();
    while (!radio_.available()) {
      if (timeout_us != 0 && (start_us + timeout_us) < TimeNowUs()) {
        LOGE("Timeout receiving response");
        return RequestResult::Timeout;
      }
      SleepUs(50);
    }
    return RequestResult::Success;
  }

  const uint64_t deadline_us = timeout_us == 0 ? 0 : (TimeNowUs() + timeout_us);
  while (!radio_.available()) {
    struct pollfd pfd = {};
    pfd.fd = irq_fd_;
    pfd.events = POLLPRI | POLLERR;

    int timeout_ms = -1;
    if (deadline_us != 0) {
      const uint64_t now_us = TimeNowUs();
      if (now_us >= deadline_us) {
        LOGE("Timeout receiving response");
        return RequestResult::Timeout;
      }
      const uint64_t remaining_us = deadline_us - now_us;
      timeout_ms = static_cast<int>((remaining_us + 999) / 1000);
      if (timeout_ms == 0) {
        timeout_ms = 1;
      }
    }

    char value = 0;
    lseek(irq_fd_, 0, SEEK_SET);
    const ssize_t clear_before_poll = read(irq_fd_, &value, 1);
    (void)clear_before_poll;

    const int poll_result = poll(&pfd, 1, timeout_ms);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOGE("IRQ poll failed: %s (%d)", strerror(errno), errno);
      return RequestResult::Timeout;
    }

    if (poll_result == 0) {
      LOGE("Timeout receiving response");
      return RequestResult::Timeout;
    }

    lseek(irq_fd_, 0, SEEK_SET);
    const ssize_t clear_after_poll = read(irq_fd_, &value, 1);
    (void)clear_after_poll;
  }

  return RequestResult::Success;
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

  const auto wait_result = WaitForRxReady(timeout_us);
  if (wait_result != RequestResult::Success) {
    return wait_result;
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

uint8_t RadioInterface::NextSeq(uint8_t seq) const {
  if (seq == 0 || seq >= kMaxSeq) {
    return 1;
  }
  return static_cast<uint8_t>(seq + 1);
}

bool RadioInterface::IsExpectedRxSeq(uint8_t seq) const {
  if (seq == 0) {
    return false;
  }
  if (!last_rx_seq_.has_value()) {
    return true;
  }
  return seq == NextSeq(last_rx_seq_.value());
}

void RadioInterface::ResetRxAssemblyLocked() {
  frame_buffer_.clear();
  last_rx_seq_.reset();
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

    std::vector<uint8_t> packet(&buffer[0], &buffer[bytes_read]);
    if (!IsValidIpPacket(packet)) {
      LOGE("Dropping non-IP or malformed tunnel packet len=%d", bytes_read);
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(read_buffer_mutex_);
      read_buffer_.push_back(std::move(packet));
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
    ResetRxAssemblyLocked();
    return false;
  }

  if (last_rx_seq_.has_value()) {
    const uint8_t last = last_rx_seq_.value();
    const uint8_t expected = NextSeq(last);

    if (frame.seq == last) {
      // Duplicate retransmission. ACK만 다시 보내게 하고 payload는 다시 붙이지 않는다.
      return true;
    }

    if (frame.seq != expected) {
      LOGE("Received unexpected DATA seq=%u expected=%u last=%u",
           frame.seq,
           expected,
           last);
      ResetRxAssemblyLocked();
      return false;
    }
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

bool RadioInterface::IsValidIpPacket(const std::vector<uint8_t>& packet) const {
  if (packet.empty()) {
    return false;
  }

  const uint8_t version = static_cast<uint8_t>(packet[0] >> 4);

  if (version == 4) {
    if (packet.size() < 20) {
      return false;
    }

    const size_t ihl = static_cast<size_t>(packet[0] & 0x0F) * 4;
    if (ihl < 20 || packet.size() < ihl) {
      return false;
    }

    const uint16_t total_len =
        static_cast<uint16_t>((static_cast<uint16_t>(packet[2]) << 8) |
                              static_cast<uint16_t>(packet[3]));
    if (total_len < ihl) {
      return false;
    }

    return packet.size() == total_len;
  }

  if (version == 6) {
    if (packet.size() < 40) {
      return false;
    }

    const uint16_t payload_len =
        static_cast<uint16_t>((static_cast<uint16_t>(packet[4]) << 8) |
                              static_cast<uint16_t>(packet[5]));
    return packet.size() == static_cast<size_t>(40 + payload_len);
  }

  return false;
}

void RadioInterface::WriteTunnel() {
  if (tunnel_logs_enabled_) {
    LOGI("Writing %zu bytes to tunnel", frame_buffer_.size());
  }

  if (!IsValidIpPacket(frame_buffer_)) {
    LOGE("Dropping invalid reassembled IP packet len=%zu", frame_buffer_.size());
    frame_buffer_.clear();
    return;
  }

  const int bytes_written =
      write(tunnel_fd_, frame_buffer_.data(), frame_buffer_.size());

  frame_buffer_.clear();

  if (bytes_written < 0) {
    LOGE("Failed to write to tunnel %s (%d)", strerror(errno), errno);
  }
}

}  // namespace nerfnet