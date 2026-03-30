/*
 *   Copyright (C) 2014 by Jonathan Naylor G4KLX and John Hays K7VE
 *   Copyright 2016 by Jeremy McDermond (NH6Z)
 *   Copyright 2021 by Thomas Early N7TAE
 *
 *   Adapted by K7VE from G4KLX dv3000d
 */

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

#include <sys/select.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <cerrno>
#include <thread>

#include "DV3003.h"
#include "Configure.h"
#include "Controller.h"

extern CController g_Cont;

CDV3003::CDV3003(Encoding t) : CDVDevice(t)
{
	// Default: all channels same as device type
	for (int i = 0; i < 3; i++)
		channel_enc[i] = t;
}

CDV3003::~CDV3003()
{
	for (int i=0; i<3; i++)
		waiting_packet[i].Shutdown();
}

void CDV3003::SetChannelEncoding(uint8_t channel, Encoding enc)
{
	if (channel < 3)
		channel_enc[channel] = enc;
}

void CDV3003::SetModuleDStarChannel(char module, uint8_t channel)
{
	dstar_channel_map[module] = channel;
}

void CDV3003::SetModuleDMRChannel(char module, uint8_t channel)
{
	dmr_channel_map[module] = channel;
}

void CDV3003::AddPacketToChannel(const std::shared_ptr<CTranscoderPacket> packet, uint8_t channel)
{
	{
		std::lock_guard<std::mutex> lock(target_mutex);
		target_channel[packet.get()] = channel;
	}
	AddPacket(packet);
}

Encoding CDV3003::GetChannelEncoding(uint8_t channel) const
{
	return (channel < 3) ? channel_enc[channel] : type;
}

uint8_t CDV3003::MapPacketToChannel(const std::shared_ptr<CTranscoderPacket> &packet) const
{
	// Determine if this packet needs a D-Star or DMR channel on this DV3003.
	// FeedDevice will check: for D-Star channel, DStarIsSet → decode, else encode.
	//                        for DMR channel, DMRIsSet → decode, else encode.
	//
	// If DStarIsSet → packet has D-Star AMBE data → route to D-Star channel for decode
	// If DMRIsSet but !DStarIsSet → packet has DMR data → route to DMR channel for decode
	// If neither is set for the target codec → packet has PCM → route for encode
	//
	// The Controller sends to this device for a specific purpose:
	//   - ReadReflectorThread sends D-Star packets here (mixed_dstar_module) → D-Star channel
	//   - ReadReflectorThread sends DMR packets here → DMR channel
	//   - RouteDstPacket sends decoded D-Star (has PCM) for DMR encode → DMR channel
	//   - RouteDmrPacket sends decoded DMR (has PCM) for D-Star encode → D-Star channel
	//
	// Key insight: if DStarIsSet, we need a D-Star channel. If DMRIsSet, we need a DMR channel.
	// If neither: the packet was decoded elsewhere and needs encoding.
	//   codec_in tells us the original source, so the target is the OTHER codec.

	char mod = packet->GetModule();
	// Check explicit channel override (set by AddPacketToChannel)
	{
		std::lock_guard<std::mutex> lock(target_mutex);
		auto it = target_channel.find(packet.get());
		if (it != target_channel.end())
		{
			uint8_t ch = it->second;
			target_channel.erase(it);
			return ch;
		}
	}

	// Fallback: route to the channel for the codec that is NOT yet set
	bool use_dstar = !packet->DStarIsSet();

	if (use_dstar)
	{
		auto it = dstar_channel_map.find(mod);
		if (it != dstar_channel_map.end())
			return it->second;
		// Fallback: first D-Star channel
		for (uint8_t ch = 0; ch < 3; ch++)
			if (channel_enc[ch] == Encoding::dstar) return ch;
	}
	else
	{
		auto it = dmr_channel_map.find(mod);
		if (it != dmr_channel_map.end())
			return it->second;
		// Fallback: first DMR channel
		for (uint8_t ch = 0; ch < 3; ch++)
			if (channel_enc[ch] == Encoding::dmrsf) return ch;
	}
	return 0xFF;
}

