// MonitorServer — HTTP dashboard + WebSocket live stats + TCP stats socket

#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <cmath>

#include "MonitorServer.h"
#include "TcdStats.h"
#include "Configure.h"
#include "mongoose.h"

extern CConfigure g_Conf;
extern CTcdStats g_Stats;
extern CMonitorServer g_Monitor;

// Embedded HTML dashboard (generated from monitor.html)
extern const char monitor_html[];
extern const unsigned int monitor_html_len;

// g_Monitor is defined in Main.cpp

CMonitorServer::CMonitorServer() {}

CMonitorServer::~CMonitorServer()
{
	Stop();
}

bool CMonitorServer::Start(uint16_t http_port, uint16_t stats_port)
{
	m_httpPort = http_port;
	m_statsPort = stats_port;
	m_running = true;

	m_httpFuture = std::async(std::launch::async, &CMonitorServer::HttpThread, this);
	m_statsFuture = std::async(std::launch::async, &CMonitorServer::StatsThread, this);

	std::cout << "Monitor: HTTP on port " << m_httpPort
	          << ", Stats on port " << m_statsPort << std::endl;
	return true;
}

void CMonitorServer::Stop()
{
	m_running = false;
	if (m_httpFuture.valid())
		m_httpFuture.get();
	if (m_statsFuture.valid())
		m_statsFuture.get();
}

////////////////////////////////////////////////////////////////////////////////
// HTTP + WebSocket thread (mongoose)

static const char *s_codec_names[] = {
	"none", "D-Star", "DMR/YSF", "Codec2 1600", "Codec2 3200", "P25", "USRP", "SVX"
};

static const char *codec_name(uint8_t c)
{
	return (c < 8) ? s_codec_names[c] : "unknown";
}

std::string CMonitorServer::BuildStatsJson()
{
	std::ostringstream js;
	js << std::fixed;
	js.precision(1);

	js << "{\"modules\":{";
	bool first = true;
	for (char mod : g_Stats.module_letters)
	{
		int idx = mod - 'A';
		if (idx < 0 || idx >= CTcdStats::MAX_MODULES) continue;
		auto &m = g_Stats.modules[idx];

		if (!first) js << ",";
		first = false;

		js << "\"" << mod << "\":{"
		   << "\"codec_in\":\"" << codec_name(m.codec_in.load(std::memory_order_relaxed)) << "\""
		   << ",\"stream_id\":" << m.stream_id.load(std::memory_order_relaxed)
		   << ",\"rms_in\":" << m.rms_in.load(std::memory_order_relaxed)
		   << ",\"peak_in\":" << m.peak_in.load(std::memory_order_relaxed)
		   << ",\"rms_out\":" << m.rms_out.load(std::memory_order_relaxed)
		   << ",\"peak_out\":" << m.peak_out.load(std::memory_order_relaxed)
		   << ",\"agc_gain\":" << m.agc_gain_db.load(std::memory_order_relaxed)
		   << ",\"agc_gate\":" << (m.agc_gate.load(std::memory_order_relaxed) ? "true" : "false")
		   << ",\"pkts_in\":" << m.packets_in.load(std::memory_order_relaxed)
		   << ",\"pkts_out\":" << m.packets_out.load(std::memory_order_relaxed)
		   << ",\"idle\":" << m.last_activity.load(std::memory_order_relaxed)
		   << "}";
	}
	js << "},\"devices\":[";
	for (int i = 0; i < g_Stats.num_devices; i++)
	{
		auto &d = g_Stats.devices[i];
		if (i > 0) js << ",";
		js << "{\"serial\":\"" << d.serial << "\""
		   << ",\"type\":\"" << d.type << "\""
		   << ",\"buf_depth\":" << d.buf_depth.load(std::memory_order_relaxed)
		   << ",\"errors\":" << d.errors.load(std::memory_order_relaxed)
		   << ",\"online\":" << (d.online.load(std::memory_order_relaxed) ? "true" : "false")
		   << "}";
	}
	js << "],\"reflector\":{"
	   << "\"connected\":" << (g_Stats.reflector.connected.load(std::memory_order_relaxed) ? "true" : "false")
	   << ",\"pkts_rx\":" << g_Stats.reflector.packets_rx.load(std::memory_order_relaxed)
	   << ",\"pkts_tx\":" << g_Stats.reflector.packets_tx.load(std::memory_order_relaxed)
	   << "},\"config\":{"
	   << "\"agc_enabled\":" << (g_Stats.config.agc_enabled.load() ? "true" : "false")
	   << ",\"agc_target\":" << g_Stats.config.agc_target.load()
	   << ",\"agc_attack\":" << g_Stats.config.agc_attack.load()
	   << ",\"agc_release\":" << g_Stats.config.agc_release.load()
	   << ",\"agc_maxgain_up\":" << g_Stats.config.agc_maxgain_up.load()
	   << ",\"agc_maxgain_down\":" << g_Stats.config.agc_maxgain_down.load()
	   << ",\"agc_noisegate\":" << g_Stats.config.agc_noisegate.load()
	   << ",\"gain_dstar_in\":" << g_Stats.config.gain_dstar_in.load()
	   << ",\"gain_dstar_out\":" << g_Stats.config.gain_dstar_out.load()
	   << ",\"gain_dmr_in\":" << g_Stats.config.gain_dmr_in.load()
	   << ",\"gain_dmr_out\":" << g_Stats.config.gain_dmr_out.load()
	   << ",\"gain_usrp_rx\":" << g_Stats.config.gain_usrp_rx.load()
	   << ",\"gain_usrp_tx\":" << g_Stats.config.gain_usrp_tx.load()
	   << ",\"gain_dmr_reencode\":" << g_Stats.config.gain_dmr_reencode.load()
	   << "}}";

	return js.str();
}

