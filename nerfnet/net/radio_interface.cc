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
#include <gpiod.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "nerfnet/util/log.h"
#include "nerfnet/util/time.h"

namespace nerfnet {

namespace {

struct ResolvedIrqTarget {
  std::string chip_name;
  std::string chip_label;
  unsigned int line_offset = 0;
  int global_gpio = -1;
};

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

std::optional<std::string> FindChipNameByLabelAndLines(const std::string& label,
                                                       unsigned int num_lines) {
  gpiod_chip_iter* iter = gpiod_chip_iter_new();
  if (iter == nullptr) {
    return std::nullopt;
  }

  std::optional<std::string> fallback;
  gpiod_chip* chip = nullptr;
  gpiod_foreach_chip(iter, chip) {
    const char* chip_name = gpiod_chip_name(chip);
    const char* chip_label = gpiod_chip_label(chip);
    const unsigned int chip_lines = gpiod_chip_num_lines(chip);
    if (chip_name == nullptr) {
      continue;
    }

    if (chip_label != nullptr && label == chip_label && chip_lines == num_lines) {
      std::string result = chip_name;
      gpiod_chip_iter_free(iter);
      return result;
    }

    if (chip_label != nullptr && label == chip_label && !fallback.has_value()) {
      fallback = std::string(chip_name);
    }
  }

  gpiod_chip_iter_free(iter);
  return fallback;
}

std::optional<ResolvedIrqTarget> ResolveIrqTargetFromGlobal(int global_gpio) {
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

    if (global_gpio < *base || global_gpio >= (*base + *ngpio)) {
      continue;
    }

    const std::string label = ReadTextFile((entry.path() / "label").string());
    const auto chip_name = FindChipNameByLabelAndLines(label, static_cast<unsigned int>(*ngpio));
    if (!chip_name.has_value()) {
      continue;
    }

    ResolvedIrqTarget target;
    target.chip_name = *chip_name;
    target.chip_label = label;
    target.line_offset = static_cast<unsigned int>(global_gpio - *base);
    target.global_gpio = global_gpio;
    return target;
  }

