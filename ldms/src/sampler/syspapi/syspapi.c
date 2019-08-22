/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2019 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of Sandia nor the names of any contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 *      Neither the name of Open Grid Computing nor the names of any
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *      Modified source versions must be plainly marked as such, and
 *      must not be misrepresented as being the original software.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * \file syspapi.c
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <unistd.h>
#include <sys/errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
#include <sys/fcntl.h>

#include "papi.h"
#include "perfmon/pfmlib_perf_event.h"

#include "ldms.h"
#include "ldmsd.h"
#include "../sampler_base.h"

#include "json/json_util.h"

#define SAMP "syspapi"

static int NCPU = 0;
static ldms_set_t set = NULL;
static ldmsd_msg_log_f msglog;
static int metric_offset;
static base_data_t base;
static const char *default_events = "PAPI_TOT_INS,PAPI_TOT_CYC,"
				    "PAPI_LD_INS,PAPI_SR_INS,PAPI_BR_INS,"
				    "PAPI_FP_OPS,"
				    "PAPI_L1_ICM,PAPI_L1_DCM,"
				    "PAPI_L2_ICA,PAPI_L2_TCA,PAPI_L2_TCM,"
				    "PAPI_L3_TCA,PAPI_L3_TCM";

typedef struct syspapi_metric_s {
	TAILQ_ENTRY(syspapi_metric_s) entry;
	int midx; /* metric index in the set */
	int init_rc; /* if 0, attr is good for perf_event_open() */
	struct perf_event_attr attr; /* perf attribute */
	char papi_name[256]; /* metric name in PAPI */
	char pfm_name[256]; /* metric name in perfmon (PAPI native) */
	int pfd[]; /* one perf fd per CPU */
} *syspapi_metric_t;

TAILQ_HEAD(syspapi_metric_list, syspapi_metric_s);
static struct syspapi_metric_list mlist = TAILQ_HEAD_INITIALIZER(mlist);

/*
 * Create metric set using global `mlist`. For `m` in `mlist`, `m->midx` is the
 * metric index in the set.
 */
static int
create_metric_set(base_data_t base)
{
	ldms_schema_t schema;
	int rc;
	syspapi_metric_t m;

	schema = base_schema_new(base);
	if (!schema) {
		msglog(LDMSD_LERROR,
		       "%s: The schema '%s' could not be created, errno=%d.\n",
		       __FILE__, base->schema_name, errno);
		rc = errno;
		goto err;
	}

	metric_offset = ldms_schema_metric_count_get(schema);
	TAILQ_FOREACH(m, &mlist, entry) {
		/* use PAPI metric name as metric name */
		rc = ldms_schema_metric_array_add(schema, m->papi_name,
						  LDMS_V_U64_ARRAY, NCPU);
		if (rc < 0) {
			rc = -rc; /* rc == -errno */
			goto err;
		}
		m->midx = rc;
	}

	set = base_set_new(base);
	if (!set) {
		rc = errno;
		goto err;
	}

	return 0;

 err:
	return rc;
}

static const char *
usage(struct ldmsd_plugin *self)
{
	return  "config name=" SAMP BASE_CONFIG_USAGE;
}

