/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk_cunit.h"

/*
 * The functions in this file are intended to ensure that only the expected log messages are
 * generated. The expected use is as follows:
 *
 *     #include "spdk_cu_log.c"
 *
 *     static void
 *     some_test(void)
 *     {
 *             int rc;
 *
 *             spdk_log_open(spdk_cu_logfunc);
 *
 *             rc = do_something_that_should_log_messages();
 *             CU_ASSERT(rc == 0);
 *
 *             SPDK_CU_LOG_EXPECT(SPDK_LOG_WARN, "foo.c", "foo_mumble", "%s: not allowed", NULL);
 *             spdk_cu_log_ignore_level(SPDK_LOG_INFO, true);
 *             spdk_cu_log_ignore_func("some_noisy_callback_invoked_many_times");
 *             CU_ASSERT(spdk_cu_log_remaining() == 0);
 *
 *             spdk_log_open(spdk_cu_log_stderr);
 *     }
 *
 * spdk_cu_log_expect() and SPDK_CU_LOG_EXPECT() accept wildcards. See the comment above
 * spdk_cu_log_expect().
 *
 * If calling these functions from cunit init or fini callbacks, keep in mind that CU_ASSERT() will
 * always abort in those functions. In those cases, avoid the CU_ASSERT() and handle failures by
 * returning non-zero from the init or fini function.
 */

#define SPDK_CU_LOG_EXPECT(level, file, func, format, msg) \
	CU_ASSERT(spdk_cu_log_expect(level, file, func, format, msg))

struct spdk_cu_log_entry {
	int level;
	const char *file;
	int line;
	const char *func;
	const char *format;
	const char *msg;
	TAILQ_ENTRY(spdk_cu_log_entry) link;
};

static const char *const spdk_cu_log_level_names[] = {
	[SPDK_LOG_ERROR]	= "ERROR",
	[SPDK_LOG_WARN]		= "WARNING",
	[SPDK_LOG_NOTICE]	= "NOTICE",
	[SPDK_LOG_INFO]		= "INFO",
	[SPDK_LOG_DEBUG]	= "DEBUG",
};

TAILQ_HEAD(, spdk_cu_log_entry) g_spdk_cu_log_entries =
	TAILQ_HEAD_INITIALIZER(g_spdk_cu_log_entries);

/*
 * Record all spdk_log() messages and do not print them.
 */
static void
spdk_cu_logfunc(int level, const char *file, const int line, const char *func,
		const char *format, va_list args)
{
	struct spdk_cu_log_entry *entry;

	assert(level >= 0 && level < (int) SPDK_COUNTOF(spdk_cu_log_level_names));
	assert(spdk_cu_log_level_names[level] != NULL);

	entry = calloc(1, sizeof(*entry));
	assert(entry != NULL);

	entry->level = level;
	entry->file = basename(file);
	entry->line = line;
	entry->func = func;
	entry->format = format;
	entry->msg = spdk_vsprintf_alloc(format, args);
	assert(entry->msg != NULL);

	TAILQ_INSERT_TAIL(&g_spdk_cu_log_entries, entry, link);
}

static void
spdk_cu_log_stderr(int level, const char *file, const int line, const char *func,
		   const char *format, va_list args)
{
	char buf[1024];

	assert(level >= 0 && level < (int) SPDK_COUNTOF(spdk_cu_log_level_names));
	assert(spdk_cu_log_level_names[level] != NULL);

	vsnprintf(buf, sizeof(buf), format, args);
	fprintf(stderr, "%s %s:%d:%s: %s", spdk_cu_log_level_names[level], file, line, func, buf);
}

/* Internal, tests probably don't call this directly. */
static void
spdk_cu_log_remove_entry(struct spdk_cu_log_entry *entry)
{
	TAILQ_REMOVE(&g_spdk_cu_log_entries, entry, link);
	free((void *)entry->msg);
	free(entry);
}

/*
 * Expect one log entry matching the criteria specified. If the expected entry is found, it is
 * removed from the list of log messages seen. Wildcards (-1 for level, NULL for others) can be
 * used.
 *
 * Returns true if a log entry was found.
 */
static bool
spdk_cu_log_expect(int level, const char *file, const char *func, const char *format,
		   const char *msg)
{
	struct spdk_cu_log_entry *entry;

	TAILQ_FOREACH(entry, &g_spdk_cu_log_entries, link) {
		if ((level != -1 && entry->level != level) ||
		    (file != NULL && strcmp(entry->file, file) != 0) ||
		    (func != NULL && strcmp(entry->func, func) != 0) ||
		    (format != NULL && strcmp(entry->format, format) != 0) ||
		    (msg != NULL && strcmp(entry->msg, msg) != 0)) {
			continue;
		}
		spdk_cu_log_remove_entry(entry);
		return true;
	}
	return false;
}

/*
 * Print out all the remaining log entries, freeing them along the way. Returns the number of log
 * entries that were present.
 */
static uint32_t
spdk_cu_log_remaining(void)
{
	struct spdk_cu_log_entry *entry, *tmp;
	int filelen = 0, funclen = 0;
	uint32_t ret = 0;

	TAILQ_FOREACH(entry, &g_spdk_cu_log_entries, link) {
		filelen = spdk_max(filelen, (int)strlen(entry->file));
		funclen = spdk_max(funclen, (int)strlen(entry->func));
		ret++;
	}
	if (ret == 0) {
		return 0;
	}
	printf("\n");
	TAILQ_FOREACH_SAFE(entry, &g_spdk_cu_log_entries, link, tmp) {
		printf("%-7s %*s:%-5d %-*s %s", spdk_cu_log_level_names[entry->level],
		       filelen, entry->file, entry->line, funclen, entry->func, entry->msg);
		spdk_cu_log_remove_entry(entry);
	}
	return ret;
}

/*
 * Not all programs that include this file will use the remaining functions.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"

/*
 * Forget that log messages at this level (and optionally, above this level) were not logged.
 * That is, ignore INFO with:
 *
 *    spdk_cu_log_ignore_level(SPDK_LOG_INFO, false);
 *
 * Ignore INFO and DEBUG (DEBUG > INFO) with:
 *
 *    spdk_cu_log_ignore_level(SPDK_LOG_INFO, true);
 */
static void
spdk_cu_log_ignore_level(int level, bool and_above)
{
	struct spdk_cu_log_entry *entry, *tmp;

	TAILQ_FOREACH_SAFE(entry, &g_spdk_cu_log_entries, link, tmp) {
		if (entry->level < level) {
			continue;
		}
		if (entry->level > level && !and_above) {
			continue;
		}
		spdk_cu_log_remove_entry(entry);
	}
}

/*
 * Ignore all messages from this function.
 */
static void
spdk_cu_log_ignore_func(const char *func)
{
	struct spdk_cu_log_entry *entry, *tmp;

	TAILQ_FOREACH_SAFE(entry, &g_spdk_cu_log_entries, link, tmp) {
		if (strcmp(func, entry->func) == 0) {
			spdk_cu_log_remove_entry(entry);
		}
	}
}

#pragma GCC diagnostic pop
