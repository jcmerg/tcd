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

#include <algorithm>
#include <iostream>
#include <fstream>
#include <vector>
#include <sstream>
#include <regex>
#include "Configure.h"

// ini file keywords
#define USRPTXGAIN     "UsrpGainOut"
#define USRPRXGAIN     "UsrpGainIn"
#define DMRGAININ      "DmrGainIn"
#define DMRGAINOUT     "DmrGainOut"
#define DSTARGAININ    "DStarGainIn"
#define DSTARGAINOUT   "DStarGainOut"
#define DMRREENCODE    "DmrReencodeGain"
#define OUTPUTGAIN     "OutputGain"
#define MODULES        "Modules"
#define SERVERADDRESS  "ServerAddress"
#define PORT           "Port"
#define DEVICESERIAL   "DeviceSerial"
#define AGCENABLE      "AGC"
#define AGCTARGET      "AGCTarget"
#define AGCATTACK      "AGCAttack"
#define AGCRELEASE     "AGCRelease"
#define AGCMAXGAIN     "AGCMaxGain"
#define AGCMAXGAINUP   "AGCMaxGainUp"
#define AGCMAXGAINDOWN "AGCMaxGainDown"
#define AGCNOISEGATE   "AGCNoiseGate"
#define MONITORENABLE  "Monitor"
#define MONITORHTTP    "MonitorHttpPort"
#define MONITORSTATS   "MonitorStatsPort"
#define STATSLOG       "StatsLog"
#define STATSLOGDIR    "StatsLogDir"
#define STATSLOGRETAIN "StatsLogRetain"

static inline void split(const std::string &s, char delim, std::vector<std::string> &v)
{
	std::istringstream iss(s);
	std::string item;
	while (std::getline(iss, item, delim))
		v.push_back(item);
}

// trim from start (in place)
static inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
        return !std::isspace(ch);
    }));
}

// trim from end (in place)
static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

// trim from both ends (in place)
static inline void trim(std::string &s) {
    ltrim(s);
    rtrim(s);
}

