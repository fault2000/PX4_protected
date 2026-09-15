/****************************************************************************
 *
 *   Copyright (c) 2026 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "protocol.h"

#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/atomic.h>
#include <px4_platform_common/posix.h>
#include <px4_platform_common/tasks.h>
#include <px4_platform_common/time.h>
#include <uORB/SubscriptionCallback.hpp>
#include <uORB/topics/orb_test.h>

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#if !defined(CONFIG_BUILD_PROTECTED) || !defined(__KERNEL__) || !defined(__arm__) || defined(CONFIG_SMP)
#error uorb_smoke_kernel requires single-core Cortex-M protected kernel space
#endif

namespace
{

constexpr unsigned poll_us = 10000;
constexpr unsigned exit_timeout_us = 3000000;

struct Session {
	px4::atomic<bool> running {false};
	px4::atomic<bool> failed {false};
	px4::atomic<bool> finished {false};
	px4::atomic<bool> passed {false};
	px4::atomic<bool> request_ok {false};
	px4::atomic<bool> reply_ok {false};
	px4::atomic<bool> cleanup_ok {false};
	px4::atomic<pid_t> command_pid {-1};
	px4::atomic<pid_t> worker_pid {-1};
	px4::atomic<uint32_t> worker_control {0};
	px4::atomic<uint32_t> worker_ipsr {0};
	px4::atomic<unsigned> callback_count {0};
	px4::atomic<pid_t> callback_pid {-1};
	px4::atomic<uint32_t> callback_control {0};
	px4::atomic<uint32_t> callback_ipsr {0};
};

// A timed-out worker may still reference this state. Failed invocations are
// latched until reboot, and this storage never belongs to the caller's stack.
Session session;

class NativeCallback final : public uORB::SubscriptionCallback
{
public:
	explicit NativeCallback(pid_t worker_pid) :
		SubscriptionCallback(ORB_ID(orb_multitest)), _worker_pid(worker_pid)
	{}

	void call() override
	{
		uint32_t control;
		uint32_t ipsr;
		asm volatile("mrs %0, control" : "=r"(control));
		asm volatile("mrs %0, ipsr" : "=r"(ipsr));
		const pid_t pid = getpid();

		if ((control & 1u) != 0 || ipsr != 0 || pid != _worker_pid) {
			_bad_context.store(true);
		}

		// Native callbacks run inline in the publisher's critical section.
		// Record only: no allocation, output, blocking or topic operations.
		session.callback_pid.store(pid);
		session.callback_control.store(control);
		session.callback_ipsr.store(ipsr);
		session.callback_count.fetch_add(1);
	}

	bool good_context() const { return !_bad_context.load(); }

private:
	const pid_t _worker_pid;
	px4::atomic<bool> _bad_context {false};
};

bool check_round_trip()
{
	uint32_t control;
	uint32_t ipsr;
	asm volatile("mrs %0, control" : "=r"(control));
	asm volatile("mrs %0, ipsr" : "=r"(ipsr));
	const pid_t worker_pid = getpid();
	session.worker_pid.store(worker_pid);
	session.worker_control.store(control);
	session.worker_ipsr.store(ipsr);

	if ((control & 1u) != 0 || ipsr != 0 || worker_pid <= 0 || worker_pid == session.command_pid.load()
	    || orb_exists(ORB_ID(orb_multitest), 0) != PX4_OK) {
		return false;
	}

	// The user command creates and seeds this existing diagnostic topic.
	// Copying it here checks user publication -> actual kernel thread.
	NativeCallback callback(worker_pid);
	orb_test_s request {};
	const bool request_ok = callback.copy(&request) && request.timestamp != 0
				&& request.val == uorb_smoke_protocol::request_value;
	session.request_ok.store(request_ok);
	bool reply_ok = false;
	orb_advert_t publisher = nullptr;

	if (request_ok && callback.registerCallback()) {
		// Obtain instance 0 without another initial publication. Only the
		// fixed reply below should notify both the kernel and user callback.
		publisher = orb_advertise(ORB_ID(orb_multitest), nullptr);

		if (publisher != nullptr) {
			orb_test_s reply {};
			reply.timestamp = hrt_absolute_time();
			reply.val = uorb_smoke_protocol::reply_value;
			const bool published = orb_publish(ORB_ID(orb_multitest), publisher, &reply) == PX4_OK;
			orb_test_s copied {};
			reply_ok = published && callback.copy(&copied) && copied.timestamp == reply.timestamp
				   && copied.val == reply.val;
		}
	}

	callback.unregisterCallback();
	callback.unsubscribe();
	const bool cleanup_ok = publisher == nullptr || orb_unadvertise(publisher) == PX4_OK;
	session.reply_ok.store(reply_ok);
	session.cleanup_ok.store(cleanup_ok);
	return request_ok && reply_ok && cleanup_ok && callback.good_context() && session.callback_count.load() == 1
	       && session.callback_pid.load() == worker_pid;
}

int worker_main(int, char *[])
{
	// Local callback destruction and unsubscription finish before the done
	// flag is published. The caller also waits for the task's actual exit.
	session.passed.store(check_round_trip());
	session.finished.store(true);
	return 0;
}

bool wait_for_exit(pid_t worker_pid)
{
	for (unsigned elapsed = 0; elapsed < exit_timeout_us; elapsed += poll_us) {
		// Signal zero checks existence only. Another error cannot establish
		// cleanup, and the finished flag alone does not cover the epilogue.
		if (kill(worker_pid, 0) != 0) {
			return errno == ESRCH && session.finished.load();
		}

		// The launch ioctl runs in the calling user task. Blocking here lets
		// the real kernel worker and user callback dispatcher both execute.
		px4_usleep(poll_us);
	}

	return false;
}

} // namespace

extern "C" __EXPORT int uorb_smoke_kernel_main(int argc, char *argv[])
{
	if (argc != 2 || argv == nullptr || argv[1] == nullptr || strcmp(argv[1], "run") != 0) {
		printf("Usage: uorb_smoke_kernel run (invoked by uorb_smoke)\n");
		return 1;
	}

	bool expected = false;

	if (!session.running.compare_exchange(&expected, true)) {
		printf("uorb_smoke_kernel: another invocation is running\n");
		return 1;
	}

	if (session.failed.load()) {
		printf("uorb_smoke_kernel: previous check failed; reboot before retrying\n");
		session.running.store(false);
		return 1;
	}

	session.finished.store(false);
	session.passed.store(false);
	session.request_ok.store(false);
	session.reply_ok.store(false);
	session.cleanup_ok.store(false);
	session.command_pid.store(getpid());
	session.worker_pid.store(-1);
	session.worker_control.store(0);
	session.worker_ipsr.store(0);
	session.callback_count.store(0);
	session.callback_pid.store(-1);
	session.callback_control.store(0);
	session.callback_ipsr.store(0);

	// The kernel library's spawn wrapper calls kthread_create. Calling the
	// builtin directly through boardctl would retain the user's task PID.
	const pid_t worker_pid = px4_task_spawn_cmd("uorb:k_smoke", SCHED_DEFAULT, SCHED_PRIORITY_DEFAULT, 2048,
				 worker_main, nullptr);
	const bool exited = worker_pid > 0 && wait_for_exit(worker_pid);
	const bool passed = exited && session.passed.load() && session.worker_pid.load() == worker_pid;
	session.failed.store(!passed);

	printf("uorb_smoke_kernel: %s roundtrip request=%d reply=%d worker_pid=%d CONTROL=0x%" PRIx32 " IPSR=%" PRIu32 "\n",
	       passed ? "PASS" : "FAIL", session.request_ok.load(), session.reply_ok.load(),
	       static_cast<int>(session.worker_pid.load()), session.worker_control.load(), session.worker_ipsr.load());
	printf("uorb_smoke_kernel: %s native-callback count=%u callback_pid=%d CONTROL=0x%" PRIx32 " IPSR=%" PRIu32 "\n",
	       passed ? "PASS" : "FAIL", session.callback_count.load(), static_cast<int>(session.callback_pid.load()),
	       session.callback_control.load(), session.callback_ipsr.load());
	printf("uorb_smoke_kernel: %s cleanup worker_pid=%d exited=%d released=%d\n",
	       passed ? "PASS" : "FAIL", static_cast<int>(worker_pid), exited, session.cleanup_ok.load());

	if (!passed) {
		printf("uorb_smoke_kernel: FAIL; reboot before retrying\n");
	}

	session.running.store(false);
	return passed ? 0 : 1;
}
