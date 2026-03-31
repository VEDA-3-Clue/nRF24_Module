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
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "nerfnet/util/non_copyable.h"

struct gpiod_chip;
struct gpiod_line;

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

  struct TxFragmentState {
    uint8_t seq = 0;
    uint8_t bytes_left = 0;
    std::vector<uint8_t> payload;
    uint64_t last_send_us = 0;
    uint32_t send_count = 0;
    uint8_t preferred_link = 0xFF;
    uint8_t last_tx_link = 0xFF;
    uint8_t duplicate_ack_count = 0;
    bool is_control = false;
  };

  struct LinkQualityState {
    int score = 1000;
    uint32_t send_successes = 0;
    uint32_t send_failures = 0;
    uint32_t receive_timeouts = 0;
    uint32_t consecutive_failures = 0;
    uint64_t last_success_us = 0;
    uint64_t last_failure_us = 0;
    uint64_t last_data_send_us = 0;
  };

  struct RxFragmentState {
    uint8_t bytes_left = 0;
    std::vector<uint8_t> payload;
  };

  struct SharedMacState {
    std::mutex read_buffer_mutex;
    std::deque<std::vector<uint8_t>> read_buffer;
    std::deque<std::vector<uint8_t>> control_read_buffer;
    std::vector<uint8_t> frame_buffer;
    uint8_t next_tx_seq = 1;
    std::deque<TxFragmentState> tx_window;
    std::optional<uint8_t> last_rx_seq;
    std::map<uint8_t, RxFragmentState> rx_reorder_buffer;
    std::vector<LinkQualityState> link_states;
  };

  RadioInterface(uint16_t ce_pin,
                 uint16_t csn_pin,
                 int tunnel_fd,
                 uint32_t primary_addr,
                 uint32_t secondary_addr,
                 uint8_t channel,
                 const RadioConfig& radio_config,
                 int irq_pin = -1,
                 std::shared_ptr<SharedMacState> shared_mac_state = nullptr);
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
  void EnqueueTunnelPacket(std::vector<uint8_t> packet);

 protected:
  static constexpr uint32_t kPollIntervalUs = 1000;
  static constexpr uint32_t kTxStandbyTimeoutUs = 20000;
  static constexpr uint32_t kTxFailureRecoveryGapUs = 200;
  static constexpr size_t kMaxPacketSize = 32;
  static constexpr size_t kHeaderSize = 5;
  static constexpr size_t kMaxPayloadSize = kMaxPacketSize - kHeaderSize;
  static constexpr uint8_t kPipeId = 1;

  static constexpr uint8_t kNoSeq = 0;
  static constexpr uint8_t kMaxSeq = 31;

  RF24 radio_;
  const uint16_t ce_pin_;
  const uint16_t csn_pin_;
  const int tunnel_fd_;
  const uint32_t primary_addr_;
  const uint32_t secondary_addr_;
  const RadioConfig radio_config_;

  std::atomic<bool> running_;

  std::shared_ptr<SharedMacState> mac_state_;

  bool tunnel_logs_enabled_;
  const int irq_pin_;
  gpiod_chip* irq_chip_;
  gpiod_line* irq_line_;
  std::string resolved_irq_chip_name_;
  unsigned int resolved_irq_line_offset_;
  int resolved_irq_global_gpio_;
  size_t link_index_;
  FrameType last_tx_frame_type_;

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
  void EnsureLinkStateLocked();
  int GetLinkScoreLocked(size_t link_index) const;
  bool CanCurrentLinkOriginateLocked(uint64_t now_us) const;
  bool CanCurrentLinkSendFragmentLocked(const TxFragmentState& fragment, uint64_t now_us) const;
  void MarkFragmentSentLocked(TxFragmentState& fragment, uint64_t now_us);

  // Frame codec.
  bool EncodeMacFrame(const MacFrame& frame, std::vector<uint8_t>& packet);
  bool DecodeMacFrame(const std::vector<uint8_t>& packet, MacFrame& frame);

  // TX fragment helpers. Caller must hold read_buffer_mutex_.
  bool BuildNextDataFrameLocked(MacFrame& frame);
  void CommitAckLocked(uint8_t ack_seq);
  void TrimQueuedPacketsForLowLatency(size_t max_control_frames,
                                      size_t max_bulk_frames,
                                      bool drop_control_fragments);

  // RX DATA handling. Caller must hold read_buffer_mutex_.
  bool ConsumeDataFrameLocked(const MacFrame& frame);

  // Flush current reassembled IP frame to TUN.
  void WriteTunnel();

  bool IsValidIpPacket(const std::vector<uint8_t>& packet) const;
  bool InitializeIrq();
  void CleanupIrq();
  RequestResult WaitForRxReady(uint64_t timeout_us);
  void RecoverAfterTransmitFailure(const char* stage);
  void RecordLinkSuccess();
  void RecordLinkFailure(bool timeout_failure);
};

}  // namespace nerfnet

#endif  // NERFNET_NET_RADIO_INTERFACE_H_
