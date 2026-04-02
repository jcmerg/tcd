// tcdmon — ncurses monitoring tool for tcd transcoder
// Connects to tcd stats socket (TCP), displays live audio levels,
// AGC state, device status. Works over SSH.
//
// Usage: tcdmon [host] [port]
//        Default: 127.0.0.1 8081

#include <ncurses.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

// Minimal JSON value extraction (no library dependency)
static std::string json_str(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":\"";
	auto pos = json.find(search);
	if (pos == std::string::npos) return "";
	pos += search.size();
	auto end = json.find('"', pos);
	if (end == std::string::npos) return "";
	return json.substr(pos, end - pos);
}

static double json_num(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":";
	auto pos = json.find(search);
	if (pos == std::string::npos) return 0.0;
	pos += search.size();
	return strtod(json.c_str() + pos, nullptr);
}

static bool json_bool(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":";
	auto pos = json.find(search);
	if (pos == std::string::npos) return false;
	pos += search.size();
	return json[pos] == 't';
}

static int json_int(const std::string &json, const std::string &key)
{
	return (int)json_num(json, key);
}

// Extract a JSON sub-object by key
static std::string json_obj(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":{";
	auto pos = json.find(search);
	if (pos == std::string::npos) return "";
	pos = json.find('{', pos);
	int depth = 0;
	size_t start = pos;
	for (size_t i = pos; i < json.size(); i++)
	{
		if (json[i] == '{') depth++;
		else if (json[i] == '}') { depth--; if (depth == 0) return json.substr(start, i - start + 1); }
	}
	return "";
}

// Extract JSON array
static std::string json_arr(const std::string &json, const std::string &key)
{
	std::string search = "\"" + key + "\":[";
	auto pos = json.find(search);
	if (pos == std::string::npos) return "[]";
	pos = json.find('[', pos);
	int depth = 0;
	size_t start = pos;
	for (size_t i = pos; i < json.size(); i++)
	{
		if (json[i] == '[') depth++;
		else if (json[i] == ']') { depth--; if (depth == 0) return json.substr(start, i - start + 1); }
	}
	return "[]";
}

// Split array into objects
static std::vector<std::string> json_arr_items(const std::string &arr)
{
	std::vector<std::string> items;
	int depth = 0;
	size_t start = 0;
	for (size_t i = 0; i < arr.size(); i++)
	{
		if (arr[i] == '{') { if (depth == 0) start = i; depth++; }
		else if (arr[i] == '}') { depth--; if (depth == 0) items.push_back(arr.substr(start, i - start + 1)); }
	}
	return items;
}

// Extract module keys from {"modules":{"F":{...},"S":{...}}}
static std::map<char, std::string> json_modules(const std::string &json)
{
	std::map<char, std::string> result;
	std::string mods = json_obj(json, "modules");
	if (mods.empty()) return result;

	// Find each single-letter key
	size_t pos = 0;
	while ((pos = mods.find("\"", pos)) != std::string::npos)
	{
		pos++;
		if (pos + 2 < mods.size() && mods[pos + 1] == '"' && mods[pos] >= 'A' && mods[pos] <= 'Z')
		{
			char mod = mods[pos];
			auto brace = mods.find('{', pos);
			if (brace == std::string::npos) break;
			int depth = 0;
			for (size_t i = brace; i < mods.size(); i++)
			{
				if (mods[i] == '{') depth++;
				else if (mods[i] == '}')
				{
					depth--;
					if (depth == 0)
					{
						result[mod] = mods.substr(brace, i - brace + 1);
						pos = i + 1;
						break;
					}
				}
			}
		}
	}
	return result;
}

struct ModuleData
{
	std::string codec_in;
	double rms_in, peak_in, rms_out, peak_out;
	double agc_gain, idle;
	bool agc_gate;
	int pkts_in, pkts_out;
};

struct DeviceData
{
	std::string serial, type, role;
	int buf_depth, errors;
	bool online;
	std::vector<std::string> ch_modules;  // active module per channel
};

static volatile sig_atomic_t g_resize = 0;
static void handle_winch(int) { g_resize = 1; }

