# SPDX-License-Identifier: BSD-3-Clause

px4_add_module(
	MODULE systemcmds__uorb_wait_smoke
	MAIN uorb_wait_smoke
	STACK_MAIN 2560
	SRCS
		uorb_wait_smoke.cpp
	DEPENDS
		uORB
)
