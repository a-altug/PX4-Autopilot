/****************************************************************************
 *
 * Copyright (c) 2015 Vijay Venkatraman. All rights reserved.
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

/**
 * @file param.c
 *
 * Global parameter store.
 *
 * Note that it might make sense to convert this into a driver.  That would
 * offer some interesting options regarding state for e.g. ORB advertisements
 * and background parameter saving.
 */

#include "param.h"
#include <parameters/px4_parameters.hpp>
#include "tinybson/tinybson.h"

#include <crc32.h>
#include <float.h>
#include <math.h>

#include <px4_atomic_bitset.hpp>
#include <drivers/drv_hrt.h>
#include <lib/perf/perf_counter.h>
#include <px4_config.h>
#include <px4_defines.h>
#include <px4_posix.h>
#include <px4_sem.h>
#include <px4_shutdown.h>
#include <systemlib/uthash/utarray.h>

using namespace time_literals;

//#define PARAM_NO_ORB ///< if defined, avoid uorb dependency. This disables publication of parameter_update on param change
//#define PARAM_NO_AUTOSAVE ///< if defined, do not autosave (avoids LP work queue dependency)

#if !defined(PARAM_NO_ORB)
# include "uORB/uORB.h"
# include "uORB/topics/parameter_update.h"
#endif

#if defined(FLASH_BASED_PARAMS)
#include "flashparams/flashparams.h"
static const char *param_default_file = nullptr; // nullptr means to store to FLASH
#else
inline static int flash_param_save(bool only_unsaved) { return -1; }
inline static int flash_param_load() { return -1; }
inline static int flash_param_import() { return -1; }
//static const char *param_default_file = PX4_ROOTFSDIR"/eeprom/parameters";
#endif

#include <sys/stat.h>

#include "shmem.h"

#ifdef __PX4_QURT
static const char *param_default_file = "/dev/fs/params";
#else
static const char *param_default_file = "/usr/share/data/adsp/params";
#endif
static char *param_user_file = nullptr;

#ifdef __PX4_QURT
//Mode not supported by qurt
#define PARAM_OPEN(a, b, ...)	open(a, b)
#else
#define PARAM_OPEN	open
#endif
#define PARAM_CLOSE	close

#ifndef PARAM_NO_AUTOSAVE
#include <px4_workqueue.h>
/* autosaving variables */
static hrt_abstime last_autosave_timestamp = 0;
static struct work_s autosave_work {};
static px4::atomic<bool> autosave_scheduled{false};
static bool autosave_disabled = false;
#endif /* PARAM_NO_AUTOSAVE */

static constexpr uint16_t param_info_count = sizeof(px4::parameters) / sizeof(param_info_s);

// Storage for modified parameters.
struct param_wbuf_s {
	union param_value_u	val;
	param_t			param;
	bool			unsaved;
};

static px4::AtomicBitset<param_info_count> params_active;

//#define ENABLE_SHMEM_DEBUG
static void init_params();

static unsigned char set_called_from_get = 0;

static int param_import_done =
	0; /*at startup, params are loaded from file, if present. we dont want to send notifications that time since muorb is not ready*/

static int param_load_default_no_notify();

/** flexible array holding modified parameter values */
UT_array *param_values{nullptr};

/** array info for the modified parameters array */
const UT_icd param_icd = {sizeof(param_wbuf_s), nullptr, nullptr, nullptr};

#if !defined(PARAM_NO_ORB)
/** parameter update topic handle */
static orb_advert_t param_topic = nullptr;
static unsigned int param_instance = 0;
#endif

static param_t param_find_internal(const char *name, bool notification);
int param_set_internal(param_t param, const void *val, bool mark_saved, bool notify_changes);
const void *param_get_value_ptr(param_t param);

// TODO: not working on Snappy just yet
// the following implements an RW-lock using 2 semaphores (used as mutexes). It gives
// priority to readers, meaning a writer could suffer from starvation, but in our use-case
// we only have short periods of reads and writes are rare.
//static px4_sem_t param_sem; ///< this protects against concurrent access to param_values
//static int reader_lock_holders = 0;
//static px4_sem_t reader_lock_holders_lock; ///< this protects against concurrent access to reader_lock_holders

static perf_counter_t param_export_perf;
static perf_counter_t param_find_perf;
static perf_counter_t param_get_perf;
static perf_counter_t param_set_perf;

