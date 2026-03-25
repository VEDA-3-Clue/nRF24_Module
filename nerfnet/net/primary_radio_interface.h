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

#include <cstdint>

#include "nerfnet/net/radio_interface.h"

namespace nerfnet {

// Coordinator-side scheduler.
// File name stays the same to minimize build churn.
class PrimaryRadioInterface : public RadioInterface {
 public:
  PrimaryRadioInterface(uint16_t ce_pin,
                        int tunnel_fd,
                        uint32_t primary_addr,
                        uint32_t secondary_addr,
                        uint8_t channel,
                        uint64_t poll_interval_us);

  void Run();

 private:
  static constexpr uint8_t kDefaultBurstGrant = 4;

  const uint64_t poll_interval_us_;
  int poll_fail_count_ = 0;
  uint64_t current_poll_interval_us_;
  bool connection_reset_required_;

  uint8_t peer_pending_hint_ = 0;
  uint8_t peer_grant_remaining_ = 0;

  bool ConnectionReset();
  bool PerformExchange();
  void HandleTransactionFailure();
};

}  // namespace nerfnet

#endif  // NERFNET_NET_PRIMARY_RADIO_INTERFACE_H_