// Simple JSON value parser (no library dependency)
static bool json_get_int(const char *json, size_t len, const char *key, int &val)
{
	char search[64];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = strstr(json, search);
	if (!p) return false;
	p += strlen(search);
	while (*p == ' ') p++;
	val = atoi(p);
	return true;
}

static bool json_get_float(const char *json, size_t len, const char *key, float &val)
{
	char search[64];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = strstr(json, search);
	if (!p) return false;
	p += strlen(search);
	while (*p == ' ') p++;
	val = strtof(p, nullptr);
	return true;
}

static bool json_get_bool(const char *json, size_t len, const char *key, bool &val)
{
	char search[64];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = strstr(json, search);
	if (!p) return false;
	p += strlen(search);
	while (*p == ' ') p++;
	val = (*p == 't' || *p == 'T' || *p == '1');
	return true;
}

void CMonitorServer::HandleRest(struct mg_connection *c, struct mg_http_message *hm)
{
	if (mg_match(hm->uri, mg_str("/api/stats"), NULL))
	{
		std::string json = g_Monitor.BuildStatsJson();
		mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n", "%s", json.c_str());
	}
	else if (mg_match(hm->uri, mg_str("/api/gain"), NULL) && hm->body.len > 0)
	{
		// POST {"type": "dstar_in", "db": 8}
		int db = 0;
		if (json_get_int(hm->body.buf, hm->body.len, "db", db))
		{
			if (db < -24) db = -24;
			if (db > 24) db = 24;

			// Find which gain to set
			char search[64];
			snprintf(search, sizeof(search), "\"type\":\"");
			const char *p = strstr(hm->body.buf, search);
			if (p)
			{
				p += strlen(search);
				if      (strncmp(p, "dstar_in", 8) == 0)      g_Stats.config.gain_dstar_in.store(db);
				else if (strncmp(p, "dstar_out", 9) == 0)     g_Stats.config.gain_dstar_out.store(db);
				else if (strncmp(p, "dmr_in", 6) == 0)        g_Stats.config.gain_dmr_in.store(db);
				else if (strncmp(p, "dmr_out", 7) == 0)       g_Stats.config.gain_dmr_out.store(db);
				else if (strncmp(p, "usrp_rx", 7) == 0)       g_Stats.config.gain_usrp_rx.store(db);
				else if (strncmp(p, "usrp_tx", 7) == 0)       g_Stats.config.gain_usrp_tx.store(db);
				else if (strncmp(p, "dmr_reencode", 12) == 0)  g_Stats.config.gain_dmr_reencode.store(db);
			}
		}
		mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n", "{\"status\":\"ok\"}");
	}
	else if (mg_match(hm->uri, mg_str("/api/agc"), NULL) && hm->body.len > 0)
	{
		bool enabled;
		float target, attack, release;
		if (json_get_bool(hm->body.buf, hm->body.len, "enabled", enabled))
			g_Stats.config.agc_enabled.store(enabled);
		if (json_get_float(hm->body.buf, hm->body.len, "target", target))
			g_Stats.config.agc_target.store(target);
		if (json_get_float(hm->body.buf, hm->body.len, "attack", attack))
			g_Stats.config.agc_attack.store(attack);
		if (json_get_float(hm->body.buf, hm->body.len, "release", release))
			g_Stats.config.agc_release.store(release);
		float maxgain_up, maxgain_down;
		if (json_get_float(hm->body.buf, hm->body.len, "maxgain_up", maxgain_up))
			g_Stats.config.agc_maxgain_up.store(maxgain_up);
		if (json_get_float(hm->body.buf, hm->body.len, "maxgain_down", maxgain_down))
			g_Stats.config.agc_maxgain_down.store(maxgain_down);
		float noisegate;
		if (json_get_float(hm->body.buf, hm->body.len, "noisegate", noisegate))
			g_Stats.config.agc_noisegate.store(noisegate);

		mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n", "{\"status\":\"ok\"}");
	}
	else if (mg_match(hm->uri, mg_str("/api/save"), NULL))
	{
		bool ok = g_Monitor.SaveConfig(g_Conf.GetIniPath());
		if (ok)
			mg_http_reply(c, 200, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
				"{\"status\":\"ok\",\"message\":\"saved to %s\"}", g_Conf.GetIniPath().c_str());
		else
			mg_http_reply(c, 500, "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n",
				"{\"status\":\"error\",\"message\":\"failed to write %s\"}", g_Conf.GetIniPath().c_str());
	}
	else
	{
		mg_http_reply(c, 404, "", "Not found\n");
	}
}

