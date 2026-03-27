/* Copyright (c) 2026 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/net_buf.h>

/* Internal headers needed to access key pool and id management */
#include "host/keys.h"
#include "host/hci_core.h"
#include "host/id.h"

#include "babblekit/testcase.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(bt_bsim_rl_race, LOG_LEVEL_DBG);

/* Three fake peer addresses and IRKs to simulate bonded peers.
 * Static random addresses (MSBs 0xC_) with distinct IRKs.
 */
#define N_BONDS 3

static const bt_addr_le_t peer_addrs[N_BONDS] = {
	{.type = BT_ADDR_LE_RANDOM,
	 .a.val = {0xC1, 0x11, 0x11, 0x11, 0x11, 0xC1}},
	{.type = BT_ADDR_LE_RANDOM,
	 .a.val = {0xC2, 0x22, 0x22, 0x22, 0x22, 0xC2}},
	{.type = BT_ADDR_LE_RANDOM,
	 .a.val = {0xC3, 0x33, 0x33, 0x33, 0x33, 0xC3}},
};

static const uint8_t peer_irks[N_BONDS][16] = {
	{0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
	{0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02},
	{0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
	 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},
};

/**
 * @brief Query controller resolving list size via HCI LE Read RL Size.
 *
 * @return RL size supported by the controller, or 0 on error.
 */
static uint8_t read_rl_size(void)
{
	struct bt_hci_rp_le_read_rl_size *rp;
	struct net_buf *rsp;
	uint8_t rl_size = 0;
	int err;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_READ_RL_SIZE, NULL, &rsp);
	if (err) {
		LOG_ERR("LE Read RL Size failed (err %d)", err);
		return 0;
	}

	rp = (struct bt_hci_rp_le_read_rl_size *)rsp->data;
	rl_size = rp->rl_size;
	LOG_INF("Controller RL size: %u", rl_size);
	net_buf_unref(rsp);

	return rl_size;
}

/**
 * @brief Probe whether a peer address is still present in the controller RL.
 *
 * Attempts to add the peer back to the resolving list:
 * - If the entry is STILL in the RL (stale), the controller returns
 *   BT_HCI_ERR_CONN_ALREADY_EXISTS (0x0B). Returns true (stale).
 * - If the entry was properly removed, the add succeeds. We remove it
 *   again immediately to leave the controller clean. Returns false.
 *
 * @param addr  Peer identity address to probe.
 * @param irk   Peer IRK.
 *
 * @return true  if the entry is still in the controller RL (stale),
 *         false if it was properly removed.
 */
static bool is_entry_in_controller_rl(const bt_addr_le_t *addr,
				      const uint8_t irk[16])
{
	struct bt_hci_cp_le_add_dev_to_rl *cp;
	struct net_buf *buf;
	struct net_buf *rsp;
	char addr_str[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		LOG_ERR("Failed to allocate HCI cmd buffer");
		return false;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	bt_addr_le_copy(&cp->peer_id_addr, addr);
	memcpy(cp->peer_irk, irk, 16);
	memset(cp->local_irk, 0, 16);

	err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_ADD_DEV_TO_RL, buf, &rsp);
	if (rsp) {
		net_buf_unref(rsp);
	}

	if (err == -EALREADY) {
		/* Controller returned BT_HCI_ERR_CONN_ALREADY_EXISTS:
		 * the entry was never removed - stale entry confirmed.
		 */
		LOG_ERR("Stale RL entry detected for %s", addr_str);
		return true;
	}

	if (err == 0) {
		/* Add succeeded - entry was clean. Remove it again to restore
		 * the controller to the state we found it in.
		 */
		struct bt_hci_cp_le_rem_dev_from_rl *rcp;

		buf = bt_hci_cmd_alloc(K_FOREVER);
		if (buf) {
			rcp = net_buf_add(buf, sizeof(*rcp));
			bt_addr_le_copy(&rcp->peer_id_addr, addr);
			err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_REM_DEV_FROM_RL,
						   buf, &rsp);
			if (rsp) {
				net_buf_unref(rsp);
			}
		}
		return false;
	}

	LOG_WRN("Unexpected error probing RL entry %s (err %d)",
		addr_str, err);
	return false;
}

/* --- Boot 1: populate resolving list and persist bonds to flash --- */