  return std::nullopt;
}

std::optional<ResolvedIrqTarget> ResolveIrqTargetFromOffset(int line_offset) {
  gpiod_chip_iter* iter = gpiod_chip_iter_new();
  if (iter == nullptr) {
    return std::nullopt;
  }

  std::optional<ResolvedIrqTarget> fallback;
  gpiod_chip* chip = nullptr;
  gpiod_foreach_chip(iter, chip) {
    const unsigned int chip_lines = gpiod_chip_num_lines(chip);
    if (line_offset < 0 || static_cast<unsigned int>(line_offset) >= chip_lines) {
      continue;
    }

    const char* chip_name = gpiod_chip_name(chip);
    if (chip_name == nullptr) {
      continue;
    }

    ResolvedIrqTarget target;
    target.chip_name = chip_name;
    const char* chip_label = gpiod_chip_label(chip);
    if (chip_label != nullptr) {
      target.chip_label = chip_label;
    }
    target.line_offset = static_cast<unsigned int>(line_offset);

    if (IsPreferredGpioChipLabel(target.chip_label)) {
      gpiod_chip_iter_free(iter);
      return target;
    }

    if (!fallback.has_value()) {
      fallback = target;
    }
  }

  gpiod_chip_iter_free(iter);
  return fallback;
}

std::optional<ResolvedIrqTarget> ResolveIrqTarget(int irq_pin) {
  if (irq_pin < 0) {
    return std::nullopt;
  }

  if (const auto global_target = ResolveIrqTargetFromGlobal(irq_pin); global_target.has_value()) {
    return global_target;
  }

  return ResolveIrqTargetFromOffset(irq_pin);
}

bool IsValidIpPacketForDispatch(const std::vector<uint8_t>& packet) {
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

uint64_t Fnv1aInit() {
  return 1469598103934665603ull;
}

void Fnv1aMix(uint64_t& hash, uint8_t value) {
  hash ^= static_cast<uint64_t>(value);
  hash *= 1099511628211ull;
}

void Fnv1aMixBytes(uint64_t& hash, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    Fnv1aMix(hash, data[i]);
  }
}

uint64_t HashPacketFlow(const std::vector<uint8_t>& packet) {
  if (packet.empty()) {
    return 0;
  }

  uint64_t hash = Fnv1aInit();
  const uint8_t version = static_cast<uint8_t>(packet[0] >> 4);

  if (version == 4 && packet.size() >= 20) {
    const size_t ihl = static_cast<size_t>(packet[0] & 0x0F) * 4;
    if (ihl >= 20 && packet.size() >= ihl) {
      const uint8_t proto = packet[9];
      Fnv1aMix(hash, proto);
      Fnv1aMixBytes(hash, &packet[12], 8);
      if ((proto == 6 || proto == 17) && packet.size() >= ihl + 4) {
        Fnv1aMixBytes(hash, &packet[ihl], 4);
      }
      return hash;
    }
  }

  if (version == 6 && packet.size() >= 40) {
    const uint8_t next_header = packet[6];
    Fnv1aMix(hash, next_header);
    Fnv1aMixBytes(hash, &packet[8], 32);
    if ((next_header == 6 || next_header == 17) && packet.size() >= 44) {
      Fnv1aMixBytes(hash, &packet[40], 4);
    }
    return hash;
  }

  Fnv1aMixBytes(hash, packet.data(), std::min<size_t>(packet.size(), 16));
  return hash;
}

enum class PacketClass {
  Bulk,
  Control,
};

struct PacketTraits {
  PacketClass packet_class = PacketClass::Control;
  bool is_udp = false;
  bool is_tcp = false;
  size_t packet_size = 0;
};

PacketTraits ClassifyPacket(const std::vector<uint8_t>& packet) {
  PacketTraits traits;
  traits.packet_size = packet.size();
  if (packet.empty()) {
    return traits;
  }

  const uint8_t version = static_cast<uint8_t>(packet[0] >> 4);
  uint8_t protocol = 0;
  size_t transport_offset = 0;

  if (version == 4 && packet.size() >= 20) {
    const size_t ihl = static_cast<size_t>(packet[0] & 0x0F) * 4;
    if (ihl >= 20 && packet.size() >= ihl) {
      protocol = packet[9];
      transport_offset = ihl;
    }
  } else if (version == 6 && packet.size() >= 40) {
    protocol = packet[6];
    transport_offset = 40;
  }

  traits.is_udp = (protocol == 17);
  traits.is_tcp = (protocol == 6);

  const bool large_packet = packet.size() >= 192;
  const bool large_udp = traits.is_udp && packet.size() >= 128;
  const bool video_like_udp = traits.is_udp && packet.size() >= 96 && transport_offset != 0;

  if (large_packet || large_udp || video_like_udp) {
    traits.packet_class = PacketClass::Bulk;
  }
  return traits;
}

size_t ChooseRoleSeparatedRadioIndex(const std::vector<uint8_t>& packet,
                                     const PacketTraits& traits,
                                     size_t interface_count) {
  if (interface_count <= 1) {
    return 0;
  }

  const uint64_t hash_index = HashPacketFlow(packet) % interface_count;

  // Keep the old flow-affinity baseline as the default/fallback.
  // We only steer small control-like packets toward radio1 when dual-radio is active.
  if (interface_count >= 2 && traits.packet_class == PacketClass::Control) {
    return 1;
  }

  return static_cast<size_t>(hash_index);
}

struct TunnelDispatcher {
  int tunnel_key_fd = -1;
  int read_fd = -1;
  std::atomic<bool> running{true};
  std::mutex mutex;
  std::vector<RadioInterface*> interfaces;
  std::thread thread;
  uint64_t bulk_packets = 0;
  uint64_t control_packets = 0;
  uint64_t radio0_packets = 0;
  uint64_t radio1_packets = 0;
  uint64_t last_stat_print_us = 0;
};

std::mutex g_tunnel_dispatchers_mutex;
std::unordered_map<int, std::shared_ptr<TunnelDispatcher>> g_tunnel_dispatchers;

void TunnelDispatchLoop(const std::shared_ptr<TunnelDispatcher>& dispatcher) {
  uint8_t buffer[3200];

  while (dispatcher->running.load()) {
    const int bytes_read = read(dispatcher->read_fd, buffer, sizeof(buffer));
    if (bytes_read < 0) {
      if (!dispatcher->running.load()) {
        break;
      }
      LOGE("Failed to read shared tunnel fd: %s (%d)", strerror(errno), errno);
      continue;
    }

    if (bytes_read == 0) {
      if (!dispatcher->running.load()) {
        break;
      }
      SleepUs(1000);
      continue;
    }

    std::vector<uint8_t> packet(&buffer[0], &buffer[bytes_read]);
    if (!IsValidIpPacketForDispatch(packet)) {
      LOGE("Dropping non-IP or malformed tunnel packet len=%d", bytes_read);
      continue;
    }

    std::lock_guard<std::mutex> lock(dispatcher->mutex);
    if (dispatcher->interfaces.empty()) {
      continue;
    }

    const PacketTraits traits = ClassifyPacket(packet);
    const size_t index = ChooseRoleSeparatedRadioIndex(packet, traits, dispatcher->interfaces.size());

    if (traits.packet_class == PacketClass::Bulk) {
      ++dispatcher->bulk_packets;
    } else {
      ++dispatcher->control_packets;
    }
    if (index == 0) {
      ++dispatcher->radio0_packets;
    } else if (index == 1) {
      ++dispatcher->radio1_packets;
    }

    dispatcher->interfaces[index]->EnqueueTunnelPacket(std::move(packet));

    const uint64_t now_us = TimeNowUs();
    if (dispatcher->last_stat_print_us == 0) {
      dispatcher->last_stat_print_us = now_us;
    } else if (now_us - dispatcher->last_stat_print_us >= 1000000) {
      LOGI("[DISPATCH] tunnel_fd=%d bulk=%llu control=%llu radio0=%llu radio1=%llu",
           dispatcher->tunnel_key_fd,
           static_cast<unsigned long long>(dispatcher->bulk_packets),
           static_cast<unsigned long long>(dispatcher->control_packets),
           static_cast<unsigned long long>(dispatcher->radio0_packets),
           static_cast<unsigned long long>(dispatcher->radio1_packets));
      dispatcher->last_stat_print_us = now_us;
    }
  }
}

void RegisterTunnelDispatcher(int tunnel_fd, RadioInterface* interface) {
  std::shared_ptr<TunnelDispatcher> dispatcher;
  bool start_thread = false;

  {
    std::lock_guard<std::mutex> lock(g_tunnel_dispatchers_mutex);
    auto& entry = g_tunnel_dispatchers[tunnel_fd];
    if (!entry) {
      entry = std::make_shared<TunnelDispatcher>();
      entry->tunnel_key_fd = tunnel_fd;
      entry->read_fd = dup(tunnel_fd);
      CHECK(entry->read_fd >= 0, "Failed to duplicate tunnel fd %d: %s (%d)",
            tunnel_fd,
            strerror(errno),
            errno);
      dispatcher = entry;
      start_thread = true;
    } else {
      dispatcher = entry;
    }
  }

  {
    std::lock_guard<std::mutex> lock(dispatcher->mutex);
    dispatcher->interfaces.push_back(interface);
  }

  if (start_thread) {
    dispatcher->thread = std::thread(TunnelDispatchLoop, dispatcher);
  }
}

void UnregisterTunnelDispatcher(int tunnel_fd, RadioInterface* interface) {
  std::shared_ptr<TunnelDispatcher> dispatcher;
  bool stop_dispatcher = false;

  {
    std::lock_guard<std::mutex> lock(g_tunnel_dispatchers_mutex);
    const auto it = g_tunnel_dispatchers.find(tunnel_fd);
    if (it == g_tunnel_dispatchers.end()) {
      return;
    }
    dispatcher = it->second;

    std::lock_guard<std::mutex> dispatcher_lock(dispatcher->mutex);
    dispatcher->interfaces.erase(
        std::remove(dispatcher->interfaces.begin(), dispatcher->interfaces.end(), interface),
        dispatcher->interfaces.end());
    if (dispatcher->interfaces.empty()) {
      g_tunnel_dispatchers.erase(it);
      stop_dispatcher = true;
    }
  }

  if (!stop_dispatcher) {
    return;
  }

  dispatcher->running.store(false);
  close(dispatcher->read_fd);
  if (dispatcher->thread.joinable()) {
    dispatcher->thread.join();
  }
}

}  // namespace

