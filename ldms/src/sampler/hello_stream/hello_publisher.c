#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <getopt.h>
#include <semaphore.h>
#include <ovis_util/util.h>
#include "ldms.h"
#include "ldmsd_stream.h"

static ldms_t ldms;
static sem_t conn_sem;
static int conn_status;
static sem_t recv_sem;

static struct option long_opts[] = {
	{"host",     required_argument, 0,  'h' },
	{"port",     required_argument, 0,  'p' },
	{"stream",   required_argument, 0,  's' },
	{"xprt",     required_argument, 0,  'x' },
	{"auth",     required_argument, 0,  'a' },
	{"auth_arg", required_argument, 0,  'A' },
	{"message",  required_argument, 0,  'm' },
	{"type",     required_argument, 0,  't' },
	{0,          0,                 0,  0 }
};

void usage(int argc, char **argv)
{
	printf("usage: %s -x <xprt> -h <host> -p <port> -s <stream-name>\n"
	       "	-a <auth> -A <auth-opt>\n"
	       "	-m <message-text> -t <data-format>\n\n"
	       "	<data-format>	str | json (default is str)\n",
	       argv[0]);
	exit(1);
}

static const char *short_opts = "h:p:s:x:a:A:m:t:";

#define AUTH_OPT_MAX 128

int server_rc;
static void event_cb(ldms_t x, ldms_xprt_event_t e, void *cb_arg)
{
	switch (e->type) {
	case LDMS_XPRT_EVENT_CONNECTED:
		sem_post(&conn_sem);
		conn_status = 0;
		break;
	case LDMS_XPRT_EVENT_REJECTED:
		ldms_xprt_put(x);
		conn_status = ECONNREFUSED;
		break;
	case LDMS_XPRT_EVENT_DISCONNECTED:
		ldms_xprt_put(x);
		conn_status = ENOTCONN;
		break;
	case LDMS_XPRT_EVENT_ERROR:
		conn_status = ECONNREFUSED;
		break;
	case LDMS_XPRT_EVENT_RECV:
		sem_post(&recv_sem);
		server_rc = ldmsd_stream_response(e);
		break;
	default:
		printf("Received invalid event type %d\n", e->type);
	}
}

struct timespec ts;
int to;
ldms_t setup_connection(const char *xprt, const char *host,
			const char *port, const char *auth)
{
	char hostname[PATH_MAX];
	const char *timeout = "5";
	int rc;

	if (!host) {
		if (0 == gethostname(hostname, sizeof(hostname)))
			host = hostname;
	}
	if (!timeout) {
		ts.tv_sec = time(NULL) + 5;
		ts.tv_nsec = 0;
	} else {
		to = atoi(timeout);
		if (to <= 0)
			to = 5;
		ts.tv_sec = time(NULL) + to;
		ts.tv_nsec = 0;
	}
	ldms = ldms_xprt_new_with_auth(xprt, NULL, auth, NULL);
	if (!ldms) {
		printf("Error %d creating the '%s' transport\n",
		       errno, xprt);
		return NULL;
	}

	sem_init(&recv_sem, 1, 0);
	sem_init(&conn_sem, 1, 0);

	rc = ldms_xprt_connect_by_name(ldms, host, port, event_cb, NULL);
	if (rc) {
		printf("Error %d connecting to %s:%s\n",
		       rc, host, port);
		return NULL;
	}
	sem_timedwait(&conn_sem, &ts);
	if (conn_status)
		return NULL;
	return ldms;
}

int main(int argc, char **argv)
{
	char *host = NULL;
	char *port = NULL;
	char *xprt = "sock";
	char *msg = "hello, world!";
	char *fmt = "str";
        ldmsd_stream_type_t type;
	char *stream = "hello_stream/hello";
	int opt, opt_idx;
	char *lval, *rval;
	char *auth = "none";
	struct attr_value_list *auth_opt = NULL;
	const int auth_opt_max = AUTH_OPT_MAX;

	auth_opt = av_new(auth_opt_max);
	if (!auth_opt) {
		perror("could not allocate auth options");
		exit(1);
	}

	while ((opt = getopt_long(argc, argv,
				  short_opts, long_opts,
				  &opt_idx)) > 0) {
		switch (opt) {
		case 'h':
			host = strdup(optarg);
			break;
		case 'p':
			port = strdup(optarg);
			break;
		case 'x':
			xprt = strdup(optarg);
			break;
		case 'a':
			auth = strdup(optarg);
			break;
		case 'A':
			lval = strtok(optarg, "=");
			rval = strtok(NULL, "");
			if (!lval || !rval) {
				printf("ERROR: Expecting -A name=value");
				exit(1);
			}
			if (auth_opt->count == auth_opt->size) {
				printf("ERROR: Too many auth options");
				exit(1);
			}
			auth_opt->list[auth_opt->count].name = lval;
			auth_opt->list[auth_opt->count].value = rval;
			auth_opt->count++;
			break;
		case 's':
			stream = strdup(optarg);
			break;
		case 'm':
			msg = strdup(optarg);
			break;
		case 't':
			fmt = strdup(optarg);
			break;
		default:
			usage(argc, argv);
		}
	}
	if (0 == strcmp(fmt, "str")) {
		type = LDMSD_STREAM_STRING;
	} else if (0 == strcmp(fmt, "json")) {
		type = LDMSD_STREAM_JSON;
	} else {
		printf("%s is an invalid data format\n", fmt);
		usage(argc, argv);
	}
	if (!host || !port)
		usage(argc, argv);

	ldms_t ldms = setup_connection(xprt, host, port, auth);
	int rc = ldmsd_stream_publish(ldms, stream, type, msg, strlen(msg) + 1);
	if (rc)
		printf("Error %d publishing data.\n", rc);
	else
		printf("The data was successfully published.\n");
	/*
	 * NB: This isn't typically required, but is here because we
	 * are exiting and unless we wait for the reply, the server
	 * will log an error attempting to respond to us.
	 */
	ts.tv_sec = time(NULL) + to;
	sem_timedwait(&recv_sem, &ts);

	printf("The server responded with %d\n", server_rc);
	return server_rc;
}
