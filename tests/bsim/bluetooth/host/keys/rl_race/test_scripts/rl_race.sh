#!/usr/bin/env bash
# Copyright 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# Test: Resolving list race condition between settings_load and bt_unpair.
#
# Boot 1: Add 3 bonded peers (with IRKs) to flash and the controller RL.
# Boot 2: On reboot, call bt_unpair() immediately after settings_load().
#         The race condition is: settings_load schedules bt_id_add() via
#         work queue; bt_unpair calls bt_keys_clear() before bt_id_add()
#         sets BT_KEYS_ID_ADDED. Without the fix, the controller RL retains
#         stale entries. With the fix, bt_id_add() detects the race via
#         BT_KEYS_ID_ADD_IN_PROGRESS and rolls back with hci_id_del().

source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

test_exe="bs_${BOARD_TS}_tests_bsim_bluetooth_host_keys_rl_race_prj_conf"
simulation_id="keys_rl_race"
verbosity_level=2

cd ${BSIM_OUT_PATH}/bin

# --- Boot 1: populate resolving list and persist to flash ---
Execute "./${test_exe}" \
  -v=${verbosity_level} -s="${simulation_id}_1" -d=0 -testid=boot1 \
  -flash="${simulation_id}.log.bin"

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s="${simulation_id}_1" \
  -D=1 -sim_length=60e6

wait_for_background_jobs

# --- Boot 2: race condition test, reuse same flash image ---
Execute "./${test_exe}" \
  -v=${verbosity_level} -s="${simulation_id}_2" -d=0 -testid=boot2 \
  -flash="${simulation_id}.log.bin" -flash_rm

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s="${simulation_id}_2" \
  -D=1 -sim_length=60e6

wait_for_background_jobs