RadioInterface::RadioInterface(uint16_t ce_pin,
                               uint16_t csn_pin,
                               int tunnel_fd,
                               uint32_t primary_addr,
                               uint32_t secondary_addr,
                               uint8_t channel,
                               const RadioConfig& radio_config,
                               int irq_pin,
                               std::shared_ptr<SharedMacState> shared_mac_state)
    : radio_(ce_pin, csn_pin),
      ce_pin_(ce_pin),
      csn_pin_(csn_pin),
      tunnel_fd_(tunnel_fd),
      primary_addr_(primary_addr),
      secondary_addr_(secondary_addr),
      radio_config_(radio_config),
      running_(true),
      mac_state_(shared_mac_state ? std::move(shared_mac_state) : std::make_shared<SharedMacState>()),
      tunnel_logs_enabled_(false),
      irq_pin_(irq_pin),
      irq_chip_(nullptr),
      irq_line_(nullptr),
      resolved_irq_line_offset_(0),
      resolved_irq_global_gpio_(-1) {
  CHECK(channel < 128, "Channel must be between 0 and 127");
  CHECK(radio_.begin(), "Failed to start NRF24L01 (ce=%u csn=%u)",
      static_cast<unsigned>(ce_pin_),
      static_cast<unsigned>(csn_pin_));
  radio_.setChannel(channel);
  radio_.setPALevel(radio_config_.pa_level);
  radio_.setDataRate(radio_config_.data_rate);
  radio_.setAddressWidth(3);
  radio_.setAutoAck(1);
  radio_.setRetries(radio_config_.retry_delay, radio_config_.retry_count);
  radio_.setCRCLength(radio_config_.crc_length);
  CHECK(radio_.isChipConnected(), "NRF24L01 is unavailable (ce=%u csn=%u)",
      static_cast<unsigned>(ce_pin_),
      static_cast<unsigned>(csn_pin_));
  if (irq_pin_ >= 0 && !InitializeIrq()) {
    LOGE("Falling back to RX polling because IRQ setup failed for pin %d", irq_pin_);
  }


  RegisterTunnelDispatcher(tunnel_fd_, this);
}

