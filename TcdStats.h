#pragma once

// TcdStats — real-time metrics for monitoring (web dashboard + tcdmon)
// Updated by audio pipeline threads, read by MonitorServer.
// Uses atomics for lock-free access from monitor thread.

#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>

#include "TCPacketDef.h"

struct ModuleStats
{
	// Audio levels in dBFS (updated per 20ms frame)
	std::atomic<float> rms_in{-100.0f};     // before AGC
	std::atomic<float> peak_in{-100.0f};
	std::atomic<float> rms_out{-100.0f};     // after AGC
	std::atomic<float> peak_out{-100.0f};

	// AGC state
	std::atomic<float> agc_gain_db{0.0f};    // current gain in dB
	std::atomic<bool>  agc_gate{false};       // noise gate active (with hysteresis)
	std::atomic<int>   agc_gate_count{0};     // consecutive gate frames (for hysteresis)

	// Stream info
	std::atomic<uint16_t> stream_id{0};
	std::atomic<uint8_t>  codec_in{0};        // ECodecType cast

	// Packet counters
	std::atomic<uint32_t> packets_in{0};      // from reflector
	std::atomic<uint32_t> packets_out{0};     // to reflector

	// Timing
	std::atomic<float> last_activity{0.0f};   // seconds since last packet (0 = active)
};

struct DeviceStats
{
	static constexpr int MAX_CHANNELS = 3;
	std::string serial;
	std::string type;       // "DV3000" or "DV3003"
	std::string role;       // "dstar", "dmr", "mixed"
	std::atomic<unsigned int> buf_depth{0};
	std::atomic<uint32_t> errors{0};
	std::atomic<bool> online{false};
	// Per-channel activity: module letter + timestamp (ms since epoch)
	std::atomic<char> ch_module[MAX_CHANNELS] = {' ', ' ', ' '};
	std::atomic<uint64_t> ch_last_ms[MAX_CHANNELS] = {0, 0, 0};
};

struct Md380Stats
{
	std::atomic<bool> available{false};       // md380 vocoder is linked
	std::atomic<bool> reencode_active{false}; // re-encode triggered by AGC/OutputGainDMR
	std::atomic<uint32_t> encodes{0};        // total encode calls
	std::atomic<uint32_t> decodes{0};        // total decode calls
	std::atomic<uint32_t> reencodes{0};      // total re-encode calls
	std::atomic<int> cached_streams{0};      // streams with cached md380 state
	std::atomic<char> active_module{' '};    // last active module
	std::atomic<uint64_t> last_active_ms{0}; // timestamp of last operation
};

struct ReflectorStats
{
	std::atomic<bool> connected{false};
	std::atomic<uint32_t> packets_rx{0};
	std::atomic<uint32_t> packets_tx{0};
};

class CTcdStats
{
public:
	static constexpr int MAX_MODULES = 26;
	static constexpr int MAX_DEVICES = 4;

	ModuleStats    modules[MAX_MODULES];    // indexed by module - 'A'
	DeviceStats    devices[MAX_DEVICES];
	Md380Stats     md380;
	int            num_devices{0};
	ReflectorStats reflector;

	// Configuration (readable by monitor, writable by REST)
	struct Config
	{
		std::atomic<bool>  agc_enabled{false};
		std::atomic<float> agc_target{-16.0f};
		std::atomic<float> agc_attack{50.0f};
		std::atomic<float> agc_release{500.0f};
		std::atomic<float> agc_maxgain_up{20.0f};
		std::atomic<float> agc_maxgain_down{24.0f};
		std::atomic<float> agc_noisegate{-55.0f};
		std::atomic<int>   gain_dstar_in{0};
		std::atomic<int>   gain_dstar_out{0};
		std::atomic<int>   gain_dmr_in{0};
		std::atomic<int>   gain_dmr_out{0};
		std::atomic<int>   gain_usrp_rx{0};
		std::atomic<int>   gain_usrp_tx{0};
		std::atomic<int>   gain_dmr_reencode{0};
		std::atomic<int>   outgain_dstar{0};
		std::atomic<int>   outgain_dmr{0};
		std::atomic<int>   outgain_usrp{0};
		std::atomic<int>   outgain_imbe{0};
		std::atomic<int>   outgain_m17{0};
		std::atomic<bool>  dmr_reencode_enabled{false};
		std::atomic<bool>  ambe_gain_enabled{true};
		std::atomic<int>   ambe_gain_db{-2};
	} config;