// Draw a horizontal VU bar
static void draw_vu(int y, int x, int width, const char *label, double rms, double peak)
{
	// Scale: -60 dBFS = empty, 0 dBFS = full
	int bar_w = width - 14;  // label(8) + value(6)
	if (bar_w < 10) bar_w = 10;

	double frac = (rms + 60.0) / 60.0;
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	int filled = (int)(frac * bar_w);

	double pfrac = (peak + 60.0) / 60.0;
	if (pfrac < 0) pfrac = 0;
	if (pfrac > 1) pfrac = 1;
	int peak_pos = (int)(pfrac * bar_w);

	mvprintw(y, x, "%-7s ", label);

	for (int i = 0; i < bar_w; i++)
	{
		if (i == peak_pos)
		{
			attron(COLOR_PAIR(4) | A_BOLD);
			addch('|');
			attroff(COLOR_PAIR(4) | A_BOLD);
		}
		else if (i < filled)
		{
			// Green < -18, Yellow < -6, Red >= -6
			double db_at = -60.0 + (double)i / bar_w * 60.0;
			if (db_at >= -6) attron(COLOR_PAIR(3));
			else if (db_at >= -18) attron(COLOR_PAIR(2));
			else attron(COLOR_PAIR(1));
			addch('#');
			attroff(COLOR_PAIR(1) | COLOR_PAIR(2) | COLOR_PAIR(3));
		}
		else
		{
			attron(COLOR_PAIR(5));
			addch('-');
			attroff(COLOR_PAIR(5));
		}
	}

	char val[16];
	if (rms > -99) snprintf(val, sizeof(val), "%5.1f", rms);
	else snprintf(val, sizeof(val), " -inf");
	mvprintw(y, x + 8 + bar_w, "%s", val);
}

// Draw signal flow line
static void draw_flow(int y, int x, int width, const ModuleData &m, const std::string &cfg_json)
{
	if (m.codec_in == "none" || m.idle > 2.0)
	{
		attron(COLOR_PAIR(5));
		mvprintw(y, x, "idle");
		attroff(COLOR_PAIR(5));
		return;
	}

	int col = x;

	// Source codec
	attron(COLOR_PAIR(1) | A_BOLD);
	mvprintw(y, col, "%s", m.codec_in.c_str());
	attroff(COLOR_PAIR(1) | A_BOLD);
	col += m.codec_in.size();

	attron(COLOR_PAIR(5));
	mvprintw(y, col, " > ");
	attroff(COLOR_PAIR(5));
	col += 3;

	// Pre-AGC level
	attron(COLOR_PAIR(2));
	char buf[32];
	snprintf(buf, sizeof(buf), "PCM %.0fdB", m.rms_in);
	mvprintw(y, col, "%s", buf);
	attroff(COLOR_PAIR(2));
	col += strlen(buf);

	attron(COLOR_PAIR(5));
	mvprintw(y, col, " > ");
	attroff(COLOR_PAIR(5));
	col += 3;

	// AGC
	bool agc_on = json_bool(cfg_json, "agc_enabled");
	if (agc_on)
	{
		if (m.agc_gate)
		{
			attron(COLOR_PAIR(3) | A_BOLD);
			mvprintw(y, col, "GATE");
			attroff(COLOR_PAIR(3) | A_BOLD);
			col += 4;
		}
		else
		{
			attron(COLOR_PAIR(6) | A_BOLD);
			snprintf(buf, sizeof(buf), "AGC %+.1fdB", m.agc_gain);
			mvprintw(y, col, "%s", buf);
			attroff(COLOR_PAIR(6) | A_BOLD);
			col += strlen(buf);
		}
	}
	else
	{
		attron(COLOR_PAIR(5));
		mvprintw(y, col, "no AGC");
		attroff(COLOR_PAIR(5));
		col += 6;
	}

	attron(COLOR_PAIR(5));
	mvprintw(y, col, " > ");
	attroff(COLOR_PAIR(5));
	col += 3;

	// Post-AGC level
	attron(COLOR_PAIR(2));
	snprintf(buf, sizeof(buf), "PCM %.0fdB", m.rms_out);
	mvprintw(y, col, "%s", buf);
	attroff(COLOR_PAIR(2));
	col += strlen(buf);

	attron(COLOR_PAIR(5));
	mvprintw(y, col, " > ");
	attroff(COLOR_PAIR(5));
	col += 3;

	// Target codecs
	attron(COLOR_PAIR(3) | A_BOLD);
	std::string targets;
	if (m.codec_in.find("Star") == std::string::npos) targets += "D-Star ";
	if (m.codec_in.find("DMR") == std::string::npos) targets += "DMR/YSF ";
	if (m.codec_in.find("Codec2") == std::string::npos) targets += "M17 ";
	if (m.codec_in.find("P25") == std::string::npos) targets += "P25/NXDN ";
	if (m.codec_in != "USRP") targets += "USRP ";
	if (m.codec_in != "SVX") targets += "SVX";
	mvprintw(y, col, "%s", targets.c_str());
	attroff(COLOR_PAIR(3) | A_BOLD);
}

