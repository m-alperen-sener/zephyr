/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "babblekit/flags.h"
#include "babblekit/sync.h"
#include "babblekit/testcase.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(peripheral, LOG_LEVEL_INF);

BUILD_ASSERT(CONFIG_BT_MAX_CONN == 1);

DEFINE_FLAG(flag_connected);
DEFINE_FLAG(flag_disconnected);
DEFINE_FLAG(flag_pairing_done);

static struct bt_conn *g_conn;
static bt_addr_le_t bonded_central_id;
static struct bt_le_ext_adv *ext_adv;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static void register_auth_cb(void);

static void disconnected(struct bt_conn *conn, uint8_t reason);

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		return;
	}

	if (!g_conn) {
		g_conn = bt_conn_ref(conn);
	}
	SET_FLAG(flag_connected);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);

	if (conn == g_conn) {
		UNSET_FLAG(flag_connected);
		SET_FLAG(flag_disconnected);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);

	if (!bonded) {
		TEST_FAIL("Expected bonding when pairing");
	}
	SET_FLAG(flag_pairing_done);
}

static struct bt_conn_auth_info_cb auth_cb = {
	.pairing_complete = pairing_complete,
};

static void register_auth_cb(void)
{
	int err;

	err = bt_conn_auth_info_cb_register(&auth_cb);
	TEST_ASSERT(err == 0, "bt_conn_auth_info_cb_register failed (%d)", err);
}

static void common_init(void)
{
	int err;

	err = bt_enable(NULL);
	TEST_ASSERT(err == 0, "bt_enable failed (%d)", err);
	register_auth_cb();
	err = settings_load();
	TEST_ASSERT(err == 0, "settings_load failed (%d)", err);
}

static void ext_adv_delete(void)
{
	int err;

	if (ext_adv == NULL) {
		return;
	}

	err = bt_le_ext_adv_stop(ext_adv);
	TEST_ASSERT(err == 0, "ext adv stop failed (%d)", err);

	err = bt_le_ext_adv_delete(ext_adv);
	TEST_ASSERT(err == 0, "ext adv delete failed (%d)", err);

	ext_adv = NULL;
}

static void advertise_undirected_ext(void)
{
	int err;

	err = bt_le_ext_adv_create(BT_LE_EXT_ADV_CONN, NULL, &ext_adv);
	TEST_ASSERT(err == 0, "ext adv create (undirected) failed (%d)", err);

	err = bt_le_ext_adv_set_data(ext_adv, ad, ARRAY_SIZE(ad), NULL, 0);
	TEST_ASSERT(err == 0, "ext adv set_data failed (%d)", err);

	err = bt_le_ext_adv_start(ext_adv, BT_LE_EXT_ADV_START_DEFAULT);
	TEST_ASSERT(err == 0, "ext adv start (undirected) failed (%d)", err);
}

/* Refresh local RPA and assert it is resolvable (AdvA uses controller privacy). */
static void assert_local_address_is_rpa(void)
{
	int err;
	struct bt_le_oob oob = {0};

	err = bt_le_oob_get_local(BT_ID_DEFAULT, &oob);
	TEST_ASSERT(err == 0, "bt_le_oob_get_local failed (%d)", err);
	TEST_ASSERT(bt_addr_le_is_rpa(&oob.addr),
		    "Local address must be RPA (own IRK / controller privacy for AdvA)");
}

static void advertise_directed_rpa_ext(const bt_addr_le_t *central_id)
{
	int err;
	struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_DIR_MODE_LOW_DUTY |
			BT_LE_ADV_OPT_DIR_ADDR_RPA,
		BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, central_id);

	/* AdvA: not identity — stack uses RPA with CONFIG_BT_PRIVACY. */
	TEST_ASSERT((param.options & BT_LE_ADV_OPT_USE_IDENTITY) == 0,
		    "Directed ext adv must not use identity for AdvA (expect RPA)");
	TEST_ASSERT((param.options & BT_LE_ADV_OPT_DIR_ADDR_RPA) != 0,
		    "TargetA must be peer RPA (BT_LE_ADV_OPT_DIR_ADDR_RPA)");

	err = bt_le_ext_adv_create(&param, NULL, &ext_adv);
	TEST_ASSERT(err == 0, "ext adv create (directed RPA) failed (%d)", err);

	err = bt_le_ext_adv_start(ext_adv, BT_LE_EXT_ADV_START_DEFAULT);
	TEST_ASSERT(err == 0, "ext adv start (directed RPA) failed (%d)", err);
}

static void create_bonds(void)
{
	advertise_undirected_ext();

	WAIT_FOR_FLAG(flag_connected);

	WAIT_FOR_FLAG(flag_pairing_done);
	UNSET_FLAG(flag_pairing_done);

	WAIT_FOR_FLAG(flag_disconnected);
	UNSET_FLAG(flag_disconnected);

	ext_adv_delete();

	bt_conn_unref(g_conn);
	g_conn = NULL;
}

struct bond_foreach_ctx {
	int count;
};

static void foreach_bond_cb(const struct bt_bond_info *info, void *user_data)
{
	struct bond_foreach_ctx *ctx = user_data;

	ctx->count++;
	bt_addr_le_copy(&bonded_central_id, &info->addr);
}

static void directed_reconnect(void)
{
	int err;
	struct bond_foreach_ctx ctx = {0};

	TEST_ASSERT(bk_sync_init() == 0, "bk_sync_init failed");

	bt_foreach_bond(BT_ID_DEFAULT, foreach_bond_cb, &ctx);

	TEST_ASSERT(ctx.count == 1, "Expected exactly one bond after reboot, got %d", ctx.count);

	bk_sync_wait();

	assert_local_address_is_rpa();
	advertise_directed_rpa_ext(&bonded_central_id);

	WAIT_FOR_FLAG(flag_connected);

	err = bt_conn_disconnect(g_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	TEST_ASSERT(err == 0, "phase2 disconnect failed (%d)", err);

	WAIT_FOR_FLAG(flag_disconnected);
	UNSET_FLAG(flag_disconnected);

	ext_adv_delete();

	bt_conn_unref(g_conn);
	g_conn = NULL;
}

void peripheral_bond(void)
{
	common_init();
	create_bonds();
	/* Allow bond keys to reach persistent storage before the process exits. */
	k_sleep(K_MSEC(200));
	TEST_PASS("Peripheral: bond stored");
}

void peripheral_reconnect(void)
{
	common_init();
	directed_reconnect();
	TEST_PASS("Peripheral: connect with directed RPA OK");
}
