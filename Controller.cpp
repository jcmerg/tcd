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

#include <unistd.h>
#include <cstring>
#include <dirent.h>
#include <sys/select.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <thread>
#include <queue>
#include <vector>
#ifdef WITH_MD380_VOCODER
#include <md380_vocoder.h>
#endif

#include "TranscoderPacket.h"
#include "Controller.h"
#include "Configure.h"
#include "TcdStats.h"
#include "StatsLogger.h"
#include "AmbeGain.h"

extern CTcdStats g_Stats;

extern CConfigure g_Conf;

int32_t CController::calcNumerator(int32_t db) const
{
	float num = 256.0f * powf(10.0f, (float(db)/20.0f));

	return int32_t(roundf(num));
}

void CController::ApplyGain(int16_t *samples, int count, int32_t num)
{
	if (num != 256)
	{
		for (int i = 0; i < count; i++)
		{
			int32_t v = ((int32_t)samples[i] * num) >> 8;
			if (v > 32767) v = 32767;
			else if (v < -32768) v = -32768;
			samples[i] = (int16_t)v;
		}
	}
}

#ifdef WITH_MD380_VOCODER
static void Md380Stat(char module, std::atomic<uint32_t> &counter)
{
	counter.fetch_add(1, std::memory_order_relaxed);
	g_Stats.md380.active_module.store(module, std::memory_order_relaxed);
	g_Stats.md380.last_active_ms.store(
		(uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count(),
		std::memory_order_relaxed);
}
#endif

CController::CController() : keep_running(true), ambe_in_num(256), ambe_out_num(256), usrp_rx_num(256), usrp_tx_num(256), dmr_reencode_num(256), outgain_dstar_num(256), outgain_dmr_num(256), outgain_usrp_num(256), outgain_imbe_num(256), outgain_m17_num(256), outgain_dmr_steps(0) {}

bool CController::Start()
{
	usrp_rx_num = calcNumerator(g_Conf.GetGain(EGainType::usrprx));
	usrp_tx_num = calcNumerator(g_Conf.GetGain(EGainType::usrptx));
	dmr_reencode_num = calcNumerator(g_Conf.GetGain(EGainType::dmrreencode));
	outgain_dstar_num = calcNumerator(g_Conf.GetGain(EGainType::outgain_dstar));
	outgain_dmr_num = calcNumerator(g_Conf.GetGain(EGainType::outgain_dmr));
	outgain_usrp_num = calcNumerator(g_Conf.GetGain(EGainType::outgain_usrp));
	outgain_imbe_num = calcNumerator(g_Conf.GetGain(EGainType::outgain_imbe));
	outgain_m17_num = calcNumerator(g_Conf.GetGain(EGainType::outgain_m17));
	outgain_dmr_steps = g_Conf.GetAmbeGainEnabled() ? g_Conf.GetAmbeGainSteps() : 0;
	m_agc.Configure(g_Conf.GetAGCEnabled(), g_Conf.GetAGCTarget(), g_Conf.GetAGCAttack(), g_Conf.GetAGCRelease(), g_Conf.GetAGCMaxGainUp(), g_Conf.GetAGCMaxGainDown(), g_Conf.GetAGCNoiseGate());

	if (InitVocoders() || tcClient.Open(g_Conf.GetAddress(), g_Conf.GetTCMods(), g_Conf.GetPort()))
	{
		keep_running = false;
		return true;
	}
	if (dstar_device) dstar_device->SetOutputGain(outgain_dstar_num);
	if (dmrsf_device) dmrsf_device->SetOutputGain(outgain_dmr_num);

	reflectorFuture = std::async(std::launch::async, &CController::ReadReflectorThread, this);
	c2Future        = std::async(std::launch::async, &CController::ProcessC2Thread,     this);
	imbeFuture      = std::async(std::launch::async, &CController::ProcessIMBEThread,   this);
	usrpFuture      = std::async(std::launch::async, &CController::ProcessUSRPThread,   this);
	if (!dmrsf_device)
		swambe2Future = std::async(std::launch::async, &CController::ProcessSWAMBE2Thread, this);
	return false;
}

void CController::ReconfigureAGC()
{
	m_agc.Configure(
		g_Stats.config.agc_enabled.load(),
		g_Stats.config.agc_target.load(),
		g_Stats.config.agc_attack.load(),
		g_Stats.config.agc_release.load(),
		g_Stats.config.agc_maxgain_up.load(),
		g_Stats.config.agc_maxgain_down.load(),
		g_Stats.config.agc_noisegate.load()
	);
	// Also update gain numerators from live config
	ambe_in_num = calcNumerator(g_Stats.config.gain_dmr_in.load());
	ambe_out_num = calcNumerator(g_Stats.config.gain_dmr_out.load());
	usrp_rx_num = calcNumerator(g_Stats.config.gain_usrp_rx.load());
	usrp_tx_num = calcNumerator(g_Stats.config.gain_usrp_tx.load());
	dmr_reencode_num = calcNumerator(g_Stats.config.gain_dmr_reencode.load());
	outgain_dstar_num = calcNumerator(g_Stats.config.outgain_dstar.load());
	outgain_dmr_num = calcNumerator(g_Stats.config.outgain_dmr.load());
	outgain_usrp_num = calcNumerator(g_Stats.config.outgain_usrp.load());
	outgain_imbe_num = calcNumerator(g_Stats.config.outgain_imbe.load());
	outgain_m17_num = calcNumerator(g_Stats.config.outgain_m17.load());
	outgain_dmr_steps = g_Stats.config.ambe_gain_enabled.load() ? g_Stats.config.ambe_gain_steps.load() : 0;
	if (dstar_device) dstar_device->SetOutputGain(outgain_dstar_num);
	if (dmrsf_device) dmrsf_device->SetOutputGain(outgain_dmr_num);
}

void CController::Stop()
{
	keep_running = false;

	if (reflectorFuture.valid())
		reflectorFuture.get();
	if (c2Future.valid())
		c2Future.get();

	tcClient.Close();
	if (dstar_device) dstar_device->CloseDevice();
	if (dmrsf_device) dmrsf_device->CloseDevice();
	dstar_device.reset();
	dmrsf_device.reset();
}

void CController::ListDevices()
{
	int iNbDevices = 0;
	auto status = FT_CreateDeviceInfoList((LPDWORD)&iNbDevices);
	if (FT_OK != status)
	{
		std::cerr << "Could not create FTDI device list" << std::endl;
		return;
	}

	if (0 == iNbDevices)
	{
		std::cout << "No USB-FTDI devices detected." << std::endl;
		std::cout << "Tip: If ftdi_sio is loaded, devices may show empty descriptions." << std::endl;
		std::cout << "     The tcd will unbind ftdi_sio automatically at startup." << std::endl;
		return;
	}

	FT_DEVICE_LIST_INFO_NODE *list = new FT_DEVICE_LIST_INFO_NODE[iNbDevices];
	status = FT_GetDeviceInfoList(list, (LPDWORD)&iNbDevices);
	if (FT_OK != status)
	{
		std::cerr << "Could not get FTDI device list" << std::endl;
		delete[] list;
		return;
	}

	std::cout << "Detected " << iNbDevices << " USB-FTDI device(s):" << std::endl;
	for (int i = 0; i < iNbDevices; i++)
	{
		std::cout << "  [" << i << "] Description: " << list[i].Description
		          << "  Serial: " << list[i].SerialNumber
		          << "  USB-ID: " << std::hex << list[i].ID << std::dec
		          << (list[i].Flags & 0x1 ? "  (in use)" : "") << std::endl;
	}
	std::cout << std::endl;
	std::cout << "Use 'DeviceSerial = <serial>' in tcd.ini to select your AMBE device." << std::endl;

	delete[] list;
}

bool CController::IsAmbeDevice(const std::string &desc)
{
	// Known AMBE/DVSI device descriptions
	// Generic FTDI names (FT232R, FT230X) are included because many
	// ThumbDV/DVstick clones report these generic names.
	static const std::vector<std::string> known = {
		"ThumbDV", "DVstick-30", "DVstick-33",
		"USB-3000", "USB-3003", "USB-3006", "USB-3012",
		"FT232R USB UART", "FT230X Basic UART",
		"DF2ET"
	};
	// Devices with empty description are not AMBE devices
	if (desc.empty())
		return false;
	for (const auto &k : known)
	{
		if (std::string::npos != desc.find(k))
			return true;
	}
	return false;
}

void CController::UnbindAllFtdiSio()
{
	// Unbind ftdi_sio only for whitelisted AMBE devices (by serial number).
	// Other FTDI devices (USB-serial adapters etc.) are left alone.
	const auto &serials = g_Conf.GetDeviceSerials();
	const std::string driverpath = "/sys/bus/usb/drivers/ftdi_sio/";
	DIR *dir = opendir(driverpath.c_str());
	if (!dir) return;

	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr)
	{
		if (!strchr(entry->d_name, ':'))
			continue;

		// Read serial number via sysfs
		std::string serialpath = driverpath + entry->d_name + "/../serial";
		std::ifstream sf(serialpath);
		if (!sf.is_open())
			continue;

		std::string serial;
		std::getline(sf, serial);
		while (!serial.empty() && (serial.back() == '\n' || serial.back() == '\r'))
			serial.pop_back();

		bool is_whitelisted = serials.empty(); // no whitelist = unbind all (legacy behavior)
		for (const auto &ws : serials)
		{
			if (serial == ws) { is_whitelisted = true; break; }
		}

		if (is_whitelisted)
		{
			std::ofstream uf(driverpath + "unbind");
			if (uf.is_open())
			{
				uf << entry->d_name;
				std::cout << "Unbound ftdi_sio from " << entry->d_name << " (SN=" << serial << ")" << std::endl;
			}
		}
	}
	closedir(dir);
}