//static px4_sem_t param_sem_save; ///< this protects against concurrent param saves (file or flash access).
///< we use a separate lock to allow concurrent param reads and saves.
///< a param_set could still be blocked by a param save, because it
///< needs to take the reader lock

/** lock the parameter store for read access */
static void
param_lock_reader()
{
	// TODO: this doesn't seem to work on Snappy
#if 0
	do {} while (px4_sem_wait(&reader_lock_holders_lock) != 0);

	++reader_lock_holders;

	if (reader_lock_holders == 1) {
		// the first reader takes the lock, the next ones are allowed to just continue
		do {} while (px4_sem_wait(&param_sem) != 0);
	}

	px4_sem_post(&reader_lock_holders_lock);
#endif
}

/** lock the parameter store for write access */
static void
param_lock_writer()
{
	// TODO: this doesn't seem to work on Snappy
#if 0
	do {} while (px4_sem_wait(&param_sem) != 0);

#endif
}

/** unlock the parameter store */
static void
param_unlock_reader()
{
	// TODO: this doesn't seem to work on Snappy
#if 0
	do {} while (px4_sem_wait(&reader_lock_holders_lock) != 0);

	--reader_lock_holders;

	if (reader_lock_holders == 0) {
		// the last reader releases the lock
		px4_sem_post(&param_sem);
	}

	px4_sem_post(&reader_lock_holders_lock);
#endif
}

/** unlock the parameter store */
static void
param_unlock_writer()
{
	// TODO: this doesn't seem to work on Snappy
#if 0
	px4_sem_post(&param_sem);
#endif
}

void
param_init()
{
	// TODO: not needed on Snappy yet.
	//px4_sem_init(&param_sem, 0, 1);
	//px4_sem_init(&param_sem_save, 0, 1);
	//px4_sem_init(&reader_lock_holders_lock, 0, 1);

	param_export_perf = perf_alloc(PC_ELAPSED, "param_export");
	param_find_perf = perf_alloc(PC_ELAPSED, "param_find");
	param_get_perf = perf_alloc(PC_ELAPSED, "param_get");
	param_set_perf = perf_alloc(PC_ELAPSED, "param_set");

#ifdef CONFIG_SHMEM
	PX4_DEBUG("Syncing params to shared memory\n");
	init_params();
#endif

	// mark all parameters active
	for (int i = 0; i < params_active.size(); i++) {
		params_active.set(i, true);
	}
}

/**
 * Test whether a param_t is value.
 *
 * @param param			The parameter handle to test.
 * @return			True if the handle is valid.
 */
bool
handle_in_range(param_t param)
{
	return (param < param_info_count);
}

/**
 * Compare two modified parameter structures to determine ordering.
 *
 * This function is suitable for passing to qsort or bsearch.
 */
static int
param_compare_values(const void *a, const void *b)
{
	struct param_wbuf_s *pa = (struct param_wbuf_s *)a;
	struct param_wbuf_s *pb = (struct param_wbuf_s *)b;

	if (pa->param < pb->param) {
		return -1;
	}

	if (pa->param > pb->param) {
		return 1;
	}

	return 0;
}

/**
 * Locate the modified parameter structure for a parameter, if it exists.
 *
 * @param param			The parameter being searched.
 * @return			The structure holding the modified value, or
 *				nullptr if the parameter has not been modified.
 */
param_wbuf_s *
param_find_changed(param_t param)
{
	param_wbuf_s *s = nullptr;

	if (params_active[param]) {

		if (param_values != nullptr) {
			param_wbuf_s key{};
			key.param = param;
			s = (param_wbuf_s *)utarray_find(param_values, &key, param_compare_values);
		}
	}

	return s;
}

static void
_param_notify_changes()
{
#if !defined(PARAM_NO_ORB)
	parameter_update_s pup = {};
	pup.timestamp = hrt_absolute_time();
	pup.instance = param_instance++;

	/*
	 * If we don't have a handle to our topic, create one now; otherwise
	 * just publish.
	 */
	if (param_topic == nullptr) {
		param_topic = orb_advertise(ORB_ID(parameter_update), &pup);

	} else {
		orb_publish(ORB_ID(parameter_update), param_topic, &pup);
	}

#endif
}

void
param_notify_changes()
{
	_param_notify_changes();
}

