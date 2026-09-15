/****************************************************************************
 *
 *   Copyright (c) 2012-2015 PX4 Development Team. All rights reserved.
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

#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <fcntl.h>

#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/posix.h>
#include <px4_platform_common/tasks.h>

#include <sys/boardctl.h>
#include <errno.h>
#include <sched.h>
#include <px4_platform_common/time.h>

#include "uORBDeviceNode.hpp"
#include "uORBUtils.hpp"
#include "uORBManager.hpp"
#include "SubscriptionCallback.hpp"

#if defined(CONFIG_SMP)
#error "Protected user uORB callbacks require a single-core scheduler"
#endif

namespace
{

struct UserCallbackEntry {
	void *node{nullptr};
	uORB::SubscriptionCallback *callback{nullptr};
	uint32_t token{0};
};

UserCallbackEntry user_callbacks[ORB_USER_CALLBACK_CAPACITY] {};
uint32_t last_callback_token{0};
px4_task_t callback_task{-1};
bool callback_task_starting{false};
unsigned callback_api_errors{0};
int callback_api_errno{0};

void record_callback_error(int error)
{
	// Registration APIs may run inside a callback. Print only after dispatch
	// releases its outer scheduler lock; console output can block.
	++callback_api_errors;
	callback_api_errno = error;
}

} // namespace

// A distinct symbol also lets the artifact checker verify user placement.
extern "C" int uorb_user_callback_dispatch(int, char **)
{
	for (;;) {
		orbiocdevwaitcallback_t event {};
		// NuttX preserves the nested task scheduler lock across a blocking
		// syscall. Once an event returns, another user task cannot unregister
		// its callback until the short, nonblocking call() has finished.
		sched_lock();
		const int ret = boardctl(ORBIOCDEVWAITCALLBACK, reinterpret_cast<unsigned long>(&event));
		const int wait_errno = errno;

		if (ret == 0 && event.token != 0) {
			for (auto &entry : user_callbacks) {
				if (entry.token == event.token) {
					uORB::SubscriptionCallback *callback = entry.callback;
					callback->call();
					// Self-unregister is allowed. Do not access the callback or
					// its map entry after call(), even for accounting.
					break;
				}
			}
		}

		const unsigned errors = callback_api_errors;
		const int api_errno = callback_api_errno;
		callback_api_errors = 0;
		sched_unlock();

		if (errors) {
			PX4_ERR("user callback API errors=%u last_errno=%d", errors, api_errno);
		}

		if (ret < 0 && wait_errno != EINTR) {
			PX4_ERR("user callback wait failed (%d)", wait_errno);
			px4_usleep(100000);
		}
	}

	return 0;
}

namespace
{

bool start_callback_dispatcher()
{
	sched_lock();

	if (callback_task >= 0) {
		sched_unlock();
		return true;
	}

	if (callback_task_starting) {
		sched_unlock();
		errno = EBUSY;
		return false;
	}

	callback_task_starting = true;
	callback_task = px4_task_spawn_cmd("usr_uorb", SCHED_DEFAULT, SCHED_PRIORITY_MAX - 1, 1536,
					   uorb_user_callback_dispatch, nullptr);
	callback_task_starting = false;
	const bool started = callback_task >= 0;
	sched_unlock();
	return started;
}

} // namespace

uORB::Manager *uORB::Manager::_Instance = nullptr;

bool uORB::Manager::initialize()
{
	if (_Instance == nullptr) {
		_Instance = new uORB::Manager();
	}

	return _Instance != nullptr && start_callback_dispatcher();
}

bool uORB::Manager::terminate()
{
	// The callback service lives for this boot. Do not asynchronously kill a
	// dispatcher that may be in a callback or a kernel notification wait.
	if (callback_task >= 0 || callback_task_starting) {
		errno = EBUSY;
		return false;
	}

	if (_Instance != nullptr) {
		delete _Instance;
		_Instance = nullptr;
		return true;
	}

	return false;
}

uORB::Manager::Manager()
{
}

uORB::Manager::~Manager()
{
}

int uORB::Manager::orb_exists(const struct orb_metadata *meta, int instance)
{
	// instance valid range: [0, ORB_MULTI_MAX_INSTANCES)
	if ((instance < 0) || (instance > (ORB_MULTI_MAX_INSTANCES - 1))) {
		return PX4_ERROR;
	}

	orbiocdevexists_t data = {static_cast<ORB_ID>(meta->o_id), static_cast<uint8_t>(instance), true, PX4_ERROR};
	boardctl(ORBIOCDEVEXISTS, reinterpret_cast<unsigned long>(&data));

	return data.ret;
}

orb_advert_t uORB::Manager::orb_advertise_multi(const struct orb_metadata *meta, const void *data, int *instance)
{
	/* open the node as an advertiser */
	int fd = node_open(meta, true, instance);

	if (fd == PX4_ERROR) {
		PX4_ERR("%s advertise failed (%i)", meta->o_name, errno);
		return nullptr;
	}

	/* get the advertiser handle and close the node */
	orb_advert_t advertiser;

	int result = px4_ioctl(fd, ORBIOCGADVERTISER, (unsigned long)&advertiser);
	px4_close(fd);

	if (result == PX4_ERROR) {
		PX4_WARN("px4_ioctl ORBIOCGADVERTISER failed. fd = %d", fd);
		return nullptr;
	}

	/* the advertiser may perform an initial publish to initialise the object */
	if (data != nullptr) {
		result = orb_publish(meta, advertiser, data);

		if (result == PX4_ERROR) {
			PX4_ERR("orb_publish failed %s", meta->o_name);
			return nullptr;
		}
	}

	return advertiser;
}

