/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ktest.h"
#include "kstring.h"
#include "pmm.h"
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

static void test_wmdev_mmap_page_returns_surface_pages(ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	uint32_t window = 0;
	uint32_t phys0 = 0;
	uint32_t phys1 = 0;

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
	                                              "map",
	                                              0,
	                                              0,
	                                              64,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);

	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_mmap_page((uint32_t)client,
	                                          surface.map_offset,
	                                          0,
	                                          &phys0),
	                0u);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_mmap_page((uint32_t)client,
	                                          surface.map_offset,
	                                          1,
	                                          &phys1),
	                0u);
	KTEST_EXPECT_NE(tc, phys0, 0u);
	KTEST_EXPECT_NE(tc, phys1, 0u);
	KTEST_EXPECT_NE(tc, phys0, phys1);
}

static void test_wmdev_user_record_create_returns_surface_response(
    ktest_case_t *tc)
{
	drwin_register_server_request_t reg;
	drwin_create_window_request_t req;
	drwin_create_window_response_t resp;
	uint32_t phys = 0;

	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int client = wmdev_open(42);
	KTEST_ASSERT_TRUE(tc, server >= 0);
	KTEST_ASSERT_TRUE(tc, client >= 0);

	k_memset(&reg, 0, sizeof(reg));
	reg.size = sizeof(reg);
	reg.type = DRWIN_REQ_REGISTER_SERVER;
	reg.magic = DRWIN_SERVER_MAGIC;
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_write_user_record((uint32_t)server,
	                                                  (const uint8_t *)&reg,
	                                                  sizeof(reg)),
	                sizeof(reg));

	k_memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.type = DRWIN_REQ_CREATE_WINDOW;
	req.w = 64;
	req.h = 32;
	k_strncpy(req.title, "record", sizeof(req.title) - 1u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_write_user_record((uint32_t)client,
	                                                  (const uint8_t *)&req,
	                                                  sizeof(req)),
	                sizeof(req));

	k_memset(&resp, 0, sizeof(resp));
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_read_user_record((uint32_t)client,
	                                                 (uint8_t *)&resp,
	                                                 sizeof(resp)),
	                sizeof(resp));
	KTEST_EXPECT_EQ(tc, resp.size, sizeof(resp));
	KTEST_EXPECT_EQ(tc, resp.type, DRWIN_CLIENT_MSG_CREATE_WINDOW_RESPONSE);
	KTEST_EXPECT_EQ(tc, (uint32_t)resp.status, 0u);
	KTEST_EXPECT_NE(tc, resp.window, 0u);
	KTEST_EXPECT_EQ(tc, resp.surface.width, 64u);
	KTEST_EXPECT_EQ(tc, resp.surface.height, 32u);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_mmap_page((uint32_t)client,
	                                          resp.surface.map_offset,
	                                          0,
	                                          &phys),
	                0u);
	KTEST_EXPECT_NE(tc, phys, 0u);
}

static void test_wmdev_retain_keeps_connection_until_final_close(
    ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	drwin_rect_t dirty = {0, 0, 8, 8};
	uint32_t window = 0;
	uint32_t page_count;
	uint32_t free_before_final_close;

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
	                                              "retained",
	                                              0,
	                                              0,
	                                              64,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);
	KTEST_ASSERT_EQ(tc, (uint32_t)wmdev_retain((uint32_t)client), 0u);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_retain(WMDEV_MAX_CONNECTIONS),
	                (uint32_t)-1);

	page_count = wmdev_window_page_count_for_test(window);
	KTEST_ASSERT_TRUE(tc, page_count > 0);
	wmdev_close((uint32_t)client);

	KTEST_EXPECT_EQ(tc, (uint32_t)wmdev_window_owner_for_test(window), 42u);
	KTEST_EXPECT_EQ(tc, wmdev_window_page_count_for_test(window), page_count);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_present_window((uint32_t)client,
	                                               window,
	                                               dirty),
	                0u);

	free_before_final_close = pmm_free_page_count();
	wmdev_close((uint32_t)client);

	KTEST_EXPECT_EQ(tc, pmm_free_page_count(),
	                free_before_final_close + page_count);
	KTEST_EXPECT_EQ(tc, (uint32_t)wmdev_window_owner_for_test(window),
	                (uint32_t)-1);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_present_window((uint32_t)client,
	                                               window,
	                                               dirty),
	                (uint32_t)-1);
}

