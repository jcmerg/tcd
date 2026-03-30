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
#include <md380_vocoder.h>

#include "TranscoderPacket.h"
#include "Controller.h"
#include "Configure.h"

extern CConfigure g_Conf;

int32_t CController::calcNumerator(int32_t db) const
{
	float num = 256.0f * powf(10.0f, (float(db)/20.0f));

	return int32_t(roundf(num));
}

CController::CController() : keep_running(true) {}

bool CController::Start()
{
	usrp_rx_num = calcNumerator(g_Conf.GetGain(EGainType::usrprx));
	usrp_tx_num = calcNumerator(g_Conf.GetGain(EGainType::usrptx));
	m_agc.Configure(g_Conf.GetAGCEnabled(), g_Conf.GetAGCTarget(), g_Conf.GetAGCAttack(), g_Conf.GetAGCRelease(), g_Conf.GetAGCMaxGain());

	if (InitVocoders() || tcClient.Open(g_Conf.GetAddress(), g_Conf.GetTCMods(), g_Conf.GetPort()))
	{
		keep_running = false;
		return true;
	}
	reflectorFuture = std::async(std::launch::async, &CController::ReadReflectorThread, this);
	c2Future        = std::async(std::launch::async, &CController::ProcessC2Thread,     this);
	imbeFuture      = std::async(std::launch::async, &CController::ProcessIMBEThread,   this);
	usrpFuture      = std::async(std::launch::async, &CController::ProcessUSRPThread,   this);
	swambe2Future   = std::async(std::launch::async, &CController::ProcessSWAMBE2Thread,this);
	return false;
}

void CController::Stop()
{
	keep_running = false;

	if (reflectorFuture.valid())
		reflectorFuture.get();
	if (c2Future.valid())
		c2Future.get();

	tcClient.Close();
	dstar_device->CloseDevice();
	dmrsf_device->CloseDevice();
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

		md380_init();
		ambe_in_num = calcNumerator(g_Conf.GetGain(EGainType::dmrin));
		ambe_out_num = calcNumerator(g_Conf.GetGain(EGainType::dmrout));

		if (dstar_device->OpenDevice(dev.first, dev.second, dvtype,
				int8_t(g_Conf.GetGain(EGainType::dstarin)), int8_t(g_Conf.GetGain(EGainType::dstarout))))
			return true;
	}
	else
	{
		std::cerr << "No suitable DVSI device configuration found" << std::endl;
		return true;
	}

	// Always init md380 software vocoder (used for DMR re-encode after AGC)
	md380_init();

	dstar_device->Start();
	if (dmrsf_device)
		dmrsf_device->Start();

	deviceset.clear();

	return false;
}

