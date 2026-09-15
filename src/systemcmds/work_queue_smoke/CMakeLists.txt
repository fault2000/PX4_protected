# SPDX-License-Identifier: BSD-3-Clause

px4_add_module(
	MODULE systemcmds__work_queue_smoke
	MAIN work_queue_smoke
	STACK_MAIN 2048
	SRCS
		work_queue_smoke.cpp
	DEPENDS
		px4_work_queue
)
