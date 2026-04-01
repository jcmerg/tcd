// StatsLogger — CSV logging of per-frame AGC/level stats

#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "StatsLogger.h"

CStatsLogger g_StatsLog;

bool CStatsLogger::Start()
{
	if (!m_enabled)
		return true;

	// Create directory if needed
	struct stat st{};
	if (stat(m_dir.c_str(), &st) != 0)
	{
		if (mkdir(m_dir.c_str(), 0755) != 0)
		{
			std::cerr << "StatsLogger: cannot create " << m_dir << ": " << strerror(errno) << std::endl;
			m_enabled = false;
			return false;
		}
	}

	m_running = true;
	m_cleanupFuture = std::async(std::launch::async, &CStatsLogger::CleanupThread, this);

	std::cout << "StatsLogger: logging to " << m_dir << " (retain " << m_retainHours << "h)" << std::endl;
	return true;
}

void CStatsLogger::Stop()
{
	m_running = false;

	// Close all open files
	std::lock_guard<std::mutex> lock(m_mutex);
	for (auto &[key, sl] : m_streams)
	{
		if (sl.file.is_open())
			sl.file.close();
	}
	m_streams.clear();

	if (m_cleanupFuture.valid())
		m_cleanupFuture.get();
}

void CStatsLogger::LogFrame(char module, uint16_t streamId, const char *codec,
                            float rms_in, float peak_in, float rms_out, float peak_out,
                            float agc_gain_db, bool gate)
{
	if (!m_enabled)
		return;

	uint32_t key = makeKey(module, streamId);

	std::lock_guard<std::mutex> lock(m_mutex);

	auto it = m_streams.find(key);
	if (it == m_streams.end())
	{
		// Open new file for this stream
		auto now = std::chrono::system_clock::now();
		auto t = std::chrono::system_clock::to_time_t(now);
		struct tm tm{};
		localtime_r(&t, &tm);

		char filename[128];
		snprintf(filename, sizeof(filename), "%c_%04d%02d%02d_%02d%02d%02d_%u.csv",
			module, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, streamId);

		std::string path = m_dir + "/" + filename;

		StreamLog sl;
		sl.path = path;
		sl.file.open(path, std::ios::out);
		if (!sl.file.is_open())
		{
			std::cerr << "StatsLogger: cannot open " << path << std::endl;
			return;
		}

		// CSV header
		sl.file << "ms,module,codec,rms_in,peak_in,rms_out,peak_out,agc_gain,gate\n";
		sl.frame_count = 0;

		auto [ins, _] = m_streams.emplace(key, std::move(sl));
		it = ins;
	}

	auto &sl = it->second;

	// Timestamp: ms since epoch
	auto now = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

	char line[256];
	snprintf(line, sizeof(line), "%lld,%c,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%d\n",
		(long long)ms, module, codec, rms_in, peak_in, rms_out, peak_out, agc_gain_db, gate ? 1 : 0);
	sl.file << line;
	sl.frame_count++;

	// Flush every 50 frames (~1s) to keep I/O reasonable
	if (sl.frame_count % 50 == 0)
		sl.file.flush();
}

void CStatsLogger::CloseStream(char module, uint16_t streamId)
{
	if (!m_enabled)
		return;

	uint32_t key = makeKey(module, streamId);
	std::lock_guard<std::mutex> lock(m_mutex);

	auto it = m_streams.find(key);
	if (it != m_streams.end())
	{
		if (it->second.file.is_open())
			it->second.file.close();
		m_streams.erase(it);
	}
}

void CStatsLogger::CleanupThread()
{
	while (m_running)
	{
		// Run cleanup every 10 minutes
		for (int i = 0; i < 600 && m_running; i++)
			usleep(1000000);

		if (!m_running)
			break;

		// Delete files older than retain period
		auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(m_retainHours);
		auto cutoff_t = std::chrono::system_clock::to_time_t(cutoff);

		DIR *dir = opendir(m_dir.c_str());
		if (!dir) continue;

		struct dirent *entry;
		while ((entry = readdir(dir)) != nullptr)
		{
			if (entry->d_name[0] == '.') continue;

			std::string path = m_dir + "/" + entry->d_name;
			struct stat st{};
			if (stat(path.c_str(), &st) == 0)
			{
				if (st.st_mtime < cutoff_t)
				{
					unlink(path.c_str());
				}
			}
		}
		closedir(dir);
	}
}