bool CController::DiscoverFtdiDevices(std::list<std::pair<std::string, std::string>> &found)
{
	// Unbind ftdi_sio first so libftd2xx can see device details
	UnbindAllFtdiSio();
	std::this_thread::sleep_for(std::chrono::milliseconds(500));

	int iNbDevices = 0;
	auto status = FT_CreateDeviceInfoList((LPDWORD)&iNbDevices);
	if (FT_OK != status)
	{
		std::cerr << "Could not create FTDI device list" << std::endl;
		return true;
	}

	std::cout << "Detected " << iNbDevices << " USB-FTDI devices" << std::endl;
	if ( iNbDevices > 0 )
	{
		FT_DEVICE_LIST_INFO_NODE *list = new FT_DEVICE_LIST_INFO_NODE[iNbDevices];
		if (nullptr == list)
		{
			std::cerr << "Could not create new device list" << std::endl;
			return true;
		}

		status = FT_GetDeviceInfoList(list, (LPDWORD)&iNbDevices);
		if (FT_OK != status)
		{
			std::cerr << "Could not get FTDI device list" << std::endl;
			return true;
		}

		const auto &whitelist = g_Conf.GetDeviceSerials();
		for ( int i = 0; i < iNbDevices; i++ )
		{
			const std::string desc(list[i].Description);
			const std::string sn(list[i].SerialNumber);

			if (whitelist.empty())
			{
				// No whitelist configured — accept all AMBE devices by name
				if (IsAmbeDevice(desc))
				{
					std::cout << "Found AMBE device: " << desc << ", SN=" << sn << std::endl;
					found.emplace_back(std::pair<std::string, std::string>(sn, desc));
				}
				else
				{
					std::cout << "Skipping non-AMBE FTDI device: " << desc << ", SN=" << sn << std::endl;
				}
			}
			else
			{
				// Whitelist configured — only accept matching serial numbers
				bool whitelisted = false;
				for (const auto &ws : whitelist)
				{
					if (sn == ws) { whitelisted = true; break; }
				}
				if (whitelisted)
				{
					std::cout << "Found AMBE device: " << desc << ", SN=" << sn << " (whitelisted)" << std::endl;
					found.emplace_back(std::pair<std::string, std::string>(sn, desc));
				}
				else
				{
					std::cout << "Skipping FTDI device: " << desc << ", SN=" << sn << " (not whitelisted)" << std::endl;
				}
			}
		}

		delete[] list;
	}

	return false;
}