param_t
param_find_internal(const char *name, bool notification)
{
	perf_begin(param_find_perf);

	param_t middle;
	param_t front = 0;
	param_t last = param_info_count;

	/* perform a binary search of the known parameters */

	while (front <= last) {
		middle = front + (last - front) / 2;
		int ret = strcmp(name, param_name(middle));

		if (ret == 0) {
			if (notification) {
				param_set_used(middle);
			}

			perf_end(param_find_perf);
			return middle;

		} else if (middle == front) {
			/* An end point has been hit, but there has been no match */
			break;

		} else if (ret < 0) {
			last = middle;

		} else {
			front = middle;
		}
	}

	perf_end(param_find_perf);

	/* not found */
	return PARAM_INVALID;
}

param_t
param_find(const char *name)
{
	return param_find_internal(name, true);
}

param_t
param_find_no_notification(const char *name)
{
	return param_find_internal(name, false);
}

unsigned
param_count()
{
	return param_info_count;
}

unsigned
param_count_used()
{
	return params_active.count();
}

param_t
param_for_index(unsigned index)
{
	if (index < param_info_count) {
		return (param_t)index;
	}

	return PARAM_INVALID;
}

param_t
param_for_used_index(unsigned index)
{
	if (index < param_info_count) {
		// walk all params and count used params
		unsigned used_count = 0;

		for (int i = 0; i < params_active.size(); i++) {

			if (params_active[i]) {
				// we found the right used count, return the param_t value
				if (index == used_count) {
					return (param_t)i;
				}

				used_count++;
			}
		}
	}

	return PARAM_INVALID;
}

int
param_get_index(param_t param)
{
	if (handle_in_range(param)) {
		return (unsigned)param;
	}

	return -1;
}

int
param_get_used_index(param_t param)
{
	/* this tests for out of bounds and does a constant time lookup */
	if (!param_used(param)) {
		return -1;
	}

	/* walk all params and count, now knowing that it has a valid index */
	int used_count = 0;

	for (int i = 0; i < params_active.size(); i++) {

		if (params_active[i]) {
			if (param == i) {
				// found the right parameter,
				//  return the index within the used set (count)
				return used_count;
			}

			used_count++;
		}
	}

	return -1;
}

const char *
param_name(param_t param)
{
	return handle_in_range(param) ? px4::parameters[param].name : nullptr;
}

param_type_t
param_type(param_t param)
{
	return handle_in_range(param) ? px4::parameters_type[param] : PARAM_TYPE_UNKNOWN;
}

bool
param_is_volatile(param_t param)
{
	if (handle_in_range(param)) {

		for (const auto &p : px4::parameters_volatile) {
			if (static_cast<px4::params>(param) == p) {
				return true;
			}
		}
	}

	return false;
}

bool
param_value_is_default(param_t param)
{
	struct param_wbuf_s *s;
	param_lock_reader();
	s = param_find_changed(param);
	param_unlock_reader();
	return s ? false : true;
}

bool
param_value_unsaved(param_t param)
{
	struct param_wbuf_s *s;
	param_lock_reader();
	s = param_find_changed(param);
	bool ret = s && s->unsaved;
	param_unlock_reader();
	return ret;
}

size_t
param_size(param_t param)
{
	if (handle_in_range(param)) {

		switch (param_type(param)) {

		case PARAM_TYPE_INT32:
		case PARAM_TYPE_FLOAT:
			return 4;

		case PARAM_TYPE_STRUCT ... PARAM_TYPE_STRUCT_MAX:
			/* decode structure size from type value */
			return param_type(param) - PARAM_TYPE_STRUCT;

		default:
			return 0;
		}
	}

	return 0;
}

/**
 * Obtain a pointer to the storage allocated for a parameter.
 *
 * @param param			The parameter whose storage is sought.
 * @return			A pointer to the parameter value, or nullptr
 *				if the parameter does not exist.
 */
const void *
param_get_value_ptr(param_t param)
{
	const void *result = nullptr;

	if (handle_in_range(param)) {

		const union param_value_u *v;

		/* work out whether we're fetching the default or a written value */
		struct param_wbuf_s *s = param_find_changed(param);

		if (s != nullptr) {
			v = &s->val;

		} else {
			v = &px4::parameters[param].val;
		}

		if (param_type(param) >= PARAM_TYPE_STRUCT &&
		    param_type(param) <= PARAM_TYPE_STRUCT_MAX) {

			result = v->p;

		} else {
			result = v;
		}
	}

	return result;
}