static void test_wmdev_mmap_page_rejects_non_base_surface_offsets(
    ktest_case_t *tc)
{
	drwin_surface_info_t first_surface;
	drwin_surface_info_t second_surface;
	uint32_t first_window = 0;
	uint32_t second_window = 0;
	uint32_t phys = 0;

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
	                                              "first",
	                                              0,
	                                              0,
	                                              64,
	                                              32,
	                                              &first_window,
	                                              &first_surface),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)client,
	                                              "second",
	                                              0,
	                                              0,
	                                              64,
	                                              32,
	                                              &second_window,
	                                              &second_surface),
	                0u);

	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_mmap_page((uint32_t)client,
	                                          first_surface.map_offset +
	                                              PAGE_SIZE,
	                                          0,
	                                          &phys),
	                (uint32_t)-1);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_mmap_page((uint32_t)client,
	                                          second_surface.map_offset,
	                                          0,
	                                          &phys),
	                0u);
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

static void test_wmdev_client_close_emits_destroy_and_frees_pages(
    ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	drwin_server_msg_t msg;
	uint32_t window = 0;
	uint32_t page_count;
	uint32_t free_before_close;

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
	                                              "close-me",
	                                              0,
	                                              0,
	                                              64,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_read_server_msg((uint32_t)server, &msg),
	                0u);
	KTEST_ASSERT_EQ(tc, msg.type, DRWIN_MSG_CREATE_WINDOW);

	page_count = wmdev_window_page_count_for_test(window);
	free_before_close = pmm_free_page_count();
	KTEST_ASSERT_TRUE(tc, page_count > 0);

	wmdev_close((uint32_t)client);

	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), free_before_close + page_count);
	KTEST_EXPECT_EQ(tc, (uint32_t)wmdev_window_owner_for_test(window),
	                (uint32_t)-1);
	KTEST_EXPECT_TRUE(tc, wmdev_server_msg_available((uint32_t)server));
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_read_server_msg((uint32_t)server, &msg),
	                0u);
	KTEST_EXPECT_EQ(tc, msg.type, DRWIN_MSG_DESTROY_WINDOW);
	KTEST_EXPECT_EQ(tc, msg.window, window);
}

static void test_wmdev_client_close_replaces_saturated_window_queue_with_destroy(
    ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	drwin_server_msg_t msg;
	drwin_rect_t dirty = {0, 0, 4, 4};
	uint32_t window = 0;
	uint32_t page_count;
	uint32_t free_before_close;

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
	                                              "saturated",
	                                              0,
	                                              0,
	                                              64,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);
	for (uint32_t i = 1; i < WMDEV_EVENT_QUEUE_CAP; i++) {
		KTEST_ASSERT_EQ(tc,
		                (uint32_t)wmdev_present_window((uint32_t)client,
		                                               window,
		                                               dirty),
		                0u);
	}
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_present_window((uint32_t)client,
	                                               window,
	                                               dirty),
	                (uint32_t)-1);

	page_count = wmdev_window_page_count_for_test(window);
	free_before_close = pmm_free_page_count();
	KTEST_ASSERT_TRUE(tc, page_count > 0);

	wmdev_close((uint32_t)client);

	KTEST_EXPECT_EQ(tc, pmm_free_page_count(), free_before_close + page_count);
	KTEST_EXPECT_EQ(tc, (uint32_t)wmdev_window_owner_for_test(window),
	                (uint32_t)-1);
	KTEST_EXPECT_TRUE(tc, wmdev_server_msg_available((uint32_t)server));
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_read_server_msg((uint32_t)server, &msg),
	                0u);
	KTEST_EXPECT_EQ(tc, msg.type, DRWIN_MSG_DESTROY_WINDOW);
	KTEST_EXPECT_EQ(tc, msg.window, window);
	KTEST_EXPECT_FALSE(tc, wmdev_server_msg_available((uint32_t)server));
}

