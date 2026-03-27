#!/usr/bin/env bash
# Copyright 2026 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#

set -eu
source ${ZEPHYR_BASE}/tests/bsim/sh_common.source

verbosity_level=2
simulation_id="id_rl_settings"

test_path="$(guess_test_long_name)"
central_exe="${BSIM_OUT_PATH}/bin/bs_${BOARD_TS}_${test_path}_central_prj_conf"
peripheral_exe="${BSIM_OUT_PATH}/bin/bs_${BOARD_TS}_${test_path}_peripheral_prj_conf"

flash_c="${simulation_id}.central.log.bin"
flash_p="${simulation_id}.peripheral.log.bin"

cd ${BSIM_OUT_PATH}/bin

# --- Run 1: establish bond, persist to flash ---
Execute "$central_exe" \
	-v=${verbosity_level} -s=${simulation_id}_1 -d=0 -testid=central_bond -RealEncryption=1 \
	-flash="${flash_c}" -flash_erase

Execute "$peripheral_exe" \
	-v=${verbosity_level} -s=${simulation_id}_1 -d=1 -testid=peripheral_bond -RealEncryption=1 \
	-flash="${flash_p}" -flash_erase -rs=200

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id}_1 \
	-D=2 -sim_length=60e6 $@

wait_for_background_jobs

# --- Run 2: cold boot from flash, scan + directed RPA reconnect ---
Execute "$central_exe" \
	-v=${verbosity_level} -s=${simulation_id}_2 -d=0 -testid=central_reconnect -RealEncryption=1 \
	-flash="${flash_c}" -flash_rm

Execute "$peripheral_exe" \
	-v=${verbosity_level} -s=${simulation_id}_2 -d=1 -testid=peripheral_reconnect -RealEncryption=1 \
	-flash="${flash_p}" -flash_rm -rs=200

Execute ./bs_2G4_phy_v1 -v=${verbosity_level} -s=${simulation_id}_2 \
	-D=2 -sim_length=60e6 $@

wait_for_background_jobs