void CMonitorServer::HandleWebSocket(struct mg_connection *c)
{
	// WebSocket connections get stats pushed in the poll loop
	// Nothing to handle on message — client is read-only
}

void CMonitorServer::HttpHandler(struct mg_connection *c, int ev, void *ev_data)
{
	if (ev == MG_EV_HTTP_MSG)
	{
		struct mg_http_message *hm = (struct mg_http_message *)ev_data;

		if (mg_match(hm->uri, mg_str("/ws"), NULL))
		{
			mg_ws_upgrade(c, hm, NULL);
		}
		else if (mg_match(hm->uri, mg_str("/api/#"), NULL))
		{
			HandleRest(c, hm);
		}
		else
		{
			// Serve embedded HTML dashboard
			mg_http_reply(c, 200,
				"Content-Type: text/html\r\nAccess-Control-Allow-Origin: *\r\n",
				"%.*s", (int)monitor_html_len, monitor_html);
		}
	}
	else if (ev == MG_EV_WS_MSG)
	{
		HandleWebSocket(c);
	}
}

void CMonitorServer::HttpThread()
{
	struct mg_mgr mgr;
	mg_log_set(MG_LL_NONE);
	mg_mgr_init(&mgr);

	char listen_url[64];
	snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%u", m_httpPort);
	mg_http_listen(&mgr, listen_url, HttpHandler, NULL);

	uint64_t last_push = 0;
	while (m_running)
	{
		mg_mgr_poll(&mgr, 50);

		// Push stats to all WebSocket clients every 200ms
		uint64_t now = mg_millis();
		if (now - last_push >= 200)
		{
			last_push = now;
			std::string json = BuildStatsJson();
			struct mg_connection *c;
			for (c = mgr.conns; c != NULL; c = c->next)
			{
				if (c->is_websocket)
					mg_ws_send(c, json.c_str(), json.size(), WEBSOCKET_OP_TEXT);
			}
		}
	}

	mg_mgr_free(&mgr);
}

////////////////////////////////////////////////////////////////////////////////
// Plain TCP stats socket (for tcdmon / scripting)