RadioInterface::~RadioInterface() {
  running_ = false;
  UnregisterTunnelDispatcher(tunnel_fd_, this);
  CleanupIrq();
}

bool RadioInterface::InitializeIrq() {
  if (irq_pin_ < 0) {
    return false;
  }

  const auto target = ResolveIrqTarget(irq_pin_);
  if (!target.has_value()) {
    LOGE("Failed to resolve IRQ pin %d to a gpiochip line", irq_pin_);
    return false;
  }

  irq_chip_ = gpiod_chip_open_by_name(target->chip_name.c_str());
  if (irq_chip_ == nullptr) {
    LOGE("Failed to open gpiochip %s for IRQ pin %d: %s (%d)",
         target->chip_name.c_str(),
         irq_pin_,
         strerror(errno),
         errno);
    return false;
  }

  irq_line_ = gpiod_chip_get_line(irq_chip_, target->line_offset);
  if (irq_line_ == nullptr) {
    LOGE("Failed to get gpiochip line %u on %s for IRQ pin %d: %s (%d)",
         target->line_offset,
         target->chip_name.c_str(),
         irq_pin_,
         strerror(errno),
         errno);
    CleanupIrq();
    return false;
  }

  if (gpiod_line_request_falling_edge_events(irq_line_, "nerfnet-rx-irq") < 0) {
    LOGE("Failed to request falling-edge IRQ events on %s line %u: %s (%d)",
         target->chip_name.c_str(),
         target->line_offset,
         strerror(errno),
         errno);
    CleanupIrq();
    return false;
  }

  resolved_irq_chip_name_ = target->chip_name;
  resolved_irq_line_offset_ = target->line_offset;
  resolved_irq_global_gpio_ = target->global_gpio;

  if (resolved_irq_global_gpio_ >= 0) {
    LOGI("RX IRQ enabled on %s line %u (requested %d, global GPIO %d)",
         resolved_irq_chip_name_.c_str(),
         resolved_irq_line_offset_,
         irq_pin_,
         resolved_irq_global_gpio_);
  } else {
    LOGI("RX IRQ enabled on %s line %u (requested %d)",
         resolved_irq_chip_name_.c_str(),
         resolved_irq_line_offset_,
         irq_pin_);
  }
  return true;
}