bool CController::InitVocoders()
{
	// M17 "devices", one for each module
	const std::string modules(g_Conf.GetTCMods());
	for ( auto c : modules)
	{
		c2_16[c] = std::unique_ptr<CCodec2>(new CCodec2(false));
		c2_32[c] = std::unique_ptr<CCodec2>(new CCodec2(true));
	}

	// the 3000 or 3003 devices
	std::list<std::pair<std::string, std::string>> deviceset;

	if (DiscoverFtdiDevices(deviceset))
		return true;

	if (deviceset.empty()) {
		std::cerr << "Could not find an AMBE device!" << std::endl;
		std::cerr << "Run '" << "tcd --list-devices" << "' to see available FTDI devices." << std::endl;
		std::cerr << "Use 'DeviceSerial = <serial>' in tcd.ini to select your AMBE device." << std::endl;
		return true;
	}

	for (unsigned int i=0; i<modules.size(); i++)
	{
		auto c = modules.at(i);
		if (c < 'A' || c > 'Z') {
			std::cerr << "Transcoded modules[" << i << "] is not an uppercase letter!" << std::endl;
			return true;
		}
	}

	// Classify each device by type
	std::pair<std::string,std::string> dv3000_dev, dv3003_dev;
	for (const auto &dev : deviceset)
	{
		const auto &desc = dev.second;
		if (0==desc.compare("ThumbDV") || 0==desc.compare("DVstick-30") || 0==desc.compare("USB-3000")
			|| 0==desc.compare("FT230X Basic UART") || 0==desc.compare("FT232R USB UART"))
			dv3000_dev = dev;
		else
			dv3003_dev = dev;
	}

	// Mixed mode: DV3000 (D-Star) + DV3003 (1x D-Star + 2x DMR) = 2 modules
	if (!dv3000_dev.first.empty() && !dv3003_dev.first.empty())
	{
		std::cout << "Mixed mode: " << dv3000_dev.second << " (" << dv3000_dev.first << ") + "
		          << dv3003_dev.second << " (" << dv3003_dev.first << ")" << std::endl;

		// DV3000: D-Star vocoder for module A (first module)
		dstar_device = std::unique_ptr<CDVDevice>(new CDV3000(Encoding::dstar));
		if (dstar_device->OpenDevice(dv3000_dev.first, dv3000_dev.second, Edvtype::dv3000,
				int8_t(g_Conf.GetGain(EGainType::dstarin)), int8_t(g_Conf.GetGain(EGainType::dstarout))))
			return true;

		// DV3003: ch0=D-Star (module B), ch1=DMR (module A), ch2=DMR (module B)
		auto *dv3003 = new CDV3003(Encoding::dmrsf);
		dmrsf_device = std::unique_ptr<CDVDevice>(dv3003);

		// Set per-channel encoding BEFORE OpenDevice so ConfigureVocoder uses the right codec
		dv3003->SetChannelEncoding(0, Encoding::dstar);
		dv3003->SetChannelEncoding(1, Encoding::dmrsf);
		dv3003->SetChannelEncoding(2, Encoding::dmrsf);

		// Open with DMR gains (ConfigureVocoder will use per-channel encoding)
		if (dmrsf_device->OpenDevice(dv3003_dev.first, dv3003_dev.second, Edvtype::dv3003,
				int8_t(g_Conf.GetGain(EGainType::dmrin)), int8_t(g_Conf.GetGain(EGainType::dmrout))))
			return true;

		// Map modules to DV3003 channels
		if (modules.size() >= 2)
		{
			char modA = modules[0], modB = modules[1];
			dv3003->SetModuleDStarChannel(modB, 0);  // D-Star module B → ch0
			dv3003->SetModuleDMRChannel(modA, 1);    // DMR module A → ch1
			dv3003->SetModuleDMRChannel(modB, 2);    // DMR module B → ch2
			mixed_mode = true;
			mixed_dstar_module = modB;
			mixed_dv3003 = dv3003;
			std::cout << "  Module " << modA << ": D-Star(DV3000) + DMR(DV3003 ch1)" << std::endl;
			std::cout << "  Module " << modB << ": D-Star(DV3003 ch0) + DMR(DV3003 ch2)" << std::endl;
		}
	}
	// Two same-type devices
	else if (deviceset.size() >= 2)
	{
		const auto &desc = deviceset.front().second;
		Edvtype dvtype = Edvtype::dv3003;
		if (0==desc.compare("ThumbDV") || 0==desc.compare("DVstick-30") || 0==desc.compare("USB-3000")
			|| 0==desc.compare("FT230X Basic UART") || 0==desc.compare("FT232R USB UART"))
			dvtype = Edvtype::dv3000;

		if (Edvtype::dv3000 == dvtype) {
			dstar_device = std::unique_ptr<CDVDevice>(new CDV3000(Encoding::dstar));
			dmrsf_device = std::unique_ptr<CDVDevice>(new CDV3000(Encoding::dmrsf));
		} else {
			dstar_device = std::unique_ptr<CDVDevice>(new CDV3003(Encoding::dstar));
			dmrsf_device = std::unique_ptr<CDVDevice>(new CDV3003(Encoding::dmrsf));
		}

		if (dstar_device->OpenDevice(deviceset.front().first, deviceset.front().second, dvtype,
				int8_t(g_Conf.GetGain(EGainType::dstarin)), int8_t(g_Conf.GetGain(EGainType::dstarout))))
			return true;
		deviceset.pop_front();

		if (dmrsf_device->OpenDevice(deviceset.front().first, deviceset.front().second, dvtype,
				int8_t(g_Conf.GetGain(EGainType::dmrin)), int8_t(g_Conf.GetGain(EGainType::dmrout))))
			return true;
	}
	// Single device with md380 software vocoder for DMR
	else if (deviceset.size() == 1)
	{
#ifndef WITH_MD380_VOCODER
		std::cerr << "ERROR: A single DVSI device requires the md380_vocoder library for software DMR encoding." << std::endl;
		std::cerr << "       Without md380_vocoder, minimum hardware is: 2x AMBE3000 (DV3000)," << std::endl;
		std::cerr << "       1x AMBE3003 (DV3003), or one DV3000 + one DV3003 (mixed mode)." << std::endl;
		return true;
#else
		std::cout << "Using one DVSI device and md380_vocoder" << std::endl;
		const auto &dev = deviceset.front();
		Edvtype dvtype = Edvtype::dv3003;
		if (0==dev.second.compare("ThumbDV") || 0==dev.second.compare("DVstick-30") || 0==dev.second.compare("USB-3000")
			|| 0==dev.second.compare("FT230X Basic UART") || 0==dev.second.compare("FT232R USB UART"))
			dvtype = Edvtype::dv3000;

		if (Edvtype::dv3000 == dvtype)
			dstar_device = std::unique_ptr<CDVDevice>(new CDV3000(Encoding::dstar));
		else
			dstar_device = std::unique_ptr<CDVDevice>(new CDV3003(Encoding::dstar));

		ambe_in_num = calcNumerator(g_Conf.GetGain(EGainType::dmrin));
		ambe_out_num = calcNumerator(g_Conf.GetGain(EGainType::dmrout));

		if (dstar_device->OpenDevice(dev.first, dev.second, dvtype,
				int8_t(g_Conf.GetGain(EGainType::dstarin)), int8_t(g_Conf.GetGain(EGainType::dstarout))))
			return true;
#endif
	}
	else
	{
		std::cerr << "No suitable DVSI device configuration found" << std::endl;
		return true;
	}

#ifdef WITH_MD380_VOCODER
	if (md380_init())
	{
		std::cerr << "ERROR: md380_init() failed — cannot use MD380 software vocoder" << std::endl;
		return true;
	}
	g_Stats.md380.available.store(true, std::memory_order_relaxed);
	if (dmrsf_device)
		std::cout << "md380 vocoder: DMR re-encode after AGC" << std::endl;
	else
		std::cout << "md380 vocoder: primary DMR codec" << std::endl;
#else
	if (g_Conf.GetDMRReEncodeEnabled())
	{
		std::cerr << "WARNING: DMRReEncode is enabled in config but md380_vocoder is not compiled in." << std::endl;
		std::cerr << "         Re-encoding will be skipped. Build with md380=true to enable it." << std::endl;
	}
	g_Stats.config.dmr_reencode_enabled.store(false, std::memory_order_relaxed);
#endif

	dstar_device->Start();
	if (dmrsf_device)
		dmrsf_device->Start();

	// Register DVSI devices in monitoring stats
	{
		int di = 0;
		auto reg = [&](const std::string &serial, const std::string &desc) {
			if (serial.empty() || di >= CTcdStats::MAX_DEVICES) return;
			g_Stats.devices[di].serial = serial;
			if (desc == "ThumbDV" || desc == "DVstick-30" || desc == "USB-3000"
				|| desc == "FT230X Basic UART" || desc == "FT232R USB UART")
				g_Stats.devices[di].type = "DV3000";
			else
				g_Stats.devices[di].type = "DV3003";
			g_Stats.devices[di].online.store(true);
			di++;
		};
		if (!dv3000_dev.first.empty()) reg(dv3000_dev.first, dv3000_dev.second);
		if (!dv3003_dev.first.empty()) reg(dv3003_dev.first, dv3003_dev.second);
		// Set roles based on device assignment
		if (di >= 1) { g_Stats.devices[0].role = "dstar"; dstar_device->SetStatsIndex(0); }
		if (di >= 2) { g_Stats.devices[1].role = mixed_mode ? "mixed" : "dmr"; dmrsf_device->SetStatsIndex(1); }
		g_Stats.num_devices = di;
	}

	deviceset.clear();

	return false;
}

