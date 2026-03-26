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

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <RF24/RF24.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <tclap/CmdLine.h>
#include <unistd.h>

#include "nerfnet/net/primary_radio_interface.h"
#include "nerfnet/net/secondary_radio_interface.h"
#include "nerfnet/util/log.h"

namespace {

bool ParseDataRate(const std::string& value, rf24_datarate_e& data_rate) {
  if (value == "2mbps") {
    data_rate = RF24_2MBPS;
    return true;
  }
  if (value == "1mbps") {
    data_rate = RF24_1MBPS;
    return true;
  }
  if (value == "250kbps") {
    data_rate = RF24_250KBPS;
    return true;
  }
  return false;
}

bool ParseCrcLength(const std::string& value, rf24_crclength_e& crc_length) {
  if (value == "8") {
    crc_length = RF24_CRC_8;
    return true;
  }
  if (value == "16") {
    crc_length = RF24_CRC_16;
    return true;
  }
  return false;
}

}  // namespace

// A description of the program.
constexpr char kDescription[] =
    "A tool for creating a network tunnel over cheap NRF24L01 radios.";

// The version of the program.
constexpr char kVersion[] = "0.0.1";

// Sets flags for a given interface. Quits and logs the error on failure.
void SetInterfaceFlags(const std::string_view& device_name, int flags) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0, "Failed to open socket: %s (%d)", strerror(errno), errno);

  struct ifreq ifr = {};
  ifr.ifr_flags = flags;
  strncpy(ifr.ifr_name, std::string(device_name).c_str(), IFNAMSIZ);
  int status = ioctl(fd, SIOCSIFFLAGS, &ifr);
  CHECK(status >= 0, "Failed to set tunnel interface: %s (%d)",
      strerror(errno), errno);
  close(fd);
}

void SetIPAddress(const std::string_view& device_name,
                  const std::string_view& ip, const std::string& ip_mask) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(fd >= 0, "Failed to open socket: %s (%d)", strerror(errno), errno);

  struct ifreq ifr = {};
  strncpy(ifr.ifr_name, std::string(device_name).c_str(), IFNAMSIZ);

  ifr.ifr_addr.sa_family = AF_INET;
  CHECK(inet_pton(AF_INET, std::string(ip).c_str(),
        &reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr)->sin_addr) == 1,
      "Failed to assign IP address: %s (%d)", strerror(errno), errno);
  int status = ioctl(fd, SIOCSIFADDR, &ifr);
  CHECK(status >= 0, "Failed to set tunnel interface ip: %s (%d)",
      strerror(errno), errno);

  ifr.ifr_netmask.sa_family = AF_INET;
  CHECK(inet_pton(AF_INET, std::string(ip_mask).c_str(),
        &reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_netmask)->sin_addr) == 1,
      "Failed to assign IP mask: %s (%d)", strerror(errno), errno);
  status = ioctl(fd, SIOCSIFNETMASK, &ifr);
  CHECK(status >= 0, "Failed to set tunnel interface mask: %s (%d)",
      strerror(errno), errno);
  close(fd);
}

// Opens the tunnel interface to listen on. Always returns a valid file
// descriptor or quits and logs the error.
int OpenTunnel(const std::string_view& device_name) {
  int fd = open("/dev/net/tun", O_RDWR);
  CHECK(fd >= 0, "Failed to open tunnel file: %s (%d)", strerror(errno), errno);

  struct ifreq ifr = {};
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  strncpy(ifr.ifr_name, std::string(device_name).c_str(), IFNAMSIZ);

  int status = ioctl(fd, TUNSETIFF, &ifr);
  CHECK(status >= 0, "Failed to set tunnel interface: %s (%d)",
      strerror(errno), errno);
  return fd;
}

