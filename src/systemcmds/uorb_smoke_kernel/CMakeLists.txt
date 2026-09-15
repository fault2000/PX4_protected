# SPDX-License-Identifier: BSD-3-Clause

px4_add_module(
	MODULE systemcmds__uorb_smoke_kernel
	MAIN uorb_smoke_kernel
	STACK_MAIN 2048
	SRCS
		uorb_smoke_kernel.cpp
)
