#pragma once

// MonitorServer — embedded HTTP + WebSocket + plain TCP stats server
// Uses mongoose for HTTP/WS, plus a separate TCP stats socket for tcdmon.
// No authentication — designed for local/trusted networks.

#include <atomic>
#include <future>
#include <string>

struct mg_mgr;

class CMonitorServer
{
public:
	CMonitorServer();
	~CMonitorServer();

	bool Start(uint16_t http_port, uint16_t stats_port);
	void Stop();
	bool IsRunning() const { return m_running.load(); }

	// Called by REST handler to save config to ini file
	bool SaveConfig(const std::string &ini_path);

private:
	void HttpThread();
	void StatsThread();

	// Mongoose event handler
	static void HttpHandler(struct mg_connection *c, int ev, void *ev_data);
	static void HandleWebSocket(struct mg_connection *c);
	static void HandleRest(struct mg_connection *c, struct mg_http_message *hm);

	// Stats socket (plain TCP, line-delimited JSON)
	void StatsAcceptLoop();
	std::string BuildStatsJson();

	std::atomic<bool> m_running{false};
	uint16_t m_httpPort{8080};
	uint16_t m_statsPort{8081};
	std::future<void> m_httpFuture;
	std::future<void> m_statsFuture;
};

// Defined in Main.cpp, used by MonitorServer.cpp static handlers
extern CMonitorServer g_Monitor;