// Encapsulate the incoming STCPacket into a CTranscoderPacket and push it into the appropriate queue
// based on packet's codec_in.
void CController::ReadReflectorThread()
{
	uint32_t reconf_counter = 0;
	while (keep_running)
	{
		// preemptively check the connection(s)...
		tcClient.ReConnect();

		// Every ~1s: re-read config + age idle modules
		if (++reconf_counter >= 10)
		{
			reconf_counter = 0;
			ReconfigureAGC();

			// Age idle timers and reset stale modules
			for (char mod : g_Conf.GetTCMods())
			{
				int idx = mod - 'A';
				if (idx < 0 || idx >= CTcdStats::MAX_MODULES) continue;
				auto &ms = g_Stats.modules[idx];
				float idle = ms.last_activity.load(std::memory_order_relaxed);
				if (idle < 100.0f)  // don't overflow
					ms.last_activity.store(idle + 1.0f, std::memory_order_relaxed);
				// Reset display after 2s of no packets
				if (idle >= 2.0f && ms.codec_in.load(std::memory_order_relaxed) != 0)
				{
					ms.codec_in.store(0, std::memory_order_relaxed);
					ms.stream_id.store(0, std::memory_order_relaxed);
					ms.rms_in.store(-100.0f, std::memory_order_relaxed);
					ms.peak_in.store(-100.0f, std::memory_order_relaxed);
					ms.rms_out.store(-100.0f, std::memory_order_relaxed);
					ms.peak_out.store(-100.0f, std::memory_order_relaxed);
					ms.agc_gain_db.store(0.0f, std::memory_order_relaxed);
					ms.agc_gate.store(false, std::memory_order_relaxed);
					ms.agc_gate_count.store(0, std::memory_order_relaxed);
				}
			}

			// Sync DVSI device stats (buf_depth, active channels)
			{
				auto now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				for (int di = 0; di < g_Stats.num_devices; di++)
				{
					auto &ds = g_Stats.devices[di];
					// Reset stale channels (2s timeout)
					for (int ch = 0; ch < DeviceStats::MAX_CHANNELS; ch++)
					{
						uint64_t last = ds.ch_last_ms[ch].load(std::memory_order_relaxed);
						if (last > 0 && (now_ms - last) > 2000)
							ds.ch_module[ch].store(' ', std::memory_order_relaxed);
					}
				}
				int di = 0;
				auto syncDev = [&](CDVDevice *dev) {
					if (!dev || di >= CTcdStats::MAX_DEVICES) return;
					g_Stats.devices[di].buf_depth.store(dev->GetBufferDepth(), std::memory_order_relaxed);
					di++;
				};
				syncDev(dstar_device.get());
				if (dmrsf_device) syncDev(dmrsf_device.get());
			}

			// Sync md380 software vocoder stats
			{
#ifdef WITH_MD380_VOCODER
				auto now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				uint64_t last = g_Stats.md380.last_active_ms.load(std::memory_order_relaxed);
				if (last > 0 && (now_ms - last) > 2000)
					g_Stats.md380.active_module.store(' ', std::memory_order_relaxed);
				g_Stats.md380.reencode_active.store(
					g_Stats.config.dmr_reencode_enabled.load(std::memory_order_relaxed) &&
					(m_agc.IsEnabled() || outgain_dmr_num != 256 || dmr_reencode_num != 256),
					std::memory_order_relaxed);
				g_Stats.md380.cached_streams.store((int)md380_state_cache.size(), std::memory_order_relaxed);
#endif
			}
		}

		// Check if at least one module has an open FD
		{
			bool conn = false;
			for (char m : g_Conf.GetTCMods())
				if (tcClient.GetFD(m) >= 0) { conn = true; break; }
			g_Stats.reflector.connected.store(conn, std::memory_order_relaxed);
		}

		std::queue<std::unique_ptr<STCPacket>> queue;
		// wait up to 100 ms to read something on the unix port
		tcClient.Receive(queue, 100);
		// If no module is connected, poll returns instantly (all FDs -1).
		// Sleep to avoid busy-spinning the loop.
		if (queue.empty())
		{
			bool any_connected = false;
			for (char m : g_Conf.GetTCMods())
				if (tcClient.GetFD(m) >= 0) { any_connected = true; break; }
			if (!any_connected)
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		while (! queue.empty())
		{
			// create a shared pointer to a new packet
			// there is only one CTranscoderPacket created for each new STCPacket received from the reflector
			auto packet = std::make_shared<CTranscoderPacket>(*queue.front());
			queue.pop();
			g_Stats.PacketIn(packet->GetModule(), packet->GetStreamId(), packet->GetCodecIn());
			g_Stats.reflector.packets_rx.fetch_add(1, std::memory_order_relaxed);
			// Idle reset is handled by 2s timeout in reconf loop
			// (not here, because the packet still flows through the pipeline
			// and Route functions would overwrite the reset values)
			switch (packet->GetCodecIn())
			{
			case ECodecType::dstar:
				if (mixed_mode && packet->GetModule() == mixed_dstar_module)
				{
					
					mixed_dv3003->AddPacketToChannel(packet, 0);  // ch0 = D-Star
				}
				else
					dstar_device->AddPacket(packet);
				break;
			case ECodecType::dmr:
				if (dmrsf_device)
				{
					if (mixed_mode)
					{
						
						char mod = packet->GetModule();
						auto it = std::string(g_Conf.GetTCMods()).find(mod);
						uint8_t dmr_ch = (it == 0) ? 1 : 2;
						mixed_dv3003->AddPacketToChannel(packet, dmr_ch);
					}
					else
						dmrsf_device->AddPacket(packet);
				}
				else
					swambe2_queue.push(packet);
				break;
			case ECodecType::p25:
				imbe_queue.push(packet);
				break;
			case ECodecType::usrp:
			case ECodecType::svx:
				usrp_queue.push(packet);
				break;
			case ECodecType::c2_1600:
			case ECodecType::c2_3200:
				codec2_queue.push(packet);
				break;
			default:
				Dump(packet, "ERROR: Received a reflector packet with unknown Codec:");
				break;
			}
		}
	}
}

// This is only called when codec_in was dstar or dmr. Obviously, the incoming
// ambe packet was already decoded to audio.
// This might complete the packet. If so, send it back to the reflector
void CController::AudiotoCodec2(std::shared_ptr<CTranscoderPacket> packet)
{
	int16_t gained[160];
	memcpy(gained, packet->GetAudioSamples(), sizeof(gained));
	ApplyGain(gained, 160, outgain_m17_num);

	uint8_t m17data[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x01, 0x43, 0x09, 0xe4, 0x9c, 0x08, 0x21 };
	const auto m = packet->GetModule();
	if (packet->IsSecond())
	{
		memcpy(m17data, data_store[packet->GetModule()], 8);
		c2_32[m]->codec2_encode(m17data+8, gained);
		packet->SetM17Data(m17data);
	}
	else
	{
		c2_32[m]->codec2_encode(m17data, gained);
		memcpy(data_store[packet->GetModule()], m17data, 8);
		packet->SetM17Data(m17data);
	}

	// we might be all done...
	send_mux.lock();
	if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
	send_mux.unlock();
}

// The original incoming coded was M17, so we will calculate the audio and then
// push the packet onto both the dstar and the dmr queue.
void CController::Codec2toAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	uint8_t imbe[11];

	if (packet->IsSecond())
	{
		if (packet->GetCodecIn() == ECodecType::c2_1600)
		{
			int16_t tmp[160];
			memcpy(tmp, audio_store[packet->GetModule()], sizeof(tmp));
			g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
			float agc_db; bool agc_gate;
			m_agc.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
		}
		else /* codec_in is ECodecType::c2_3200 */
		{
			int16_t tmp[160];
			c2_32[packet->GetModule()]->codec2_decode(tmp, packet->GetM17Data()+8);
			g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
			float agc_db; bool agc_gate;
			m_agc.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
		}
	}
	else /* it's a "first packet" */
	{
		const auto m = packet->GetModule();
		if (packet->GetCodecIn() == ECodecType::c2_1600)
		{
			int16_t tmp[320];
			c2_16[m]->codec2_decode(tmp, packet->GetM17Data());
			g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
			float agc_db; bool agc_gate;
			m_agc.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
			// AGC the second half too before storing
			m_agc.Process(&tmp[160], 160, packet->GetStreamId());
			memcpy(audio_store[packet->GetModule()], &(tmp[160]), 320);
		}
		else /* codec_in is ECodecType::c2_3200 */
		{
			int16_t tmp[160];
			c2_32[m]->codec2_decode(tmp, packet->GetM17Data());
			g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
			float agc_db; bool agc_gate;
			m_agc.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
		}
	}
	g_Stats.UpdateLevelsOut(packet->GetModule(), packet->GetAudioSamples(), 160);
	{
		int i = packet->GetModule() - 'A';
		auto &ms = g_Stats.modules[i];
		g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "codec2",
			ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
			ms.agc_gain_db.load(), ms.agc_gate.load());
	}
	// D-Star DVSI (gain applied in FeedDevice)
	if (mixed_mode && packet->GetModule() == mixed_dstar_module)
		mixed_dv3003->AddPacketToChannel(packet, 0);
	else
		dstar_device->AddPacket(packet);

	// DMR/YSF DVSI (gain applied in FeedDevice) or sw encode
	if (dmrsf_device)
	{
		if (mixed_mode)
		{
			char mod = packet->GetModule();
			auto it = std::string(g_Conf.GetTCMods()).find(mod);
			uint8_t dmr_ch = (it == 0) ? 1 : 2;
			mixed_dv3003->AddPacketToChannel(packet, dmr_ch);
		}
		else
			dmrsf_device->AddPacket(packet);
	}
	else
	{
#ifdef WITH_MD380_VOCODER
		uint8_t ambe2[9];
		int16_t dmr_buf[160];
		memcpy(dmr_buf, packet->GetAudioSamples(), sizeof(dmr_buf));
		ApplyGain(dmr_buf, 160, outgain_dmr_num);
		std::lock_guard<std::mutex> lock(md380_mux);
		md380_encode_fec(ambe2, dmr_buf);
		Md380Stat(packet->GetModule(), g_Stats.md380.encodes);
		packet->SetDMRData(ambe2);
#endif
	}

	// IMBE/P25
	{
		int16_t imbe_buf[160];
		memcpy(imbe_buf, packet->GetAudioSamples(), sizeof(imbe_buf));
		ApplyGain(imbe_buf, 160, outgain_imbe_num);
		std::lock_guard<std::mutex> lock(p25vocoder_mux);
		p25vocoder.encode_4400(imbe_buf, imbe);
	}
	packet->SetP25Data(imbe);

	// USRP
	{
		int16_t usrp_buf[160];
		memcpy(usrp_buf, packet->GetAudioSamples(), sizeof(usrp_buf));
		ApplyGain(usrp_buf, 160, outgain_usrp_num);
		packet->SetUSRPData(usrp_buf);
	}
}

