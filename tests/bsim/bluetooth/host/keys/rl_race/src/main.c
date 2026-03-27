/* Copyright (c) 2026 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

#include "bstests.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bt_bsim_rl_race, LOG_LEVEL_DBG);

extern void run_boot1(void);
extern void run_boot2(void);

static const struct bst_test_instance test_def[] = {
	{
		.test_id = "boot1",
		.test_descr = "First boot: create 3 bonds to populate resolving list",
		.test_main_f = run_boot1,
	},
	{
		.test_id = "boot2",
		.test_descr = "Second boot: call bt_unpair immediately after settings_load, "
			      "verify controller resolving list is empty",
		.test_main_f = run_boot2,
	},
	BSTEST_END_MARKER,
};

static struct bst_test_list *test_rl_race_install(struct bst_test_list *tests)
{
	return bst_add_tests(tests, test_def);
}

bst_test_install_t test_installers[] = {test_rl_race_install, NULL};

int main(void)
{
	bst_main();
	return 0;
}
