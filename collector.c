/*
 * Copyright 2013-2016 Fabian Groffen
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>

#include "relay.h"
#include "dispatcher.h"
#include "server.h"
#include "aggregator.h"
#include "collector.h"

static dispatcher **dispatchers;
static char debug = 0;
static pthread_t collectorid;
static char keep_running = 1;
int collector_interval = 60;
static char cluster_refresh_pending = 0;
static cluster *pending_clusters = NULL;
static aggregator *pending_aggrs = NULL;

/**
 * Collects metrics from dispatchers and servers and emits them.
 */
static void *
collector_runner(void *s)
{
	int i;
	size_t totticks;
	size_t totmetrics;
	size_t totblackholes;
	size_t totqueued;
	size_t totstalls;
	size_t totdropped;
	size_t ticks;
	size_t metrics;
	size_t blackholes;
	size_t queued;
	size_t stalls;
	size_t dropped;
	size_t dispatchers_idle;
	size_t dispatchers_busy;
	time_t now;
	time_t nextcycle;
	char ipbuf[32];
	char *p;
	size_t numaggregators = 0;
	aggregator *aggrs = NULL;
	server *submission = (server *)s;
	server **srvs = NULL;
	char metric[METRIC_BUFSIZ];
	char *m;
	size_t sizem = 0;
	size_t (*s_ticks)(server *);
	size_t (*s_metrics)(server *);
	size_t (*s_stalls)(server *);
	size_t (*s_dropped)(server *);
	size_t (*d_ticks)(dispatcher *);
	size_t (*d_metrics)(dispatcher *);
	size_t (*d_blackholes)(dispatcher *);
	size_t (*a_received)(aggregator *);
	size_t (*a_sent)(aggregator *);
	size_t (*a_dropped)(aggregator *);

	/* prepare hostname for graphite metrics */
	snprintf(metric, sizeof(metric), "carbon.relays.%s", relay_hostname);
	for (m = metric + strlen("carbon.relays."); *m != '\0'; m++)
		if (*m == '.')
			*m = '_';
	*m++ = '.';
	*m = '\0';
	sizem = sizeof(metric) - (m - metric);

	/* setup functions to target what the user wants */
	if (debug & 2) {
		s_ticks = server_get_ticks_sub;
		s_metrics = server_get_metrics_sub;
		s_stalls = server_get_stalls_sub;
		s_dropped = server_get_dropped_sub;
		d_ticks = dispatch_get_ticks_sub;
		d_metrics = dispatch_get_metrics_sub;
		d_blackholes = dispatch_get_blackholes_sub;
		a_received = aggregator_get_received_sub;
		a_sent = aggregator_get_sent_sub;
		a_dropped = aggregator_get_dropped_sub;
	} else {
		s_ticks = server_get_ticks;
		s_metrics = server_get_metrics;
		s_stalls = server_get_stalls;
		s_dropped = server_get_dropped;
		d_ticks = dispatch_get_ticks;
		d_metrics = dispatch_get_metrics;
		d_blackholes = dispatch_get_blackholes;
		a_received = aggregator_get_received;
		a_sent = aggregator_get_sent;
		a_dropped = aggregator_get_dropped;
	}

#define send(metric) \
	if (debug & 1) \
		logout("%s", metric); \
	else \
		server_send(submission, strdup(metric), 1);

	nextcycle = time(NULL) + collector_interval;
	while (keep_running) {
		if (cluster_refresh_pending) {
			server **newservers = router_getservers(pending_clusters);
			if (srvs != NULL)
				free(srvs);
			srvs = newservers;
			aggrs = pending_aggrs;
			numaggregators = aggregator_numaggregators(aggrs);
			cluster_refresh_pending = 0;
		}
		assert(srvs != NULL);
		sleep(1);
		now = time(NULL);
		if (nextcycle > now)
			continue;
		nextcycle += collector_interval;
		totticks = 0;
		totmetrics = 0;
		totblackholes = 0;
		dispatchers_idle = 0;
		dispatchers_busy = 0;
		for (i = 0; dispatchers[i] != NULL; i++) {
			if (dispatch_busy(dispatchers[i])) {
				dispatchers_busy++;
			} else {
				dispatchers_idle++;
			}
			totticks += ticks = d_ticks(dispatchers[i]);
			totmetrics += metrics = d_metrics(dispatchers[i]);
			totblackholes += blackholes = d_blackholes(dispatchers[i]);
			snprintf(m, sizem, "dispatcher%d.metricsReceived %zu %zu\n",
					i + 1, metrics, (size_t)now);
			send(metric);
			snprintf(m, sizem, "dispatcher%d.metricsBlackholed %zu %zu\n",
					i + 1, blackholes, (size_t)now);
			send(metric);
			snprintf(m, sizem, "dispatcher%d.wallTime_us %zu %zu\n",
					i + 1, ticks, (size_t)now);
			send(metric);
		}
		snprintf(m, sizem, "metricsReceived %zu %zu\n",
				totmetrics, (size_t)now);
		send(metric);
		snprintf(m, sizem, "metricsBlackholed %zu %zu\n",
				totblackholes, (size_t)now);
		send(metric);
		snprintf(m, sizem, "dispatch_wallTime_us %zu %zu\n",
				totticks, (size_t)now);
		send(metric);
		snprintf(m, sizem, "dispatch_busy %zu %zu\n",
				dispatchers_busy, (size_t)now);
		send(metric);
		snprintf(m, sizem, "dispatch_idle %zu %zu\n",
				dispatchers_idle, (size_t)now);
		send(metric);

#define send_server_metrics(ipbuf, ticks, metrics, queued, stalls, dropped) \
			snprintf(m, sizem, "destinations.%s.sent %zu %zu\n", \
					ipbuf, metrics, (size_t)now); \
			send(metric); \
			snprintf(m, sizem, "destinations.%s.queued %zu %zu\n", \
					ipbuf, queued, (size_t)now); \
			send(metric); \
			snprintf(m, sizem, "destinations.%s.stalls %zu %zu\n", \
					ipbuf, stalls, (size_t)now); \
			send(metric); \
			snprintf(m, sizem, "destinations.%s.dropped %zu %zu\n", \
					ipbuf, dropped, (size_t)now); \
			send(metric); \
			snprintf(m, sizem, "destinations.%s.wallTime_us %zu %zu\n", \
					ipbuf, ticks, (size_t)now); \
			send(metric);

		totticks = 0;
		totmetrics = 0;
		totqueued = 0;
		totstalls = 0;
		totdropped = 0;

		/* exclude internal_submission metrics from the totals to avoid
		 * artificial doubles due to internal routing details */
		strncpy(ipbuf, "internal", sizeof(ipbuf));
		ticks = s_ticks(submission);
		metrics = s_metrics(submission);
		queued = server_get_queue_len(submission);
		stalls = s_stalls(submission);
		dropped = s_dropped(submission);
		send_server_metrics(ipbuf, ticks, metrics, queued, stalls, dropped);

		for (i = 0; srvs[i] != NULL; i++) {
			if (server_ctype(srvs[i]) == CON_FILE) {
				strncpy(ipbuf, server_ip(srvs[i]), sizeof(ipbuf));
			} else {
				snprintf(ipbuf, sizeof(ipbuf), "%s:%u",
						server_ip(srvs[i]), server_port(srvs[i]));
			}
			for (p = ipbuf; *p != '\0'; p++)
				if (*p == '.')
					*p = '_';

			totticks += ticks = s_ticks(srvs[i]);
			totmetrics += metrics = s_metrics(srvs[i]);
			totqueued += queued = server_get_queue_len(srvs[i]);
			totstalls += stalls = s_stalls(srvs[i]);
			totdropped += dropped = s_dropped(srvs[i]);
			send_server_metrics(ipbuf, ticks, metrics, queued, stalls, dropped);
		}

		snprintf(m, sizem, "metricsSent %zu %zu\n",
				totmetrics, (size_t)now);
		send(metric);
		snprintf(m, sizem, "metricsQueued %zu %zu\n",
				totqueued, (size_t)now);
		send(metric);
		snprintf(m, sizem, "metricStalls %zu %zu\n",
				totstalls, (size_t)now);
		send(metric);
		snprintf(m, sizem, "metricsDropped %zu %zu\n",
				totdropped, (size_t)now);
		send(metric);
		snprintf(m, sizem, "server_wallTime_us %zu %zu\n",
				totticks, (size_t)now);
		send(metric);
		snprintf(m, sizem, "connections %zu %zu\n",
				dispatch_get_accepted_connections(), (size_t)now);
		send(metric);
		snprintf(m, sizem, "disconnects %zu %zu\n",
				dispatch_get_closed_connections(), (size_t)now);
		send(metric);

		if (numaggregators > 0) {
			snprintf(m, sizem, "aggregators.metricsReceived %zu %zu\n",
					a_received(aggrs), (size_t)now);
			send(metric);
			snprintf(m, sizem, "aggregators.metricsSent %zu %zu\n",
					a_sent(aggrs), (size_t)now);
			send(metric);
			snprintf(m, sizem, "aggregators.metricsDropped %zu %zu\n",
					a_dropped(aggrs), (size_t)now);
			send(metric);
		}

		if (debug & 1)
			fflush(stdout);
	}

	if (srvs != NULL)
		free(srvs);

	return NULL;
}