void RadioInterface::CleanupIrq() {
  if (irq_line_ != nullptr) {
    gpiod_line_release(irq_line_);
    irq_line_ = nullptr;
  }
  if (irq_chip_ != nullptr) {
    gpiod_chip_close(irq_chip_);
    irq_chip_ = nullptr;
  }
  resolved_irq_chip_name_.clear();
  resolved_irq_line_offset_ = 0;
  resolved_irq_global_gpio_ = -1;
}

RadioInterface::RequestResult RadioInterface::WaitForRxReady(uint64_t timeout_us) {
  if (irq_line_ == nullptr) {
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
    std::timespec timeout = {};
    std::timespec* timeout_ptr = nullptr;
    if (deadline_us != 0) {
      const uint64_t now_us = TimeNowUs();
      if (now_us >= deadline_us) {
        LOGE("Timeout receiving response");
        return RequestResult::Timeout;
      }
      const uint64_t remaining_us = deadline_us - now_us;
      timeout.tv_sec = static_cast<time_t>(remaining_us / 1000000);
      timeout.tv_nsec = static_cast<long>((remaining_us % 1000000) * 1000);
      timeout_ptr = &timeout;
    }

    const int wait_result = gpiod_line_event_wait(irq_line_, timeout_ptr);
    if (wait_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOGE("IRQ wait failed on %s line %u: %s (%d)",
           resolved_irq_chip_name_.c_str(),
           resolved_irq_line_offset_,
           strerror(errno),
           errno);
      return RequestResult::Timeout;
    }

    if (wait_result == 0) {
      LOGE("Timeout receiving response");
      return RequestResult::Timeout;
    }

    gpiod_line_event event = {};
    if (gpiod_line_event_read(irq_line_, &event) < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOGE("Failed to read IRQ event on %s line %u: %s (%d)",
           resolved_irq_chip_name_.c_str(),
           resolved_irq_line_offset_,
           strerror(errno),
           errno);
      return RequestResult::Timeout;
    }
  }

  return RequestResult::Success;
}

void RadioInterface::RecoverAfterTransmitFailure(const char* stage) {
  LOGE("TX recovery after %s failure", stage);
  radio_.flush_tx();
  SleepUs(kTxFailureRecoveryGapUs);
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
    RecoverAfterTransmitFailure("write");
    return RequestResult::TransmitError;
  }

  const uint64_t standby_deadline_us = TimeNowUs() + kTxStandbyTimeoutUs;
  while (!radio_.txStandBy()) {
    if (TimeNowUs() >= standby_deadline_us) {
      LOGE("Timed out waiting for TX standby");
      RecoverAfterTransmitFailure("txStandBy");
      return RequestResult::TransmitError;
    }
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
  std::lock_guard<std::mutex> lock(mac_state_->read_buffer_mutex);
  return mac_state_->read_buffer.size();
}

