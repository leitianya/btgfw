#include "csnet.h"
#include "csnet-cond.h"
#include "csnet-fast.h"
#include "csnet-utils.h"
#include "csnet-socket.h"
#include "csnet-socket-api.h"
#include "csnet-el.h"
#include "csnet-msg.h"
#include "csnet-module.h"
#include "csnet-btgfw.h"

#if defined(__APPLE__)
#include "csnet-kqueue.h"
#else
#include "csnet-epoll.h"
#endif

#include "cs-lfqueue.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

#define MAGIC_NUMBER 1024
#define CPUID_MASK 127

static struct csnet_cond cond = CSNET_COND_INITILIAZER;

static void _do_accept(struct csnet* csnet, int* listenfd);

struct csnet*
csnet_new(int port, int nthread, int max_conn,
          struct csnet_log* log, struct csnet_module* module, struct cs_lfqueue* q) {
	struct csnet* csnet;
	csnet = calloc(1, sizeof(*csnet) + nthread * sizeof(struct csnet_el*));
	if (!csnet) {
		csnet_oom(sizeof(*csnet));
	}

	csnet->listenfd = csnet_listen_port(port);
	if (csnet->listenfd == -1) {
		log_f(log, "epoll_create(): %s", strerror(errno));
	}

	if (csnet_set_nonblocking(csnet->listenfd) == -1) {
		log_f(log, "cannot set socket: %d to nonblock", csnet->listenfd);
	}

	csnet->nthread = nthread;
	csnet->max_conn = max_conn;
	csnet->epoller = csnet_epoller_new(MAGIC_NUMBER);

	if (!csnet->epoller) {
		log_f(log, "epoll_create(): %s", strerror(errno));
	}

	if (csnet_epoller_add(csnet->epoller, csnet->listenfd, 0) == -1) {
		log_f(log, "epoll_ctl(): %s", strerror(errno));
	}

	for (int i = 0; i < nthread; i++) {
		int count = max_conn / nthread + 1;
		csnet->els[i] = csnet_el_new(count, log, module, q);
	}

	csnet->log = log;
	csnet->q = q;
	return csnet;
}

void
csnet_reset_module(struct csnet* csnet, struct csnet_module* module) {
	for (int i = 0; i < csnet->nthread; i++) {
		csnet->els[i]->module = module;
	}
}

void*
csnet_dispatch_thread(void* arg) {
	struct csnet* csnet = (struct csnet*)arg;
	struct cs_lfqueue* q = csnet->q;
	cs_lfqueue_register_thread(q);

	while (1) {
		struct csnet_msg* msg = NULL;
		int ret = cs_lfqueue_deq(q, (void*)&msg);
		if (csnet_fast(ret == 0)) {
			int s = csnet_socket_send(msg->socket, msg->data, msg->size);
			if (s < 0) {
				log_e(csnet->log, "send to socket %d failed",
				      msg->socket->fd);
			}
			csnet_msg_free(msg);
		} else {
#if defined(__APPLE__)
			csnet_cond_nonblocking_wait(&cond, 0, 100);
#else
			csnet_cond_nonblocking_wait(&cond, 0, 1000);
#endif
		}
	}

	debug("csnet_dispatch_thread exit");
	return NULL;
}

void
csnet_loop(struct csnet* csnet, int timeout) {
	int online_cpus = csnet_online_cpus();

	for (int i = 0; i < csnet->nthread; i++) {
		csnet_el_run(csnet->els[i]);
		/* Skip CPU0 and CPU1
		   FIXME: Only work when online_cpus <= CPUID_MASK + 1. */
		int cpuid = ((i % (online_cpus - 2)) + 2) & CPUID_MASK;
		csnet_bind_to_cpu(csnet->els[i]->tid, cpuid);
	}

	if (pthread_create(&csnet->tid, NULL, csnet_dispatch_thread, csnet) < 0) {
		log_f(csnet->log, "pthread_create(): %s", strerror(errno));
	}

	csnet_bind_to_cpu(csnet->tid, online_cpus - 1);

	while (1) {
		int ready = csnet_epoller_wait(csnet->epoller, timeout);
		for (int i = 0; i < ready; ++i) {
			csnet_epoller_event_t* ee = csnet_epoller_get_event(csnet->epoller, i);
			int fd = csnet_epoller_event_fd(ee);

			if (csnet_epoller_event_is_r(ee)) {
				if (fd == csnet->listenfd) {
					/* Have a notification on the listening socket,
					   which means one or more new incoming connecttions */
					_do_accept(csnet, &csnet->listenfd);
				}
			}

			if (csnet_epoller_event_is_e(ee)) {
				log_e(csnet->log, "epoll event error");
				close(fd);
				continue;
			}
		}

		if (ready == -1) {
			if (errno == EINTR) {
				/* Stopped by a signal */
				continue;
			} else {
				log_e(csnet->log, "epoll_wait(): %s", strerror(errno));
				return;
			}
		}
	}
	debug("csnet_loop exit");
}

void
csnet_free(struct csnet* csnet) {
	pthread_join(csnet->tid, NULL);
	close(csnet->listenfd);
	csnet_epoller_free(csnet->epoller);
	for (int i = 0; i < csnet->nthread; i++) {
		csnet_el_free(csnet->els[i]);
	}
	free(csnet);
}

int
csnet_sendto(struct cs_lfqueue* q, struct csnet_msg* msg) {
	cs_lfqueue_enq(q, msg);
	csnet_cond_signal_one(&cond);
	return 0;
}

static inline void
_do_accept(struct csnet* csnet, int* listenfd) {
	while (1) {
		int fd;
		struct sockaddr_in sin;
		socklen_t len = sizeof(struct sockaddr_in);
		bzero(&sin, len);
		fd = accept(*listenfd, (struct sockaddr*)&sin, &len);

		if (fd > 0) {
			log_i(csnet->log, "accept incoming [%s:%d] with socket %d.",
				inet_ntoa(sin.sin_addr), ntohs(sin.sin_port), fd);
			csnet_set_nonblocking(fd);
			if (csnet_el_watch(csnet->els[fd % csnet->nthread], fd) == -1) {
				close(fd);
				return;
			}
		} else {
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				/* We have processed all incoming connections. */
				return;
			} else {
				log_e(csnet->log, "accept(): %s", strerror(errno));
				return;
			}
		}
	}
}