int
param_get(param_t param, void *val)
{
	int result = -1;

	param_lock_reader();
	perf_begin(param_get_perf);

	if (!handle_in_range(param)) {
		return result;
	}

	union param_value_u value;

	if (update_from_shmem(param, &value)) {
		set_called_from_get = 1;
		param_set_internal(param, &value, true, false);
		set_called_from_get = 0;
	}

	const void *v = param_get_value_ptr(param);

	if (val && v) {
		memcpy(val, v, param_size(param));
		result = 0;
	}

#ifdef ENABLE_SHMEM_DEBUG

	if (param_type(param) == PARAM_TYPE_INT32) {
		PX4_INFO("param_get for %s : %d", param_name(param), ((union param_value_u *)val)->i);
	}

	else if (param_type(param) == PARAM_TYPE_FLOAT) {
		PX4_INFO("param_get for %s : %f", param_name(param), (double)((union param_value_u *)val)->f);
	}

	else {
		PX4_INFO("Unknown param type for %s", param_name(param));
	}

#endif

	perf_end(param_get_perf);
	param_unlock_reader();

	return result;
}

#ifndef PARAM_NO_AUTOSAVE
/**
 * worker callback method to save the parameters
 * @param arg unused
 */
static void
autosave_worker(void *arg)
{
	bool disabled = false;

	param_lock_writer();
	last_autosave_timestamp = hrt_absolute_time();
	autosave_scheduled.store(false);
	disabled = autosave_disabled;
	param_unlock_writer();

	if (disabled) {
		return;
	}

	PX4_DEBUG("Autosaving params");
	int ret = param_save_default();

	if (ret != 0) {
		PX4_ERR("param auto save failed (%i)", ret);
	}
}
#endif /* PARAM_NO_AUTOSAVE */

/**
 * Automatically save the parameters after a timeout and limited rate.
 *
 * This needs to be called with the writer lock held (it's not necessary that it's the writer lock, but it
 * needs to be the same lock as autosave_worker() and param_control_autosave() use).
 */
static void
param_autosave()
{
#ifndef PARAM_NO_AUTOSAVE

	if (autosave_scheduled.load() || autosave_disabled) {
		return;
	}

	// wait at least 300ms before saving, because:
	// - tasks often call param_set() for multiple params, so this avoids unnecessary save calls
	// - the logger stores changed params. He gets notified on a param change via uORB and then
	//   looks at all unsaved params.
	hrt_abstime delay = 300_ms;

	static constexpr hrt_abstime rate_limit = 2_s; // rate-limit saving to 2 seconds
	const hrt_abstime last_save_elapsed = hrt_elapsed_time(&last_autosave_timestamp);

	if (last_save_elapsed < rate_limit && rate_limit > last_save_elapsed + delay) {
		delay = rate_limit - last_save_elapsed;
	}

	autosave_scheduled.store(true);
	work_queue(LPWORK, &autosave_work, (worker_t)&autosave_worker, nullptr, USEC2TICK(delay));
#endif /* PARAM_NO_AUTOSAVE */
}

void
param_control_autosave(bool enable)
{
#ifndef PARAM_NO_AUTOSAVE
	param_lock_writer();

	if (!enable && autosave_scheduled.load()) {
		work_cancel(LPWORK, &autosave_work);
		autosave_scheduled.store(false);
	}

	autosave_disabled = !enable;
	param_unlock_writer();
#endif /* PARAM_NO_AUTOSAVE */
}

