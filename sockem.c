/*
 * sockem - socket-level network emulation
 *
 * Copyright (c) 2016, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE /* for strdupa() and RTLD_NEXT */
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <poll.h>
#include <assert.h>
#include <netinet/in.h>
#include <dlfcn.h>

#include "sockem.h"

#include <sys/queue.h>

#ifdef _MSC_VER
#define socket_errno() WSAGetLastError()
#else
#define socket_errno() errno
#define SOCKET_ERROR -1
#endif

#include <pthread.h>
typedef pthread_mutex_t mtx_t;
#define mtx_init(M) pthread_mutex_init(M, NULL)
#define mtx_destroy(M) pthread_mutex_destroy(M)
#define mtx_lock(M) pthread_mutex_lock(M)
#define mtx_unlock(M) pthread_mutex_unlock(M)

typedef pthread_t thrd_t;
#define thrd_create(THRD,START_ROUTINE,ARG) \
  pthread_create(THRD, NULL, START_ROUTINE, ARG)
#define thrd_join(THRD,RETVAL) \
  pthread_join(THRD, NULL)


#ifdef LIBSOCKEM_PRELOAD
static mtx_t sockem_lock;
#endif

static LIST_HEAD(, sockem_s) sockems;


typedef int64_t sockem_ts_t;

static pthread_once_t sockem_once = PTHREAD_ONCE_INIT;
static int (*sockem_orig_connect) (int, const struct sockaddr *, socklen_t);
static int (*sockem_orig_close) (int);
static char *sockem_conf_str = "";

#ifdef LIBSOCKEM_PRELOAD
#define sockem_close0(S)        (sockem_orig_close(S))
#define sockem_connect0(S,A,AL) (sockem_orig_connect(S,A,AL))
#else
#define sockem_close0(S)        close(S)
#define sockem_connect0(S,A,AL) connect(S,A,AL)
#endif


struct sockem_conf {
        /* FIXME: these needs to be implemented */
        int tx_thruput;  /* app->peer bytes/second */
        int rx_thruput;  /* peer->app bytes/second */
        int delay;       /* latency in ms */
        int jitter;      /* latency variation in ms */
        int debug;       /* enable sockem printf debugging */
        size_t bufsz;    /* recv chunk/buffer size */
};




struct sockem_s {
        LIST_ENTRY(sockem_s) link;

        enum {
                /* Forwarder thread run states */
                SOCKEM_INIT,
                SOCKEM_START,
                SOCKEM_RUN,
                SOCKEM_TERM
        } run;

        int as;        /* application's socket. */
        int ls;        /* internal application listen socket */
        int ps;        /* internal peer socket connecting sockem to the peer.*/

        void *buf;     /* Receive buffer */
        size_t bufsz;  /* .. size */

        int linked;    /* On sockems list */

        thrd_t thrd;   /* Forwarder thread */

        mtx_t  lock;

        struct sockem_conf conf;  /* application-set config.
                                   * protected by .lock */

        struct sockem_conf use;   /* last copy of .conf
                                   * local to skm thread */
};


static int sockem_vset (sockem_t *skm, va_list ap);


/**
 * A microsecond monotonic clock
 */
static __attribute__((unused)) __inline int64_t sockem_clock (void) {
#ifdef __APPLE__
	/* No monotonic clock on Darwin */
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return ((int64_t)tv.tv_sec * 1000000LLU) + (int64_t)tv.tv_usec;
#elif _MSC_VER
	return (int64_t)GetTickCount64() * 1000LLU;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((int64_t)ts.tv_sec * 1000000LLU) +
		((int64_t)ts.tv_nsec / 1000LLU);
#endif
}


/**
 * @returns the maximum waittime in ms for poll()
 * @remark lock must be held
 */
static int sockem_calc_waittime (sockem_t *skm) {
        if (skm->use.delay + skm->use.jitter == 0)
                return 1000;
        else
                return skm->use.jitter < skm->use.delay ?
                        skm->use.jitter : skm->use.delay;
}


/**
 * @brief read from \p ifd, write to \p ofd in a blocking fashion.
 *
 * @returns the number of bytes forwarded, or -1 on error.
 */
static int sockem_recv_fwd (sockem_t *skm, int ifd, int ofd) {
        ssize_t r, wr;
        int64_t delay;

        r = recv(ifd, skm->buf, skm->bufsz, MSG_DONTWAIT);
        if (r == -1) {
                int serr = socket_errno();
                if (serr == EAGAIN || serr == EWOULDBLOCK)
                        return 0;
                return -1;

        } else if (r == 0) {
                /* Socket closed */
                return -1;
        }

        /* FIXME: proper */
        delay = skm->use.delay + (skm->use.jitter / 2);
        if (delay)
                usleep(delay * 1000);

        wr = send(ofd, skm->buf, r, 0);
        if (wr < r)
                return -1;

        return wr;
}


/**
 * @brief Close all sockets and unsets ->run.
 * @remark Preserves caller's errno.
 * @remark lock must be held.
 */
