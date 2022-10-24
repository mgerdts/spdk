/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/** \file
 * pthread lock wrappers
 */

#ifndef SPDK_MUTEX_H
#define SPDK_MUTEX_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize a mutex as an errorcheck mutex. Use spdk_mutex_lock() to automatically check locks.
 *
 * \param mutex The mutex to initialize.
 * \return 0 on success or error returned from  pthread_mutexattr_init() or pthread_mutex_init().
 */
static inline int
spdk_mutex_init(pthread_mutex_t *mutex)
{
	pthread_mutexattr_t attr;
	int rc;

	rc = pthread_mutexattr_init(&attr);
	if (rc != 0) {
		return rc;
	}
	rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
	if (rc != 0) {
		return rc;
	}
	return pthread_mutex_init(mutex, &attr);
}

/**
 * Acquire mutex with an error check. An error leads to abort(). If this is unacceptable, call
 * pthread_mutex_lock() and handle the error appropriately.
 *
 * \param mutex An errorcheck mutex.
 */
static inline void
spdk_mutex_lock(pthread_mutex_t *mutex)
{
	int rc;

	rc = pthread_mutex_lock(mutex);
	assert(rc == 0);
}

/**
 * Release mutex with an error check. An error leads to abort(). If this is unacceptable, call
 * pthread_mutex_lock() and handle the error appropriately.
 *
 * \param mutex An errorcheck mutex.
 */
static inline void
spdk_mutex_unlock(pthread_mutex_t *mutex)
{
	int rc;

	rc = pthread_mutex_unlock(mutex);
	assert(rc == 0);
}

/**
 * Determine if the current thread holds the mutex. The mutex must be an errorcheck mutex.
 *
 * \param mutex An errorcheck mutex.
 * \return true if mutex is held by this thread, else false
 */
static inline bool
spdk_mutex_held(pthread_mutex_t *mutex)
{
	int rc;

	rc = pthread_mutex_lock(mutex);
	if (rc == EDEADLK) {
		return true;
	}
	spdk_mutex_unlock(mutex);
	return false;
}

#ifdef __cplusplus
}
#endif

#endif
