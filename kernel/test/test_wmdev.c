/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ktest.h"
#include "wmdev.h"

static void test_wmdev_registers_single_server(ktest_case_t *tc)
{
	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int second = wmdev_open(11);
	KTEST_ASSERT_TRUE(tc, server >= 0);
	KTEST_ASSERT_TRUE(tc, second >= 0);

	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)second,
	                                                DRWIN_SERVER_MAGIC),
	                (uint32_t)-1);
}

static void test_wmdev_create_window_tracks_owner_and_surface(ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	uint32_t window = 0;

	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int client = wmdev_open(42);
	KTEST_ASSERT_TRUE(tc, server >= 0);
	KTEST_ASSERT_TRUE(tc, client >= 0);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);

	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)client,
	                                              "paint",
	                                              20,
	                                              30,
	                                              64,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);
	KTEST_EXPECT_NE(tc, window, 0u);
	KTEST_EXPECT_EQ(tc, (uint32_t)wmdev_window_owner_for_test(window), 42u);
	KTEST_EXPECT_EQ(tc, surface.width, 64u);
	KTEST_EXPECT_EQ(tc, surface.height, 32u);
	KTEST_EXPECT_EQ(tc, surface.pitch, 64u * 4u);
	KTEST_EXPECT_EQ(tc, surface.bpp, 32u);
	KTEST_EXPECT_GE(tc, wmdev_window_page_count_for_test(window), 2u);
}

static void test_wmdev_rejects_cross_owner_present(ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	uint32_t window = 0;
	drwin_rect_t dirty = {0, 0, 10, 10};

	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int owner = wmdev_open(42);
	int other = wmdev_open(43);
	KTEST_ASSERT_TRUE(tc, server >= 0);
	KTEST_ASSERT_TRUE(tc, owner >= 0);
	KTEST_ASSERT_TRUE(tc, other >= 0);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)owner,
	                                              "owned",
	                                              0,
	                                              0,
	                                              32,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);

	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_present_window((uint32_t)other,
	                                               window,
	                                               dirty),
	                (uint32_t)-1);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_present_window((uint32_t)owner,
	                                               window,
	                                               dirty),
	                0u);
}

static void test_wmdev_event_queue_round_trips_to_owner(ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	uint32_t window = 0;
	drwin_event_t in = {DRWIN_EVENT_CLOSE, 0, 0, 0, 0, 0};
	drwin_event_t out;

	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int client = wmdev_open(42);
	KTEST_ASSERT_TRUE(tc, server >= 0);
	KTEST_ASSERT_TRUE(tc, client >= 0);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)client,
	                                              "events",
	                                              0,
	                                              0,
	                                              32,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);
	in.window = (int32_t)window;

	KTEST_EXPECT_EQ(tc, (uint32_t)wmdev_queue_event(window, &in), 0u);
	KTEST_EXPECT_TRUE(tc, wmdev_event_available((uint32_t)client));
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_read_event((uint32_t)client, &out),
	                0u);
	KTEST_EXPECT_EQ(tc, out.type, DRWIN_EVENT_CLOSE);
	KTEST_EXPECT_EQ(tc, (uint32_t)out.window, window);
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_wmdev_registers_single_server),
    KTEST_CASE(test_wmdev_create_window_tracks_owner_and_surface),
    KTEST_CASE(test_wmdev_rejects_cross_owner_present),
    KTEST_CASE(test_wmdev_event_queue_round_trips_to_owner),
};

static ktest_suite_t suite = KTEST_SUITE("wmdev", cases);

ktest_suite_t *ktest_suite_wmdev(void)
{
	return &suite;
}
