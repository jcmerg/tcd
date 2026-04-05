#pragma once

// tcd - a hybid transcoder using DVSI hardware and Codec2 software
// Copyright © 2021 Thomas A. Early N7TAE
// Copyright © 2021 Doug McLain AD8DP

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <unordered_map>
#include <memory>
#include <atomic>
#include <future>
#include <mutex>
#include <list>
#include <utility>
#include <imbe_vocoder_api.h>

#include "codec2.h"
#include "DV3000.h"
#include "DV3003.h"
#include "TCSocket.h"
#include "AGC.h"

class CController
{
public:
	std::mutex dstar_mux, dmrst_mux;

	CController();
	bool Start();
	void Stop();
	void ListDevices();
	void RouteDstPacket(std::shared_ptr<CTranscoderPacket> packet);
	void RouteDmrPacket(std::shared_ptr<CTranscoderPacket> packet);
	void ReconfigureAGC();  // re-read AGC params from g_Stats.config
	void Dump(const std::shared_ptr<CTranscoderPacket> packet, const std::string &title) const;

protected:
	std::atomic<bool> keep_running;
	std::future<void> reflectorFuture, c2Future, imbeFuture, usrpFuture;
	std::unordered_map<char, int16_t[160]> audio_store;
	std::unordered_map<char, uint8_t[8]> data_store;
	CTCClient tcClient;
	std::unordered_map<char, std::unique_ptr<CCodec2>> c2_16, c2_32;
	std::unique_ptr<CDVDevice> dstar_device, dmrsf_device;
	bool mixed_mode = false;
	char mixed_dstar_module = ' ';
	CDV3003 *mixed_dv3003 = nullptr;  // cached pointer, owned by dmrsf_device

	CPacketQueue codec2_queue;

	CPacketQueue imbe_queue;
	CPacketQueue usrp_queue;
	std::mutex send_mux;
	std::mutex p25vocoder_mux;
#ifdef WITH_MD380_VOCODER
	std::mutex md380_mux;
	uint16_t md380_enc_streamid = 0;
	struct Md380CacheEntry { std::vector<uint8_t> state; std::chrono::steady_clock::time_point last_used; };
	std::unordered_map<uint16_t, Md380CacheEntry> md380_state_cache;
#endif
	int32_t ambe_in_num, ambe_out_num, usrp_rx_num, usrp_tx_num, dmr_reencode_num;
	int32_t outgain_dstar_num, outgain_dmr_num, outgain_usrp_num, outgain_imbe_num, outgain_m17_num;
	imbe_vocoder p25vocoder;
	CAGC m_agc;

	int32_t calcNumerator(int32_t db) const;
	static void ApplyGain(int16_t *samples, int count, int32_t num);
	bool DiscoverFtdiDevices(std::list<std::pair<std::string, std::string>> &found);
	bool IsAmbeDevice(const std::string &desc);
	void UnbindAllFtdiSio();
	bool InitVocoders();
	// processing threads
	void ReadReflectorThread();
	void ProcessC2Thread();

	void ProcessIMBEThread();
	void ProcessUSRPThread();
	void Codec2toAudio(std::shared_ptr<CTranscoderPacket> packet);
	void AudiotoCodec2(std::shared_ptr<CTranscoderPacket> packet);
	void IMBEtoAudio(std::shared_ptr<CTranscoderPacket> packet);
	void AudiotoIMBE(std::shared_ptr<CTranscoderPacket> packet);
	void USRPtoAudio(std::shared_ptr<CTranscoderPacket> packet);
	void AudiotoUSRP(std::shared_ptr<CTranscoderPacket> packet);
	void SvxToAudio(std::shared_ptr<CTranscoderPacket> packet);
	void SendToReflector(std::shared_ptr<CTranscoderPacket> packet);
	std::future<void> swambe2Future;
	CPacketQueue swambe2_queue;
	void ProcessSWAMBE2Thread();
	void SWAMBE2toAudio(std::shared_ptr<CTranscoderPacket> packet);
	void AudiotoSWAMBE2(std::shared_ptr<CTranscoderPacket> packet);
};