void CMonitorServer::StatsThread()
{
	int listenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenFd < 0)
	{
		std::cerr << "Monitor: stats socket failed: " << strerror(errno) << std::endl;
		return;
	}

	int opt = 1;
	setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	// Non-blocking listen socket
	int flags = fcntl(listenFd, F_GETFL, 0);
	fcntl(listenFd, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(m_statsPort);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		std::cerr << "Monitor: stats bind failed on port " << m_statsPort << ": " << strerror(errno) << std::endl;
		close(listenFd);
		return;
	}

	listen(listenFd, 4);

	// Track connected stats clients
	std::vector<int> clients;

	while (m_running)
	{
		// Accept new connections (non-blocking)
		struct sockaddr_in clientAddr{};
		socklen_t addrLen = sizeof(clientAddr);
		int clientFd = accept(listenFd, (struct sockaddr *)&clientAddr, &addrLen);
		if (clientFd >= 0)
		{
			// Set non-blocking
			int fl = fcntl(clientFd, F_GETFL, 0);
			fcntl(clientFd, F_SETFL, fl | O_NONBLOCK);
			clients.push_back(clientFd);
		}

		if (!clients.empty())
		{
			// Push stats JSON to all connected clients
			std::string json = BuildStatsJson() + "\n";
			auto it = clients.begin();
			while (it != clients.end())
			{
				ssize_t n = send(*it, json.c_str(), json.size(), MSG_NOSIGNAL);
				if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
				{
					close(*it);
					it = clients.erase(it);
				}
				else
					++it;
			}

			// Drain any input from clients (commands)
			char buf[512];
			for (int fd : clients)
				while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
		}

		// 200ms push interval
		usleep(200000);
	}

	for (int fd : clients)
		close(fd);
	close(listenFd);
}

////////////////////////////////////////////////////////////////////////////////
// Save config to ini file

bool CMonitorServer::SaveConfig(const std::string &ini_path)
{
	// Read existing ini, update gain/AGC values, write back
	std::ifstream in(ini_path);
	if (!in.is_open()) return false;

	std::ostringstream out;
	std::string line;
	while (std::getline(in, line))
	{
		// Check if this line matches a configurable key
		std::string trimmed = line;
		// skip comments
		if (!trimmed.empty() && trimmed[0] != '#')
		{
			auto eq = trimmed.find('=');
			if (eq != std::string::npos)
			{
				std::string key = trimmed.substr(0, eq);
				// trim key
				while (!key.empty() && key.back() == ' ') key.pop_back();

				if (key == "DStarGainIn")       { out << key << " = " << g_Stats.config.gain_dstar_in.load() << "\n"; continue; }
				if (key == "DStarGainOut")      { out << key << " = " << g_Stats.config.gain_dstar_out.load() << "\n"; continue; }
				if (key == "DmrYsfGainIn")      { out << key << " = " << g_Stats.config.gain_dmr_in.load() << "\n"; continue; }
				if (key == "DmrYsfGainOut")     { out << key << " = " << g_Stats.config.gain_dmr_out.load() << "\n"; continue; }
				if (key == "UsrpRxGain")        { out << key << " = " << g_Stats.config.gain_usrp_rx.load() << "\n"; continue; }
				if (key == "UsrpTxGain")        { out << key << " = " << g_Stats.config.gain_usrp_tx.load() << "\n"; continue; }
				if (key == "DmrReencodeGain")   { out << key << " = " << g_Stats.config.gain_dmr_reencode.load() << "\n"; continue; }
				if (key == "AGC")               { out << key << " = " << (g_Stats.config.agc_enabled.load() ? "true" : "false") << "\n"; continue; }
				if (key == "AGCTarget")         { out << key << " = " << g_Stats.config.agc_target.load() << "\n"; continue; }
				if (key == "AGCAttack")         { out << key << " = " << g_Stats.config.agc_attack.load() << "\n"; continue; }
				if (key == "AGCRelease")        { out << key << " = " << g_Stats.config.agc_release.load() << "\n"; continue; }
				if (key == "AGCMaxGain")        { out << "AGCMaxGainUp = " << g_Stats.config.agc_maxgain_up.load() << "\n"
			                                       << "AGCMaxGainDown = " << g_Stats.config.agc_maxgain_down.load() << "\n"; continue; }
			if (key == "AGCMaxGainUp")     { out << key << " = " << g_Stats.config.agc_maxgain_up.load() << "\n"; continue; }
			if (key == "AGCMaxGainDown")   { out << key << " = " << g_Stats.config.agc_maxgain_down.load() << "\n"; continue; }
			}
		}
		out << line << "\n";
	}
	in.close();

	std::ofstream of(ini_path);
	if (!of.is_open()) return false;
	of << out.str();
	return true;
}
