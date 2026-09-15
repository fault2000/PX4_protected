// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// External platform interfaces for the protected HRT host regression only.
// The public HRT and boardctl declarations come from the production headers.
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#define __PX4_NUTTX 1
#define __EXPORT
#define FAR
#define OK 0
#define _PX4_IOC(base, number) ((base) | (number))
#define SEM_PRIO_NONE 0
#define SCHED_DEFAULT 0
#define SCHED_PRIORITY_MAX 255
#define PX4_ERR(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define PX4_WARN(...) PX4_ERR(__VA_ARGS__)
#define DEBUGASSERT(condition) assert(condition)

typedef unsigned int irqstate_t;
typedef int px4_task_t;
typedef struct { unsigned int value; } px4_sem_t;

__BEGIN_DECLS
irqstate_t px4_enter_critical_section(void);
void px4_leave_critical_section(irqstate_t flags);
int sched_lock(void);
int sched_unlock(void);
int px4_sem_init(px4_sem_t *sem, int shared, unsigned int value);
int px4_sem_setprotocol(px4_sem_t *sem, int protocol);
int px4_sem_wait(px4_sem_t *sem);
int px4_sem_post(px4_sem_t *sem);
int px4_task_spawn_cmd(const char *name, int policy, int priority, int stack_size,
		       int (*entry)(int, char **), char *const argv[]);
int px4_task_delete(px4_task_t task);
int px4_usleep(unsigned int usec);
int px4_register_shutdown_hook(bool (*hook)(void));
int boardctl(unsigned int cmd, uintptr_t arg);
__END_DECLS