	// Module name list (set at startup)
	std::string module_letters;

	// Helper: compute dBFS from PCM samples
	static void MeasureLevels(const int16_t *samples, int count, float &rms_dbfs, float &peak_dbfs)
	{
		float sumSq = 0.0f;
		float peak = 0.0f;
		for (int i = 0; i < count; i++)
		{
			float s = (float)samples[i];
			sumSq += s * s;
			float a = fabsf(s);
			if (a > peak) peak = a;
		}
		float rms = sqrtf(sumSq / count);
		rms_dbfs  = (rms  > 0.0f) ? 20.0f * log10f(rms  / 32767.0f) : -100.0f;
		peak_dbfs = (peak > 0.0f) ? 20.0f * log10f(peak / 32767.0f) : -100.0f;
	}

	// Update pre-AGC levels for a module
	void UpdateLevelsIn(char module, const int16_t *samples, int count)
	{
		int idx = module - 'A';
		if (idx < 0 || idx >= MAX_MODULES) return;
		float rms, peak;
		MeasureLevels(samples, count, rms, peak);
		modules[idx].rms_in.store(rms, std::memory_order_relaxed);
		modules[idx].peak_in.store(peak, std::memory_order_relaxed);
	}

	// Update post-AGC levels for a module
	void UpdateLevelsOut(char module, const int16_t *samples, int count)
	{
		int idx = module - 'A';
		if (idx < 0 || idx >= MAX_MODULES) return;
		float rms, peak;
		MeasureLevels(samples, count, rms, peak);
		modules[idx].rms_out.store(rms, std::memory_order_relaxed);
		modules[idx].peak_out.store(peak, std::memory_order_relaxed);
	}

	// Update AGC state for a module (gate with hysteresis: 5 consecutive frames = ~100ms)
	void UpdateAGC(char module, float gain_db, bool gate)
	{
		int idx = module - 'A';
		if (idx < 0 || idx >= MAX_MODULES) return;
		modules[idx].agc_gain_db.store(gain_db, std::memory_order_relaxed);
		if (gate)
		{
			int cnt = modules[idx].agc_gate_count.fetch_add(1, std::memory_order_relaxed) + 1;
			modules[idx].agc_gate.store(cnt >= 5, std::memory_order_relaxed);  // ~100ms of silence
		}
		else
		{
			modules[idx].agc_gate_count.store(0, std::memory_order_relaxed);
			modules[idx].agc_gate.store(false, std::memory_order_relaxed);
		}
	}

	// Record incoming packet
	void PacketIn(char module, uint16_t sid, ECodecType codec)
	{
		int idx = module - 'A';
		if (idx < 0 || idx >= MAX_MODULES) return;
		modules[idx].packets_in.fetch_add(1, std::memory_order_relaxed);
		modules[idx].stream_id.store(sid, std::memory_order_relaxed);
		modules[idx].codec_in.store((uint8_t)codec, std::memory_order_relaxed);
		modules[idx].last_activity.store(0.0f, std::memory_order_relaxed);
	}

	// Record outgoing packet
	void PacketOut(char module)
	{
		int idx = module - 'A';
		if (idx < 0 || idx >= MAX_MODULES) return;
		modules[idx].packets_out.fetch_add(1, std::memory_order_relaxed);
	}
};

// Global instance
extern CTcdStats g_Stats;
