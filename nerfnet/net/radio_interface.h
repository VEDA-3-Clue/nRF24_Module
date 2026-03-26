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

#ifndef NERFNET_NET_RADIO_INTERFACE_H_
#define NERFNET_NET_RADIO_INTERFACE_H_

#include <RF24/RF24.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "nerfnet/util/non_copyable.h"

namespace nerfnet {

// Common MAC/radio interface used by both coordinator and peer.
class RadioInterface : public NonCopyable {
 public:
  struct RadioConfig {
    rf24_pa_dbm_e pa_level = RF24_PA_MAX;
    rf24_datarate_e data_rate = RF24_2MBPS;
    rf24_crclength_e crc_length = RF24_CRC_8;
    uint8_t retry_delay = 0;
    uint8_t retry_count = 15;
  };

  RadioInterface(uint16_t ce_pin,
                 int tunnel_fd,
                 uint32_t primary_addr,
                 uint32_t secondary_addr,
                 uint8_t channel,
                 const RadioConfig& radio_config,
                 int irq_pin = -1);
  virtual ~RadioInterface();

  enum class RequestResult {
    Success,
    Timeout,
    Malformed,
    TransmitError,
  };

  enum class FrameType : uint8_t {
    Invalid = 0,
    Pending = 1,  // "I have queued data"
    Grant   = 2,  // "You may send N DATA frames"
    Data    = 3,  // One RF fragment
    Ack     = 4,  // Pure ACK, no payload
    Reset   = 5,  // Link/session reset
  };

  struct MacFrame {
    FrameType type = FrameType::Invalid;
    uint8_t seq = 0;      // valid only for DATA
    uint8_t ack = 0;      // cumulative ACK for peer DATA
    uint8_t pending = 0;  // number of queued local frames/fragments hint
    uint8_t arg = 0;      // GRANT: credits, DATA: bytes_left
    std::vector<uint8_t> payload;
  };

  void SetTunnelLogsEnabled(bool enabled) { tunnel_logs_enabled_ = enabled; }

 protected:
  static constexpr uint32_t kPollIntervalUs = 1000;
  static constexpr size_t kMaxPacketSize = 32;
  static constexpr size_t kHeaderSize = 5;
  static constexpr size_t kMaxPayloadSize = kMaxPacketSize - kHeaderSize;
  static constexpr uint8_t kPipeId = 1;

  static constexpr uint8_t kNoSeq = 0;
  static constexpr uint8_t kMaxSeq = 31;

  RF24 radio_;
  const int tunnel_fd_;
  const uint32_t primary_addr_;
  const uint32_t secondary_addr_;
  const RadioConfig radio_config_;

  std::atomic<bool> running_;
  std::thread tunnel_thread_;

  std::mutex read_buffer_mutex_;
  std::deque<std::vector<uint8_t>> read_buffer_;
  std::vector<uint8_t> frame_buffer_;

  // TX state
  uint8_t next_tx_seq_;
  bool tx_in_flight_;
  uint8_t tx_inflight_seq_;
  uint8_t tx_inflight_bytes_left_;
  std::vector<uint8_t> tx_inflight_payload_;

  // RX ACK state
  std::optional<uint8_t> last_rx_seq_;

  bool tunnel_logs_enabled_;
  const int irq_pin_;
  int irq_fd_;
  int resolved_irq_gpio_;

  RequestResult Send(const std::vector<uint8_t>& request);
  RequestResult Receive(std::vector<uint8_t>& response, uint64_t timeout_us = 0);

  // Raw queue helpers.
  size_t GetReadBufferSize();
  size_t GetTransferSize(const std::vector<uint8_t>& frame);
  uint8_t PendingHintLocked() const;

  // Sequence helpers.
  void AdvanceTxSeq();
  bool IsExpectedRxSeq(uint8_t seq) const;
  uint8_t NextSeq(uint8_t seq) const;
  void ResetRxAssemblyLocked();

  // Thread that reads from TUN and buffers complete IP frames.
  void TunnelThread();

  // Frame codec.
  bool EncodeMacFrame(const MacFrame& frame, std::vector<uint8_t>& packet);
  bool DecodeMacFrame(const std::vector<uint8_t>& packet, MacFrame& frame);

  // TX fragment helpers. Caller must hold read_buffer_mutex_.
  bool BuildNextDataFrameLocked(MacFrame& frame);
  void CommitAckLocked(uint8_t ack_seq);

  // RX DATA handling. Caller must hold read_buffer_mutex_.
  bool ConsumeDataFrameLocked(const MacFrame& frame);

  // Flush current reassembled IP frame to TUN.
  void WriteTunnel();

  bool IsValidIpPacket(const std::vector<uint8_t>& packet) const;
  bool InitializeIrq();
  void CleanupIrq();
  RequestResult WaitForRxReady(uint64_t timeout_us);
  bool WriteSysfsFile(const std::string& path, const std::string& value);
  bool ExportIrqGpio(int gpio);
  std::optional<int> ResolveIrqGpio() const;
};

}  // namespace nerfnet

#endif  // NERFNET_NET_RADIO_INTERFACE_H_