static int
syspapi_metric_init(syspapi_metric_t m, const char *papi_name)
{
	int len;
	int i, rc, papi_code;
	PAPI_event_info_t papi_info;
	const PAPI_component_info_t *comp_info;
	const char *pfm_name;

	len = snprintf(m->papi_name, sizeof(m->papi_name), "%s", papi_name);
	if (len >= sizeof(m->papi_name)) {
		ldmsd_lerror(SAMP": event name too long: %s\n", papi_name);
		return ENAMETOOLONG;
	}
	m->midx = -1;
	for (i = 0; i < NCPU; i++) {
		m->pfd[i] = -1;
	}

	/* get the pfm name */
	rc = PAPI_event_name_to_code(papi_name, &papi_code);
	if (rc != PAPI_OK) {
		ldmsd_lerror(SAMP": PAPI_event_name_to_code for %s failed, "
				 "error: %d\n", papi_name, rc);
		return -1;
	}
	rc = PAPI_get_event_info(papi_code, &papi_info);
	if (rc != PAPI_OK) {
		ldmsd_lerror(SAMP": PAPI_get_event_info for %s failed, "
				 "error: %d\n", papi_name, rc);
		return -1;
	}
	comp_info = PAPI_get_component_info(papi_info.component_index);
	if (strcmp("perf_event", comp_info->name)) {
		ldmsd_lerror(SAMP": event %s not supported, "
			"only events in perf_event are supported.\n",
			m->papi_name);
		return EINVAL;
	}
	if (comp_info->disabled) {
		ldmsd_lerror(SAMP": cannot initialize event %s, "
			"PAPI component `perf_event` disabled, "
			"reason: %s\n",
			m->papi_name, comp_info->disabled_reason);
		return ENODATA;
	}
	if (IS_PRESET(papi_code)) {
		if (strcmp(papi_info.derived, "NOT_DERIVED")) {
			/* not NOT_DERIVED ==> this is a derived preset */
			ldmsd_lerror(SAMP": Unsupported PAPI derived "
					 "event: %s\n", m->papi_name);
			return ENOTSUP;
		}
		switch (papi_info.count) {
		case 0:
			/* unavailable */
			ldmsd_lerror(SAMP": no native event describing "
				"papi event %s\n", m->papi_name);
			return ENODATA;
		case 1:
			/* good */
			pfm_name = papi_info.name[0];
			break;
		default:
			/* unsupported */
			ldmsd_lerror(SAMP": %s not supported: the event "
				"contains multiple native events.\n",
				m->papi_name);
			return ENOTSUP;
		}
	} else if (IS_NATIVE(papi_code)) {
		pfm_name = papi_info.symbol;
	} else {
		/* invalid */
		ldmsd_lerror(SAMP": %s is neither a PAPI-preset event "
				"nor a native event.\n", m->papi_name);
		return EINVAL;
	}
	snprintf(m->pfm_name, sizeof(m->pfm_name), "%s", pfm_name);

	/* Now, get perf attr */
	bzero(&m->attr, sizeof(m->attr));
	m->attr.size = sizeof(m->attr);
	pfm_perf_encode_arg_t pfm_arg = { .attr = &m->attr,
					  .size = sizeof(pfm_arg) };
	/* populate perf attr using pfm */
	rc = pfm_get_os_event_encoding(pfm_name, PFM_PLM0|PFM_PLM3,
				       PFM_OS_PERF_EVENT, &pfm_arg);
	if (rc) {
		ldmsd_lerror(SAMP": pfm_get_os_event_encoding for %s failed, "
				 "error: %d\n", m->papi_name, rc);
	}
	return rc;
}

/* create and add metric (by name) into the mlist */
static int
syspapi_metric_add(const char *name, struct syspapi_metric_list *mlist)
{
	syspapi_metric_t m;
	m = calloc(1, sizeof(*m) + NCPU*sizeof(int));
	if (!m)
		return ENOMEM;
	m->init_rc = syspapi_metric_init(m, name);
	TAILQ_INSERT_TAIL(mlist, m, entry);
	return 0;
}

static int
populate_mlist(char *events, struct syspapi_metric_list *mlist)
{
	int rc;
	char *tkn, *ptr;
	tkn = strtok_r(events, ",", &ptr);
	while (tkn) {
		rc = syspapi_metric_add(tkn, mlist);
		if (rc)
			return rc;
		tkn = strtok_r(NULL, ",", &ptr);
	}
	return 0;
}

static void
syspapi_close(struct syspapi_metric_list *mlist)
{
	syspapi_metric_t m;
	int i;
	TAILQ_FOREACH(m, mlist, entry) {
		for (i = 0; i < NCPU; i++) {
			if (m->pfd[i] < 0)
				continue;
			close(m->pfd[i]);
			m->pfd[i] = -1;
		}
	}
}