/**
 * Writes messages about dropped events or high queue sizes.
 */
static size_t lastdropped = 0;
static size_t lastaggrdropped = 0;
static void *
collector_writer(void *unused)
{
	int i = 0;
	size_t queued;
	size_t queuesize;
	double queueusage;
	size_t totdropped;
	size_t lastconn = 0;
	size_t lastdisc = 0;
	size_t numaggregators = 0;
	server **srvs = NULL;
	aggregator *aggrs = NULL;

	while (keep_running) {
		if (cluster_refresh_pending) {
			server **newservers = router_getservers(pending_clusters);
			if (srvs != NULL)
				free(srvs);
			srvs = newservers;
			aggrs = pending_aggrs;
			numaggregators = aggregator_numaggregators(aggrs);
			cluster_refresh_pending = 0;
		}
		assert(srvs != NULL);
		sleep(1);
		if (debug & 1) {
			size_t mpsout;
			size_t totout;
			size_t mpsdrop;
			size_t totdrop;
			size_t totqueue;
			size_t mpsin;
			size_t totin;
			size_t totconn;
			size_t totdisc;
			short widle;
			short wbusy;
			int j;
			/* Solaris iostat like output:
 metrics in     metrics out    metrics drop  queue    conns     disconn   workr
  mps     tot    mps     tot    dps     tot    cur  cps   tot  dps   tot  id bs
99999 9999999  99999 9999999  99999 9999999  99999  999 99999  999 99999  99 99
			 */
			if (i % 24 == 0)
				printf(" metrics in     metrics out    metrics drop  queue    conns     disconn   workr\n"
						"  mps     tot    mps     tot    dps     tot    cur  cps   tot  dps   tot  id bs\n");

			mpsout = totout = 0;
			mpsdrop = totdrop = 0;
			totqueue = 0;
			for (j = 0; srvs[j] != NULL; j++) {
				mpsout += server_get_metrics_sub(srvs[j]);
				totout += server_get_metrics(srvs[j]);

				mpsdrop += server_get_dropped_sub(srvs[j]);
				totdrop += server_get_dropped(srvs[j]);

				totqueue += server_get_queue_len(srvs[j]);
			}
			mpsin = totin = 0;
			widle = wbusy = 0;
			for (j = 0; dispatchers[j] != NULL; j++) {
				mpsin += dispatch_get_metrics_sub(dispatchers[j]);
				totin += dispatch_get_metrics(dispatchers[j]);

				if (dispatch_busy(dispatchers[j])) {
					wbusy++;
				} else {
					widle++;
				}
			}
			totconn = dispatch_get_accepted_connections();
			totdisc = dispatch_get_closed_connections();
			printf("%5zu %7zu  "   /* metrics in */
					"%5zu %7zu  "  /* metrics out */
					"%5zu %7zu  "  /* metrics dropped */
					"%5zu  "        /* queue */
					"%3zu %5zu  "  /* conns */
					"%3zu %5zu  "  /* disconns */
					"%2d %2d\n",   /* workers */
					mpsin, totin,
					mpsout, totout,
					mpsdrop, totdrop,
					totqueue,
					totconn - lastconn, totconn,
					totdisc - lastdisc, totdisc,
					widle, wbusy
				  );
			lastconn = totconn;
			lastdisc = totdisc;
		}
		i++;
		if (i < collector_interval)
			continue;
		totdropped = 0;
		for (i = 0; srvs[i] != NULL; i++) {
			queued = server_get_queue_len(srvs[i]);
			queuesize = server_get_queue_size(srvs[i]);
			totdropped += server_get_dropped(srvs[i]);
			queueusage = (double)queued / (double)queuesize;

			if (queueusage >= 0.75)
				logout("warning: metrics queuing up "
						"for %s:%u: %zu metrics (%d%% of queue size)\n",
						server_ip(srvs[i]), server_port(srvs[i]), queued,
						(int)(queueusage * 100));
		}
		if (totdropped - lastdropped > 0)
			logout("warning: dropped %zu metrics\n", totdropped - lastdropped);
		lastdropped = totdropped;
		if (numaggregators > 0) {
			totdropped = aggregator_get_dropped(aggrs);
			if (totdropped - lastaggrdropped > 0)
				logout("warning: aggregator dropped %zu metrics\n",
						totdropped - lastaggrdropped);
			lastaggrdropped = totdropped;
		}

		i = 0;
	}
	return NULL;
}

