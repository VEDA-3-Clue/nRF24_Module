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

#ifndef NERFNET_NET_SECONDARY_RADIO_INTERFACE_H_
#define NERFNET_NET_SECONDARY_RADIO_INTERFACE_H_

#include "nerfnet/net/radio_interface.h"

namespace nerfnet {

class SecondaryRadioInterface : public RadioInterface {
 public:
  SecondaryRadioInterface(uint16_t ce_pin,
                          int tunnel_fd,
                          uint32_t primary_addr,
                          uint32_t secondary_addr,
                          uint8_t channel,
                          const RadioConfig& radio_config);

  void Run();

 private:
  uint8_t grant_budget_;
  bool granted_to_send_;
  uint64_t idle_log_counter_ = 0;

  bool HandleReset();
  bool ApplyCoordinatorRequest(const MacFrame& request);
  bool ChoosePeerResponse(MacFrame& response);

  void LogPeerRx(const MacFrame& rx);
  void LogPeerTx(const MacFrame& tx);
};

}  // namespace nerfnet

#endif