int uORB::Manager::orb_unadvertise(orb_advert_t handle)
{
	orbiocdevunadvertise_t data = {handle, PX4_ERROR};
	boardctl(ORBIOCDEVUNADVERTISE, reinterpret_cast<unsigned long>(&data));

	return data.ret;
}

int uORB::Manager::orb_subscribe(const struct orb_metadata *meta)
{
	return node_open(meta, false);
}

int uORB::Manager::orb_subscribe_multi(const struct orb_metadata *meta, unsigned instance)
{
	int inst = instance;
	return node_open(meta, false, &inst);
}

int uORB::Manager::orb_unsubscribe(int fd)
{
	return px4_close(fd);
}

int uORB::Manager::orb_publish(const struct orb_metadata *meta, orb_advert_t handle, const void *data)
{
	orbiocdevpublish_t d = {meta, handle, data, PX4_ERROR};
	boardctl(ORBIOCDEVPUBLISH, reinterpret_cast<unsigned long>(&d));

	return d.ret;
}

int uORB::Manager::orb_copy(const struct orb_metadata *meta, int handle, void *buffer)
{
	int ret;

	ret = px4_read(handle, buffer, meta->o_size);

	if (ret < 0) {
		return PX4_ERROR;
	}

	if (ret != (int)meta->o_size) {
		errno = EIO;
		return PX4_ERROR;
	}

	return PX4_OK;
}

int uORB::Manager::orb_check(int handle, bool *updated)
{
	/* Set to false here so that if `px4_ioctl` fails to false. */
	*updated = false;
	return px4_ioctl(handle, ORBIOCUPDATED, (unsigned long)(uintptr_t)updated);
}

int uORB::Manager::orb_set_interval(int handle, unsigned interval)
{
	return px4_ioctl(handle, ORBIOCSETINTERVAL, interval * 1000);
}

int uORB::Manager::orb_get_interval(int handle, unsigned *interval)
{
	int ret = px4_ioctl(handle, ORBIOCGETINTERVAL, (unsigned long)interval);
	*interval /= 1000;
	return ret;
}

int uORB::Manager::node_open(const struct orb_metadata *meta, bool advertiser, int *instance)
{
	char path[orb_maxpath];
	int fd = -1;
	int ret = PX4_ERROR;

	/*
	 * If meta is null, the object was not defined, i.e. it is not
	 * known to the system.  We can't advertise/subscribe such a thing.
	 */
	if (nullptr == meta) {
		errno = ENOENT;
		return PX4_ERROR;
	}

	/* if we have an instance and are an advertiser, we will generate a new node and set the instance,
	 * so we do not need to open here */
	if (!instance || !advertiser) {
		/*
		 * Generate the path to the node and try to open it.
		 */
		ret = uORB::Utils::node_mkpath(path, meta, instance);

		if (ret != OK) {
			errno = -ret;
			return PX4_ERROR;
		}

		/* open the path as either the advertiser or the subscriber */
		fd = px4_open(path, advertiser ? PX4_F_WRONLY : PX4_F_RDONLY);

	} else {
		*instance = 0;
	}

	/* we may need to advertise the node... */
	if (fd < 0) {
		ret = PX4_ERROR;

		orbiocdevadvertise_t data = {meta, advertiser, instance, PX4_ERROR};
		boardctl(ORBIOCDEVADVERTISE, (unsigned long)&data);
		ret = data.ret;

		/* it's OK if it already exists */
		if ((ret != PX4_OK) && (EEXIST == errno)) {
			ret = PX4_OK;
		}

		if (ret == PX4_OK) {
			/* update the path, as it might have been updated during the node advertise call */
			ret = uORB::Utils::node_mkpath(path, meta, instance);

			/* on success, try to open again */
			if (ret == PX4_OK) {
				fd = px4_open(path, (advertiser) ? PX4_F_WRONLY : PX4_F_RDONLY);

			} else {
				errno = -ret;
				return PX4_ERROR;
			}
		}
	}

	if (fd < 0) {
		errno = EIO;
		return PX4_ERROR;
	}

	/* everything has been OK, we can return the handle now */
	return fd;
}