int main(int argc, char *argv[])
{
	const char *host = "127.0.0.1";
	int port = 8081;

	if (argc >= 2) host = argv[1];
	if (argc >= 3) port = atoi(argv[2]);

	// Connect to tcd stats socket
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { perror("socket"); return 1; }

	struct sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
	{
		struct hostent *he = gethostbyname(host);
		if (!he) { fprintf(stderr, "Cannot resolve %s\n", host); return 1; }
		memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
	}

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		fprintf(stderr, "Cannot connect to %s:%d: %s\n", host, port, strerror(errno));
		return 1;
	}

	// Non-blocking reads
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	// Setup ncurses
	signal(SIGWINCH, handle_winch);
	initscr();
	cbreak();
	noecho();
	nodelay(stdscr, TRUE);
	curs_set(0);
	keypad(stdscr, TRUE);

	if (has_colors())
	{
		start_color();
		use_default_colors();
		init_pair(1, COLOR_GREEN, -1);
		init_pair(2, COLOR_YELLOW, -1);
		init_pair(3, COLOR_RED, -1);
		init_pair(4, COLOR_WHITE, -1);
		init_pair(5, 8, -1);  // dark gray (if supported)
		init_pair(6, COLOR_MAGENTA, -1);
		init_pair(7, COLOR_CYAN, -1);
	}

	std::string linebuf;
	std::string last_json;

	while (true)
	{
		// Check for keypress
		int ch = getch();
		if (ch == 'q' || ch == 'Q') break;

		if (g_resize)
		{
			g_resize = 0;
			endwin();
			refresh();
		}

		// Read data from stats socket
		char buf[8192];
		ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
		if (n > 0)
		{
			buf[n] = '\0';
			linebuf += buf;

			// Process complete lines
			size_t nlpos;
			while ((nlpos = linebuf.find('\n')) != std::string::npos)
			{
				last_json = linebuf.substr(0, nlpos);
				linebuf.erase(0, nlpos + 1);
			}
		}
		else if (n == 0)
		{
			// Connection closed
			break;
		}

		if (last_json.empty())
		{
			usleep(50000);
			continue;
		}

		// Parse and render
		auto modules = json_modules(last_json);
		auto dev_arr = json_arr(last_json, "devices");
		auto dev_items = json_arr_items(dev_arr);
		auto ref_json = json_obj(last_json, "reflector");
		auto cfg_json = json_obj(last_json, "config");

		int rows, cols;
		getmaxyx(stdscr, rows, cols);

		erase();

		// Header
		attron(COLOR_PAIR(7) | A_BOLD);
		mvprintw(0, 0, "TCD Transcoder Monitor");
		attroff(COLOR_PAIR(7) | A_BOLD);

		// Reflector status
		bool ref_conn = json_bool(ref_json, "connected");
		int ref_rx = json_int(ref_json, "pkts_rx");
		int ref_tx = json_int(ref_json, "pkts_tx");
		if (ref_conn)
		{
			attron(COLOR_PAIR(1));
			mvprintw(0, cols - 35, "Reflector: CONNECTED");
			attroff(COLOR_PAIR(1));
		}
		else
		{
			attron(COLOR_PAIR(3) | A_BOLD);
			mvprintw(0, cols - 35, "Reflector: DISCONNECTED");
			attroff(COLOR_PAIR(3) | A_BOLD);
		}
		mvprintw(0, cols - 14, "RX:%d TX:%d", ref_rx, ref_tx);

		int row = 2;

		// Modules
		for (auto &[mod, mj] : modules)
		{
			ModuleData m;
			m.codec_in = json_str(mj, "codec_in");
			m.rms_in = json_num(mj, "rms_in");
			m.peak_in = json_num(mj, "peak_in");
			m.rms_out = json_num(mj, "rms_out");
			m.peak_out = json_num(mj, "peak_out");
			m.agc_gain = json_num(mj, "agc_gain");
			m.agc_gate = json_bool(mj, "agc_gate");
			m.idle = json_num(mj, "idle");
			m.pkts_in = json_int(mj, "pkts_in");
			m.pkts_out = json_int(mj, "pkts_out");

			bool active = (m.codec_in != "none" && m.idle < 2.0);

			// Module header
			if (active)
				attron(COLOR_PAIR(7) | A_BOLD);
			else
				attron(COLOR_PAIR(5));
			mvprintw(row, 0, "Module %c", mod);
			if (active)
			{
				attroff(COLOR_PAIR(7) | A_BOLD);
				attron(COLOR_PAIR(2));
				printw("  [%s]", m.codec_in.c_str());
				attroff(COLOR_PAIR(2));
				printw("  pkts: %d/%d", m.pkts_in, m.pkts_out);
			}
			else
				attroff(COLOR_PAIR(5));
			row++;

			// Signal flow
			draw_flow(row, 2, cols - 4, m, cfg_json);
			row++;

			// VU meters
			draw_vu(row, 2, cols - 4, "Pre", m.rms_in, m.peak_in);
			row++;
			draw_vu(row, 2, cols - 4, "Post", m.rms_out, m.peak_out);
			row++;

			row++;  // spacing

			if (row >= rows - 6) break;  // don't overflow terminal
		}

		// Devices
		attron(COLOR_PAIR(7) | A_BOLD);
		mvprintw(row, 0, "Devices");
		attroff(COLOR_PAIR(7) | A_BOLD);
		row++;

		for (auto &dj : dev_items)
		{
			std::string serial = json_str(dj, "serial");
			std::string type = json_str(dj, "type");
			std::string role = json_str(dj, "role");
			bool online = json_bool(dj, "online");
			int bufd = json_int(dj, "buf_depth");
			int errs = json_int(dj, "errors");

			// Parse active channels
			auto ch_arr = json_arr(dj, "channels");
			auto ch_items = json_arr_items(ch_arr);
			std::string active_mods;
			int active_count = 0;
			for (auto &cj : ch_items)
			{
				std::string m = json_str(cj, "module");
				if (!m.empty()) { active_count++; if (!active_mods.empty()) active_mods += ","; active_mods += m; }
			}
			int total_ch = (type == "DV3003") ? 3 : 1;

			if (online && active_count > 0) attron(COLOR_PAIR(7) | A_BOLD);
			else if (online) attron(COLOR_PAIR(1));
			else attron(COLOR_PAIR(3));
			mvprintw(row, 2, "%s", online ? (active_count > 0 ? ">>" : "OK") : "!!");
			attroff(COLOR_PAIR(1) | COLOR_PAIR(3) | COLOR_PAIR(7) | A_BOLD);

			printw("  %s (%s) [%s]  slots:%d/%d  buf:%d",
				serial.c_str(), type.c_str(), role.c_str(), active_count, total_ch, bufd);
			if (!active_mods.empty())
			{
				attron(COLOR_PAIR(7));
				printw("  %s", active_mods.c_str());
				attroff(COLOR_PAIR(7));
			}
			if (errs > 0) { attron(COLOR_PAIR(3)); printw("  err:%d", errs); attroff(COLOR_PAIR(3)); }
			row++;
		}

		// md380 software vocoder
		auto md380_json = json_obj(last_json, "md380");
		if (!md380_json.empty() && json_bool(md380_json, "available"))
		{
			std::string active_mod = json_str(md380_json, "active_module");
			bool active = !active_mod.empty();
			bool reencode = json_bool(md380_json, "reencode_active");
			int cached = json_int(md380_json, "cached_streams");
			int enc = json_int(md380_json, "encodes");
			int dec = json_int(md380_json, "decodes");
			int re = json_int(md380_json, "reencodes");

			if (active) attron(COLOR_PAIR(7) | A_BOLD);
			else attron(COLOR_PAIR(1));
			mvprintw(row, 2, "%s", active ? ">>" : "OK");
			attroff(COLOR_PAIR(1) | COLOR_PAIR(7) | A_BOLD);

			printw("  MD380 (Software) [dmr]  re-enc:%s  cache:%d", reencode ? "ON" : "off", cached);
			if (active)
			{
				attron(COLOR_PAIR(7));
				printw("  %s", active_mod.c_str());
				attroff(COLOR_PAIR(7));
			}
			attron(COLOR_PAIR(5));
			printw("  enc:%d dec:%d re:%d", enc, dec, re);
			attroff(COLOR_PAIR(5));
			row++;
		}

		// AGC config summary
		row++;
		bool agc_on = json_bool(cfg_json, "agc_enabled");
		attron(COLOR_PAIR(5));
		mvprintw(row, 0, "AGC: %s", agc_on ? "ON" : "OFF");
		if (agc_on)
		{
			printw("  Target:%.0f  Attack:%.0f  Release:%.0f  Up:+%.0f  Down:-%.0f",
				json_num(cfg_json, "agc_target"), json_num(cfg_json, "agc_attack"),
				json_num(cfg_json, "agc_release"),
				json_num(cfg_json, "agc_maxgain_up"), json_num(cfg_json, "agc_maxgain_down"));
		}
		attroff(COLOR_PAIR(5));
		row++;
		attron(COLOR_PAIR(5));
		mvprintw(row, 0, "Output: DStar:%+.0f  DMR/YSF:%+.0f  P25/NXDN:%+.0f  USRP:%+.0f",
			json_num(cfg_json, "outgain_dstar"), json_num(cfg_json, "outgain_dmr"),
			json_num(cfg_json, "outgain_imbe"), json_num(cfg_json, "outgain_usrp"));
		attroff(COLOR_PAIR(5));

		row++;
		attron(COLOR_PAIR(5));
		mvprintw(rows - 1, 0, "q=quit  |  %s:%d", host, port);
		attroff(COLOR_PAIR(5));

		refresh();
		usleep(100000);  // 100ms refresh
	}

	endwin();
	close(fd);
	return 0;
}
