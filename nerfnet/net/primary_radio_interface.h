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

#ifndef NERFNET_NET_PRIMARY_RADIO_INTERFACE_H_
#define NERFNET_NET_PRIMARY_RADIO_INTERFACE_H_

#include <algorithm>
#include <cstdint>

#include "nerfnet/net/radio_interface.h"

namespace nerfnet {

class PrimaryRadioInterface : public RadioInterface {
public:
  PrimaryRadioInterface(uint16_t ce_pin,
                        int tunnel_fd,
                        uint32_t primary_addr,
                        uint32_t secondary_addr,
                        uint8_t channel,
                        uint64_t poll_interval_us,
                        const RadioConfig& radio_config);

  void Run();

private:
  enum class CoordinatorState {
    ResetSync,
    Idle,
    IssueGrant,
    ActiveRx,
  };

  static constexpr uint8_t kDefaultBurstGrant = 3;
  static constexpr uint64_t kIdlePollIntervalUs = 20000;
  static constexpr uint64_t kActivePollIntervalUs = 1000;

  static constexpr int kDisconnectFailureThreshold = 5;
  static constexpr uint64_t kInitialDisconnectBackoffUs = 100000;   // 100 ms
  static constexpr uint64_t kMaxDisconnectBackoffUs = 1000000;      // 1 s
  static constexpr uint64_t kDisconnectedSleepSliceUs = 1000;       // 1 ms

  uint8_t peer_grant_budget_;

  uint64_t tx_pending_count_ = 0;
  uint64_t tx_grant_count_ = 0;
  uint64_t tx_data_count_ = 0;
  uint64_t rx_ack_count_ = 0;
  uint64_t rx_pending_count_ = 0;
  uint64_t rx_data_count_ = 0;
  uint64_t tx_send_fail_count_ = 0;
  uint64_t rx_timeout_count_ = 0;

  const uint64_t poll_interval_us_;
  uint64_t current_poll_interval_us_;
  int poll_fail_count_;

  bool disconnected_ = false;
  uint64_t disconnect_backoff_us_ = kInitialDisconnectBackoffUs;
  uint64_t next_retry_time_us_ = 0;
  uint64_t reset_fail_log_counter_ = 0;

  CoordinatorState state_;
  bool connection_reset_required_;

  bool peer_has_pending_;
  bool last_tx_was_data_;

  uint64_t idle_log_counter_ = 0;
  uint64_t stat_loop_counter_ = 0;
  uint64_t fail_log_counter_ = 0;
  uint64_t last_stat_print_us_ = 0;

  bool ConnectionReset();
  bool PerformExchange();
  void HandleTransactionFailure();

  bool ChooseCoordinatorTxFrame(MacFrame& tx);
  bool ApplyPeerResponse(const MacFrame& rx);
};

}  // namespace nerfnet

#endif