void CController::ProcessC2Thread()
{
	while (keep_running)
	{
		auto packet = codec2_queue.pop();

		switch (packet->GetCodecIn())
		{
			case ECodecType::c2_1600:
			case ECodecType::c2_3200:
				// this is an original M17 packet, so decode it to audio
				// Codec2toAudio will send it on for AMBE processing
				Codec2toAudio(packet);
				break;

			case ECodecType::dstar:
			case ECodecType::dmr:
			case ECodecType::p25:
			case ECodecType::usrp:
			case ECodecType::svx:
				// codec_in was AMBE, so we need to calculate the the M17 data
				AudiotoCodec2(packet);
				break;
		}
	}
}

void CController::AudiotoSWAMBE2(std::shared_ptr<CTranscoderPacket> packet)
{
#ifdef WITH_MD380_VOCODER
	uint8_t ambe2[9];
	int16_t gained[160];
	memcpy(gained, packet->GetAudioSamples(), sizeof(gained));
	ApplyGain(gained, 160, outgain_dmr_num);

	{
		std::lock_guard<std::mutex> lock(md380_mux);
		if (ambe_in_num != 256)
		{
			int16_t tmp[160];
			for(int i = 0; i < 160; ++i)
				tmp[i] = int16_t((gained[i] * ambe_in_num) >> 8);
			md380_encode_fec(ambe2, tmp);
		}
		else
			md380_encode_fec(ambe2, gained);
		Md380Stat(packet->GetModule(), g_Stats.md380.encodes);
	}
	packet->SetDMRData(ambe2);

	// we might be all done...
	send_mux.lock();
	if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
	send_mux.unlock();
#endif
}

