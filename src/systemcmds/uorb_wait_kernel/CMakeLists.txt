# SPDX-License-Identifier: BSD-3-Clause

px4_add_module(
	MODULE systemcmds__uorb_wait_kernel
	MAIN uorb_wait_kernel
	STACK_MAIN 2048
	SRCS
		uorb_wait_kernel.cpp
)
