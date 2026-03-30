#pragma once

// tcd - a hybid transcoder using DVSI hardware and Codec2 software
// Copyright © 2022 Thomas A. Early N7TAE
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "DVSIDevice.h"
#include <unordered_map>

class CDV3003 : public CDVDevice
{
public:
	CDV3003(Encoding t);
	virtual ~CDV3003();

	// Mixed-mode: set per-channel encoding (call before OpenDevice)
	void SetChannelEncoding(uint8_t channel, Encoding enc);

	// Map a module letter to a specific channel per codec type
	void SetModuleDStarChannel(char module, uint8_t channel);
	void SetModuleDMRChannel(char module, uint8_t channel);

	// Add packet targeting a specific channel (avoids ambiguous routing)
	void AddPacketToChannel(const std::shared_ptr<CTranscoderPacket> packet, uint8_t channel);

protected:
	void PushWaitingPacket(unsigned int channel, std::shared_ptr<CTranscoderPacket> packet);
	std::shared_ptr<CTranscoderPacket> PopWaitingPacket(unsigned int channel);
	void ProcessPacket(const SDV_Packet &p);
	bool SendAudio(const uint8_t channel, const int16_t *audio) const;
	bool SendData(const uint8_t channel, const uint8_t *data) const;

	// Override: use per-channel encoding and module mapping
	Encoding GetChannelEncoding(uint8_t channel) const override;
	uint8_t MapPacketToChannel(const std::shared_ptr<CTranscoderPacket> &packet) const override;

private:
	CPacketQueue waiting_packet[3];
	Encoding channel_enc[3];
	std::unordered_map<char, uint8_t> dstar_channel_map;
	std::unordered_map<char, uint8_t> dmr_channel_map;
	mutable std::unordered_map<CTranscoderPacket*, uint8_t> target_channel;
	mutable std::mutex target_mutex;
};