/**
 * Schedules routes r to be put in place for the current routes.  The
 * replacement is performed at the next cycle of the collector.
 */
inline void
collector_schedulereload(cluster *c, aggregator *a)
{
	pending_clusters = c;
	pending_aggrs = a;
	cluster_refresh_pending = 1;
}

/**
 * Returns true if the routes scheduled to be reloaded by a call to
 * collector_schedulereload() have been activated.
 */
inline char
collector_reloadcomplete(void)
{
	return cluster_refresh_pending == 0;
}

/**
 * Initialises and starts the collector.
 */
void
collector_start(dispatcher **d, cluster *c, aggregator *a, server *submission, char cum)
{
	dispatchers = d;
	collector_schedulereload(c, a);

	if (mode == DEBUG || mode == DEBUGTEST || mode == DEBUGSUBMISSION)
		debug = 1;
	debug |= (cum ? 0 : 2);

	if (mode != SUBMISSION && mode != DEBUGSUBMISSION) {
		if (pthread_create(&collectorid, NULL, collector_runner, submission) != 0)
			logerr("failed to start collector!\n");
	} else {
		if (pthread_create(&collectorid, NULL, collector_writer, NULL) != 0)
			logerr("failed to start collector!\n");
	}
}

/**
 * Shuts down the collector.
 */
void
collector_stop(void)
{
	keep_running = 0;
	pthread_join(collectorid, NULL);
}