int
param_set_internal(param_t param, const void *val, bool mark_saved, bool notify_changes)
{
	int result = -1;
	bool params_changed = false;

	param_lock_writer();
	perf_begin(param_set_perf);

	// create the parameter store if it doesn't exist
	if (param_values == nullptr) {
		utarray_new(param_values, &param_icd);

		// mark all parameters inactive
		for (int i = 0; i < params_active.size(); i++) {
			params_active.set(i, false);
		}
	}

	if (param_values == nullptr) {
		PX4_ERR("failed to allocate modified values array");
		goto out;
	}

	if (handle_in_range(param)) {

		param_wbuf_s *s = param_find_changed(param);

		if (s == nullptr) {

			/* construct a new parameter */
			param_wbuf_s buf{};
			buf.param = param;

			params_changed = true;

			/* add it to the array and sort */
			utarray_push_back(param_values, &buf);
			utarray_sort(param_values, param_compare_values);

			/* find it after sorting */
			param_set_used(param);
			s = param_find_changed(param);
		}

		/* update the changed value */
		switch (param_type(param)) {

		case PARAM_TYPE_INT32:
			params_changed = params_changed || s->val.i != *(int32_t *)val;
			s->val.i = *(int32_t *)val;
			break;

		case PARAM_TYPE_FLOAT:
			params_changed = params_changed || fabsf(s->val.f - * (float *)val) > FLT_EPSILON;
			s->val.f = *(float *)val;
			break;

		case PARAM_TYPE_STRUCT ... PARAM_TYPE_STRUCT_MAX:
			if (s->val.p == nullptr) {
				size_t psize = param_size(param);

				if (psize > 0) {
					s->val.p = malloc(psize);

				} else {
					s->val.p = nullptr;
				}

				if (s->val.p == nullptr) {
					PX4_ERR("failed to allocate parameter storage");
					goto out;
				}
			}

			memcpy(s->val.p, val, param_size(param));
			params_changed = true;
			break;

		default:
			goto out;
		}

		s->unsaved = !mark_saved;
		result = 0;

		if (!mark_saved) { // this is false when importing parameters
			param_autosave();
		}
	}

out:
	perf_end(param_set_perf);
	param_unlock_writer();

	/*
	 * If we set something, now that we have unlocked, go ahead and advertise that
	 * a thing has been set.
	 */

	if (!param_import_done) {
		notify_changes = 0;
	}

	if (params_changed && notify_changes) {
		_param_notify_changes();
	}

	if (result == 0 && !set_called_from_get) {
		update_to_shmem(param, *(union param_value_u *)val);
	}

#ifdef ENABLE_SHMEM_DEBUG

	if (param_type(param) == PARAM_TYPE_INT32) {
		PX4_INFO("param_set for %s : %d", param_name(param), ((union param_value_u *)val)->i);
	}

	else if (param_type(param) == PARAM_TYPE_FLOAT) {
		PX4_INFO("param_set for %s : %f", param_name(param), (double)((union param_value_u *)val)->f);
	}

	else {
		PX4_INFO("Unknown param type for %s", param_name(param));
	}

#endif

	return result;
}

#if defined(FLASH_BASED_PARAMS)
int param_set_external(param_t param, const void *val, bool mark_saved, bool notify_changes)
{
	return param_set_internal(param, val, mark_saved, notify_changes);
}

const void *param_get_value_ptr_external(param_t param)
{
	return param_get_value_ptr(param);
}
#endif

int
param_set(param_t param, const void *val)
{
	return param_set_internal(param, val, false, true);
}

int
param_set_no_notification(param_t param, const void *val)
{
	return param_set_internal(param, val, false, false);
}

bool
param_used(param_t param)
{
	if (handle_in_range(param)) {
		return params_active[param];
	}

	return false;
}

void param_set_used(param_t param)
{
	if (handle_in_range(param)) {
		params_active.set(param, true);
	}
}

int
param_reset(param_t param)
{
	param_wbuf_s *s = nullptr;
	bool param_found = false;

	param_lock_writer();

	if (handle_in_range(param)) {

		/* look for a saved value */
		s = param_find_changed(param);

		/* if we found one, erase it */
		if (s != nullptr) {
			int pos = utarray_eltidx(param_values, s);
			utarray_erase(param_values, pos, 1);
		}

		param_found = true;
	}

	param_autosave();

	param_unlock_writer();

	if (s != nullptr) {
		_param_notify_changes();
	}

	return (!param_found);
}
static void
param_reset_all_internal(bool auto_save)
{
	param_lock_writer();

	if (param_values != nullptr) {
		utarray_free(param_values);
	}

	/* mark as reset / deleted */
	param_values = nullptr;

	if (auto_save) {
		param_autosave();
	}

	param_unlock_writer();

	_param_notify_changes();
}

void
param_reset_all()
{
	param_reset_all_internal(true);
}

void
param_reset_excludes(const char *excludes[], int num_excludes)
{
	for (param_t param = 0; handle_in_range(param); param++) {
		const char *name = param_name(param);
		bool exclude = false;

		for (int index = 0; index < num_excludes; index ++) {
			int len = strlen(excludes[index]);

			if ((excludes[index][len - 1] == '*'
			     && strncmp(name, excludes[index], len - 1) == 0)
			    || strcmp(name, excludes[index]) == 0) {

				exclude = true;
				break;
			}
		}

		if (!exclude) {
			param_reset(param);
		}
	}

	_param_notify_changes();
}

