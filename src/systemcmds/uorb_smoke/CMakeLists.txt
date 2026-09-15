# SPDX-License-Identifier: BSD-3-Clause

px4_add_module(
	MODULE systemcmds__uorb_smoke
	MAIN uorb_smoke
	STACK_MAIN 2048
	SRCS
		uorb_smoke.cpp
	DEPENDS
		px4_work_queue
		uORB
)
