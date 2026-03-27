/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/settings/settings.h>

#include "babblekit/flags.h"
#include "babblekit/sync.h"
#include "babblekit/testcase.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(central, LOG_LEVEL_INF);

BUILD_ASSERT(CONFIG_BT_MAX_CONN == 1);

DEFINE_FLAG(flag_connected);
DEFINE_FLAG(flag_disconnected);
DEFINE_FLAG(flag_pairing_done);

static struct bt_conn *g_conn;
static bt_addr_le_t bonded_peer_id;

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

static bool ext_adv_props_skip(const struct bt_le_scan_recv_info *info)
{
	if (info->adv_props & BT_GAP_ADV_PROP_SCAN_RESPONSE) {
		return true;
	}

	return false;
}

static void bond_scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
	int err;

	ARG_UNUSED(buf);

	if (g_conn != NULL) {
		return;
	}

	if (ext_adv_props_skip(info)) {
		return;
	}

	if (info->adv_type != BT_GAP_ADV_TYPE_EXT_ADV) {
		return;
	}

	if (!(info->adv_props & BT_GAP_ADV_PROP_EXT_ADV)) {
		return;
	}

	if (!(info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE)) {
		return;
	}

	if (info->adv_props & BT_GAP_ADV_PROP_DIRECTED) {
		return;
	}

	err = bt_le_scan_stop();
	TEST_ASSERT(err == 0, "bt_le_scan_stop failed (%d)", err);

	err = bt_conn_le_create(info->addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &g_conn);
	TEST_ASSERT(err == 0, "bt_conn_le_create failed (%d)", err);
}

static struct bt_le_scan_cb bond_scan = {
	.recv = bond_scan_recv,
};

static void create_bonds(void)
{
	int err;

	err = bt_le_scan_cb_register(&bond_scan);
	TEST_ASSERT(err == 0, "bt_le_scan_cb_register failed (%d)", err);

	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);
	TEST_ASSERT(err == 0, "bt_le_scan_start failed (%d)", err);

	WAIT_FOR_FLAG(flag_connected);

	err = bt_conn_set_security(g_conn, BT_SECURITY_L2);
	TEST_ASSERT(err == 0, "bt_conn_set_security failed (%d)", err);

	WAIT_FOR_FLAG(flag_pairing_done);
	UNSET_FLAG(flag_pairing_done);

	err = bt_conn_disconnect(g_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	TEST_ASSERT(err == 0, "bt_conn_disconnect failed (%d)", err);

	WAIT_FOR_FLAG(flag_disconnected);
	UNSET_FLAG(flag_disconnected);

	bt_conn_unref(g_conn);
	g_conn = NULL;
}

struct central_bond_ctx {
	int count;
	bt_addr_le_t addr;
};

static void central_foreach_bond_cb(const struct bt_bond_info *info, void *user_data)
{
	struct central_bond_ctx *ctx = user_data;

	ctx->count++;
	bt_addr_le_copy(&ctx->addr, &info->addr);
}

static void load_bonded_peer_identity(void)
{
	struct central_bond_ctx ctx = {0};

	bt_foreach_bond(BT_ID_DEFAULT, central_foreach_bond_cb, &ctx);
	TEST_ASSERT(ctx.count == 1, "Expected one bond from settings, got %d", ctx.count);
	bt_addr_le_copy(&bonded_peer_id, &ctx.addr);
}

static void reconnect_scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
	int err;

	ARG_UNUSED(buf);

	if (g_conn != NULL) {
		return;
	}

	if (ext_adv_props_skip(info)) {
		return;
	}

	if (info->adv_type != BT_GAP_ADV_TYPE_EXT_ADV) {
		return;
	}

	if (!(info->adv_props & BT_GAP_ADV_PROP_EXT_ADV)) {
		return;
	}

	if (!(info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE)) {
		return;
	}

	if (!(info->adv_props & BT_GAP_ADV_PROP_DIRECTED)) {
		return;
	}

	/* Peer IRK must be in the controller RL (flushed on scan start): AdvA resolves
	 * to the bonded peripheral identity.
	 */
	TEST_ASSERT(bt_addr_le_eq(info->addr, &bonded_peer_id),
		    "AdvA must match bonded identity (peer IRK / RL resolution)");

	err = bt_le_scan_stop();
	TEST_ASSERT(err == 0, "phase2 bt_le_scan_stop failed (%d)", err);

	err = bt_conn_le_create(info->addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &g_conn);
	TEST_ASSERT(err == 0, "phase2 bt_conn_le_create failed (%d)", err);
}

static struct bt_le_scan_cb reconnect_scan = {
	.recv = reconnect_scan_recv,
};

static void reconnect(void)
{
	int err;

	TEST_ASSERT(bk_sync_init() == 0, "bk_sync_init failed");

	err = bt_le_scan_cb_register(&reconnect_scan);
	TEST_ASSERT(err == 0, "phase2 bt_le_scan_cb_register failed (%d)", err);

	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);
	TEST_ASSERT(err == 0, "phase2 bt_le_scan_start failed (%d)", err);

	bk_sync_send();

	WAIT_FOR_FLAG(flag_connected);

	{
		struct bt_conn_info info;

		err = bt_conn_get_info(g_conn, &info);
		TEST_ASSERT(err == 0, "bt_conn_get_info failed (%d)", err);
		TEST_ASSERT(info.le.dst != NULL, "LE conn info missing dst");
		TEST_ASSERT(bt_addr_le_eq(info.le.dst, &bonded_peer_id),
			    "Connection peer must match bonded identity after reconnect");
	}

	err = bt_conn_disconnect(g_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	TEST_ASSERT(err == 0, "phase2 disconnect failed (%d)", err);

	WAIT_FOR_FLAG(flag_disconnected);
	UNSET_FLAG(flag_disconnected);

	bt_conn_unref(g_conn);
	g_conn = NULL;
}

void central_bond(void)
{
	common_init();
	create_bonds();
	/* Make sure bond information is stored */
	k_sleep(K_MSEC(200));
	TEST_PASS("Central: bond stored");
}

void central_reconnect(void)
{
	common_init();
	load_bonded_peer_identity();
	reconnect();
	TEST_PASS("central: reconnect");
}