void CController::SWAMBE2toAudio(std::shared_ptr<CTranscoderPacket> packet)
{
#ifdef WITH_MD380_VOCODER
	int16_t tmp[160];
	{
		std::lock_guard<std::mutex> lock(md380_mux);
		md380_decode_fec(const_cast<uint8_t*>(packet->GetDMRData()), tmp);
		Md380Stat(packet->GetModule(), g_Stats.md380.decodes);
	}
	if (ambe_out_num != 256)
	{
		for (int i=0; i<160; i++)
			tmp[i] = (tmp[i] * ambe_out_num) >> 8;
	}
	g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
	{
		float agc_db; bool agc_gate;
		m_agc.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
		g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
	}
	packet->SetAudioSamples(tmp, false);
	g_Stats.UpdateLevelsOut(packet->GetModule(), tmp, 160);
	{
		int i = packet->GetModule() - 'A';
		auto &ms = g_Stats.modules[i];
		g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "dmr",
			ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
			ms.agc_gain_db.load(), ms.agc_gate.load());
	}
	if (mixed_mode && packet->GetModule() == mixed_dstar_module)
	{

		mixed_dv3003->AddPacketToChannel(packet, 0);
	}
	else
		dstar_device->AddPacket(packet);
	codec2_queue.push(packet);
	imbe_queue.push(packet);
	usrp_queue.push(packet);
#endif
}

void CController::ProcessSWAMBE2Thread()
{
#ifdef WITH_MD380_VOCODER
	while (keep_running)
	{
		auto packet = swambe2_queue.pop();

		switch (packet->GetCodecIn())
		{
			case ECodecType::c2_1600:
			case ECodecType::c2_3200:
			case ECodecType::dstar:
			case ECodecType::p25:
			case ECodecType::usrp:
			case ECodecType::svx:
				AudiotoSWAMBE2(packet);
				break;

			case ECodecType::dmr:
				SWAMBE2toAudio(packet);
				break;
		}
	}
#endif
}

void CController::AudiotoIMBE(std::shared_ptr<CTranscoderPacket> packet)
{
	uint8_t imbe[11];
	int16_t gained[160];
	memcpy(gained, packet->GetAudioSamples(), sizeof(gained));
	ApplyGain(gained, 160, outgain_imbe_num);

	{
		std::lock_guard<std::mutex> lock(p25vocoder_mux);
		p25vocoder.encode_4400(gained, imbe);
	}
	packet->SetP25Data(imbe);
	// we might be all done...
	send_mux.lock();
	if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
	send_mux.unlock();
}

void CController::IMBEtoAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	int16_t tmp[160] = { 0 };
	{
		std::lock_guard<std::mutex> lock(p25vocoder_mux);
		p25vocoder.decode_4400(tmp, (uint8_t*)packet->GetP25Data());
	}
	g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
	{
		float agc_db; bool agc_gate;
		m_agc.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
		g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
	}
	packet->SetAudioSamples(tmp, false);
	g_Stats.UpdateLevelsOut(packet->GetModule(), tmp, 160);
	{
		int i = packet->GetModule() - 'A';
		auto &ms = g_Stats.modules[i];
		g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "imbe",
			ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
			ms.agc_gain_db.load(), ms.agc_gate.load());
	}
	if (mixed_mode && packet->GetModule() == mixed_dstar_module)
	{

		mixed_dv3003->AddPacketToChannel(packet, 0);
	}
	else
		dstar_device->AddPacket(packet);
	codec2_queue.push(packet);

	if (dmrsf_device)
	{
		if (mixed_mode)
		{

			char mod = packet->GetModule();
			auto it = std::string(g_Conf.GetTCMods()).find(mod);
			uint8_t dmr_ch = (it == 0) ? 1 : 2;
			mixed_dv3003->AddPacketToChannel(packet, dmr_ch);
		}
		else
			dmrsf_device->AddPacket(packet);
	}
	else
		swambe2_queue.push(packet);

	usrp_queue.push(packet);
}

void CController::ProcessIMBEThread()
{
	while (keep_running)
	{
		auto packet = imbe_queue.pop();

		switch (packet->GetCodecIn())
		{
			case ECodecType::c2_1600:
			case ECodecType::c2_3200:
			case ECodecType::dstar:
			case ECodecType::dmr:
			case ECodecType::usrp:
			case ECodecType::svx:
				AudiotoIMBE(packet);
				break;

			case ECodecType::p25:
				IMBEtoAudio(packet);
				break;
		}
	}
}

