# SPDX-License-Identifier: BSD-3-Clause

px4_add_module(
	MODULE systemcmds__hrt_smoke
	MAIN hrt_smoke
	STACK_MAIN 2048
	SRCS
		hrt_smoke.cpp
)