int
param_set_default_file(const char *filename)
{
#ifdef FLASH_BASED_PARAMS
	// the default for flash-based params is always the FLASH
	(void)filename;
#else

	if (param_user_file != nullptr) {
		// we assume this is not in use by some other thread
		free(param_user_file);
		param_user_file = nullptr;
	}

	if (filename) {
		param_user_file = strdup(filename);
	}

#endif /* FLASH_BASED_PARAMS */

	return 0;
}

const char *
param_get_default_file()
{
	return (param_user_file != nullptr) ? param_user_file : param_default_file;
}

int
param_save_default()
{
	int res = OK;
	int fd = -1;

	const char *filename = param_get_default_file();

	fd = PARAM_OPEN(filename, O_WRONLY | O_CREAT, PX4_O_MODE_666);

	if (fd < 0) {
		PX4_ERR("failed to open param file: %s", filename);
		goto do_exit;
	}

	res = param_export(fd, false);

	if (res != OK) {
		PX4_ERR("failed to write parameters to file: %s", filename);
		goto do_exit;
	}

	PARAM_CLOSE(fd);


	fd = -1;

do_exit:

	if (fd >= 0) {
		close(fd);
	}

	if (res == OK) {
		PX4_DEBUG("saving params completed successfully");
	}

	return res;
}

/**
 * @return 0 on success, 1 if all params have not yet been stored, -1 if device open failed, -2 if writing parameters failed
 */
int
param_load_default()
{
	int res = 0;
	const char *filename = param_get_default_file();

	if (!filename) {
		return flash_param_load();
	}

	int fd_load = PARAM_OPEN(filename, O_RDONLY);

	if (fd_load < 0) {
		/* no parameter file is OK, otherwise this is an error */
		if (errno != ENOENT) {
			PX4_ERR("open '%s' for reading failed", filename);
			return -1;
		}

		return 1;
	}

	int result = param_load(fd_load);
	PARAM_CLOSE(fd_load);

	if (result != 0) {
		PX4_ERR("error reading parameters from '%s'", filename);
		return -2;
	}

	return res;
}

/**
 * @return 0 on success, 1 if all params have not yet been stored, -1 if device open failed, -2 if writing parameters failed
 */
static int
param_load_default_no_notify()
{
	int fd_load = open(param_get_default_file(), O_RDONLY);

	if (fd_load < 0) {
#ifdef __PX4_QURT
		release_shmem_lock(__FILE__, __LINE__);
#endif

		/* no parameter file is OK, otherwise this is an error */
		if (errno != ENOENT) {
			PX4_DEBUG("open '%s' for reading failed", param_get_default_file());
			return -1;
		}

		return 1;
	}

	int result = param_import(fd_load);

	close(fd_load);

	PX4_DEBUG("param loading done");

	if (result != 0) {
		PX4_WARN("error reading parameters from '%s'", param_get_default_file());
		return -2;
	}

	return 0;
}