void CDV3003::PushWaitingPacket(unsigned int channel, std::shared_ptr<CTranscoderPacket> packet)
{
	waiting_packet[channel].push(packet);
}

std::shared_ptr<CTranscoderPacket> CDV3003::PopWaitingPacket(unsigned int channel)
{
	return waiting_packet[channel].pop();
}

bool CDV3003::SendAudio(const uint8_t channel, const int16_t *audio) const
{
	SDV_Packet p;
	p.start_byte = PKT_HEADER;
	p.header.payload_length = htons(1 + sizeof(p.payload.audio));
	p.header.packet_type = PKT_SPEECH;
	p.field_id = channel + PKT_CHANNEL0;
	p.payload.audio.speechd = PKT_SPEECHD;
	p.payload.audio.num_samples = 160U;
	for (int i=0; i<160; i++)
		p.payload.audio.samples[i] = htons(audio[i]);

	const DWORD size = packet_size(p);
	DWORD written;
	auto status = FT_Write(ftHandle, &p, size, &written);
	if (FT_OK != status)
	{
		FTDI_Error("Error writing audio packet", status);
		return true;
	}
	else if (size != written)
	{
		std::cerr << "Incomplete Speech Packet write on " << description << std::endl;
		return true;
	}
	return false;
}

bool CDV3003::SendData(const uint8_t channel, const uint8_t *data) const
{
	SDV_Packet p;
	p.start_byte = PKT_HEADER;
	p.header.payload_length = htons(1 + sizeof(p.payload.ambe));
	p.header.packet_type = PKT_CHANNEL;
	p.field_id = channel + PKT_CHANNEL0;
	p.payload.ambe.chand = PKT_CHAND;
	p.payload.ambe.num_bits = 72U;
	memcpy(p.payload.ambe.data, data, 9);

	const DWORD size = packet_size(p);
	DWORD written;
	auto status = FT_Write(ftHandle, &p, size, &written);
	if (FT_OK != status)
	{
		FTDI_Error("Error writing AMBE Packet", status);
		return true;
	}
	else if (size != written)
	{
		std::cerr << "Incomplete AMBE Packet write on " << description << std::endl;
		return true;
	}
	return false;
}

void CDV3003::ProcessPacket(const SDV_Packet &p)
{
	unsigned int channel = p.field_id - PKT_CHANNEL0;
	auto packet = PopWaitingPacket(channel);
	if (packet)
	{
		Encoding ch_enc = GetChannelEncoding(channel);

		if (PKT_CHANNEL == p.header.packet_type)
		{
			if (12!=ntohs(p.header.payload_length) || PKT_CHAND!=p.payload.ambe.chand || 72!=p.payload.ambe.num_bits)
				dump("Improper ambe packet:", &p, packet_size(p));
			buffer_depth--;
			buffer_cv.notify_one();
			if (Encoding::dstar == ch_enc)
				packet->SetDStarData(p.payload.ambe.data);
			else
				packet->SetDMRData(p.payload.ambe.data);
		}
		else if (PKT_SPEECH == p.header.packet_type)
		{
			if (323!=ntohs(p.header.payload_length) || PKT_SPEECHD!=p.payload.audio.speechd || 160!=p.payload.audio.num_samples)
				dump("Improper audio packet:", &p, packet_size(p));
			buffer_depth--;
			buffer_cv.notify_one();
			packet->SetAudioSamples(p.payload.audio.samples, true);
		}
		else
		{
			dump("ReadDevice() ERROR: Read an unexpected device packet:", &p, packet_size(p));
			return;
		}

		// Route based on per-channel encoding, not device type
		if (Encoding::dstar == ch_enc)
		{
			g_Cont.dstar_mux.lock();
			g_Cont.RouteDstPacket(packet);
			g_Cont.dstar_mux.unlock();
		}
		else
		{
			g_Cont.dmrst_mux.lock();
			g_Cont.RouteDmrPacket(packet);
			g_Cont.dmrst_mux.unlock();
		}
	}
}