static void sockem_close_all (sockem_t *skm) {
        int serr = socket_errno();

        if (skm->ls != -1) {
                sockem_close0(skm->ls);
                skm->ls = -1;
        }

        if (skm->ps != -1) {
                sockem_close0(skm->ps);
                skm->ps = -1;
        }

        skm->run = SOCKEM_TERM;

        errno = serr;
}



/**
 * @brief sockem internal per-socket forwarder thread
 */
static void *sockem_run (void *arg) {
        sockem_t *skm = arg;
        int waittime;
        int cs = -1;
        int ls;
        struct pollfd pfd[2];

        mtx_lock(&skm->lock);
        if (skm->run == SOCKEM_START)
                skm->run = SOCKEM_RUN;
        skm->use = skm->conf;
        ls = skm->ls;
        mtx_unlock(&skm->lock);

        skm->bufsz = skm->use.bufsz;
        skm->buf = malloc(skm->bufsz);

        /* Accept connection from sockfd in sockem_connect() */
        cs = accept(ls, NULL, 0);
        if (cs == -1) {
                mtx_lock(&skm->lock);
                if (skm->run == SOCKEM_TERM) {
                        /* App socket was closed. */
                        goto done;
                }
                fprintf(stderr, "%% sockem: accept(%d) failed: %s\n",
                        ls, strerror(socket_errno()));
                assert(cs != -1);
        }

        /* Set up poll (blocking IO) */
        memset(pfd, 0, sizeof(pfd));
        pfd[0].fd = cs;
        pfd[0].events = POLLIN;

        mtx_lock(&skm->lock);
        pfd[1].fd = skm->ps;
        mtx_unlock(&skm->lock);
        pfd[1].events = POLLIN;

        waittime = sockem_calc_waittime(skm);

        mtx_lock(&skm->lock);
        while (skm->run == SOCKEM_RUN) {
                int r;
                int i;

                mtx_unlock(&skm->lock);
                r = poll(pfd, 2, waittime);

                if (r == -1)
                        break;

                for (i = 0 ; r > 0 && i < 2 ; i++) {
                        if (pfd[i].revents & (POLLHUP|POLLERR)) {
                                skm->run = SOCKEM_TERM;

                        } else if (pfd[i].revents & POLLIN) {
                                if (sockem_recv_fwd(skm,
                                                    pfd[i].fd,
                                                    pfd[i^1].fd) == -1) {
                                        skm->run = SOCKEM_TERM;
                                        break;
                                }
                        }
                }

                mtx_lock(&skm->lock);
                skm->use = skm->conf;
        }
 done:
        if (cs != -1)
                sockem_close0(cs);
        sockem_close_all(skm);

        mtx_unlock(&skm->lock);
        free(skm->buf);


        return NULL;
}



/**
 * @brief Connect socket \p s to \p addr
 */
static int sockem_do_connect (int s, const struct sockaddr *addr,
                              socklen_t addrlen) {
        int r;

        r = sockem_connect0(s, addr, addrlen);
        if (r == SOCKET_ERROR) {
                int serr = socket_errno();
                if (serr != EINPROGRESS
#ifdef _MSC_VER
                    && serr != WSAEWOULDBLOCK
#endif
                        ) {
#ifndef _MSC_VER
                        errno = serr;
#endif
                        return -1;
                }
        }

        return 0;
}

sockem_t *sockem_connect (int sockfd, const struct sockaddr *addr,
                          socklen_t addrlen, ...) {
        sockem_t *skm;
        int ls, ps;
        struct sockaddr_in6 sin6 = { sin6_family: addr->sa_family };
        socklen_t addrlen2 = addrlen;
        va_list ap;

        /* Create internal app listener socket */
        ls = socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
        if (ls == -1)
                return NULL;

        if (bind(ls, (struct sockaddr *)&sin6, addrlen) == -1) {
                sockem_close0(ls);
                return NULL;
        }

        /* Get bound address */
        if (getsockname(ls, (struct sockaddr *)&sin6, &addrlen2) == -1) {
                sockem_close0(ls);
                return NULL;
        }

        if (listen(ls, 1) == -1) {
                sockem_close0(ls);
                return NULL;
        }

        /* Create internal peer socket */
        ps = socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
        if (ps == -1) {
                sockem_close0(ls);
                return NULL;
        }

        /* Connect to peer */
        if (sockem_do_connect(ps, addr, addrlen) == -1) {
                sockem_close0(ls);
                sockem_close0(ps);
                return NULL;
        }

        /* Create sockem handle */
        skm = calloc(1, sizeof(*skm));
        skm->as = sockfd;
        skm->ls = ls;
        skm->ps = ps;
        mtx_init(&skm->lock);

        /* Default config*/
        skm->conf.rx_thruput = 1 << 30;
        skm->conf.tx_thruput = 1 << 30;
        skm->conf.delay = 0;
        skm->conf.jitter = 0;
        skm->conf.bufsz = 1024*1024;

        /* Apply passed configuration */
        va_start(ap, addrlen);
        if (sockem_vset(skm, ap) == -1) {
                va_end(ap);
                sockem_close(skm);
                return NULL;
        }
        va_end(ap);

        mtx_lock(&skm->lock);
        skm->run = SOCKEM_START;

        /* Create pipe thread */
        if (thrd_create(&skm->thrd, sockem_run, skm) != 0) {
                mtx_unlock(&skm->lock);
                sockem_close(skm);
                return NULL;
        }
        mtx_unlock(&skm->lock);

        /* Connect application socket to listen socket */
        if (sockem_connect0(sockfd, (struct sockaddr *)&sin6, addrlen2) == -1) {
                sockem_close(skm);
                return NULL;
        }

        mtx_lock(&skm->lock);
        skm->linked = 1;
        mtx_unlock(&skm->lock);

#ifdef LIBSOCKEM_PRELOAD
        mtx_lock(&sockem_lock);
#endif
        LIST_INSERT_HEAD(&sockems, skm, link);

#ifdef LIBSOCKEM_PRELOAD
        mtx_unlock(&sockem_lock);
#endif

        return skm;
}