int main(int argc, char** argv) {
  // Parse command-line arguments.
  TCLAP::CmdLine cmd(kDescription, ' ', kVersion);
  TCLAP::ValueArg<std::string> interface_name_arg("i", "interface_name",
      "Set to the name of the tunnel device.", false, "nerf0", "name", cmd);
  TCLAP::ValueArg<uint16_t> ce_pin_arg("", "ce_pin",
      "Set to the index of the NRF24L01 chip-enable pin.", false, 22, "index",
      cmd);
  TCLAP::ValueArg<int> irq_pin_arg("", "irq_pin",
      "Optional IRQ GPIO. Accepts either a sysfs global GPIO number or a line offset such as BCM GPIO 6.", false, -1, "index",
      cmd);
  TCLAP::SwitchArg primary_arg("", "primary",
      "Run this side of the network in primary mode.", false);
  TCLAP::SwitchArg secondary_arg("", "secondary",
      "Run this side of the network in secondary mode.", false);
  TCLAP::ValueArg<std::string> tunnel_ip_arg("", "tunnel_ip",
      "The IP address to assign to the tunnel interface.", false, "", "ip",
      cmd);
  TCLAP::ValueArg<std::string> tunnel_ip_mask("", "tunnel_mask",
      "The network mask to use for the tunnel interface.", false,
      "255.255.255.0", "mask", cmd);
  cmd.xorAdd(primary_arg, secondary_arg);
  TCLAP::ValueArg<uint32_t> primary_addr_arg("", "primary_addr",
      "The address to use for the primary side of nerfnet.",
      false, 0x90019001, "address", cmd);
  TCLAP::ValueArg<uint32_t> secondary_addr_arg("", "secondary_addr",
      "The address to use for the secondary side of nerfnet.",
      false, 0x90009000, "address", cmd);
  TCLAP::ValueArg<uint8_t> channel_arg("", "channel",
      "The channel to use for transmit/receive.", false, 1, "channel", cmd);
  TCLAP::ValueArg<uint32_t> poll_interval_us_arg("", "poll_interval_us",
      "Used by the primary radio only to determine how often to poll.",
      false, 100, "microseconds", cmd);
  TCLAP::SwitchArg enable_tunnel_logs_arg("", "enable_tunnel_logs",
      "Set to enable verbose logs for read/writes from the tunnel.", cmd);
  TCLAP::ValueArg<std::string> rf_data_rate_arg("", "rf_data_rate",
      "NRF24 data rate: 250kbps, 1mbps, or 2mbps.", false, "2mbps", "rate",
      cmd);
  TCLAP::ValueArg<std::string> rf_crc_arg("", "rf_crc",
      "NRF24 CRC length: 8 or 16.", false, "8", "bits", cmd);
  TCLAP::ValueArg<unsigned int> rf_retry_delay_arg("", "rf_retry_delay",
      "NRF24 auto-retry delay (0-15 => 250us to 4000us).", false, 0,
      "ticks", cmd);
  TCLAP::ValueArg<unsigned int> rf_retry_count_arg("", "rf_retry_count",
      "NRF24 auto-retry count (0-15).", false, 15, "count", cmd);
  cmd.parse(argc, argv);

  nerfnet::RadioInterface::RadioConfig radio_config;
  CHECK(ParseDataRate(rf_data_rate_arg.getValue(), radio_config.data_rate),
      "Invalid --rf_data_rate '%s' (expected 250kbps, 1mbps, or 2mbps)",
      rf_data_rate_arg.getValue().c_str());
  CHECK(ParseCrcLength(rf_crc_arg.getValue(), radio_config.crc_length),
      "Invalid --rf_crc '%s' (expected 8 or 16)",
      rf_crc_arg.getValue().c_str());
  CHECK(rf_retry_delay_arg.getValue() <= 15,
      "--rf_retry_delay must be between 0 and 15");
  CHECK(rf_retry_count_arg.getValue() <= 15,
      "--rf_retry_count must be between 0 and 15");
  radio_config.retry_delay = static_cast<uint8_t>(rf_retry_delay_arg.getValue());
  radio_config.retry_count = static_cast<uint8_t>(rf_retry_count_arg.getValue());

  std::string tunnel_ip = tunnel_ip_arg.getValue();
  if (!tunnel_ip_arg.isSet()) {
    if (primary_arg.getValue()) {
      tunnel_ip = "192.168.10.1";
    } else if (secondary_arg.getValue()) {
      tunnel_ip = "192.168.10.2";
    }
  }

  // Setup tunnel.
  int tunnel_fd = OpenTunnel(interface_name_arg.getValue());
  LOGI("tunnel '%s' opened", interface_name_arg.getValue().c_str());
  SetInterfaceFlags(interface_name_arg.getValue(), IFF_UP);
  LOGI("tunnel '%s' up", interface_name_arg.getValue().c_str());
  SetIPAddress(interface_name_arg.getValue(), tunnel_ip,
      tunnel_ip_mask.getValue());
  LOGI("tunnel '%s' configured with '%s' mask '%s'",
       interface_name_arg.getValue().c_str(), tunnel_ip.c_str(),
       tunnel_ip_mask.getValue().c_str());

  if (primary_arg.getValue()) {
    nerfnet::PrimaryRadioInterface radio_interface(
        ce_pin_arg.getValue(), tunnel_fd,
        primary_addr_arg.getValue(), secondary_addr_arg.getValue(),
        channel_arg.getValue(), poll_interval_us_arg.getValue(), radio_config,
        irq_pin_arg.getValue());
    radio_interface.SetTunnelLogsEnabled(enable_tunnel_logs_arg.getValue());
    radio_interface.Run();
  } else if (secondary_arg.getValue()) {
    nerfnet::SecondaryRadioInterface radio_interface(
        ce_pin_arg.getValue(), tunnel_fd,
        primary_addr_arg.getValue(), secondary_addr_arg.getValue(),
        channel_arg.getValue(), radio_config, irq_pin_arg.getValue());
    radio_interface.SetTunnelLogsEnabled(enable_tunnel_logs_arg.getValue());
    radio_interface.Run();
  } else {
    CHECK(false, "Primary or secondary mode must be enabled");
  }

  return 0;
}
