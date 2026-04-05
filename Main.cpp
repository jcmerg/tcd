// tcd - a hybrid transcoder using DVSI hardware and Codec2 software
// Copyright © 2021,2023 Thomas A. Early N7TAE
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <unistd.h>
#include <cstring>
#include <csignal>
#include <iostream>

#include "Controller.h"
#include "Configure.h"
#include "TcdStats.h"
#include "MonitorServer.h"
#include "StatsLogger.h"

// the global objects
CConfigure     g_Conf;
CController    g_Cont;
CTcdStats      g_Stats;
CMonitorServer g_Monitor;

int main(int argc, char *argv[])
{
	// Ignore SIGPIPE so send() on a broken connection returns EPIPE
	// instead of killing the process. Needed for clean reconnection.
	signal(SIGPIPE, SIG_IGN);
	if (argc == 2 && 0 == strcmp(argv[1], "--list-devices"))
	{
		g_Cont.ListDevices();
		return EXIT_SUCCESS;
	}

	if (2 != argc)
	{
		std::cerr << "Usage: " << argv[0] << " PATHTOINIFILE" << std::endl;
		std::cerr << "       " << argv[0] << " --list-devices" << std::endl;
		return EXIT_FAILURE;
	}

	if (g_Conf.ReadData(argv[1]))
		return EXIT_FAILURE;

	if (g_Cont.Start())
		return EXIT_FAILURE;

	// Start monitor server (web dashboard + stats socket)
	if (g_Conf.GetMonitorEnabled())
	{
		g_Stats.module_letters = g_Conf.GetTCMods();
		// Sync initial config into stats for monitor display
		g_Stats.config.agc_enabled.store(g_Conf.GetAGCEnabled());
		g_Stats.config.agc_target.store(g_Conf.GetAGCTarget());
		g_Stats.config.agc_attack.store(g_Conf.GetAGCAttack());
		g_Stats.config.agc_release.store(g_Conf.GetAGCRelease());
		g_Stats.config.agc_maxgain_up.store(g_Conf.GetAGCMaxGainUp());
		g_Stats.config.agc_maxgain_down.store(g_Conf.GetAGCMaxGainDown());
		g_Stats.config.agc_noisegate.store(g_Conf.GetAGCNoiseGate());
		g_Stats.config.gain_dstar_in.store(g_Conf.GetGain(EGainType::dstarin));
		g_Stats.config.gain_dstar_out.store(g_Conf.GetGain(EGainType::dstarout));
		g_Stats.config.gain_dmr_in.store(g_Conf.GetGain(EGainType::dmrin));
		g_Stats.config.gain_dmr_out.store(g_Conf.GetGain(EGainType::dmrout));
		g_Stats.config.gain_usrp_rx.store(g_Conf.GetGain(EGainType::usrprx));
		g_Stats.config.gain_usrp_tx.store(g_Conf.GetGain(EGainType::usrptx));
		g_Stats.config.gain_dmr_reencode.store(g_Conf.GetGain(EGainType::dmrreencode));
		g_Stats.config.outgain_dstar.store(g_Conf.GetGain(EGainType::outgain_dstar));
		g_Stats.config.outgain_dmr.store(g_Conf.GetGain(EGainType::outgain_dmr));
		g_Stats.config.outgain_usrp.store(g_Conf.GetGain(EGainType::outgain_usrp));
		g_Stats.config.outgain_imbe.store(g_Conf.GetGain(EGainType::outgain_imbe));
		g_Stats.config.outgain_m17.store(g_Conf.GetGain(EGainType::outgain_m17));
		g_Stats.config.dmr_reencode_enabled.store(
			g_Conf.GetDMRReEncodeEnabled() && g_Stats.md380.available.load(std::memory_order_relaxed));
		g_Monitor.Start(g_Conf.GetMonitorHttpPort(), g_Conf.GetMonitorStatsPort());
	}

	// Start stats CSV logger
	g_StatsLog.Configure(g_Conf.GetStatsLogEnabled(), g_Conf.GetStatsLogDir(), g_Conf.GetStatsLogRetain());
	g_StatsLog.Start();

	std::cout << "Hybrid Transcoder version 0.1.1 successfully started" << std::endl;

	pause();

	g_StatsLog.Stop();
	g_Monitor.Stop();
	g_Cont.Stop();

	return EXIT_SUCCESS;
}