// Encapsulate the incoming STCPacket into a CTranscoderPacket and push it into the appropriate queue
// based on packet's codec_in.
void CController::ReadReflectorThread()
{
	while (keep_running)
	{
		// preemptively check the connection(s)...
		tcClient.ReConnect();

		std::queue<std::unique_ptr<STCPacket>> queue;
		// wait up to 100 ms to read something on the unix port
		tcClient.Receive(queue, 100);
		while (! queue.empty())
		{
			// create a shared pointer to a new packet
			// there is only one CTranscoderPacket created for each new STCPacket received from the reflector
			auto packet = std::make_shared<CTranscoderPacket>(*queue.front());
			queue.pop();
			switch (packet->GetCodecIn())
			{
			case ECodecType::dstar:
				if (mixed_mode && packet->GetModule() == mixed_dstar_module)
				{
					auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
					dv3003->AddPacketToChannel(packet, 0);  // ch0 = D-Star
				}
				else
					dstar_device->AddPacket(packet);
				break;
			case ECodecType::dmr:
				if (dmrsf_device)
				{
					if (mixed_mode)
					{
						auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
						char mod = packet->GetModule();
						auto it = std::string(g_Conf.GetTCMods()).find(mod);
						uint8_t dmr_ch = (it == 0) ? 1 : 2;
						dv3003->AddPacketToChannel(packet, dmr_ch);
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
	// the second half is silent in case this is frame is last.
	uint8_t m17data[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x01, 0x43, 0x09, 0xe4, 0x9c, 0x08, 0x21 };
	const auto m = packet->GetModule();
	if (packet->IsSecond())
	{
		// get the first half from the store
		memcpy(m17data, data_store[packet->GetModule()], 8);
		// and then calculate the second half
		c2_32[m]->codec2_encode(m17data+8, packet->GetAudioSamples());
		packet->SetM17Data(m17data);
	}
	else /* the packet is first */
	{
		// calculate the first half...
		c2_32[m]->codec2_encode(m17data, packet->GetAudioSamples());
		// and then copy the calculated data to the data_store
		memcpy(data_store[packet->GetModule()], m17data, 8);
		// set the m17_is_set flag if this is the last packet
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
	uint8_t ambe2[9];
	uint8_t imbe[11];

	if (packet->IsSecond())
	{
		if (packet->GetCodecIn() == ECodecType::c2_1600)
		{
			int16_t tmp[160];
			memcpy(tmp, audio_store[packet->GetModule()], sizeof(tmp));
			m_agc.Process(tmp, 160, packet->GetStreamId());
			packet->SetAudioSamples(tmp, false);
		}
		else /* codec_in is ECodecType::c2_3200 */
		{
			int16_t tmp[160];
			c2_32[packet->GetModule()]->codec2_decode(tmp, packet->GetM17Data()+8);
			m_agc.Process(tmp, 160, packet->GetStreamId());
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
			m_agc.Process(tmp, 160, packet->GetStreamId());
			packet->SetAudioSamples(tmp, false);
			// AGC the second half too before storing
			m_agc.Process(&tmp[160], 160, packet->GetStreamId());
			memcpy(audio_store[packet->GetModule()], &(tmp[160]), 320);
		}
		else /* codec_in is ECodecType::c2_3200 */
		{
			int16_t tmp[160];
			c2_32[m]->codec2_decode(tmp, packet->GetM17Data());
			m_agc.Process(tmp, 160, packet->GetStreamId());
			packet->SetAudioSamples(tmp, false);
		}
	}
	// the only thing left is to encode the two ambe, so push the packet onto both AMBE queues
	if (mixed_mode && packet->GetModule() == mixed_dstar_module)
	{
		auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
		dv3003->AddPacketToChannel(packet, 0);
	}
	else
		dstar_device->AddPacket(packet);

	if (dmrsf_device)
	{
		if (mixed_mode)
		{
			auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
			char mod = packet->GetModule();
			auto it = std::string(g_Conf.GetTCMods()).find(mod);
			uint8_t dmr_ch = (it == 0) ? 1 : 2;
			dv3003->AddPacketToChannel(packet, dmr_ch);
		}
		else
			dmrsf_device->AddPacket(packet);
	}
	else
	{
		md380_encode_fec(ambe2, packet->GetAudioSamples());
		packet->SetDMRData(ambe2);
	}
	{
		std::lock_guard<std::mutex> lock(p25vocoder_mux);
		p25vocoder.encode_4400((int16_t*)packet->GetAudioSamples(), imbe);
	}
	packet->SetP25Data(imbe);
	packet->SetUSRPData((int16_t*)packet->GetAudioSamples());
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
	uint8_t ambe2[9];
	const int16_t *p = packet->GetAudioSamples();

	if (ambe_in_num != 256)
	{
		int16_t tmp[160];
		for(int i = 0; i < 160; ++i)
			tmp[i] = int16_t((p[i] * ambe_in_num) >> 8);
		md380_encode_fec(ambe2, tmp);
	}
	else
		md380_encode_fec(ambe2, p);

	packet->SetDMRData(ambe2);

	// we might be all done...
	send_mux.lock();
	if (packet->AllCodecsAreSet() && packet->HasNotBeenSent()) SendToReflector(packet);
	send_mux.unlock();
}

void CController::SWAMBE2toAudio(std::shared_ptr<CTranscoderPacket> packet)
{
	int16_t tmp[160];
	md380_decode_fec(packet->GetDMRData(), tmp);
	if (ambe_out_num != 256)
	{
		for (int i=0; i<160; i++)
			tmp[i] = (tmp[i] * ambe_out_num) >> 8;
	}
	m_agc.Process(tmp, 160, packet->GetStreamId());
	packet->SetAudioSamples(tmp, false);

	if (mixed_mode && packet->GetModule() == mixed_dstar_module)
	{
		auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
		dv3003->AddPacketToChannel(packet, 0);
	}
	else
		dstar_device->AddPacket(packet);
	codec2_queue.push(packet);
	imbe_queue.push(packet);
	usrp_queue.push(packet);
}

void CController::ProcessSWAMBE2Thread()
{
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
}

void CController::AudiotoIMBE(std::shared_ptr<CTranscoderPacket> packet)
{
	uint8_t imbe[11];

	{
		std::lock_guard<std::mutex> lock(p25vocoder_mux);
		p25vocoder.encode_4400((int16_t *)packet->GetAudioSamples(), imbe);
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
	m_agc.Process(tmp, 160, packet->GetStreamId());
	packet->SetAudioSamples(tmp, false);
	if (mixed_mode && packet->GetModule() == mixed_dstar_module)
	{
		auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
		dv3003->AddPacketToChannel(packet, 0);
	}
	else
		dstar_device->AddPacket(packet);
	codec2_queue.push(packet);

	if (dmrsf_device)
	{
		if (mixed_mode)
		{
			auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
			char mod = packet->GetModule();
			auto it = std::string(g_Conf.GetTCMods()).find(mod);
			uint8_t dmr_ch = (it == 0) ? 1 : 2;
			dv3003->AddPacketToChannel(packet, dmr_ch);
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
	const int16_t *p = packet->GetAudioSamples();

	if (usrp_tx_num != 256)
	{
		int16_t tmp[160];
		for(int i = 0; i < 160; ++i)
			tmp[i] = int16_t((p[i] * usrp_tx_num) >> 8);
		packet->SetUSRPData(tmp);
	}
	else
		packet->SetUSRPData(p);


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
	m_agc.Process(tmp, 160, packet->GetStreamId());
	packet->SetAudioSamples(tmp, false);

	if (mixed_mode && packet->GetModule() == mixed_dstar_module)
	{
		auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
		dv3003->AddPacketToChannel(packet, 0);
	}
	else
		dstar_device->AddPacket(packet);
	codec2_queue.push(packet);

	if (dmrsf_device)
	{
		if (mixed_mode)
		{
			auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
			char mod = packet->GetModule();
			auto it = std::string(g_Conf.GetTCMods()).find(mod);
			uint8_t dmr_ch = (it == 0) ? 1 : 2;
			dv3003->AddPacketToChannel(packet, dmr_ch);
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

	m_agc.Process(tmp, 160, packet->GetStreamId());
	packet->SetAudioSamples(tmp, false);

	if (mixed_mode && packet->GetModule() == mixed_dstar_module)
	{
		auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
		dv3003->AddPacketToChannel(packet, 0);  // ch0 = D-Star
	}
	else
		dstar_device->AddPacket(packet);
	codec2_queue.push(packet);

	if (dmrsf_device)
	{
		if (mixed_mode)
		{
			auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
			char mod = packet->GetModule();
			auto it = std::string(g_Conf.GetTCMods()).find(mod);
			uint8_t dmr_ch = (it == 0) ? 1 : 2;
			dv3003->AddPacketToChannel(packet, dmr_ch);
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
		if (m_agc.IsEnabled())
		{
			int16_t tmp[160];
			memcpy(tmp, packet->GetAudioSamples(), sizeof(tmp));
			m_agc.Process(tmp, 160, packet->GetStreamId());
			packet->SetAudioSamples(tmp, false);
		}
		codec2_queue.push(packet);
		imbe_queue.push(packet);
		usrp_queue.push(packet);
		if (dmrsf_device)
		{
			if (mixed_mode)
			{
				auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
				char mod = packet->GetModule();
				auto it = std::string(g_Conf.GetTCMods()).find(mod);
				uint8_t dmr_ch = (it == 0) ? 1 : 2;
				dv3003->AddPacketToChannel(packet, dmr_ch);
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
		if (m_agc.IsEnabled())
		{
			int16_t tmp[160];
			memcpy(tmp, packet->GetAudioSamples(), sizeof(tmp));
			m_agc.Process(tmp, 160, packet->GetStreamId());
			packet->SetAudioSamples(tmp, false);
		}
		// Re-encode DMR from AGC'd PCM via md380 software vocoder
		// so DMR/YSF output has correct levels (not the original hot passthrough)
		{
			uint8_t ambe2[9];
			md380_encode_fec(ambe2, packet->GetAudioSamples());
			packet->SetDMRData(ambe2);
		}
		codec2_queue.push(packet);
		imbe_queue.push(packet);
		usrp_queue.push(packet);
		if (mixed_mode && packet->GetModule() == mixed_dstar_module)
		{
			auto *dv3003 = dynamic_cast<CDV3003*>(dmrsf_device.get());
			dv3003->AddPacketToChannel(packet, 0);  // ch0 = D-Star
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
