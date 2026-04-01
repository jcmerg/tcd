#pragma once

// StatsLogger — writes per-frame AGC/level statistics to CSV files
// One file per stream (named by module + streamId), auto-cleanup of old files.
// Called from audio pipeline threads, file I/O is buffered and fast.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <fstream>
#include <mutex>
#include <chrono>
#include <atomic>
#include <future>

class CStatsLogger
{
public:
	void Configure(bool enable, const std::string &dir, int retain_hours)
	{
		m_enabled = enable;
		m_dir = dir;
		m_retainHours = retain_hours;
	}

	bool Start();
	void Stop();

	// Log one frame (called from audio pipeline, ~every 20ms)
	void LogFrame(char module, uint16_t streamId, const char *codec,
	              float rms_in, float peak_in, float rms_out, float peak_out,
	              float agc_gain_db, bool gate);

	// Called when a stream ends
	void CloseStream(char module, uint16_t streamId);

	bool IsEnabled() const { return m_enabled; }

private:
	struct StreamLog
	{
		std::ofstream file;
		std::string path;
		uint32_t frame_count{0};
	};

	// Key: module << 16 | streamId
	uint32_t makeKey(char module, uint16_t sid) const { return ((uint32_t)module << 16) | sid; }

	void CleanupThread();

	bool m_enabled{false};
	std::string m_dir;
	int m_retainHours{24};
	std::mutex m_mutex;
	std::unordered_map<uint32_t, StreamLog> m_streams;

	std::atomic<bool> m_running{false};
	std::future<void> m_cleanupFuture;
};

extern CStatsLogger g_StatsLog;