int
param_export(int fd, bool only_unsaved)
{
	int	result = -1;
	perf_begin(param_export_perf);

	if (fd < 0) {
		param_lock_writer();
		// flash_param_save() will take the shutdown lock
		result = flash_param_save(only_unsaved);
		param_unlock_writer();
		perf_end(param_export_perf);
		return result;
	}

	param_wbuf_s *s = nullptr;
	struct bson_encoder_s encoder;

	int shutdown_lock_ret = px4_shutdown_lock();

	if (shutdown_lock_ret) {
		PX4_ERR("px4_shutdown_lock() failed (%i)", shutdown_lock_ret);
	}

	// take the file lock
	//do {} while (px4_sem_wait(&param_sem_save) != 0);

	param_lock_reader();

	uint8_t bson_buffer[256];
	bson_encoder_init_buf_file(&encoder, fd, &bson_buffer, sizeof(bson_buffer));

	/* no modified parameters -> we are done */
	if (param_values == nullptr) {
		result = 0;
		goto out;
	}

#ifdef CONFIG_SHMEM
	/* First of all, update the index which will call param_get for params
	 * that have recently been changed. */
	update_index_from_shmem();
#endif /* CONFIG_SHMEM */

	while ((s = (struct param_wbuf_s *)utarray_next(param_values, s)) != nullptr) {
		/*
		 * If we are only saving values changed since last save, and this
		 * one hasn't, then skip it
		 */
		if (only_unsaved && !s->unsaved) {
			continue;
		}

		s->unsaved = false;

#ifdef CONFIG_SHMEM
		/* Make sure to get latest from shmem before saving. */
		update_from_shmem(s->param, &s->val);
#endif /* CONFIG_SHMEM */

		const char *name = param_name(s->param);
		const size_t size = param_size(s->param);

		/* append the appropriate BSON type object */
		switch (param_type(s->param)) {

		case PARAM_TYPE_INT32: {
				const int32_t i = s->val.i;

				PX4_DEBUG("exporting: %s (%d) size: %d val: %d", name, s->param, size, i);

				if (bson_encoder_append_int(&encoder, name, i)) {
					PX4_ERR("BSON append failed for '%s'", name);
					goto out;
				}
			}
			break;

		case PARAM_TYPE_FLOAT: {
				const double f = (double)s->val.f;

				PX4_DEBUG("exporting: %s (%d) size: %d val: %.3f", name, s->param, size, (double)f);

				if (bson_encoder_append_double(&encoder, name, f)) {
					PX4_ERR("BSON append failed for '%s'", name);
					goto out;
				}
			}
			break;

		case PARAM_TYPE_STRUCT ... PARAM_TYPE_STRUCT_MAX: {
				const void *value_ptr = param_get_value_ptr(s->param);

				/* lock as short as possible */
				if (bson_encoder_append_binary(&encoder,
							       name,
							       BSON_BIN_BINARY,
							       size,
							       value_ptr)) {

					PX4_ERR("BSON append failed for '%s'", name);
					goto out;
				}
			}
			break;

		default:
			PX4_ERR("unrecognized parameter type");
			goto out;
		}
	}

	result = 0;

out:

	if (result == 0) {
		if (bson_encoder_fini(&encoder) != PX4_OK) {
			PX4_ERR("bson encoder finish failed");
		}
	}

	param_unlock_reader();

	//px4_sem_post(&param_sem_save);

	if (shutdown_lock_ret == 0) {
		px4_shutdown_unlock();
	}

	perf_end(param_export_perf);

	return result;
}

struct param_import_state {
	bool mark_saved;
};

static int
param_import_callback(bson_decoder_t decoder, void *priv, bson_node_t node)
{
	float f = 0.0f;
	int32_t i = 0;
	void *tmp = nullptr;
	void *v = nullptr;
	int result = -1;
	param_import_state *state = (param_import_state *)priv;

	/*
	 * EOO means the end of the parameter object. (Currently not supporting
	 * nested BSON objects).
	 */
	if (node->type == BSON_EOO) {
		PX4_DEBUG("end of parameters");
		return 0;
	}

	/*
	 * Find the parameter this node represents.  If we don't know it,
	 * ignore the node.
	 */
	param_t param = param_find_no_notification(node->name);

	if (param == PARAM_INVALID) {
		PX4_ERR("ignoring unrecognised parameter '%s'", node->name);
		return 1;
	}

	/*
	 * Handle setting the parameter from the node
	 */

	switch (node->type) {
	case BSON_INT32: {
			if (param_type(param) != PARAM_TYPE_INT32) {
				PX4_WARN("unexpected type for %s", node->name);
				result = 1; // just skip this entry
				goto out;
			}

			i = node->i;
			v = &i;

			PX4_DEBUG("Imported %s with value %d", param_name(param), i);
		}
		break;

	case BSON_DOUBLE: {
			if (param_type(param) != PARAM_TYPE_FLOAT) {
				PX4_WARN("unexpected type for %s", node->name);
				result = 1; // just skip this entry
				goto out;
			}

			f = node->d;
			v = &f;

			PX4_DEBUG("Imported %s with value %f", param_name(param), (double)f);
		}
		break;

	case BSON_BINDATA: {
			if (node->subtype != BSON_BIN_BINARY) {
				PX4_WARN("unexpected subtype for %s", node->name);
				result = 1; // just skip this entry
				goto out;
			}

			if (bson_decoder_data_pending(decoder) != param_size(param)) {
				PX4_WARN("bad size for '%s'", node->name);
				result = 1; // just skip this entry
				goto out;
			}

			/* XXX check actual file data size? */
			size_t psize = param_size(param);

			if (psize > 0) {
				tmp = malloc(psize);

			} else {
				tmp = nullptr;
			}

			if (tmp == nullptr) {
				PX4_ERR("failed allocating for '%s'", node->name);
				goto out;
			}

			if (bson_decoder_copy_data(decoder, tmp)) {
				PX4_ERR("failed copying data for '%s'", node->name);
				goto out;
			}

			v = tmp;
		}
		break;

	default:
		PX4_DEBUG("unrecognised node type");
		goto out;
	}

	if (param_set_internal(param, v, state->mark_saved, false)) {
		PX4_DEBUG("error setting value for '%s'", node->name);
		goto out;
	}

	if (tmp != nullptr) {
		free(tmp);
		tmp = nullptr;
	}

	/* don't return zero, that means EOF */
	result = 1;

out:

	if (tmp != nullptr) {
		free(tmp);
	}

	return result;
}