/* Report errors with helpful info (from perf_event_open(2))*/
static void
syspapi_open_error(syspapi_metric_t m, int rc)
{
	switch (rc) {
	case EACCES:
	case EPERM:
		ldmsd_lerror(SAMP": perf_event_open() failed (Permission "
			"denied) for %s. Please make sure that ldmsd has "
			"CAP_SYS_ADMIN or /proc/sys/kernel/perf_event_paranoid "
			"is permissive (e.g. -1, see "
			"https://www.kernel.org/doc/Documentation/"
			"sysctl/kernel.txt for more info).\n", m->papi_name);
		break;
	case EBUSY:
		ldmsd_lerror(SAMP": perf_event_open() failed (EBUSY) for %s, "
			"another event already has exclusive access to the "
			"PMU.\n", m->papi_name);
		break;
	case EINVAL:
		ldmsd_lerror(SAMP": perf_event_open() failed (EINVAL) for %s, "
			"invalid event\n", m->papi_name);
		break;
	case EMFILE:
		ldmsd_lerror(SAMP": perf_event_open() failed (EMFILE) for %s, "
			"too many open file descriptors.\n", m->papi_name);
		break;
	case ENODEV:
	case ENOENT:
	case ENOSYS:
	case EOPNOTSUPP:
		ldmsd_lerror(SAMP": perf_event_open() failed (%d) for %s, "
			"event not supported.\n", rc, m->papi_name);
		break;
	case ENOSPC:
		ldmsd_lerror(SAMP": perf_event_open() failed (%d) for %s, "
			"too many events.\n", rc, m->papi_name);
		break;
	default:
		ldmsd_lerror(SAMP": perf_event_open() failed for %s, "
				 "errno: %d\n", m->papi_name, rc);
		break;
	}
}

/* perf_event_open for all metrics in mlist */
static int
syspapi_open(struct syspapi_metric_list *mlist)
{
	int i, rc = 0;
	syspapi_metric_t m;
	TAILQ_FOREACH(m, mlist, entry) {
		if (m->init_rc) /* don't open the failed metrics */
			continue;
		for (i = 0; i < NCPU; i++) {
			m->pfd[i] = perf_event_open(&m->attr, -1, i, -1, 0);
			if (m->pfd[i] < 0) {
				rc = errno;
				syspapi_open_error(m, rc);
				/* just print error and continue */
				if (rc == EMFILE) { /* except EMFILE */
					syspapi_close(mlist);
					return rc;
				}
			} else {
				ldmsd_log(LDMSD_LINFO, SAMP": %s "
					  "successfully added\n", m->papi_name);
			}
		}
	}
	return 0;
}

static int
handle_cfg_file(struct ldmsd_plugin *self, const char *cfg_file)
{
	int rc = 0, fd = -1;
	ssize_t off, rsz, sz;
	char *buff = NULL;
	json_parser_t parser = NULL;
	json_entity_t json = NULL;
	json_entity_t events;
	json_entity_t event;
	json_entity_t schema;

	fd = open(cfg_file, O_RDONLY);
	if (fd < 0) {
		rc = errno;
		msglog(LDMSD_LERROR, SAMP": open failed on %s, "
				"errno: %d\n", cfg_file, errno);
		goto out;
	}
	sz = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	buff = malloc(sz);
	if (!buff) {
		rc = ENOMEM;
		msglog(LDMSD_LERROR, SAMP": out of memory\n");
		goto out;
	}
	off = 0;
	while (off < sz) {
		rsz = read(fd, buff + off, sz - off);
		if (rsz < 0) {
			rc = errno;
			msglog(LDMSD_LERROR, SAMP": cfg_file read "
					"error: %d\n", rc);
			goto out;
		}
		if (rsz == 0) {
			rc = EIO;
			msglog(LDMSD_LERROR, SAMP": unexpected EOF.\n");
			goto out;
		}
		off += rsz;
	}

	parser = json_parser_new(0);
	if (!parser) {
		rc = ENOMEM;
		goto out;
	}

	rc = json_parse_buffer(parser, buff, sz, &json);
	if (rc) {
		msglog(LDMSD_LERROR, SAMP": `%s` JSON parse error.\n",
				cfg_file);
		goto out;
	}

	schema = json_attr_find(json, "schema");
	if (schema) {
		schema = json_attr_value(schema);
		if (json_entity_type(schema) != JSON_STRING_VALUE) {
			msglog(LDMSD_LERROR, SAMP": cfg_file error, `schema` "
				"attribute must be a string.\n");
			rc = EINVAL;
			goto out;
		}
		if (base->schema_name)
			free(base->schema_name);
		base->schema_name = strdup(json_value_str(schema)->str);
		if (!base->schema_name) {
			msglog(LDMSD_LERROR, SAMP": out of memory.\n");
			rc = ENOMEM;
			goto out;
		}
	}

	events = json_attr_find(json, "events");
	if (!events) {
		msglog(LDMSD_LERROR, SAMP": cfg_file parse error: `events` "
				"attribute not found.\n");
		rc = ENOENT;
		goto out;
	}
	events = json_attr_value(events);
	if (json_entity_type(events) != JSON_LIST_VALUE) {
		rc = EINVAL;
		msglog(LDMSD_LERROR, SAMP": cfg_file error: `events` must "
				"be a list of strings.\n");
		goto out;
	}

	event = json_item_first(events);
	while (event) {
		if (json_entity_type(event) != JSON_STRING_VALUE) {
			rc = EINVAL;
			msglog(LDMSD_LERROR, SAMP": cfg_file error: "
					"entries in `events` list must be "
					"strings.\n");
			goto out;
		}
		rc = syspapi_metric_add(json_value_str(event)->str, &mlist);
		if (rc)
			goto out;
		event = json_item_next(event);
	}

out:
	if (fd > -1)
		close(fd);
	if (buff)
		free(buff);
	if (parser)
		json_parser_free(parser);
	if (json)
		json_entity_free(json);
	return rc;
}

