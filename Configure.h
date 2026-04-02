/*
 *   Copyright (c) 2023 by Thomas A. Early N7TAE
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <regex>

enum class EGainType { dmrin, dmrout, dstarin, dstarout, usrptx, usrprx, dmrreencode, output };

#define IS_TRUE(a) ((a)=='t' || (a)=='T' || (a)=='1')

class CConfigure
{
public:
	bool ReadData(const std::string &path);
	int GetGain(EGainType gt) const;
	std::string GetTCMods(void) const { return tcmods; }
	std::string GetAddress(void) const { return address; }
	unsigned GetPort(void) const { return port; }
	const std::vector<std::string> &GetDeviceSerials(void) const { return device_serials; }

	// AGC
	bool  GetAGCEnabled(void) const    { return agc_enabled; }
	float GetAGCTarget(void) const     { return agc_target; }
	float GetAGCAttack(void) const     { return agc_attack; }
	float GetAGCRelease(void) const    { return agc_release; }
	float GetAGCMaxGainUp(void) const  { return agc_maxgain_up; }
	float GetAGCMaxGainDown(void) const { return agc_maxgain_down; }
	float GetAGCNoiseGate(void) const   { return agc_noisegate; }

	// Monitor
	uint16_t GetMonitorHttpPort(void) const  { return monitor_http_port; }
	uint16_t GetMonitorStatsPort(void) const { return monitor_stats_port; }
	bool     GetMonitorEnabled(void) const   { return monitor_enabled; }
	const std::string &GetIniPath(void) const { return ini_path; }

	// Stats logging
	bool               GetStatsLogEnabled(void) const { return statslog_enabled; }
	const std::string &GetStatsLogDir(void) const     { return statslog_dir; }
	int                GetStatsLogRetain(void) const   { return statslog_retain; }

private:
	std::string tcmods, address, ini_path;
	std::vector<std::string> device_serials;
	uint16_t port;
	int dstar_in, dstar_out, dmr_in, dmr_out, usrp_tx, usrp_rx, dmr_reencode, output_gain;
	bool  agc_enabled = false;
	float agc_target  = -16.0f;
	float agc_attack  = 50.0f;
	float agc_release = 500.0f;
	float agc_maxgain_up   = 20.0f;   // max amplification (dB)
	float agc_maxgain_down = 24.0f;   // max attenuation (dB)
	float agc_noisegate    = -55.0f;  // noise gate threshold (dBFS)
	bool     monitor_enabled    = true;
	uint16_t monitor_http_port  = 8080;
	uint16_t monitor_stats_port = 8081;
	bool        statslog_enabled = false;
	std::string statslog_dir     = "/tmp/tcd-stats";
	int         statslog_retain  = 24;

	int getSigned(const std::string &key, const std::string &value) const;
	void badParam(const std::string &key) const;
};