static void test_wmdev_client_close_prioritizes_destroy_when_other_windows_fill_queue(
    ktest_case_t *tc)
{
	drwin_surface_info_t closing_surface;
	drwin_surface_info_t other_surface;
	drwin_server_msg_t msg;
	drwin_rect_t dirty = {0, 0, 4, 4};
	uint32_t closing_window = 0;
	uint32_t other_window = 0;
	uint32_t closing_page_count;
	uint32_t free_before_close;
	uint32_t saw_destroy = 0;

	wmdev_reset_for_test();
	int server = wmdev_open(10);
	int closing_client = wmdev_open(42);
	int other_client = wmdev_open(43);
	KTEST_ASSERT_TRUE(tc, server >= 0);
	KTEST_ASSERT_TRUE(tc, closing_client >= 0);
	KTEST_ASSERT_TRUE(tc, other_client >= 0);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)closing_client,
	                                              "closing",
	                                              0,
	                                              0,
	                                              64,
	                                              32,
	                                              &closing_window,
	                                              &closing_surface),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_read_server_msg((uint32_t)server, &msg),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)other_client,
	                                              "other",
	                                              0,
	                                              0,
	                                              64,
	                                              32,
	                                              &other_window,
	                                              &other_surface),
	                0u);
	for (uint32_t i = 1; i < WMDEV_EVENT_QUEUE_CAP; i++) {
		KTEST_ASSERT_EQ(tc,
		                (uint32_t)wmdev_present_window((uint32_t)other_client,
		                                               other_window,
		                                               dirty),
		                0u);
	}
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_present_window((uint32_t)other_client,
	                                               other_window,
	                                               dirty),
	                (uint32_t)-1);

	closing_page_count = wmdev_window_page_count_for_test(closing_window);
	free_before_close = pmm_free_page_count();
	KTEST_ASSERT_TRUE(tc, closing_page_count > 0);

	wmdev_close((uint32_t)closing_client);

	KTEST_EXPECT_EQ(tc,
	                pmm_free_page_count(),
	                free_before_close + closing_page_count);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_window_owner_for_test(closing_window),
	                (uint32_t)-1);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_window_owner_for_test(other_window),
	                43u);
	KTEST_EXPECT_TRUE(tc, wmdev_window_page_count_for_test(other_window) > 0);

	while (wmdev_server_msg_available((uint32_t)server)) {
		KTEST_ASSERT_EQ(tc,
		                (uint32_t)wmdev_read_server_msg((uint32_t)server,
		                                                &msg),
		                0u);
		if (msg.type == DRWIN_MSG_DESTROY_WINDOW &&
		    msg.window == closing_window)
			saw_destroy = 1;
	}
	KTEST_EXPECT_TRUE(tc, saw_destroy);
}

static void test_wmdev_server_close_destroys_windows_and_stales_handles(
    ktest_case_t *tc)
{
	drwin_surface_info_t surface;
	drwin_server_msg_t msg;
	drwin_event_t event;
	drwin_rect_t dirty = {0, 0, 1, 1};
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
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_create_window((uint32_t)client,
	                                              "stale",
	                                              0,
	                                              0,
	                                              32,
	                                              32,
	                                              &window,
	                                              &surface),
	                0u);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_read_server_msg((uint32_t)server, &msg),
	                0u);

	wmdev_close((uint32_t)server);

	KTEST_EXPECT_EQ(tc, wmdev_window_page_count_for_test(window), 0u);
	KTEST_EXPECT_EQ(tc, (uint32_t)wmdev_window_owner_for_test(window),
	                (uint32_t)-1);
	KTEST_EXPECT_TRUE(tc, wmdev_event_available((uint32_t)client));
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_read_event((uint32_t)client, &event),
	                0u);
	KTEST_EXPECT_EQ(tc, event.type, DRWIN_EVENT_DISCONNECT);

	int new_server = wmdev_open(11);
	KTEST_ASSERT_TRUE(tc, new_server >= 0);
	KTEST_ASSERT_EQ(tc,
	                (uint32_t)wmdev_register_server((uint32_t)new_server,
	                                                DRWIN_SERVER_MAGIC),
	                0u);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_present_window((uint32_t)client,
	                                               window,
	                                               dirty),
	                (uint32_t)-1);
	KTEST_EXPECT_EQ(tc,
	                (uint32_t)wmdev_destroy_window((uint32_t)client, window),
	                (uint32_t)-1);
	KTEST_EXPECT_FALSE(tc, wmdev_server_msg_available((uint32_t)new_server));
}

static ktest_case_t cases[] = {
    KTEST_CASE(test_wmdev_registers_single_server),
    KTEST_CASE(test_wmdev_create_window_tracks_owner_and_surface),
    KTEST_CASE(test_wmdev_mmap_page_returns_surface_pages),
    KTEST_CASE(test_wmdev_user_record_create_returns_surface_response),
    KTEST_CASE(test_wmdev_retain_keeps_connection_until_final_close),
    KTEST_CASE(test_wmdev_mmap_page_rejects_non_base_surface_offsets),
    KTEST_CASE(test_wmdev_rejects_cross_owner_present),
    KTEST_CASE(test_wmdev_event_queue_round_trips_to_owner),
    KTEST_CASE(test_wmdev_client_close_emits_destroy_and_frees_pages),
    KTEST_CASE(
        test_wmdev_client_close_replaces_saturated_window_queue_with_destroy),
    KTEST_CASE(
        test_wmdev_client_close_prioritizes_destroy_when_other_windows_fill_queue),
    KTEST_CASE(test_wmdev_server_close_destroys_windows_and_stales_handles),
};

static ktest_suite_t suite = KTEST_SUITE("wmdev", cases);

ktest_suite_t *ktest_suite_wmdev(void)
{
	return &suite;
}