static int
config(struct ldmsd_plugin *self, struct attr_value_list *kwl,
       struct attr_value_list *avl)
{
	int rc;
	char *events;
	char *cfg_file;

	if (set) {
		msglog(LDMSD_LERROR, SAMP": Set already created.\n");
		return EINVAL;
	}

	cfg_file = av_value(avl, "cfg_file"); /* JSON config file */
	events = av_value(avl, "events");

	if (!events && !cfg_file) {
		msglog(LDMSD_LERROR, SAMP": `events` and `cfg_file` "
					 "not specified\n");
		return EINVAL;
	}

	base = base_config(avl, SAMP, SAMP, msglog);
	if (!base) {
		rc = errno;
		goto err;
	}

	if (cfg_file) {
		rc = handle_cfg_file(self, cfg_file);
		if (rc)
			goto err;
	}

	if (events) {
		rc = populate_mlist(events, &mlist);
		if (rc)
			goto err;
	}
	rc = syspapi_open(&mlist);
	if (rc) /* error has already been logged */
		goto err;

	rc = create_metric_set(base);
	if (rc) {
		msglog(LDMSD_LERROR, SAMP ": failed to create a metric set.\n");
		goto err;
	}
	return 0;
 err:
	if (base)
		base_del(base);
	return rc;
}

static ldms_set_t
get_set(struct ldmsd_sampler *self)
{
	return set;
}

static int
sample(struct ldmsd_sampler *self)
{
	uint64_t v;
	int i;
	syspapi_metric_t m;

	if (!set) {
		msglog(LDMSD_LDEBUG, SAMP ": plugin not initialized\n");
		return EINVAL;
	}

	base_sample_begin(base);

	TAILQ_FOREACH(m, &mlist, entry) {
		for (i = 0; i < NCPU; i++) {
			v = 0;
			if (m->pfd[i] >= 0)
				read(m->pfd[i], &v, sizeof(v));
			ldms_metric_array_set_u64(set, m->midx, i, v);
		}
	}

	base_sample_end(base);

	return 0;
}

static void
term(struct ldmsd_plugin *self)
{
	syspapi_metric_t m;
	if (base)
		base_del(base);
	if (set)
		ldms_set_delete(set);
	set = NULL;
	syspapi_close(&mlist);
	while ((m = TAILQ_FIRST(&mlist))) {
		TAILQ_REMOVE(&mlist, m, entry);
		free(m);
	}
}

static struct ldmsd_sampler syspapi_plugin = {
	.base = {
		.name = SAMP,
		.type = LDMSD_PLUGIN_SAMPLER,
		.term = term,
		.config = config,
		.usage = usage,
	},
	.get_set = get_set,
	.sample = sample,
};

struct ldmsd_plugin *get_plugin(ldmsd_msg_log_f pf)
{
	msglog = pf;
	PAPI_library_init(PAPI_VERSION);
	NCPU = sysconf(_SC_NPROCESSORS_CONF);
	return &syspapi_plugin.base;
}