#pragma once

#include "DVSIDevice.h"
#include <unordered_map>

class CDV3003 : public CDVDevice
{
public:
	CDV3003(Encoding t);
	virtual ~CDV3003();

	// Mixed-mode: set per-channel encoding (call after OpenDevice, before Start)
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
	std::unordered_map<CTranscoderPacket*, uint8_t> target_channel;  // explicit channel override
	std::mutex target_mutex;
};