void RadioInterface::EnqueueTunnelPacket(std::vector<uint8_t> packet) {
  constexpr size_t kMaxBufferedFrames = 1024;

  std::lock_guard<std::mutex> lock(mac_state_->read_buffer_mutex);
  if (mac_state_->read_buffer.size() >= kMaxBufferedFrames) {
    LOGE("Dropping tunnel packet because TX queue is full on ce=%u csn=%u", 
         static_cast<unsigned>(ce_pin_),
         static_cast<unsigned>(csn_pin_));
    return;
  }

  mac_state_->read_buffer.push_back(std::move(packet));
  if (tunnel_logs_enabled_) {
    LOGI("Queued %zu bytes from shared tunnel dispatcher", mac_state_->read_buffer.back().size());
  }
}

size_t RadioInterface::GetTransferSize(const std::vector<uint8_t>& frame) {
  return std::min(frame.size(), static_cast<size_t>(kMaxPayloadSize));
}

uint8_t RadioInterface::PendingHintLocked() const {
  const size_t queued = mac_state_->read_buffer.size() + mac_state_->tx_window.size();
  return static_cast<uint8_t>(std::min<size_t>(queued, 255));
}

void RadioInterface::AdvanceTxSeq() {
  ++mac_state_->next_tx_seq;
  if (mac_state_->next_tx_seq == 0 || mac_state_->next_tx_seq > kMaxSeq) {
    mac_state_->next_tx_seq = 1;
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
  if (!mac_state_->last_rx_seq.has_value()) {
    return true;
  }
  return seq == NextSeq(mac_state_->last_rx_seq.value());
}

void RadioInterface::ResetRxAssemblyLocked() {
  mac_state_->frame_buffer.clear();
  mac_state_->last_rx_seq.reset();
  mac_state_->rx_reorder_buffer.clear();
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
  constexpr size_t kBondTxWindowSize = 2;
  constexpr uint64_t kBondRetransmitIntervalUs = 2000;

  frame = {};
  frame.type = FrameType::Data;
  frame.ack = mac_state_->last_rx_seq.value_or(kNoSeq);
  frame.pending = PendingHintLocked();

  const uint64_t now_us = TimeNowUs();
  if (!mac_state_->tx_window.empty()) {
    auto& oldest = mac_state_->tx_window.front();
    const bool window_full = mac_state_->tx_window.size() >= kBondTxWindowSize;
    const bool retry_due = oldest.last_send_us == 0 ||
        (now_us - oldest.last_send_us) >= kBondRetransmitIntervalUs;
    if ((window_full || mac_state_->read_buffer.empty()) && retry_due) {
      oldest.last_send_us = now_us;
      ++oldest.send_count;
      frame.seq = oldest.seq;
      frame.arg = oldest.bytes_left;
      frame.payload = oldest.payload;
      return true;
    }
  }

  if (mac_state_->tx_window.size() >= kBondTxWindowSize) {
    if (mac_state_->tx_window.empty()) {
      return false;
    }
    auto& oldest = mac_state_->tx_window.front();
    oldest.last_send_us = now_us;
    ++oldest.send_count;
    frame.seq = oldest.seq;
    frame.arg = oldest.bytes_left;
    frame.payload = oldest.payload;
    return true;
  }

  if (mac_state_->read_buffer.empty()) {
    if (mac_state_->tx_window.empty()) {
      return false;
    }
    auto& oldest = mac_state_->tx_window.front();
    oldest.last_send_us = now_us;
    ++oldest.send_count;
    frame.seq = oldest.seq;
    frame.arg = oldest.bytes_left;
    frame.payload = oldest.payload;
    return true;
  }

  auto& current = mac_state_->read_buffer.front();
  const size_t transfer_size = GetTransferSize(current);

  TxFragmentState fragment;
  fragment.seq = mac_state_->next_tx_seq;
  fragment.bytes_left = static_cast<uint8_t>(std::min<size_t>(current.size(), 255));
  fragment.payload.assign(current.begin(), current.begin() + transfer_size);
  fragment.last_send_us = now_us;
  fragment.send_count = 1;

  current.erase(current.begin(), current.begin() + transfer_size);
  if (current.empty()) {
    mac_state_->read_buffer.pop_front();
  }

  mac_state_->tx_window.push_back(fragment);
  AdvanceTxSeq();

  frame.seq = fragment.seq;
  frame.arg = fragment.bytes_left;
  frame.payload = fragment.payload;
  return true;
}

void RadioInterface::CommitAckLocked(uint8_t ack_seq) {
  if (ack_seq == kNoSeq || mac_state_->tx_window.empty()) {
    return;
  }

  auto ack_it = mac_state_->tx_window.end();
  for (auto it = mac_state_->tx_window.begin(); it != mac_state_->tx_window.end(); ++it) {
    if (it->seq == ack_seq) {
      ack_it = it;
      break;
    }
  }

  if (ack_it == mac_state_->tx_window.end()) {
    return;
  }

  mac_state_->tx_window.erase(mac_state_->tx_window.begin(), std::next(ack_it));
}

bool RadioInterface::ConsumeDataFrameLocked(const MacFrame& frame) {
  constexpr uint8_t kBondRxReorderWindow = 4;

  if (frame.type != FrameType::Data) {
    return true;
  }

  if (frame.seq == 0) {
    LOGE("DATA frame missing seq");
    ResetRxAssemblyLocked();
    return false;
  }

  const auto seq_distance_forward = [this](uint8_t base, uint8_t seq) -> uint8_t {
    if (seq == 0) {
      return 0xFF;
    }
    if (base == 0) {
      return seq;
    }
    if (seq == base) {
      return 0;
    }
    if (seq > base) {
      return static_cast<uint8_t>(seq - base);
    }
    return static_cast<uint8_t>(kMaxSeq - base + seq);
  };

  const uint8_t delivered_seq = mac_state_->last_rx_seq.value_or(0);
  if (delivered_seq != 0 && frame.seq == delivered_seq) {
    return true;
  }
  if (mac_state_->rx_reorder_buffer.find(frame.seq) != mac_state_->rx_reorder_buffer.end()) {
    return true;
  }

  const uint8_t forward_distance = seq_distance_forward(delivered_seq, frame.seq);
  const uint8_t reverse_distance = delivered_seq == 0 ? 0xFF : seq_distance_forward(frame.seq, delivered_seq);

  if (delivered_seq != 0 && reverse_distance < forward_distance) {
    return true;
  }

  if (forward_distance == 0 || forward_distance > kBondRxReorderWindow) {
    LOGE("Received DATA seq=%u outside reorder window (last=%u dist=%u)",
         frame.seq,
         delivered_seq,
         forward_distance);
    return false;
  }

  mac_state_->rx_reorder_buffer.emplace(
      frame.seq,
      RxFragmentState{frame.arg, frame.payload});

  while (true) {
    const uint8_t next_expected = NextSeq(mac_state_->last_rx_seq.value_or(0));
    const auto it = mac_state_->rx_reorder_buffer.find(next_expected);
    if (it == mac_state_->rx_reorder_buffer.end()) {
      break;
    }

    if (!it->second.payload.empty()) {
      mac_state_->frame_buffer.insert(mac_state_->frame_buffer.end(),
                           it->second.payload.begin(),
                           it->second.payload.end());
      if (it->second.bytes_left <= kMaxPayloadSize) {
        WriteTunnel();
      }
    }

    mac_state_->last_rx_seq = next_expected;
    mac_state_->rx_reorder_buffer.erase(it);
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
    LOGI("Writing %zu bytes to tunnel", mac_state_->frame_buffer.size());
  }

  if (!IsValidIpPacket(mac_state_->frame_buffer)) {
    LOGE("Dropping invalid reassembled IP packet len=%zu", mac_state_->frame_buffer.size());
    mac_state_->frame_buffer.clear();
    return;
  }

  const int bytes_written =
      write(tunnel_fd_, mac_state_->frame_buffer.data(), mac_state_->frame_buffer.size());

  mac_state_->frame_buffer.clear();

  if (bytes_written < 0) {
    LOGE("Failed to write to tunnel %s (%d)", strerror(errno), errno);
  }
}

}  // namespace nerfnet