bool CConfigure::ReadData(const std::string &path)
// returns true on failure
{
	ini_path = path;
	std::regex IPv4RegEx = std::regex("^((25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])\\.){3,3}(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9]){1,1}$", std::regex::extended);
	std::regex IPv6RegEx = std::regex("^(([0-9a-fA-F]{1,4}:){7,7}[0-9a-fA-F]{1,4}|([0-9a-fA-F]{1,4}:){1,7}:|([0-9a-fA-F]{1,4}:){1,6}(:[0-9a-fA-F]{1,4}){1,1}|([0-9a-fA-F]{1,4}:){1,5}(:[0-9a-fA-F]{1,4}){1,2}|([0-9a-fA-F]{1,4}:){1,4}(:[0-9a-fA-F]{1,4}){1,3}|([0-9a-fA-F]{1,4}:){1,3}(:[0-9a-fA-F]{1,4}){1,4}|([0-9a-fA-F]{1,4}:){1,2}(:[0-9a-fA-F]{1,4}){1,5}|([0-9a-fA-F]{1,4}:){1,1}(:[0-9a-fA-F]{1,4}){1,6}|:((:[0-9a-fA-F]{1,4}){1,7}|:))$", std::regex::extended);

	std::string modstmp, porttmp;

	std::ifstream cfgfile(path.c_str(), std::ifstream::in);
	if (! cfgfile.is_open()) {
		std::cerr << "ERROR: '" << path << "' was not found!" << std::endl;
		return true;
	}

	std::string line;
	while (std::getline(cfgfile, line))
	{
		trim(line);
		if (3 > line.size())
			continue;	// can't be anything
		if ('#' == line.at(0))
			continue;	// skip comments

		std::vector<std::string> tokens;
		split(line, '=', tokens);
		// check value for end-of-line comment
		if (2 > tokens.size())
		{
			std::cout << "WARNING: '" << line << "' does not contain an equal sign, skipping" << std::endl;
			continue;
		}
		auto pos = tokens[1].find('#');
		if (std::string::npos != pos)
		{
			tokens[1].assign(tokens[1].substr(0, pos));
			rtrim(tokens[1]); // whitespace between the value and the end-of-line comment
		}
		// trim whitespace from around the '='
		rtrim(tokens[0]);
		ltrim(tokens[1]);
		const std::string key(tokens[0]);
		const std::string value(tokens[1]);
		if (key.empty() || value.empty())
		{
			std::cout << "WARNING: missing key or value: '" << line << "'" << std::endl;
			continue;
		}
		if (0 == key.compare(SERVERADDRESS))
			address.assign(value);
		else if (0 == key.compare(PORT))
			porttmp.assign(value);
		else if (0 == key.compare(MODULES))
			modstmp.assign(value);
		else if (0 == key.compare(DSTARGAININ))
			dstar_in = getSigned(key, value);
		else if (0 == key.compare(DSTARGAINOUT))
			dstar_out = getSigned(key, value);
		else if (0 == key.compare(DMRGAININ))
			dmr_in = getSigned(key, value);
		else if (0 == key.compare(DMRGAINOUT))
			dmr_out = getSigned(key, value);
		else if (0 == key.compare(USRPTXGAIN))
			usrp_tx = getSigned(key, value);
		else if (0 == key.compare(USRPRXGAIN))
			usrp_rx = getSigned(key, value);
		else if (0 == key.compare(DMRREENCODE))
			dmr_reencode = getSigned(key, value);
		else if (0 == key.compare(OUTPUTGAIN))
			output_gain = getSigned(key, value);
		else if (0 == key.compare(DEVICESERIAL))
			device_serials.push_back(value);
		else if (0 == key.compare(AGCENABLE))
			agc_enabled = IS_TRUE(value[0]);
		else if (0 == key.compare(AGCTARGET))
			agc_target = std::stof(value);
		else if (0 == key.compare(AGCATTACK))
			agc_attack = std::stof(value);
		else if (0 == key.compare(AGCRELEASE))
			agc_release = std::stof(value);
		else if (0 == key.compare(AGCMAXGAIN))
		{
			// Legacy: single value sets both up and down
			agc_maxgain_up = std::stof(value);
			agc_maxgain_down = std::stof(value);
		}
		else if (0 == key.compare(AGCMAXGAINUP))
			agc_maxgain_up = std::stof(value);
		else if (0 == key.compare(AGCMAXGAINDOWN))
			agc_maxgain_down = std::stof(value);
		else if (0 == key.compare(AGCNOISEGATE))
			agc_noisegate = std::stof(value);
		else if (0 == key.compare(MONITORENABLE))
			monitor_enabled = IS_TRUE(value[0]);
		else if (0 == key.compare(MONITORHTTP))
			monitor_http_port = (uint16_t)std::stoul(value);
		else if (0 == key.compare(MONITORSTATS))
			monitor_stats_port = (uint16_t)std::stoul(value);
		else if (0 == key.compare(STATSLOG))
			statslog_enabled = IS_TRUE(value[0]);
		else if (0 == key.compare(STATSLOGDIR))
			statslog_dir = value;
		else if (0 == key.compare(STATSLOGRETAIN))
			statslog_retain = std::stoi(value);
		else
			badParam(key);
	}
	cfgfile.close();

	for (auto c : modstmp)
	{
		if (isalpha(c))
		{
			if (islower(c))
				c = toupper(c);
			if (std::string::npos == tcmods.find(c))
				tcmods.append(1, c);
		}
	}
	if (tcmods.empty())
	{
		std::cerr << "ERROR: no identifable module letters in '" << modstmp << "'. Halt." << std::endl;
		return true;
	}

	if (! std::regex_match(address, IPv4RegEx) && ! std::regex_match(address, IPv6RegEx))
	{
		std::cerr << "ERROR: '" << address << "' is malformed, Halt." << std::endl;
		return true;
	}

	port = std::strtoul(porttmp.c_str(), nullptr, 10);
	if (port < 1025 || port > 49000)
	{
		std::cerr << "ERROR: Port '" << porttmp << "' must be between >1024 and <49000. Halt." << std::endl;
		return true;
	}

	std::cout << MODULES << " = " << tcmods << std::endl;
	std::cout << SERVERADDRESS << " = " << address << std::endl;
	std::cout << PORT << " = " << port << std::endl;
	std::cout << DSTARGAININ << " = " << dstar_in << std::endl;
	std::cout << DSTARGAINOUT << " = " << dstar_out << std::endl;
	std::cout << DMRGAININ << " = " << dmr_in << std::endl;
	std::cout << DMRGAINOUT << " = " << dmr_out << std::endl;
	std::cout << USRPTXGAIN << " = " << usrp_tx << std::endl;
	std::cout << USRPRXGAIN << " = " << usrp_rx << std::endl;
	if (dmr_reencode != 0)
		std::cout << DMRREENCODE << " = " << dmr_reencode << std::endl;
	if (output_gain != 0)
		std::cout << OUTPUTGAIN << " = " << output_gain << std::endl;
	if (agc_enabled)
		std::cout << "AGC = enabled, Target=" << agc_target << "dBFS, Attack=" << agc_attack << "ms, Release=" << agc_release << "ms, Up=+" << agc_maxgain_up << "dB, Down=-" << agc_maxgain_down << "dB" << std::endl;
	else
		std::cout << "AGC = disabled" << std::endl;
	if (monitor_enabled)
		std::cout << "Monitor = HTTP:" << monitor_http_port << " Stats:" << monitor_stats_port << std::endl;

	return false;
}

int CConfigure::getSigned(const std::string &key, const std::string &value) const
{
	auto i = std::stoi(value.c_str());
	if (i < -40)
	{
		std::cout << "WARNING: " << key << " = " << value << " is too low. Limit to -40!" << std::endl;
		i = -40;
	}
	else if (i > 40)
	{
		std::cout << "WARNING: " << key << " = " << value << " is too high. Limit to 40!" << std::endl;
		i = 40;
	}
	return i;
}

void CConfigure::badParam(const std::string &key) const
{
	std::cout << "WARNING: Unexpected parameter: '" << key << "'" << std::endl;
}

int CConfigure::GetGain(EGainType gt) const
{
	switch (gt)
	{
		case EGainType::dmrin:    return dmr_in;
		case EGainType::dmrout:   return dmr_out;
		case EGainType::dstarin:  return dstar_in;
		case EGainType::dstarout: return dstar_out;
		case EGainType::usrptx:   return usrp_tx;
		case EGainType::usrprx:      return usrp_rx;
		case EGainType::dmrreencode: return dmr_reencode;
		case EGainType::output:      return output_gain;
		default:                     return 0;
	}
}
