// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdint.h>

#define __PX4_NUTTX 1
#define __KERNEL__ 1
#define SEM_PRIO_NONE 0

using irqstate_t = unsigned;

struct px4_sem_t {
	unsigned value;
};

irqstate_t px4_enter_critical_section();
void px4_leave_critical_section(irqstate_t flags);
int px4_sem_init(px4_sem_t *sem, int shared, unsigned value);
int px4_sem_destroy(px4_sem_t *sem);
int px4_sem_setprotocol(px4_sem_t *sem, int protocol);
int px4_sem_wait(px4_sem_t *sem);
int px4_sem_post(px4_sem_t *sem);