bool uORB::Manager::orb_device_node_exists(ORB_ID orb_id, uint8_t instance)
{
	orbiocdevexists_t data = {orb_id, instance, false, 0};
	boardctl(ORBIOCDEVEXISTS, reinterpret_cast<unsigned long>(&data));

	return data.ret == PX4_OK ? true : false;
}

void *uORB::Manager::orb_add_internal_subscriber(ORB_ID orb_id, uint8_t instance, unsigned *initial_generation)
{
	orbiocdevaddsubscriber_t data = {orb_id, instance, initial_generation, nullptr};
	boardctl(ORBIOCDEVADDSUBSCRIBER, reinterpret_cast<unsigned long>(&data));

	return data.handle;
}

void uORB::Manager::orb_remove_internal_subscriber(void *node_handle)
{
	boardctl(ORBIOCDEVREMSUBSCRIBER, reinterpret_cast<unsigned long>(node_handle));
}

uint8_t uORB::Manager::orb_get_queue_size(const void *node_handle)
{
	orbiocdevqueuesize_t data = {node_handle, 0};
	boardctl(ORBIOCDEVQUEUESIZE, reinterpret_cast<unsigned long>(&data));

	return data.size;
}

bool uORB::Manager::orb_data_copy(void *node_handle, void *dst, unsigned &generation, bool only_if_updated)
{
	orbiocdevdatacopy_t data = {node_handle, dst, generation, only_if_updated, false};
	boardctl(ORBIOCDEVDATACOPY, reinterpret_cast<unsigned long>(&data));
	generation = data.generation;

	return data.ret;
}

bool uORB::Manager::register_callback(void *node_handle, SubscriptionCallback *callback_sub)
{
	detail::CallbackStateLock guard;

	if (!node_handle || !callback_sub || callback_task < 0) {
		errno = EINVAL;
		return false;
	}

	UserCallbackEntry *available = nullptr;

	for (auto &entry : user_callbacks) {
		if (entry.callback == callback_sub) {
			return entry.node == node_handle;
		}

		if (!entry.callback && !available) {
			available = &entry;
		}
	}

	if (!available || last_callback_token == UINT32_MAX) {
		errno = available ? EOVERFLOW : ENOSPC;
		return false;
	}

	// Never recycle token values, even after a failed registration. A late
	// token can never refer to a new callback reusing an old object address.
	const uint32_t token = ++last_callback_token;
	orbiocdevregcallback_t data = {node_handle, token, false};
	const int ret = boardctl(ORBIOCDEVREGCALLBACK, reinterpret_cast<unsigned long>(&data));

	if (ret != 0 || !data.registered) {
		if (ret == 0) {
			// The transport succeeded, but the kernel rejected registration.
			errno = EIO;
		}

		record_callback_error(errno);
		return false;
	}

	*available = {node_handle, callback_sub, token};

	return true;
}

void uORB::Manager::unregister_callback(void *node_handle, SubscriptionCallback *callback_sub)
{
	detail::CallbackStateLock guard;

	for (auto &entry : user_callbacks) {
		if (entry.callback == callback_sub && entry.node == node_handle) {
			orbiocdevunregcallback_t data = {entry.node, entry.token, PX4_ERROR};
			// Drop the user mapping first. Even if the syscall fails, a stale
			// kernel notification cannot call an object whose lifetime ended.
			entry = {};
			const int ret = boardctl(ORBIOCDEVUNREGCALLBACK, reinterpret_cast<unsigned long>(&data));

			if (ret != 0 || data.ret != 0) {
				record_callback_error(ret != 0 ? errno : -data.ret);
			}

			return;
		}
	}
}

uint8_t uORB::Manager::orb_get_instance(const void *node_handle)
{
	orbiocdevgetinstance_t data = {node_handle, 0};
	boardctl(ORBIOCDEVGETINSTANCE, reinterpret_cast<unsigned long>(&data));

	return data.instance;
}

unsigned uORB::Manager::updates_available(const void *node_handle, unsigned last_generation)
{
	orbiocdevupdatesavail_t data = {node_handle, last_generation, 0};
	boardctl(ORBIOCDEVUPDATESAVAIL, reinterpret_cast<unsigned long>(&data));
	return data.ret;
}

bool uORB::Manager::is_advertised(const void *node_handle)
{
	orbiocdevisadvertised_t data = {node_handle, false};
	boardctl(ORBIOCDEVISADVERTISED, reinterpret_cast<unsigned long>(&data));
	return data.ret;
}
