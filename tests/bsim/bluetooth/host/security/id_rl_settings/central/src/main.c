/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * This test verifies that the central can succesfully used IRK restored from settings,
 * added to the controller's resolving list, to connect to a peripheral with a directed RPA.
 *
 * In the first run central creates a bond, central and peripheral distributed their IRKs
 * In the second run central reconnects with the peripheral with directed advertisement
 * with RPA targeting itself.
 */

#include "bstests.h"

void central_bond(void);
void central_reconnect(void);

static const struct bst_test_instance test_to_add[] = {
	{
		.test_id = "central_bond",
		.test_main_f = central_bond,
	},
	{
		.test_id = "central_reconnect",
		.test_main_f = central_reconnect,
	},
	BSTEST_END_MARKER,
};

static struct bst_test_list *install(struct bst_test_list *tests)
{
	return bst_add_tests(tests, test_to_add);
}

bst_test_install_t test_installers[] = {install, NULL};

int main(void)
{
	bst_main();
	return 0;
}