void sockem_close (sockem_t *skm) {

        mtx_lock(&skm->lock);

        /* If thread is running let it close the sockets
         * to avoid race condition. */
        if (skm->run == SOCKEM_START ||
            skm->run == SOCKEM_RUN)
                skm->run = SOCKEM_TERM;
        else
                sockem_close_all(skm);

        /* LIBSOCKEM_PRELOAD: caller must hold sockem_lock. */
        if (skm->linked)
                LIST_REMOVE(skm, link);

        mtx_unlock(&skm->lock);

        thrd_join(skm->thrd, NULL);

        mtx_destroy(&skm->lock);


        free(skm);
}


/**
 * @brief Set single conf key.
 * @remark lock must be held.
 * @returns 0 on success or -1 if key is unknown
 */
static int sockem_set0 (sockem_t *skm, const char *key, int val) {
        if (!strcmp(key, "rx.thruput") ||
            !strcmp(key, "rx.throughput"))
                skm->conf.rx_thruput = val;
        else if (!strcmp(key, "tx.thruput") ||
                 !strcmp(key, "tx.throughput"))
                skm->conf.tx_thruput = val;
        else if (!strcmp(key, "delay"))
                skm->conf.delay = val;
        else if (!strcmp(key, "jitter"))
                skm->conf.jitter = val;
        else if (!strcmp(key, "rx.bufsz"))
                skm->conf.bufsz = val;
        else if (!strcmp(key, "debug"))
                skm->conf.debug = val;
        else if (!strcmp(key, "true"))
                ; /* dummy key for allowing non-empty but default config */
        else if (!strchr(key, ',')) {
                char *s = strdupa(key);
                while (*s) {
                        char *t = strchr(s, ',');
                        char *d = strchr(s, '=');
                        if (t)
                                *t = '\0';
                        if (!d)
                                return -1;
                        *(d++) = '\0';

                        if (sockem_set0(skm, s, atoi(d)) == -1)
                                return -1;

                        if (!t)
                                break;
                        s += 1;
                }
        } else
                return -1;
        return 0;
}


/**
 * @brief Set sockem config parameters
 */
static int sockem_vset (sockem_t *skm, va_list ap) {
        const char *key;
        int val;

        mtx_lock(&skm->lock);
        while ((key = va_arg(ap, const char *))) {
                val = va_arg(ap, int);
                if (sockem_set0(skm, key, val) == -1) {
                        mtx_unlock(&skm->lock);
                        return -1;
                }
        }
        mtx_unlock(&skm->lock);

        return 0;
}

int sockem_set (sockem_t *skm, ...) {
        va_list ap;
        int r;

        va_start(ap, skm);
        r = sockem_vset(skm, ap);
        va_end(ap);

        return r;
}


sockem_t *sockem_find (int sockfd) {
        sockem_t *skm;

        LIST_FOREACH(skm, &sockems, link)
                if (skm->as == sockfd)
                        return skm;

        return NULL;
}


#ifdef LIBSOCKEM_PRELOAD
/**
 * Provide overloading socket APIs and conf bootstrapping from env vars.
 *
 */

/**
 * @brief Initialize preloadable libsockem once.
 */
static void sockem_init (void) {
        mtx_init(&sockem_lock);
        sockem_conf_str = getenv("SOCKEM_CONF");
        fprintf(stderr, "%% libsockem pre-loaded (%s)\n", sockem_conf_str);
        sockem_orig_connect = dlsym(RTLD_NEXT, "connect");
        sockem_orig_close = dlsym(RTLD_NEXT, "close");
}


/**
 * @brief connect(2) overload
 */
int connect (int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
        sockem_t *skm;

        pthread_once(&sockem_once, sockem_init);

        skm = sockem_connect(sockfd, addr, addrlen, sockem_conf_str, 0, NULL);
        if (!skm)
                return -1;

        return 0;
}

/**
 * @brief close(2) overload
 */
int close (int fd) {
        sockem_t *skm;

        pthread_once(&sockem_once, sockem_init);

        mtx_lock(&sockem_lock);
        skm = sockem_find(fd);

        if (skm)
                sockem_close(skm);
        mtx_unlock(&sockem_lock);

        return sockem_close0(fd);
}

#endif