void CController::AudiotoUSRP(std::shared_ptr<CTranscoderPacket> packet)
{
	int16_t tmp[160];
	memcpy(tmp, packet->GetAudioSamples(), sizeof(tmp));
	ApplyGain(tmp, 160, outgain_usrp_num);

	if (usrp_tx_num != 256)
	{
		for(int i = 0; i < 160; ++i)
			tmp[i] = int16_t((tmp[i] * usrp_tx_num) >> 8);
	}
	packet->SetUSRPData(tmp);


	// we might be all done...
	send_mux.lock();
	if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
	send_mux.unlock();
}

void CController::USRPtoAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	const int16_t *p = packet->GetUSRPData();
	int16_t tmp[160];

	if (usrp_rx_num != 256)
	{
		for(int i = 0; i < 160; ++i)
			tmp[i] = int16_t((p[i] * usrp_rx_num) >> 8);
	}
	else
	{
		memcpy(tmp, p, sizeof(tmp));
	}
	g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
	{
		float agc_db; bool agc_gate;
		m_agc.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
		g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
	}
	packet->SetAudioSamples(tmp, false);
	g_Stats.UpdateLevelsOut(packet->GetModule(), tmp, 160);
	{
		int i = packet->GetModule() - 'A';
		auto &ms = g_Stats.modules[i];
		g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "usrp",
			ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
			ms.agc_gain_db.load(), ms.agc_gate.load());
	}
	if (mixed_mode && packet->GetModule() == mixed_dstar_module)
	{

		mixed_dv3003->AddPacketToChannel(packet, 0);
	}
	else
		dstar_device->AddPacket(packet);
	codec2_queue.push(packet);

	if (dmrsf_device)
	{
		if (mixed_mode)
		{

			char mod = packet->GetModule();
			auto it = std::string(g_Conf.GetTCMods()).find(mod);
			uint8_t dmr_ch = (it == 0) ? 1 : 2;
			mixed_dv3003->AddPacketToChannel(packet, dmr_ch);
		}
		else
			dmrsf_device->AddPacket(packet);
	}
	else
		swambe2_queue.push(packet);

	imbe_queue.push(packet);
}

void CController::ProcessUSRPThread()
{
	while (keep_running)
	{
		auto packet = usrp_queue.pop();

		switch (packet->GetCodecIn())
		{
			case ECodecType::c2_1600:
			case ECodecType::c2_3200:
			case ECodecType::dstar:
			case ECodecType::dmr:
			case ECodecType::p25:
				AudiotoUSRP(packet);
				break;

			case ECodecType::usrp:
				USRPtoAudio(packet);
				break;

			case ECodecType::svx:
				SvxToAudio(packet);
				break;
		}
	}
}

void CController::SvxToAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	const int16_t *p = packet->GetUSRPData();
	int16_t tmp[160];
	memcpy(tmp, p, sizeof(tmp));

	g_Stats.UpdateLevelsIn(packet->GetModule(), tmp, 160);
	{
		float agc_db; bool agc_gate;
		m_agc.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
		g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
	}
	packet->SetAudioSamples(tmp, false);
	g_Stats.UpdateLevelsOut(packet->GetModule(), tmp, 160);
	{
		int i = packet->GetModule() - 'A';
		auto &ms = g_Stats.modules[i];
		g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "svx",
			ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
			ms.agc_gain_db.load(), ms.agc_gate.load());
	}
	if (mixed_mode && packet->GetModule() == mixed_dstar_module)
	{

		mixed_dv3003->AddPacketToChannel(packet, 0);  // ch0 = D-Star
	}
	else
		dstar_device->AddPacket(packet);
	codec2_queue.push(packet);

	if (dmrsf_device)
	{
		if (mixed_mode)
		{

			char mod = packet->GetModule();
			auto it = std::string(g_Conf.GetTCMods()).find(mod);
			uint8_t dmr_ch = (it == 0) ? 1 : 2;
			mixed_dv3003->AddPacketToChannel(packet, dmr_ch);
		}
		else
			dmrsf_device->AddPacket(packet);
	}
	else
		swambe2_queue.push(packet);

	imbe_queue.push(packet);
}

void CController::SendToReflector(std::shared_ptr<CTranscoderPacket> packet)
{
	constexpr int max_retries = 5;
	for (int attempt = 0; attempt < max_retries; ++attempt)
	{
		if (!tcClient.Send(packet->GetTCPacket()))
		{
			packet->Sent();
			g_Stats.PacketOut(packet->GetModule());
			g_Stats.reflector.packets_tx.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		std::cerr << "SendToReflector: send failed, attempt " << (attempt + 1) << "/" << max_retries << std::endl;
		tcClient.ReConnect();
		std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt)));
	}
	std::cerr << "SendToReflector: dropping packet after " << max_retries << " retries" << std::endl;
	packet->Sent();
}

void CController::RouteDstPacket(std::shared_ptr<CTranscoderPacket> packet)
{
	if (ECodecType::dstar == packet->GetCodecIn())
	{
		// codec_in is dstar, the audio has just completed — apply AGC before encoding
		g_Stats.UpdateLevelsIn(packet->GetModule(), packet->GetAudioSamples(), 160);
		if (m_agc.IsEnabled())
		{
			int16_t tmp[160];
			float agc_db; bool agc_gate;
			memcpy(tmp, packet->GetAudioSamples(), sizeof(tmp));
			m_agc.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
		}
		g_Stats.UpdateLevelsOut(packet->GetModule(), packet->GetAudioSamples(), 160);
		{
			int i = packet->GetModule() - 'A';
			auto &ms = g_Stats.modules[i];
			g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "dstar",
				ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
				ms.agc_gain_db.load(), ms.agc_gate.load());
		}
		// Output gains applied in encode functions and DVSI FeedDevice
		codec2_queue.push(packet);
		imbe_queue.push(packet);
		usrp_queue.push(packet);
		if (dmrsf_device)
		{
			if (mixed_mode)
			{

				char mod = packet->GetModule();
				auto it = std::string(g_Conf.GetTCMods()).find(mod);
				uint8_t dmr_ch = (it == 0) ? 1 : 2;
				mixed_dv3003->AddPacketToChannel(packet, dmr_ch);
			}
			else
				dmrsf_device->AddPacket(packet);
		}
		else
			swambe2_queue.push(packet);
	}
	else
	{
		send_mux.lock();
		if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
		send_mux.unlock();
	}
}