static int
param_import_internal(int fd, bool mark_saved)
{
	bson_decoder_s decoder;
	param_import_state state;
	int result = -1;

	if (bson_decoder_init_file(&decoder, fd, param_import_callback, &state)) {
		PX4_ERR("decoder init failed");
		return PX4_ERROR;
	}

	state.mark_saved = mark_saved;

	do {
		result = bson_decoder_next(&decoder);

	} while (result > 0);

	param_notify_changes();

	return result;
}

int
param_import(int fd)
{
	if (fd < 0) {
		return flash_param_import();
	}

	return param_import_internal(fd, false);
}

int
param_load(int fd)
{
	if (fd < 0) {
		return flash_param_load();
	}

	param_reset_all_internal(false);
	return param_import_internal(fd, true);
}

void
param_foreach(void (*func)(void *arg, param_t param), void *arg, bool only_changed, bool only_used)
{
	for (param_t param = 0; handle_in_range(param); param++) {

		/* if requested, skip unchanged values */
		if (only_changed && (param_find_changed(param) == nullptr)) {
			continue;
		}

		if (only_used && !param_used(param)) {
			continue;
		}

		func(arg, param);
	}
}

uint32_t param_hash_check()
{
	uint32_t param_hash = 0;

	param_lock_reader();

	/* compute the CRC32 over all string param names and 4 byte values */
	for (param_t param = 0; handle_in_range(param); param++) {
		if (!param_used(param) || param_is_volatile(param)) {
			continue;
		}

		const char *name = param_name(param);
		const void *val = param_get_value_ptr(param);
		param_hash = crc32part((const uint8_t *)name, strlen(name), param_hash);
		param_hash = crc32part((const uint8_t *)val, param_size(param), param_hash);
	}

	param_unlock_reader();

	return param_hash;
}

void param_print_status()
{
	PX4_INFO("summary: %d/%d (used/total)", param_count_used(), param_count());

#ifndef FLASH_BASED_PARAMS
	const char *filename = param_get_default_file();

	if (filename != nullptr) {
		PX4_INFO("file: %s", param_get_default_file());
	}

#endif /* FLASH_BASED_PARAMS */

	if (param_values != nullptr) {
		PX4_INFO("storage array: %d/%d elements (%zu bytes total)",
			 utarray_len(param_values), param_values->n, param_values->n * sizeof(UT_icd));
	}

#ifndef PARAM_NO_AUTOSAVE
	PX4_INFO("auto save: %s", autosave_disabled ? "off" : "on");

	if (!autosave_disabled && (last_autosave_timestamp > 0)) {
		PX4_INFO("last auto save: %.3f seconds ago", hrt_elapsed_time(&last_autosave_timestamp) * 1e-6);
	}

#endif /* PARAM_NO_AUTOSAVE */

	perf_print_counter(param_export_perf);
	perf_print_counter(param_find_perf);
	perf_print_counter(param_get_perf);
	perf_print_counter(param_set_perf);
}

void init_params()
{
#ifdef __PX4_QURT
	//copy params to shared memory
	init_shared_memory();
#endif

	/*load params automatically*/
#ifdef __PX4_POSIX
	param_load_default_no_notify();
#endif

	param_import_done = 1;

#ifdef __PX4_QURT
	copy_params_to_shmem(px4::parameters);

#ifdef ENABLE_SHMEM_DEBUG
	PX4_INFO("Offsets:");
	PX4_INFO("params_val %lu, krait_changed %lu, adsp_changed %lu",
		 (unsigned char *)shmem_info_p->params_val - (unsigned char *)shmem_info_p,
		 (unsigned char *)&shmem_info_p->krait_changed_index - (unsigned char *)shmem_info_p,
		 (unsigned char *)&shmem_info_p->adsp_changed_index - (unsigned char *)shmem_info_p);
#endif /* ENABLE_SHMEM_DEBUG */

#endif /* __PX4_QURT */
}
