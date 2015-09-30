/*

Copyright (c) 2015, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/session.hpp"
#include "libtorrent/torrent_handle.hpp"
#include "libtorrent/settings_pack.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/deadline_timer.hpp"
#include "test.hpp"
#include "swarm_config.hpp"
#include "settings.hpp"
#include "simulator/simulator.hpp"
#include <iostream>

using namespace sim;

const int num_torrents = 10;

lt::add_torrent_params create_torrent(int idx, bool seed)
{
	// TODO: if we want non-seeding torrents, that could be a bit cheaper to
	// create
	lt::add_torrent_params params;
	int swarm_id = test_counter();
	char name[200];
	snprintf(name, sizeof(name), "temp-%02d", idx);
	char path[200];
	snprintf(path, sizeof(path), "swarm-%04d-peer-%02d"
		, swarm_id, idx);
	error_code ec;
	create_directory(path, ec);
	if (ec) fprintf(stderr, "failed to create directory: \"%s\": %s\n"
		, path, ec.message().c_str());
	std::ofstream file(combine_path(path, name).c_str());
	params.ti = ::create_torrent(&file, name
		, 0x4000, 9 + idx, false);
	file.close();

	// by setting the save path to a dummy path, it won't be seeding
	params.save_path = seed ? path : "dummy";
	return params;
}

using sim::asio::ip::address_v4;

std::unique_ptr<sim::asio::io_service> make_io_service(sim::simulation& sim, int i)
{
	char ep[30];
	snprintf(ep, sizeof(ep), "50.0.%d.%d", (i + 1) >> 8, (i + 1) & 0xff);
	return std::unique_ptr<sim::asio::io_service>(new sim::asio::io_service(
		sim, asio::ip::address_v4::from_string(ep)));
}

// this is the general template for these tests. create the session with custom
// settings (Settings), set up the test, by adding torrents with certain
// arguments (Setup), run the test and verify the end state (Test)
template <typename Settings, typename Setup, typename Test>
void run_test(Settings const& sett, Setup const& setup, Test const& test)
{
	// setup the simulation
	sim::default_config network_cfg;
	sim::simulation sim{network_cfg};
	std::unique_ptr<sim::asio::io_service> ios = make_io_service(sim, 0);
	lt::session_proxy zombie;

	// setup settings pack to use for the session (customization point)
	lt::settings_pack pack = settings();
	sett(pack);

	// create session
	std::shared_ptr<lt::session> ses = std::make_shared<lt::session>(pack, *ios);

	// set up test, like adding torrents (customization point)
	setup(*ses);

	// set up a timer to fire later, to verify everything we expected to happen
	// happened
	lt::deadline_timer timer(*ios);
	timer.expires_from_now(lt::seconds((num_torrents + 1) * 60));
	timer.async_wait([&](boost::system::error_code const& ec)
	{
		test(*ses);

		// shut down
		zombie = ses->abort();
		ses.reset();
	});

	sim.run();
}

TORRENT_TEST(dont_count_slow_torrents)
{
	run_test(
		[](settings_pack& sett) {
			// session settings
			sett.set_bool(settings_pack::dont_count_slow_torrents, true);
			sett.set_int(settings_pack::active_downloads, 1);
			sett.set_int(settings_pack::active_seeds, 1);
		},

		[](lt::session& ses) {
			// add torrents
			for (int i = 0; i < num_torrents; ++i)
			{
				lt::add_torrent_params params = create_torrent(i, false);
				params.flags |= add_torrent_params::flag_auto_managed;
				params.flags |= add_torrent_params::flag_paused;
				ses.async_add_torrent(params);
			}
		},

		[](lt::session& ses) {
			// verify result

			std::vector<lt::alert*> alerts;
			ses.pop_alerts(&alerts);

			lt::time_point last = lt::time_point::min();
			lt::time_point start_time = alerts[0]->timestamp();

			int num_started = 0;
			for (alert* a : alerts)
			{
				printf("%-3d %s\n", int(duration_cast<lt::seconds>(a->timestamp()
						- start_time).count()), a->message().c_str());
				if (alert_cast<torrent_resumed_alert>(a) == nullptr) continue;

				lt::time_point t = a->timestamp();
				if (last != lt::time_point::min())
				{
					// expect starting of new torrents to be spaced by 60 seconds
					// the division by 2 is to allow some slack (it's integer
					// division)
					TEST_EQUAL(duration_cast<lt::seconds>(t - last).count() / 2, 60 / 2);
				}
				last = t;
				++num_started;
			}

			TEST_EQUAL(num_started, num_torrents);

			for (torrent_handle const& h : ses.get_torrents())
			{
				TEST_CHECK(h.status().auto_managed);
				TEST_EQUAL(h.status().paused, false);
			}
		});
}

TORRENT_TEST(count_slow_torrents)
{
	run_test(
		[](settings_pack& sett) {
			// session settings
			sett.set_bool(settings_pack::dont_count_slow_torrents, false);
			sett.set_int(settings_pack::active_downloads, 1);
			sett.set_int(settings_pack::active_seeds, 1);
		},

		[](lt::session& ses) {
			// add torrents
			for (int i = 0; i < num_torrents; ++i)
			{
				lt::add_torrent_params params = create_torrent(i, false);
				params.flags |= add_torrent_params::flag_auto_managed;
				params.flags |= add_torrent_params::flag_paused;
				ses.async_add_torrent(params);
			}
		},

		[](lt::session& ses) {
			// verify result (only one should have been started, even though
			// they're all idle)

			std::vector<lt::alert*> alerts;
			ses.pop_alerts(&alerts);

			lt::time_point start_time = alerts[0]->timestamp();

			int num_started = 0;
			for (alert* a : alerts)
			{
				printf("%-3d %s\n", int(duration_cast<lt::seconds>(a->timestamp()
						- start_time).count()), a->message().c_str());
				if (alert_cast<torrent_resumed_alert>(a) == nullptr) continue;
				++num_started;
			}

			TEST_EQUAL(num_started, 1);

			num_started = 0;
			for (torrent_handle const& h : ses.get_torrents())
			{
				TEST_CHECK(h.status().auto_managed);
				num_started += !h.status().paused;
			}
			TEST_EQUAL(num_started, 1);
		});
}

TORRENT_TEST(force_stopped_download)
{
	run_test(
		[](settings_pack& sett) {
			// session settings
			sett.set_bool(settings_pack::dont_count_slow_torrents, true);
			sett.set_int(settings_pack::active_downloads, 10);
			sett.set_int(settings_pack::active_seeds, 10);
		},

		[](lt::session& ses) {
			// add torrents
			for (int i = 0; i < num_torrents; ++i)
			{
				lt::add_torrent_params params = create_torrent(i, false);
				// torrents are paused and not auto-managed
				params.flags &= ~add_torrent_params::flag_auto_managed;
				params.flags |= add_torrent_params::flag_paused;
				ses.async_add_torrent(params);
			}
		},

		[](lt::session& ses) {
			// verify result (none should have been started)

			std::vector<lt::alert*> alerts;
			ses.pop_alerts(&alerts);

			lt::time_point start_time = alerts[0]->timestamp();

			for (alert* a : alerts)
			{
				printf("%-3d %s\n", int(duration_cast<lt::seconds>(a->timestamp()
						- start_time).count()), a->message().c_str());
				// we don't expect any torrents being started or stopped, since
				// they're all force stopped
				TEST_CHECK(alert_cast<torrent_resumed_alert>(a) == nullptr);
				TEST_CHECK(alert_cast<torrent_paused_alert>(a) == nullptr);
			}

			for (torrent_handle const& h : ses.get_torrents())
			{
				TEST_CHECK(!h.status().auto_managed);
				TEST_CHECK(h.status().paused);
			}
		});
}

TORRENT_TEST(force_started)
{
	run_test(
		[](settings_pack& sett) {
			// session settings
			sett.set_bool(settings_pack::dont_count_slow_torrents, false);
			sett.set_int(settings_pack::active_downloads, 1);
			sett.set_int(settings_pack::active_seeds, 1);
		},

		[](lt::session& ses) {
			// add torrents
			for (int i = 0; i < num_torrents; ++i)
			{
				lt::add_torrent_params params = create_torrent(i, false);
				// torrents are started and not auto-managed
				params.flags &= ~add_torrent_params::flag_auto_managed;
				params.flags &= ~add_torrent_params::flag_paused;
				ses.async_add_torrent(params);
			}
		},

		[](lt::session& ses) {
			// verify result (none should have been started)

			std::vector<lt::alert*> alerts;
			ses.pop_alerts(&alerts);

			lt::time_point start_time = alerts[0]->timestamp();

			for (alert* a : alerts)
			{
				printf("%-3d %s\n", int(duration_cast<lt::seconds>(a->timestamp()
						- start_time).count()), a->message().c_str());
				// we don't expect any torrents being started or stopped, since
				// they're all force started
				TEST_CHECK(alert_cast<torrent_resumed_alert>(a) == nullptr);
				TEST_CHECK(alert_cast<torrent_paused_alert>(a) == nullptr);
			}

			for (torrent_handle const& h : ses.get_torrents())
			{
				TEST_CHECK(!h.status().auto_managed);
				TEST_CHECK(!h.status().paused);
			}
		});
}

TORRENT_TEST(seed_limit)
{
	run_test(
		[](settings_pack& sett) {
			// session settings
			// set the seed limit to 3
			sett.set_bool(settings_pack::dont_count_slow_torrents, false);
			sett.set_int(settings_pack::active_checking, 1);
			sett.set_int(settings_pack::active_seeds, 3);
		},

		[](lt::session& ses) {
			// add torrents
			// add 5 seeds
			for (int i = 0; i < num_torrents; ++i)
			{
				lt::add_torrent_params params = create_torrent(i, true);
				// torrents are paused and auto-managed
				params.flags |= add_torrent_params::flag_auto_managed;
				params.flags |= add_torrent_params::flag_paused;
				ses.async_add_torrent(params);
			}
		},

		[](lt::session& ses) {
			// verify result (none should have been started)
			// make sure only 3 got started
			std::vector<lt::alert*> alerts;
			ses.pop_alerts(&alerts);

			lt::time_point start_time = alerts[0]->timestamp();

			int num_started = 0;
			int num_checking = 0;
			int num_seeding = 0;
			for (alert* a : alerts)
			{
				fprintf(stderr, "%-3d %s\n", int(duration_cast<lt::seconds>(a->timestamp()
						- start_time).count()), a->message().c_str());
				if (alert_cast<torrent_resumed_alert>(a))
				{
					++num_started;

					fprintf(stderr, "started: %d checking: %d seeding: %d\n"
						, num_started, num_checking, num_seeding);
				}
				else if (alert_cast<torrent_paused_alert>(a))
				{
					TEST_CHECK(num_started > 0);
					--num_started;

					fprintf(stderr, "started: %d checking: %d seeding: %d\n"
						, num_started, num_checking, num_seeding);
				}
				else if (state_changed_alert* sc = alert_cast<state_changed_alert>(a))
				{
					if (sc->prev_state == torrent_status::checking_files)
						--num_checking;
					else if (sc->prev_state == torrent_status::seeding)
						--num_seeding;

					if (sc->state == torrent_status::checking_files)
						++num_checking;
					else if (sc->state == torrent_status::seeding)
						++num_seeding;

					fprintf(stderr, "started: %d checking: %d seeding: %d\n"
						, num_started, num_checking, num_seeding);

					// while at least one torrent is checking, there may be another
					// started torrent (the checking one), other than that, only 3
					// torrents are allowed to be started and seeding
					TEST_CHECK(num_started <= 3 + 1);
					TEST_CHECK(num_started <= 1 || num_seeding > 0);
				}
			}

			TEST_EQUAL(num_started, 3);

			num_started = 0;
			for (torrent_handle const& h : ses.get_torrents())
			{
				TEST_CHECK(h.status().auto_managed);
				TEST_CHECK(h.status().is_seeding);
				num_started += !h.status().paused;
			}
			TEST_EQUAL(num_started, 3);
		});
}

TORRENT_TEST(download_limit)
{
	run_test(
		[](settings_pack& sett) {
			// session settings
			// set the seed limit to 3
			sett.set_bool(settings_pack::dont_count_slow_torrents, false);
			sett.set_int(settings_pack::active_checking, 1);
			sett.set_int(settings_pack::active_downloads, 3);
		},

		[](lt::session& ses) {
			// add torrents
			// add 5 seeds
			for (int i = 0; i < num_torrents; ++i)
			{
				lt::add_torrent_params params = create_torrent(i, false);
				// torrents are paused and auto-managed
				params.flags |= add_torrent_params::flag_auto_managed;
				params.flags |= add_torrent_params::flag_paused;
				ses.async_add_torrent(params);
			}
		},

		[](lt::session& ses) {
			// verify result (none should have been started)
			// make sure only 3 got started
			std::vector<lt::alert*> alerts;
			ses.pop_alerts(&alerts);

			lt::time_point start_time = alerts[0]->timestamp();

			int num_started = 0;
			int num_checking = 0;
			int num_downloading = 0;
			for (alert* a : alerts)
			{
				fprintf(stderr, "%-3d %s\n", int(duration_cast<lt::seconds>(a->timestamp()
						- start_time).count()), a->message().c_str());
				if (alert_cast<torrent_resumed_alert>(a))
				{
					++num_started;

					fprintf(stderr, "started: %d checking: %d downloading: %d\n"
						, num_started, num_checking, num_downloading);
				}
				else if (alert_cast<torrent_paused_alert>(a))
				{
					TEST_CHECK(num_started > 0);
					--num_started;

					fprintf(stderr, "started: %d checking: %d downloading: %d\n"
						, num_started, num_checking, num_downloading);
				}
				else if (state_changed_alert* sc = alert_cast<state_changed_alert>(a))
				{
					if (sc->prev_state == torrent_status::checking_files)
						--num_checking;
					else if (sc->prev_state == torrent_status::downloading)
						--num_downloading;

					if (sc->state == torrent_status::checking_files)
						++num_checking;
					else if (sc->state == torrent_status::downloading)
						++num_downloading;

					fprintf(stderr, "started: %d checking: %d downloading: %d\n"
						, num_started, num_checking, num_downloading);

					// while at least one torrent is checking, there may be another
					// started torrent (the checking one), other than that, only 3
					// torrents are allowed to be started and seeding
					TEST_CHECK(num_started <= 3 + 1);
					TEST_CHECK(num_started <= 1 || num_downloading > 0);
				}
			}

			TEST_EQUAL(num_started, 3);

			num_started = 0;
			for (torrent_handle const& h : ses.get_torrents())
			{
				TEST_CHECK(h.status().auto_managed);
				TEST_CHECK(!h.status().is_finished);
				num_started += !h.status().paused;
			}
			TEST_EQUAL(num_started, 3);
		});
}
// make sure torrents don't announce to the tracker when transitioning from
// checking to paused downloading
TORRENT_TEST(checking_announce)
{
	run_test(
		[](settings_pack& sett) {
			// session settings
			// set the seed limit to 3
			sett.set_bool(settings_pack::dont_count_slow_torrents, false);
			sett.set_int(settings_pack::active_checking, 1);

			// just set the tracker retry intervals really long, to make sure we
			// don't keep retrying the tracker (since there's nothing running
			// there, it will fail)
			sett.set_int(settings_pack::tracker_backoff, 100000);
			// only the first torrent added should ever announce
			sett.set_int(settings_pack::active_seeds, 1);
		},

		[](lt::session& ses) {
			// add torrents
			// add 5 seeds
			for (int i = 0; i < num_torrents; ++i)
			{
				lt::add_torrent_params params = create_torrent(i, true);
				// torrents are paused and auto-managed
				params.flags |= add_torrent_params::flag_auto_managed;
				params.flags |= add_torrent_params::flag_paused;
				// we need this to get the tracker_announce_alert
				params.trackers.push_back("http://10.10.0.2/announce");
				ses.async_add_torrent(params);
			}
		},

		[](lt::session& ses) {
			// verify result (none should have been started)
			// make sure only 3 got started
			std::vector<lt::alert*> alerts;
			ses.pop_alerts(&alerts);

			lt::time_point start_time = alerts[0]->timestamp();

			int num_announce = 0;
			for (alert* a : alerts)
			{
				fprintf(stderr, "%-3d %s\n", int(duration_cast<lt::seconds>(a->timestamp()
						- start_time).count()), a->message().c_str());
				if (alert_cast<tracker_announce_alert>(a))
					++num_announce;
			}

			TEST_EQUAL(num_announce, 1);

			int num_started = 0;
			for (torrent_handle const& h : ses.get_torrents())
			{
				TEST_CHECK(h.status().auto_managed);
				num_started += !h.status().paused;
			}
			TEST_EQUAL(num_started, 1);
		});
}

TORRENT_TEST(paused_checking)
{
	run_test(
		[](settings_pack& sett) {
			// session settings
			// set the seed limit to 3
			sett.set_bool(settings_pack::dont_count_slow_torrents, true);
			sett.set_int(settings_pack::active_checking, 1);
		},

		[](lt::session& ses) {
			// add torrents
			// add 5 seeds
			for (int i = 0; i < num_torrents; ++i)
			{
				lt::add_torrent_params params = create_torrent(i, true);
				// torrents are paused and auto-managed
				params.flags &= ~add_torrent_params::flag_auto_managed;
				params.flags |= add_torrent_params::flag_paused;
				ses.async_add_torrent(params);
			}
		},

		[](lt::session& ses) {
			// verify result (none should have been started)
			// make sure only 3 got started
			std::vector<lt::alert*> alerts;
			ses.pop_alerts(&alerts);

			lt::time_point start_time = alerts[0]->timestamp();

			for (alert* a : alerts)
			{
				fprintf(stderr, "%-3d %s\n", int(duration_cast<lt::seconds>(a->timestamp()
						- start_time).count()), a->message().c_str());
				if (state_changed_alert* sc = alert_cast<state_changed_alert>(a))
				{
					TEST_CHECK(sc->state == torrent_status::checking_files
						|| sc->state == torrent_status::checking_resume_data);
				}
			}

			for (torrent_handle const& h : ses.get_torrents())
			{
				// even though all torrents are seeding, libtorrent shouldn't know
				// that, because they should never have been checked (because they
				// were force stopped)
				TEST_CHECK(!h.status().is_seeding);
				TEST_CHECK(!h.status().auto_managed);
				TEST_CHECK(h.status().paused);
			}
		});
}
// TODO: assert that the torrent_paused_alert is posted when pausing
//       downloading, seeding, checking torrents as well as the graceful pause
// TODO: test limits of tracker, DHT and LSD announces