void CController::RouteDmrPacket(std::shared_ptr<CTranscoderPacket> packet)
{
	if (ECodecType::dmr == packet->GetCodecIn())
	{
		// codec_in is dmr (hardware decode), apply AGC before encoding
		g_Stats.UpdateLevelsIn(packet->GetModule(), packet->GetAudioSamples(), 160);
		if (m_agc.IsEnabled())
		{
			int16_t tmp[160];
			float agc_db; bool agc_gate;
			memcpy(tmp, packet->GetAudioSamples(), sizeof(tmp));
			m_agc.Process(tmp, 160, packet->GetStreamId(), agc_db, agc_gate);
			packet->SetAudioSamples(tmp, false);
			g_Stats.UpdateAGC(packet->GetModule(), agc_db, agc_gate);
		}
		g_Stats.UpdateLevelsOut(packet->GetModule(), packet->GetAudioSamples(), 160);
		{
			int i = packet->GetModule() - 'A';
			auto &ms = g_Stats.modules[i];
			g_StatsLog.LogFrame(packet->GetModule(), packet->GetStreamId(), "dmr",
				ms.rms_in.load(), ms.peak_in.load(), ms.rms_out.load(), ms.peak_out.load(),
				ms.agc_gain_db.load(), ms.agc_gate.load());
		}
		// Output gains applied in encode functions and DVSI FeedDevice
		// Re-encode DMR from AGC'd PCM via md380 software vocoder
		// Always re-encode when AGC or OutputGainDMR is active, otherwise the
		// original (un-gained) AMBE data passes through unchanged
#ifdef WITH_MD380_VOCODER
		if (g_Stats.config.dmr_reencode_enabled.load(std::memory_order_relaxed) &&
		    (m_agc.IsEnabled() || outgain_dmr_num != 256 || dmr_reencode_num != 256))
		{
			uint8_t ambe2[9];
			const int16_t *pcm = packet->GetAudioSamples();
			std::lock_guard<std::mutex> lock(md380_mux);
			// Save/restore md380 encoder state per stream to prevent crosstalk
			// md380 firmware RAM at 0x20000000, 128KB
			static constexpr uintptr_t MD380_RAM = 0x20000000;
			static constexpr size_t MD380_RAM_SIZE = 0x20000;
			uint16_t sid = packet->GetStreamId();
			if (sid != md380_enc_streamid)
			{
				// Save current stream's state
				if (md380_enc_streamid != 0)
				{
					auto &entry = md380_state_cache[md380_enc_streamid];
					entry.state.resize(MD380_RAM_SIZE);
					memcpy(entry.state.data(), (void*)MD380_RAM, MD380_RAM_SIZE);
					entry.last_used = std::chrono::steady_clock::now();
				}
				// Restore target stream's state (or init fresh)
				auto it = md380_state_cache.find(sid);
				if (it != md380_state_cache.end())
					memcpy((void*)MD380_RAM, it->second.state.data(), MD380_RAM_SIZE);
				md380_enc_streamid = sid;

				// Purge stale (>30s) or excess cache entries
				static constexpr size_t MD380_CACHE_MAX = 8;
				auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(30);
				for (auto ci = md380_state_cache.begin(); ci != md380_state_cache.end(); )
				{
					if (ci->first != sid && (ci->second.last_used < cutoff || md380_state_cache.size() > MD380_CACHE_MAX))
						ci = md380_state_cache.erase(ci);
					else
						++ci;
				}
			}
			int16_t tmp[160];
			memcpy(tmp, pcm, sizeof(tmp));
			ApplyGain(tmp, 160, outgain_dmr_num);
			if (dmr_reencode_num != 256)
			{
				for (int i = 0; i < 160; i++)
					tmp[i] = (int16_t)((tmp[i] * dmr_reencode_num) >> 8);
			}
			md380_encode_fec(ambe2, tmp);
			Md380Stat(packet->GetModule(), g_Stats.md380.reencodes);
			packet->SetDMRData(ambe2);
		}
#endif
		// Bitstream gain: adjust b2 (delta-gamma) in AMBE2+ frame when
		// re-encode did not already handle the gain via PCM path
		if (outgain_dmr_steps != 0)
		{
			bool reencode_handled = false;
#ifdef WITH_MD380_VOCODER
			reencode_handled = g_Stats.config.dmr_reencode_enabled.load(std::memory_order_relaxed) &&
				(m_agc.IsEnabled() || outgain_dmr_num != 256 || dmr_reencode_num != 256);
#endif
			if (!reencode_handled)
			{
				uint8_t dmr_data[9];
				memcpy(dmr_data, packet->GetDMRData(), 9);
				AmbeAdjustGain(dmr_data, outgain_dmr_steps);
				packet->SetDMRData(dmr_data);
			}
		}
		codec2_queue.push(packet);
		imbe_queue.push(packet);
		usrp_queue.push(packet);
		if (mixed_mode && packet->GetModule() == mixed_dstar_module)
		{
			
			mixed_dv3003->AddPacketToChannel(packet, 0);  // ch0 = D-Star
		}
		else
			dstar_device->AddPacket(packet);
	}
	else
	{
		send_mux.lock();
		if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
		send_mux.unlock();
	}
}

void CController::Dump(const std::shared_ptr<CTranscoderPacket> p, const std::string &title) const
{
	std::stringstream line;
	line << title << " Mod='" << p->GetModule() << "' SID=" << std::showbase << std::hex << ntohs(p->GetStreamId()) << std::noshowbase;

	ECodecType in = p->GetCodecIn();
	if (p->DStarIsSet())
		line << " DStar";
	if (ECodecType::dstar == in)
		line << '*';
	if (p->DMRIsSet())
		line << " DMR";
	if (ECodecType::dmr == in)
		line << '*';
	if (p->M17IsSet())
		line << " M17";
	if (ECodecType::c2_1600 == in)
		line << "**";
	else if (ECodecType::c2_3200 == in)
		line << '*';
	if (p->P25IsSet())
		line << " P25";
	if (ECodecType::p25 == in)
		line << "*";
	if (p->USRPIsSet())
		line << " USRP";
	if (ECodecType::usrp == in)
		line << "*";
	if (ECodecType::svx == in)
		line << "(svx)";
	if (p->IsSecond())
		line << " IsSecond";
	if (p->IsLast())
		line << " IsLast";

	std::cout << line.str() << std::dec << std::endl;
}