void run_boot1(void)
{
	int err;
	struct bt_keys *keys;

	LOG_INF("Boot 1: creating %u bonds to populate resolving list", N_BONDS);

	err = bt_enable(NULL);
	TEST_ASSERT(err == 0, "bt_enable failed (err %d)", err);

	err = settings_load();
	TEST_ASSERT(err == 0, "settings_load failed (err %d)", err);

	uint8_t rl_size = read_rl_size();

	TEST_ASSERT(rl_size >= N_BONDS,
		    "Controller RL too small (%u); need at least %u slots",
		    rl_size, N_BONDS);

	/* Simulate 3 completed pairings by directly inserting IRK keys into
	 * the key pool and persisting them to flash storage.
	 */
	for (size_t i = 0; i < N_BONDS; i++) {
		keys = bt_keys_get_type(BT_KEYS_IRK, BT_ID_DEFAULT,
					&peer_addrs[i]);
		TEST_ASSERT(keys != NULL,
			    "Failed to allocate key slot for bond %u", (unsigned)i);

		memcpy(keys->irk.val, peer_irks[i], sizeof(keys->irk.val));

		err = bt_keys_store(keys);
		TEST_ASSERT(err == 0,
			    "Failed to store bond %u to flash (err %d)", (unsigned)i, err);

		bt_id_add(keys);

		/* Allow pending work (add_id_work) to complete before checking */
		k_msleep(50);

		TEST_ASSERT(keys->state & BT_KEYS_ID_ADDED,
			    "Bond %u not added to resolving list (state=0x%x)",
			    (unsigned)i, keys->state);

		char addr_str[BT_ADDR_LE_STR_LEN];

		bt_addr_le_to_str(&peer_addrs[i], addr_str, sizeof(addr_str));
		LOG_INF("Boot 1: bond %u added to RL (addr %s)",
			(unsigned)i, addr_str);
	}

	TEST_ASSERT(bt_dev.le.rl_entries == N_BONDS,
		    "Expected %u RL entries, got %u",
		    N_BONDS, bt_dev.le.rl_entries);

	LOG_INF("Boot 1: done - %u entries confirmed in resolving list", N_BONDS);

	TEST_PASS("Boot 1 passed");
}

/* --- Boot 2: race condition test --- */

void run_boot2(void)
{
	int err;

	LOG_INF("Boot 2: testing race between settings_load and bt_unpair");

	err = bt_enable(NULL);
	TEST_ASSERT(err == 0, "bt_enable failed (err %d)", err);

	/* Trigger the race condition:
	 * settings_load() schedules bt_id_add() for each restored key via
	 * add_id_work on the system work queue.  Calling bt_unpair()
	 * immediately after races with those pending HCI add operations:
	 *
	 * - bt_id_add() calls hci_id_add() which blocks waiting for HCI
	 *   response.
	 * - bt_unpair() -> bt_keys_clear() runs (higher priority), finds
	 *   BT_KEYS_ID_ADDED not set, skips bt_id_del(), does memset() which
	 *   clears BT_KEYS_ID_ADD_IN_PROGRESS.
	 * - bt_id_add() resumes, checks BT_KEYS_ID_ADD_IN_PROGRESS, detects
	 *   race, calls hci_id_del() to roll back.
	 *
	 * WITHOUT the fix: controller RL retains the 3 stale entries.
	 * WITH the fix:    controller RL is empty after rollback.
	 */
	err = settings_load();
	TEST_ASSERT(err == 0, "settings_load failed (err %d)", err);

	/* Immediately unpair all - this is the racing call */
	err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	TEST_ASSERT(err == 0, "bt_unpair failed (err %d)", err);

	/* Allow time for any in-flight bt_id_add() to complete its rollback */
	k_msleep(200);

	/* Verify 1: host-side RL entry count must be 0 */
	TEST_ASSERT(bt_dev.le.rl_entries == 0,
		    "Host RL entry count is %u, expected 0 - stale entries remain",
		    bt_dev.le.rl_entries);

	/* Verify 2: probe the controller RL directly for each peer.
	 * Attempt to re-add each known address - if the controller returns
	 * BT_HCI_ERR_CONN_ALREADY_EXISTS the entry was never removed (stale).
	 * This is the definitive check that the host/controller are in sync.
	 */
	for (size_t i = 0; i < N_BONDS; i++) {
		bool stale = is_entry_in_controller_rl(&peer_addrs[i],
						       peer_irks[i]);

		TEST_ASSERT(!stale,
			    "Bond %u still present in controller RL after bt_unpair "
			    "- host/controller resolving list desynced",
			    (unsigned)i);
	}

	/* Verify 3: all keys must be gone from the host key pool */
	for (size_t i = 0; i < N_BONDS; i++) {
		struct bt_keys *keys = bt_keys_find(BT_KEYS_IRK,
						    BT_ID_DEFAULT,
						    &peer_addrs[i]);

		TEST_ASSERT(keys == NULL,
			    "Bond %u still present in key pool after bt_unpair",
			    (unsigned)i);
	}

	LOG_INF("Boot 2: all %u bonds cleared, controller RL is empty", N_BONDS);

	TEST_PASS("Boot 2 passed - no stale entries in controller resolving list");
}
