/*
 * Copyright (c) 2004-2006 Maxim Sobolev <sobomax@FreeBSD.org>
 * Copyright (c) 2006-2007 Sippy Software, Inc., http://www.sippysoft.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id$
 *
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <assert.h>
#if !defined(__solaris__)
#include <err.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "rtp.h"
#include "rtp_resizer.h"
#include "rtp_server.h"
#include "rtpp_defines.h"
#include "rtpp_log.h"
#include "rtpp_record.h"
#include "rtpp_session.h"
#include "rtpp_util.h"

#define	GET_RTP(sp)	(((sp)->rtp != NULL) ? (sp)->rtp : (sp))
#define	NOT(x)		(((x) == 0) ? 1 : 0)

static const char *cmd_sock = CMD_SOCK;
static const char *pid_file = PID_FILE;
static rtpp_log_t glog;

struct proto_cap {
    const char	*pc_id;
    const char	*pc_description;
};

static struct proto_cap proto_caps[] = {
    /*
     * The first entry must be basic protocol version and isn't shown
     * as extension on -v.
     */
    { "20040107", "Basic RTP proxy functionality" },
    { "20050322", "Support for multiple RTP streams and MOH" },
    { "20060704", "Support for extra parameter in the V command" },
    { "20071116", "Support for RTP re-packetization" },
    { NULL, NULL }
};

static void setbindhost(struct sockaddr *, int, const char *, const char *);
static void remove_session(struct cfg *, struct rtpp_session *);
static void alarmhandler(struct cfg *);
static int create_twinlistener(struct cfg *, struct sockaddr *, int, int *);
static int create_listener(struct cfg *, struct sockaddr *, int, int *, int *);
static int handle_command(struct cfg *, int);
static void usage(void);
static void send_packet(struct cfg *, struct rtpp_session *, int,
  struct rtp_packet *);

static void
setbindhost(struct sockaddr *ia, int pf, const char *bindhost,
  const char *servname)
{
    int n;

    /*
     * If user specified * then change it to NULL,
     * that will make getaddrinfo to return addr_any socket
     */
    if (bindhost && (strcmp(bindhost, "*") == 0))
	bindhost = NULL;

    if ((n = resolve(ia, pf, bindhost, servname, AI_PASSIVE)) != 0)
	errx(1, "setbindhost: %s", gai_strerror(n));
}

static void
append_session(struct cfg *cf, struct rtpp_session *sp, int index)
{

    if (sp->fds[index] != -1) {
	cf->sessions[cf->nsessions] = sp;
	cf->pfds[cf->nsessions].fd = sp->fds[index];
	cf->pfds[cf->nsessions].events = POLLIN;
	cf->pfds[cf->nsessions].revents = 0;
	sp->sidx[index] = cf->nsessions;
	cf->nsessions++;
    } else {
	sp->sidx[index] = -1;
    }
}

static void
append_server(struct cfg *cf, struct rtpp_session *sp)
{

    if (sp->rtps[0] != NULL || sp->rtps[1] != NULL) {
        if (sp->sridx == -1) {
	    cf->rtp_servers[cf->rtp_nsessions] = sp;
	    sp->sridx = cf->rtp_nsessions;
	    cf->rtp_nsessions++;
	}
    } else {
        sp->sridx = -1;
    }
}

/* Function that gets called approximately every TIMETICK seconds */
static void
alarmhandler(struct cfg *cf)
{
    struct rtpp_session *sp;
    int i;

    for (i = 1; i < cf->nsessions; i++) {
	sp = cf->sessions[i];
	if (sp == NULL || sp->rtcp == NULL || sp->sidx[0] != i)
	    continue;
	if (sp->ttl == 0) {
	    rtpp_log_write(RTPP_LOG_INFO, sp->log, "session timeout");
	    remove_session(cf, sp);
	    continue;
	}
	sp->ttl--;
    }
}

static void
remove_session(struct cfg *cf, struct rtpp_session *sp)
{
    int i;

    rtpp_log_write(RTPP_LOG_INFO, sp->log, "RTP stats: %lu in from callee, %lu "
      "in from caller, %lu relayed, %lu dropped", sp->pcount[0], sp->pcount[1],
      sp->pcount[2], sp->pcount[3]);
    rtpp_log_write(RTPP_LOG_INFO, sp->log, "RTCP stats: %lu in from callee, %lu "
      "in from caller, %lu relayed, %lu dropped", sp->rtcp->pcount[0],
      sp->rtcp->pcount[1], sp->rtcp->pcount[2], sp->rtcp->pcount[3]);
    rtpp_log_write(RTPP_LOG_INFO, sp->log, "session on ports %d/%d is cleaned up",
      sp->ports[0], sp->ports[1]);
    for (i = 0; i < 2; i++) {
	if (sp->addr[i] != NULL)
	    free(sp->addr[i]);
	if (sp->rtcp->addr[i] != NULL)
	    free(sp->rtcp->addr[i]);
	if (sp->fds[i] != -1) {
	    close(sp->fds[i]);
	    assert(cf->sessions[sp->sidx[i]] == sp);
	    cf->sessions[sp->sidx[i]] = NULL;
	    assert(cf->pfds[sp->sidx[i]].fd == sp->fds[i]);
	    cf->pfds[sp->sidx[i]].fd = -1;
	    cf->pfds[sp->sidx[i]].events = 0;
	}
	if (sp->rtcp->fds[i] != -1) {
	    close(sp->rtcp->fds[i]);
	    assert(cf->sessions[sp->rtcp->sidx[i]] == sp->rtcp);
	    cf->sessions[sp->rtcp->sidx[i]] = NULL;
	    assert(cf->pfds[sp->rtcp->sidx[i]].fd == sp->rtcp->fds[i]);
	    cf->pfds[sp->rtcp->sidx[i]].fd = -1;
	    cf->pfds[sp->rtcp->sidx[i]].events = 0;
	}
	if (sp->rrcs[i] != NULL)
	    rclose(sp, sp->rrcs[i]);
	if (sp->rtcp->rrcs[i] != NULL)
	    rclose(sp, sp->rtcp->rrcs[i]);
	if (sp->rtps[i] != NULL) {
	    cf->rtp_servers[sp->sridx] = NULL;
	    rtp_server_free(sp->rtps[i]);
	}
    }
    if (sp->call_id != NULL)
	free(sp->call_id);
    if (sp->tag != NULL)
	free(sp->tag);
    rtpp_log_close(sp->log);
    free(sp->rtcp);
    rtp_resizer_free(&sp->resizers[0]);
    rtp_resizer_free(&sp->resizers[1]);
    free(sp);
}

static int
create_twinlistener(struct cfg *cf, struct sockaddr *ia, int port, int *fds)
{
    struct sockaddr_storage iac;
    int rval, i, flags;

    fds[0] = fds[1] = -1;

    rval = -1;
    for (i = 0; i < 2; i++) {
	fds[i] = socket(ia->sa_family, SOCK_DGRAM, 0);
	if (fds[i] == -1) {
	    rtpp_log_ewrite(RTPP_LOG_ERR, cf->glog, "can't create %s socket",
	      (ia->sa_family == AF_INET) ? "IPv4" : "IPv6");
	    goto failure;
	}
	memcpy(&iac, ia, SA_LEN(ia));
	satosin(&iac)->sin_port = htons(port);
	if (bind(fds[i], sstosa(&iac), SA_LEN(ia)) != 0) {
	    if (errno != EADDRINUSE && errno != EACCES) {
		rtpp_log_ewrite(RTPP_LOG_ERR, cf->glog, "can't bind to the %s port %d",
		  (ia->sa_family == AF_INET) ? "IPv4" : "IPv6", port);
	    } else {
		rval = -2;
	    }
	    goto failure;
	}
	port++;
	if ((ia->sa_family == AF_INET) &&
	  (setsockopt(fds[i], IPPROTO_IP, IP_TOS, &cf->tos, sizeof(cf->tos)) == -1))
	    rtpp_log_ewrite(RTPP_LOG_ERR, cf->glog, "unable to set TOS to %d", cf->tos);
	flags = fcntl(fds[i], F_GETFL);
	fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }
    return 0;

failure:
    for (i = 0; i < 2; i++)
	if (fds[i] != -1) {
	    close(fds[i]);
	    fds[i] = -1;
	}
    return rval;
}

static int
create_listener(struct cfg *cf, struct sockaddr *ia,  int startport,
  int *port, int *fds)
{
    int i, init, rval;

    for (i = 0; i < 2; i++)
	fds[i] = -1;

    init = 0;
    if (startport < cf->port_min || startport > cf->port_max)
	startport = cf->port_min;
    for (*port = startport; *port != startport || init == 0; (*port) += 2) {
	init = 1;
	rval = create_twinlistener(cf, ia, *port, fds);
	if (rval != 0) {
	    if (rval == -1)
		break;
	    if (*port >= cf->port_max)
		*port = cf->port_min - 2;
	    continue;
	}
	return 0;
    }
    return -1;
}

static int
compare_session_tags(char *tag1, char *tag0, unsigned *medianum_p)
{
    size_t len0 = strlen(tag0);

    if (!strncmp(tag1, tag0, len0)) {
	if (tag1[len0] == ';') {
	    if (medianum_p != 0)
		*medianum_p = strtoul(tag1 + len0 + 1, NULL, 10);
	    return 2;
	}
	if (tag1[len0] == 0)
	    return 1;
	return 0;
    }
    return 0;
}

static int
handle_command(struct cfg *cf, int controlfd)
{
    int len, delete, argc, i, j, pidx, request, response, asymmetric;
    int external, pf, ecode, lidx, play, record, noplay, weak;
    int ndeleted, cmpr, cmpr1;
    int fds[2], lport, n;
    socklen_t rlen;
    unsigned medianum;
    char buf[1024 * 8];
    char *cp, *call_id, *from_tag, *to_tag, *addr, *port, *cookie;
    char *pname, *codecs;
    struct rtpp_session *spa, *spb;
    char **ap, *argv[10];
    const char *rname;
    struct sockaddr *ia[2], *lia[2];
    struct sockaddr_storage raddr;
    int requested_nsamples;

    requested_nsamples = -1;

#define	doreply() \
    { \
	buf[len] = '\0'; \
	rtpp_log_write(RTPP_LOG_DBUG, cf->glog, "sending reply \"%s\"", buf); \
	if (cf->umode == 0) { \
	    write(controlfd, buf, len); \
	} else { \
	    while (sendto(controlfd, buf, len, 0, sstosa(&raddr), \
	      rlen) == -1 && errno == ENOBUFS); \
	} \
    }

    ia[0] = ia[1] = NULL;
    spa = spb = NULL;
    lia[0] = lia[1] = cf->bindaddr[0];
    lidx = 1;
    fds[0] = fds[1] = -1;

    if (cf->umode == 0) {
	for (;;) {
	    len = read(controlfd, buf, sizeof(buf) - 1);
	    if (len != -1 || (errno != EAGAIN && errno != EINTR))
		break;
	    sched_yield();
	}
    } else {
	rlen = sizeof(raddr);
	len = recvfrom(controlfd, buf, sizeof(buf) - 1, 0,
	  sstosa(&raddr), &rlen);
    }
    if (len == -1) {
	if (errno != EAGAIN && errno != EINTR)
	    rtpp_log_ewrite(RTPP_LOG_ERR, cf->glog, "can't read from control socket");
	return -1;
    }
    buf[len] = '\0';

    rtpp_log_write(RTPP_LOG_DBUG, cf->glog, "received command \"%s\"", buf);

    cp = buf;
    argc = 0;
    memset(argv, 0, sizeof(argv));
    for (ap = argv; (*ap = strsep(&cp, "\r\n\t ")) != NULL;)
	if (**ap != '\0') {
	    argc++;
	    if (++ap >= &argv[10])
		break;
	}
    cookie = NULL;
    if (argc < 1 || (cf->umode != 0 && argc < 2)) {
	rtpp_log_write(RTPP_LOG_ERR, cf->glog, "command syntax error");
	ecode = 0;
	goto goterror;
    }

    /* Stream communication mode doesn't use cookie */
    if (cf->umode != 0) {
	cookie = argv[0];
	for (i = 1; i < argc; i++)
	    argv[i - 1] = argv[i];
	argc--;
	argv[argc] = NULL;
    } else {
	cookie = NULL;
    }

    request = response = delete = play = record = noplay = 0;
    addr = port = NULL;
    switch (argv[0][0]) {
    case 'u':
    case 'U':
	request = 1;
	break;

    case 'l':
    case 'L':
	response = 1;
	break;

    case 'd':
    case 'D':
	delete = 1;
	break;

    case 'p':
    case 'P':
	/* P callid pname codecs from_tag to_tag */
	play = 1;
	pname = argv[2];
	codecs = argv[3];
	break;

    case 'r':
    case 'R':
        record = 1;
        break;

    case 's':
    case 'S':
        noplay = 1;
        break;

    case 'v':
    case 'V':
	if (argv[0][1] == 'F' || argv[0][1] == 'f') {
	    int i, known;
	    /*
	     * Wait for protocol version datestamp and check whether we
	     * know it.
	     */
	    if (argc != 2 && argc != 3) {
		rtpp_log_write(RTPP_LOG_ERR, cf->glog, "command syntax error");
		ecode = 2;
		goto goterror;
	    }
	    for (known = i = 0; proto_caps[i].pc_id != NULL; ++i) {
		if (!strcmp(argv[1], proto_caps[i].pc_id)) {
		    known = 1;
		    break;
		}
	    }
	    if (cookie == NULL)
		len = sprintf(buf, "%d\n", known);
	    else
		len = sprintf(buf, "%s %d\n", cookie, known);
	    goto doreply;
	}
	if (argc != 1 && argc != 2) {
	    rtpp_log_write(RTPP_LOG_ERR, cf->glog, "command syntax error");
	    ecode = 2;
	    goto goterror;
	}
	/* This returns base version. */
	if (cookie == NULL)
	    len = sprintf(buf, "%d\n", CPROTOVER);
	else
	    len = sprintf(buf, "%s %d\n", cookie, CPROTOVER);
	goto doreply;
	break;

    case 'i':
    case 'I':
	if (cookie == NULL)
	    len = sprintf(buf, "sessions created: %llu\nactive sessions: %d\n",
	      cf->sessions_created, cf->nsessions / 2);
	else
	    len = sprintf(buf, "%s sessions created: %llu\nactive sessions: %d\n",
	      cookie, cf->sessions_created, cf->nsessions / 2);
	for (i = 1; i < cf->nsessions; i++) {
	    char addrs[4][256];

            spa = cf->sessions[i];
            if (spa == NULL || spa->sidx[0] != i)
                continue;
	    /* RTCP twin session */
	    if (spa->rtcp == NULL) {
		spb = spa->rtp;
		buf[len++] = '\t';
	    } else {
		spb = spa->rtcp;
		buf[len++] = '\t';
		buf[len++] = 'C';
		buf[len++] = ' ';
	    }

	    addr2char_r(spb->laddr[1], addrs[0], sizeof(addrs[0]));
	    if (spb->addr[1] == NULL) {
		strcpy(addrs[1], "NONE");
	    } else {
		sprintf(addrs[1], "%s:%d", addr2char(spb->addr[1]),
		  addr2port(spb->addr[1]));
	    }
	    addr2char_r(spb->laddr[0], addrs[2], sizeof(addrs[2]));
	    if (spb->addr[0] == NULL) {
		strcpy(addrs[3], "NONE");
	    } else {
		sprintf(addrs[3], "%s:%d", addr2char(spb->addr[0]),
		  addr2port(spb->addr[0]));
	    }

	    len += sprintf(buf + len,
	      "%s/%s: caller = %s:%d/%s, callee = %s:%d/%s, "
	      "stats = %lu/%lu/%lu/%lu, ttl = %d\n",
	      spb->call_id, spb->tag, addrs[0], spb->ports[1], addrs[1],
	      addrs[2], spb->ports[0], addrs[3], spa->pcount[0], spa->pcount[1],
	      spa->pcount[2], spa->pcount[3], spb->ttl);
	    if (len + 512 > sizeof(buf)) {
		doreply();
		len = 0;
	    }
	}
	if (len > 0)
	    doreply();
	return 0;
	break;

    default:
	rtpp_log_write(RTPP_LOG_ERR, cf->glog, "unknown command");
	ecode = 3;
	goto goterror;
    }
    call_id = argv[1];
    if (request != 0 || response != 0 || play != 0) {
	if (argc < 5 || argc > 6) {
	    rtpp_log_write(RTPP_LOG_ERR, cf->glog, "command syntax error");
	    ecode = 4;
	    goto goterror;
	}
	from_tag = argv[4];
	to_tag = argv[5];
	if (play != 0 && argv[0][1] != '\0')
	    play = atoi(argv[0] + 1);
    }
    if (delete != 0 || record != 0 || noplay != 0) {
	if (argc < 3 || argc > 4) {
	    rtpp_log_write(RTPP_LOG_ERR, cf->glog, "command syntax error");
	    ecode = 1;
	    goto goterror;
	}
	from_tag = argv[2];
	to_tag = argv[3];
    }
    if (delete != 0 || record != 0 || noplay !=0) {
	/* D, R and S commands don't take any modifiers */
	if (argv[0][1] != '\0') {
	    rtpp_log_write(RTPP_LOG_ERR, cf->glog, "command syntax error");
	    ecode = 1;
	    goto goterror;
	}
    }
    if (request != 0 || response != 0 || delete != 0) {
	addr = argv[2];
	port = argv[3];
	/* Process additional command modifiers */
	external = 1;
	/* In bridge mode all clients are assumed to be asymmetric */
	asymmetric = (cf->bmode != 0) ? 1 : 0;
	pf = AF_INET;
	weak = 0;
	for (cp = argv[0] + 1; *cp != '\0'; cp++) {
	    switch (*cp) {
	    case 'a':
	    case 'A':
		asymmetric = 1;
		break;

	    case 'e':
	    case 'E':
		if (lidx < 0) {
		    rtpp_log_write(RTPP_LOG_ERR, cf->glog, "command syntax error");
		    ecode = 1;
		    goto goterror;
		}
		lia[lidx] = cf->bindaddr[1];
		lidx--;
		break;

	    case 'i':
	    case 'I':
		if (lidx < 0) {
		    rtpp_log_write(RTPP_LOG_ERR, cf->glog, "command syntax error");
		    ecode = 1;
		    goto goterror;
		}
		lia[lidx] = cf->bindaddr[0];
		lidx--;
		break;

	    case '6':
		pf = AF_INET6;
		break;

	    case 's':
	    case 'S':
		asymmetric = 0;
		break;

	    case 'w':
	    case 'W':
		weak = 1;
		break;

	    case 'z':
	    case 'Z':
		requested_nsamples = (strtol(cp + 1, &cp, 10) / 10) * 80;
		if (requested_nsamples <= 0) {
		    rtpp_log_write(RTPP_LOG_ERR, cf->glog, "command syntax error");
		    ecode = 1;
		    goto goterror;
		}
		cp--;
		break;

	    default:
		rtpp_log_write(RTPP_LOG_ERR, cf->glog, "unknown command modifier `%c'",
		  *cp);
		break;
	    }
	}
	if (delete == 0 && addr != NULL && port != NULL && strlen(addr) >= 7) {
	    struct sockaddr_storage tia;

	    if ((n = resolve(sstosa(&tia), pf, addr, port,
	      AI_NUMERICHOST)) == 0) {
		if (!ishostnull(sstosa(&tia))) {
		    for (i = 0; i < 2; i++) {
			ia[i] = malloc(SS_LEN(&tia));
			if (ia[i] == NULL) {
			    ecode = 5;
			    goto nomem;
			}
			memcpy(ia[i], &tia, SS_LEN(&tia));
		    }
		    /* Set port for RTCP, will work both for IPv4 and IPv6 */
		    n = ntohs(satosin(ia[1])->sin_port);
		    satosin(ia[1])->sin_port = htons(n + 1);
		}
	    } else {
		rtpp_log_write(RTPP_LOG_ERR, cf->glog, "getaddrinfo: %s",
		  gai_strerror(n));
	    }
	}
    }

    lport = 0;
    pidx = 1;
    ndeleted = 0;
    for (i = 1; i < cf->nsessions; i++) {
        spa = cf->sessions[i];
	if (spa == NULL || spa->sidx[0] != i || spa->rtcp == NULL ||
	  spa->call_id == NULL || strcmp(spa->call_id, call_id) != 0)
	    continue;
	medianum = 0;
	if ((cmpr1 = compare_session_tags(spa->tag, from_tag, &medianum)) != 0)
	{
	    i = (request == 0) ? 1 : 0;
	    cmpr = cmpr1;
	} else if (to_tag != NULL &&
	  (cmpr1 = compare_session_tags(spa->tag, to_tag, &medianum)) != 0)
	{
	    i = (request == 0) ? 0 : 1;
	    cmpr = cmpr1;
	} else
	    continue;

	if (delete != 0) {
	    if (weak)
		spa->weak[i] = 0;
	    else
		spa->strong = 0;
	    /*
	     * This seems to be stable from reiterations, the only side
	     * effect is less efficient work.
	     */
	    if (spa->strong || spa->weak[0] || spa->weak[1]) {
		rtpp_log_write(RTPP_LOG_INFO, spa->log,
		    "delete: medianum=%u: removing %s flag, seeing flags to"
		    " continue session (strong=%d, weak=%d/%d)",
		    medianum,
		    weak ? ( i ? "weak[1]" : "weak[0]" ) : "strong",
		    spa->strong, spa->weak[0], spa->weak[1]);
		/* Skipping to next possible stream for this call */
		++ndeleted;
		continue;
	    }
	    rtpp_log_write(RTPP_LOG_INFO, spa->log,
	      "forcefully deleting session %d on ports %d/%d",
	      medianum, spa->ports[0], spa->ports[1]);
	    remove_session(cf, spa);
	    if (cmpr == 2) {
		++ndeleted;
		continue;
	    }
	    goto do_ok;
	}

	if (play != 0 || noplay != 0) {
	    if (spa->rtps[i] != NULL) {
		rtp_server_free(spa->rtps[i]);
		spa->rtps[i] = NULL;
		rtpp_log_write(RTPP_LOG_INFO, spa->log,
	          "stopping player at port %d", spa->ports[i]);
		if (spa->rtps[0] == NULL && spa->rtps[1] == NULL) {
		    assert(cf->rtp_servers[spa->sridx] == spa);
		    cf->rtp_servers[spa->sridx] = NULL;
		    spa->sridx = -1;
		}
	    }
	    if (play == 0)
		goto do_ok;
	}

	if (play != 0) {
	    while (*codecs != '\0') {
		n = strtol(codecs, &cp, 10);
		if (cp == codecs)
		    break;
		codecs = cp;
		if (*codecs != '\0')
		    codecs++;
		spa->rtps[i] = rtp_server_new(pname, n, play);
		if (spa->rtps[i] == NULL)
		    continue;
		rtpp_log_write(RTPP_LOG_INFO, spa->log,
		  "%d times playing prompt %s codec %d", play, pname, n);
		if (spa->sridx == -1)
		    append_server(cf, spa);
		goto do_ok;
	    }
	    rtpp_log_write(RTPP_LOG_ERR, spa->log, "can't create player");
	    ecode = 6;
	    goto goterror;
	}

	if (record != 0) {
	    if (cf->rdir != NULL) {
	        if (spa->rrcs[i] == NULL) {
		    spa->rrcs[i] = ropen(cf, spa, i);
	            rtpp_log_write(RTPP_LOG_INFO, spa->log,
	              "starting recording RTP session on port %d", spa->ports[i]);
	        }
	        if (spa->rtcp->rrcs[i] == NULL && cf->rrtcp != 0) {
		    spa->rtcp->rrcs[i] = ropen(cf, spa->rtcp, i);
	            rtpp_log_write(RTPP_LOG_INFO, spa->log,
	              "starting recording RTCP session on port %d", spa->rtcp->ports[i]);
	        }
	    }
	    goto do_ok;
	}

	if (spa->fds[i] == -1) {
	    j = ishostseq(cf->bindaddr[0], spa->laddr[i]) ? 0 : 1;
	    if (create_listener(cf, spa->laddr[i], cf->nextport[j],
	      &lport, fds) == -1) {
		rtpp_log_write(RTPP_LOG_ERR, spa->log, "can't create listener");
		ecode = 7;
		goto goterror;
	    }
	    cf->nextport[j] = lport + 2;
	    assert(spa->fds[i] == -1);
	    spa->fds[i] = fds[0];
	    assert(spa->rtcp->fds[i] == -1);
	    spa->rtcp->fds[i] = fds[1];
	    spa->ports[i] = lport;
	    spa->rtcp->ports[i] = lport + 1;
	    spa->complete = spa->rtcp->complete = 1;
	    append_session(cf, spa, i);
	    append_session(cf, spa->rtcp, i);
	}
	if (weak)
	    spa->weak[i] = 1;
	else if (response == 0)
	    spa->strong = 1;
	lport = spa->ports[i];
	lia[0] = spa->laddr[i];
	pidx = (i == 0) ? 1 : 0;
	spa->ttl = cf->max_ttl;
	if (response == 0) {
		rtpp_log_write(RTPP_LOG_INFO, spa->log,
		  "adding %s flag to existing session, new=%d/%d/%d",
		  weak ? ( i ? "weak[1]" : "weak[0]" ) : "strong",
		  spa->strong, spa->weak[0], spa->weak[1]);
	}
	rtpp_log_write(RTPP_LOG_INFO, spa->log,
	  "lookup on ports %d/%d, session timer restarted", spa->ports[0],
	  spa->ports[1]);
	goto writeport;
    }
    if (delete != 0 && ndeleted != 0) {
	/*
	 * Multiple stream deleting stops here because we had to
	 * iterate full list.
	 */
	goto do_ok;
    }
    rname = NULL;
    if (delete != 0)
        rname = "delete";
    if (play != 0)
        rname = "play";
    if (noplay != 0)
	rname = "noplay";
    if (record != 0)
        rname = "record";
    if (response != 0)
        rname = "lookup";
    if (rname != NULL) {
	rtpp_log_write(RTPP_LOG_INFO, cf->glog,
	  "%s request failed: session %s, tags %s/%s not found", rname,
	  call_id, from_tag, to_tag != NULL ? to_tag : "NONE");
	if (response != 0) {
	    pidx = -1;
	    goto writeport;
	}
	ecode = 8;
	goto goterror;
    }

    rtpp_log_write(RTPP_LOG_INFO, cf->glog,
	"new session %s, tag %s requested, type %s",
	call_id, from_tag, weak ? "weak" : "strong");

    j = ishostseq(cf->bindaddr[0], lia[0]) ? 0 : 1;
    if (create_listener(cf, cf->bindaddr[j], cf->nextport[j], &lport,
      fds) == -1) {
	rtpp_log_write(RTPP_LOG_ERR, cf->glog, "can't create listener");
	ecode = 10;
	goto goterror;
    }
    cf->nextport[j] = lport + 2;

    /* Session creation. If creation is requested with weak flag,
     * set weak[0].
     */
    spa = malloc(sizeof(*spa));
    if (spa == NULL) {
    	ecode = 11;
	goto nomem;
    }
    /* spb is RTCP twin session for this one. */
    spb = malloc(sizeof(*spb));
    if (spb == NULL) {
	ecode = 12;
	goto nomem;
    }
    memset(spa, 0, sizeof(*spa));
    memset(spb, 0, sizeof(*spb));
    for (i = 0; i < 2; i++)
	spa->fds[i] = spb->fds[i] = -1;
    spa->call_id = strdup(call_id);
    if (spa->call_id == NULL) {
	ecode = 13;
	goto nomem;
    }
    spb->call_id = spa->call_id;
    spa->tag = strdup(from_tag);
    if (spa->tag == NULL) {
	ecode = 14;
	goto nomem;
    }
    spb->tag = spa->tag;
    for (i = 0; i < 2; i++) {
	spa->rrcs[i] = NULL;
	spb->rrcs[i] = NULL;
	spa->laddr[i] = lia[i];
	spb->laddr[i] = lia[i];
    }
    spa->strong = spa->weak[0] = spa->weak[1] = 0;
    if (weak)
	spa->weak[0] = 1;
    else
	spa->strong = 1;
    assert(spa->fds[0] == -1);
    spa->fds[0] = fds[0];
    assert(spb->fds[0] == -1);
    spb->fds[0] = fds[1];
    spa->ports[0] = lport;
    spb->ports[0] = lport + 1;
    spa->ttl = cf->max_ttl;
    spb->ttl = -1;
    spa->log = rtpp_log_open("rtpproxy", spa->call_id, 0);
    spb->log = spa->log;
    spa->rtcp = spb;
    spb->rtcp = NULL;
    spa->rtp = NULL;
    spb->rtp = spa;
    spa->sridx = spb->sridx = -1;

    append_session(cf, spa, 0);
    append_session(cf, spa, 1);
    append_session(cf, spb, 0);
    append_session(cf, spb, 1);

    cf->sessions_created++;

    rtpp_log_write(RTPP_LOG_INFO, spa->log, "new session on a port %d created, "
      "tag %s", lport, from_tag);

writeport:
    if (pidx >= 0) {
	if (ia[0] != NULL && ia[1] != NULL) {
	    /* If address is different from one that recorded update it */
	    if (!(spa->addr[pidx] != NULL &&
	      SA_LEN(ia[0]) == SA_LEN(spa->addr[pidx]) &&
	      memcmp(ia[0], spa->addr[pidx], SA_LEN(ia[0])) == 0)) {
		rtpp_log_write(RTPP_LOG_INFO, spa->log, "pre-filling %s's address "
		  "with %s:%s", (pidx == 0) ? "callee" : "caller", addr, port);
		if (spa->addr[pidx] != NULL)
		    free(spa->addr[pidx]);
		spa->addr[pidx] = ia[0];
		ia[0] = NULL;
	    }
	    if (!(spa->rtcp->addr[pidx] != NULL &&
	      SA_LEN(ia[1]) == SA_LEN(spa->rtcp->addr[pidx]) &&
	      memcmp(ia[1], spa->rtcp->addr[pidx], SA_LEN(ia[1])) == 0)) {
		if (spa->rtcp->addr[pidx] != NULL)
		    free(spa->rtcp->addr[pidx]);
		spa->rtcp->addr[pidx] = ia[1];
		ia[1] = NULL;
	    }
	}
	spa->asymmetric[pidx] = spa->rtcp->asymmetric[pidx] = asymmetric;
	spa->canupdate[pidx] = spa->rtcp->canupdate[pidx] = NOT(asymmetric);
	if (request != 0 || response != 0) {
	    if (requested_nsamples > 0) {
		rtpp_log_write(RTPP_LOG_INFO, spa->log, "RTP packets from %s "
		  "will be resized to %d milliseconds",
		  (pidx == 0) ? "callee" : "caller", requested_nsamples / 8);
	    } else if (spa->resizers[pidx].output_nsamples > 0) {
		rtpp_log_write(RTPP_LOG_INFO, spa->log, "Resizing of RTP "
		  "packets from %s has been disabled",
		  (pidx == 0) ? "callee" : "caller");
	    }
	    spa->resizers[pidx].output_nsamples = requested_nsamples;
	}
    }
    for (i = 0; i < 2; i++)
	if (ia[i] != NULL)
	    free(ia[i]);
    cp = buf;
    len = 0;
    if (cookie != NULL) {
	len = sprintf(cp, "%s ", cookie);
	cp += len;
    }
    if (lia[0] == NULL || ishostnull(lia[0]))
	len += sprintf(cp, "%d\n", lport);
    else
	len += sprintf(cp, "%d %s%s\n", lport, addr2char(lia[0]),
	  (lia[0]->sa_family == AF_INET) ? "" : " 6");
doreply:
    doreply();
    return 0;

nomem:
    rtpp_log_write(RTPP_LOG_ERR, cf->glog, "can't allocate memory");
    for (i = 0; i < 2; i++)
	if (ia[i] != NULL)
	    free(ia[i]);
    if (spa != NULL) {
	if (spa->call_id != NULL)
	    free(spa->call_id);
	free(spa);
    }
    if (spb != NULL)
	free(spb);
    for (i = 0; i < 2; i++)
	if (fds[i] != -1)
	    close(fds[i]);
goterror:
    if (cookie != NULL)
	len = sprintf(buf, "%s E%d\n", cookie, ecode);
    else
	len = sprintf(buf, "E%d\n", ecode);
    goto doreply;
do_ok:
    if (cookie != NULL)
	len = sprintf(buf, "%s 0\n", cookie);
    else {
	strcpy(buf, "0\n");
	len = 2;
    }
    goto doreply;
    return 0;
}

static void
usage(void)
{

    fprintf(stderr, "usage: rtpproxy [-2fv] [-l addr1[/addr2]] "
      "[-6 addr1[/addr2]] [-s path] [-t tos] [-r rdir [-S sdir]] [-T ttl] [-L nfiles] [-m port_min] [-M port_max]\n");
    exit(1);
}

static void
fatsignal(int sig)
{

    rtpp_log_write(RTPP_LOG_INFO, glog, "got signal %d", sig);
    exit(0);
}

static void
ehandler(void)
{

    unlink(cmd_sock);
    unlink(pid_file);
    rtpp_log_write(RTPP_LOG_INFO, glog, "rtpproxy ended");
    rtpp_log_close(glog);
}

static void 
init_config(struct cfg *cf, int argc, char **argv)
{
    int ch, i;
    struct rlimit lim;
    char *bh[2], *bh6[2];

    bh[0] = bh[1] = bh6[0] = bh6[1] = NULL;

    cf->port_min = PORT_MIN;
    cf->port_max = PORT_MAX;

    cf->max_ttl = SESSION_TIMEOUT;
    cf->tos = TOS;
    cf->rrtcp = 1;

    while ((ch = getopt(argc, argv, "vf2Rl:6:s:S:t:r:p:T:L:m:M:")) != -1)
	switch (ch) {
	case 'f':
	    cf->nodaemon = 1;
	    break;

	case 'l':
	    bh[0] = optarg;
	    bh[1] = strchr(bh[0], '/');
	    if (bh[1] != NULL) {
		*bh[1] = '\0';
		bh[1]++;
		cf->bmode = 1;
	    }
	    break;

	case '6':
	    bh6[0] = optarg;
	    bh6[1] = strchr(bh6[0], '/');
	    if (bh6[1] != NULL) {
		*bh6[1] = '\0';
		bh6[1]++;
		cf->bmode = 1;
	    }
	    break;

	case 's':
	    if (strncmp("udp:", optarg, 4) == 0) {
		cf->umode = 1;
		optarg += 4;
	    } else if (strncmp("udp6:", optarg, 5) == 0) {
		cf->umode = 6;
		optarg += 5;
	    } else if (strncmp("unix:", optarg, 5) == 0) {
		cf->umode = 0;
		optarg += 5;
	    }
	    cmd_sock = optarg;
	    break;

	case 't':
	    cf->tos = atoi(optarg);
	    break;

	case '2':
	    cf->dmode = 1;
	    break;

	case 'v':
	    printf("Basic version: %d\n", CPROTOVER);
	    for (i = 1; proto_caps[i].pc_id != NULL; ++i) {
		printf("Extension %s: %s\n", proto_caps[i].pc_id,
		    proto_caps[i].pc_description);
	    }
	    exit(0);
	    break;

	case 'r':
	    cf->rdir = optarg;
	    break;

	case 'S':
	    cf->sdir = optarg;
	    break;

	case 'R':
	    cf->rrtcp = 0;
	    break;

	case 'p':
	    pid_file = optarg;
	    break;

	case 'T':
	    cf->max_ttl = atoi(optarg);
	    break;

	case 'L':
	    lim.rlim_cur = lim.rlim_max = atoi(optarg);
	    if (setrlimit(RLIMIT_NOFILE, &lim) != 0)
		err(1, "setrlimit");
	    break;

	case 'm':
	    cf->port_min = atoi(optarg);
	    break;

	case 'M':
	    cf->port_max = atoi(optarg);
	    break;

	case '?':
	default:
	    usage();
	}
    if (cf->rdir == NULL && cf->sdir != NULL)
        errx(1, "-S switch requires -r switch");

    if (cf->port_min <= 0 || cf->port_min > 65535)
	errx(1, "invalid value of the port_min argument, "
	  "not in the range 1-65535");
    if (cf->port_max <= 0 || cf->port_max > 65535)
	errx(1, "invalid value of the port_max argument, "
	  "not in the range 1-65535");
    if (cf->port_min > cf->port_max)
	errx(1, "port_min should be less than port_max");

    /* make sure that port_min and port_max are even */
    if ((cf->port_min % 2) != 0)
	cf->port_min++;
    if ((cf->port_max % 2) != 0)
	cf->port_max--;

    cf->nextport[0] = cf->nextport[1] = cf->port_min;
    cf->sessions = malloc((sizeof cf->sessions[0]) *
      (((cf->port_max - cf->port_min + 1) * 2) + 1));
    cf->rtp_servers =  malloc((sizeof cf->rtp_servers[0]) *
      (((cf->port_max - cf->port_min + 1) * 2) + 1));
    cf->pfds = malloc((sizeof cf->pfds[0]) *
      (((cf->port_max - cf->port_min + 1) * 2) + 1));

    if (bh[0] == NULL && bh[1] == NULL && bh6[0] == NULL && bh6[1] == NULL) {
	if (cf->umode != 0)
	    errx(1, "explicit binding address has to be specified in UDP "
	      "command mode");
	bh[0] = "*";
    }

    for (i = 0; i < 2; i++) {
	if (bh[i] != NULL && *bh[i] == '\0')
	    bh[i] = NULL;
	if (bh6[i] != NULL && *bh6[i] == '\0')
	    bh6[i] = NULL;
    }

    i = ((bh[0] == NULL) ? 0 : 1) + ((bh[1] == NULL) ? 0 : 1) +
      ((bh6[0] == NULL) ? 0 : 1) + ((bh6[1] == NULL) ? 0 : 1);
    if (cf->bmode != 0) {
	if (bh[0] != NULL && bh6[0] != NULL)
	    errx(1, "either IPv4 or IPv6 should be configured for external "
	      "interface in bridging mode, not both");
	if (bh[1] != NULL && bh6[1] != NULL)
	    errx(1, "either IPv4 or IPv6 should be configured for internal "
	      "interface in bridging mode, not both");
	if (i != 2)
	    errx(1, "incomplete configuration of the bridging mode - exactly "
	      "2 listen addresses required, %d provided", i);
    } else if (i != 1) {
	errx(1, "exactly 1 listen addresses required, %d provided", i);
    }

    for (i = 0; i < 2; i++) {
	cf->bindaddr[i] = NULL;
	if (bh[i] != NULL) {
	    cf->bindaddr[i] = malloc(sizeof(struct sockaddr_storage));
	    setbindhost(cf->bindaddr[i], AF_INET, bh[i], SERVICE);
	    continue;
	}
	if (bh6[i] != NULL) {
	    cf->bindaddr[i] = malloc(sizeof(struct sockaddr_storage));
	    setbindhost(cf->bindaddr[i], AF_INET6, bh6[i], SERVICE);
	    continue;
	}
    }
    if (cf->bindaddr[0] == NULL) {
	cf->bindaddr[0] = cf->bindaddr[1];
	cf->bindaddr[1] = NULL;
    }
}

static int
init_controlfd(struct cfg *cf)
{
    struct sockaddr_un ifsun;
    struct sockaddr_storage ifsin;
    char *cp;
    int i, controlfd, flags;

    if (cf->umode == 0) {
	unlink(cmd_sock);
	memset(&ifsun, '\0', sizeof ifsun);
#if !defined(__linux__) && !defined(__solaris__)
	ifsun.sun_len = strlen(cmd_sock);
#endif
	ifsun.sun_family = AF_LOCAL;
	strcpy(ifsun.sun_path, cmd_sock);
	controlfd = socket(PF_LOCAL, SOCK_STREAM, 0);
	if (controlfd == -1)
	    err(1, "can't create socket");
	setsockopt(controlfd, SOL_SOCKET, SO_REUSEADDR, &controlfd,
	  sizeof controlfd);
	if (bind(controlfd, sstosa(&ifsun), sizeof ifsun) < 0)
	    err(1, "can't bind to a socket");
	if (listen(controlfd, 32) != 0)
	    err(1, "can't listen on a socket");
    } else {
	cp = strrchr(cmd_sock, ':');
	if (cp != NULL) {
	    *cp = '\0';
	    cp++;
	}
	if (cp == NULL || *cp == '\0')
	    cp = CPORT;
	i = (cf->umode == 6) ? AF_INET6 : AF_INET;
	setbindhost(sstosa(&ifsin), i, cmd_sock, cp);
	controlfd = socket(i, SOCK_DGRAM, 0);
	if (controlfd == -1)
	    err(1, "can't create socket");
	if (bind(controlfd, sstosa(&ifsin), SS_LEN(&ifsin)) < 0)
	    err(1, "can't bind to a socket");
    }
    flags = fcntl(controlfd, F_GETFL);
    fcntl(controlfd, F_SETFL, flags | O_NONBLOCK);

    return controlfd;
}

static void
process_rtp_servers(struct cfg *cf, double ctime)
{
    int j, k, sidx, len, skipfd;
    struct rtpp_session *sp;

    skipfd = 0;
    for (j = 0; j < cf->rtp_nsessions; j++) {
        sp = cf->rtp_servers[j];
        if (sp == NULL) {
            skipfd++;
            continue;
        }
        if (skipfd > 0) {
            cf->rtp_servers[j - skipfd] = cf->rtp_servers[j];
            sp->sridx = j - skipfd;
        }
        for (sidx = 0; sidx < 2; sidx++) {
            if (sp->rtps[sidx] == NULL || sp->addr[sidx] == NULL)
                continue;
            while ((len = rtp_server_get(sp->rtps[sidx], ctime)) != RTPS_LATER) {
                if (len == RTPS_EOF) {
                    rtp_server_free(sp->rtps[sidx]);
                    sp->rtps[sidx] = NULL;
                    if (sp->rtps[0] == NULL && sp->rtps[1] == NULL) {
                        assert(cf->rtp_servers[sp->sridx] == sp);
                        cf->rtp_servers[sp->sridx] = NULL;
                        sp->sridx = -1;
                    }
                    break;
                }
                for (k = (cf->dmode && len < LBR_THRS) ? 2 : 1; k > 0; k--) {
                    sendto(sp->fds[sidx], sp->rtps[sidx]->buf, len, 0,
                      sp->addr[sidx], SA_LEN(sp->addr[sidx]));
                }
            }
        }
    }
    cf->rtp_nsessions -= skipfd;
}

static void
rxmit_packets(struct cfg *cf, struct rtpp_session *sp, int ridx,
  double ctime)
{
    int ndrain, i, port;
    struct rtp_packet *packet = NULL;

    /* Repeat since we may have several packets queued on the same socket */
    for (ndrain = 0; ndrain < 5; ndrain++) {
	if (packet != NULL)
	    rtp_packet_free(packet);

	packet = rtp_recv(sp->fds[ridx]);
	if (packet == NULL)
	    break;
	packet->rtime = ctime;

	i = 0;
	if (sp->addr[ridx] != NULL) {
	    /* Check that the packet is authentic, drop if it isn't */
	    if (sp->asymmetric[ridx] == 0) {
		if (memcmp(sp->addr[ridx], &packet->raddr, packet->rlen) != 0) {
		    if (sp->canupdate[ridx] == 0) {
			/*
			 * Continue, since there could be good packets in
			 * queue.
			 */
			continue;
		    }
		    /* Signal that an address have to be updated */
		    i = 1;
		}
	    } else {
		/*
		 * For asymmetric clients don't check
		 * source port since it may be different.
		 */
		if (!ishostseq(sp->addr[ridx], sstosa(&packet->raddr)))
		    /*
		     * Continue, since there could be good packets in
		     * queue.
		     */
		    continue;
	    }
	    sp->pcount[ridx]++;
	} else {
	    sp->pcount[ridx]++;
	    sp->addr[ridx] = malloc(packet->rlen);
	    if (sp->addr[ridx] == NULL) {
		sp->pcount[3]++;
		rtpp_log_write(RTPP_LOG_ERR, sp->log,
		  "can't allocate memory for remote address - "
		  "removing session");
		remove_session(cf, GET_RTP(sp));
		/* Break, sp is invalid now */
		break;
	    }
	    /* Signal that an address have to be updated. */
	    i = 1;
	}

	/* Update recorded address if it's necessary. */
	if (i != 0) {
	    memcpy(sp->addr[ridx], &packet->raddr, packet->rlen);
	    sp->canupdate[ridx] = 0;

	    port = ntohs(satosin(&packet->raddr)->sin_port);

	    rtpp_log_write(RTPP_LOG_INFO, sp->log,
	      "%s's address filled in: %s:%d (%s)",
	      (ridx == 0) ? "callee" : "caller",
	      addr2char(sstosa(&packet->raddr)), port,
	      (sp->rtp == NULL) ? "RTP" : "RTCP");

	    /*
	     * Check if we have updated RTP while RTCP is still
	     * empty or contains address that differs from one we
	     * used when updating RTP. Try to guess RTCP if so,
	     * should be handy for non-NAT'ed clients, and some
	     * NATed as well.
	     */
	    if (sp->rtcp != NULL && (sp->rtcp->addr[ridx] == NULL ||
	      !ishostseq(sp->rtcp->addr[ridx], sstosa(&packet->raddr)))) {
		if (sp->rtcp->addr[ridx] == NULL) {
		    sp->rtcp->addr[ridx] = malloc(packet->rlen);
		    if (sp->rtcp->addr[ridx] == NULL) {
			sp->pcount[3]++;
			rtpp_log_write(RTPP_LOG_ERR, sp->log,
			  "can't allocate memory for remote address - "
			  "removing session");
			remove_session(cf, sp);
			/* Break, sp is invalid now */
			break;
		    }
		}
		memcpy(sp->rtcp->addr[ridx], &packet->raddr, packet->rlen);
		satosin(sp->rtcp->addr[ridx])->sin_port = htons(port + 1);
		/* Use guessed value as the only true one for asymmetric clients */
		sp->rtcp->canupdate[ridx] = NOT(sp->rtcp->asymmetric[ridx]);
		rtpp_log_write(RTPP_LOG_INFO, sp->log, "guessing RTCP port "
		  "for %s to be %d",
		  (ridx == 0) ? "callee" : "caller", port + 1);
	    }
	}

	if (sp->resizers[ridx].output_nsamples > 0)
	    rtp_resizer_enqueue(&sp->resizers[ridx], &packet);
	if (packet != NULL)
	    send_packet(cf, sp, ridx, packet);
    }

    if (packet != NULL)
	rtp_packet_free(packet);
}

static void
send_packet(struct cfg *cf, struct rtpp_session *sp, int ridx,
  struct rtp_packet *packet)
{
    int i, sidx;

    GET_RTP(sp)->ttl = cf->max_ttl;

    /* Select socket for sending packet out. */
    sidx = (ridx == 0) ? 1 : 0;

    /*
     * Check that we have some address to which packet is to be
     * sent out, drop otherwise.
     */
    if (sp->addr[sidx] == NULL || GET_RTP(sp)->rtps[sidx] != NULL) {
        sp->pcount[3]++;
    } else {
	sp->pcount[2]++;
	for (i = (cf->dmode && packet->size < LBR_THRS) ? 2 : 1; i > 0; i--) {
	    sendto(sp->fds[sidx], packet->buf, packet->size, 0, sp->addr[sidx],
	      SA_LEN(sp->addr[sidx]));
	}
    }

    if (sp->rrcs[ridx] != NULL && GET_RTP(sp)->rtps[ridx] == NULL)
        rwrite(sp, sp->rrcs[ridx], packet);
}

static void
process_rtp(struct cfg *cf, double ctime)
{
    int readyfd, skipfd, ridx;
    struct rtpp_session *sp;
    struct rtp_packet *packet;

    /* Relay RTP/RTCP */
    skipfd = 0;
    for (readyfd = 1; readyfd < cf->nsessions; readyfd++) {
        if (cf->pfds[readyfd].fd == -1) {
	    /* Deleted session, count and move one */
            skipfd++;
            continue;
        }

	/* Find index of the call leg within a session */
	sp = cf->sessions[readyfd];
	for (ridx = 0; ridx < 2; ridx++)
	    if (cf->pfds[readyfd].fd == sp->fds[ridx])
		break;
	/*
	 * Can't happen.
	 */
	assert(ridx != 2);

	/* Compact pfds[] and sessions[] by eliminating removed sessions */
	if (skipfd > 0) {
	    cf->pfds[readyfd - skipfd] = cf->pfds[readyfd];
	    cf->sessions[readyfd - skipfd] = cf->sessions[readyfd];
	    sp->sidx[ridx] = readyfd - skipfd;;
	}

	if (sp->complete != 0) {
	    if ((cf->pfds[readyfd].revents & POLLIN) != 0)
		rxmit_packets(cf, sp, ridx, ctime);
	    if (sp->resizers[ridx].output_nsamples > 0) {
		while ((packet = rtp_resizer_get(&sp->resizers[ridx], ctime)) != NULL) {
		    send_packet(cf, sp, ridx, packet);
		    rtp_packet_free(packet);
		}
	    }
	}
    }
    /* Trim any deleted sessions at the end */
    cf->nsessions -= skipfd;
}

static void
process_commands(struct cfg *cf)
{
    int controlfd, i;
    socklen_t rlen;
    struct sockaddr_un ifsun;

    if ((cf->pfds[0].revents & POLLIN) == 0)
	return;

    do {
	if (cf->umode == 0) {
	    rlen = sizeof(ifsun);
	    controlfd = accept(cf->pfds[0].fd, sstosa(&ifsun), &rlen);
	    if (controlfd == -1) {
		if (errno != EWOULDBLOCK)
		    rtpp_log_ewrite(RTPP_LOG_ERR, cf->glog,
		      "can't accept connection on control socket");
		break;
	    }
	} else {
	    controlfd = cf->pfds[0].fd;
	}
	i = handle_command(cf, controlfd);
	if (cf->umode == 0) {
	    close(controlfd);
	}
    } while (i == 0);
}

int
main(int argc, char **argv)
{
    int i, len, timeout, controlfd;
    double sptime, eptime, last_tick_time;
    unsigned long delay;
    struct cfg cf;
    char buf[256];

    memset(&cf, 0, sizeof(cf));

    init_config(&cf, argc, argv);

    controlfd = init_controlfd(&cf);

#if !defined(__solaris__)
    if (cf.nodaemon == 0) {
	if (daemon(0, 0) == -1)
	    err(1, "can't switch into daemon mode");
	    /* NOTREACHED */
    }
#endif

    atexit(ehandler);
    glog = cf.glog = rtpp_log_open("rtpproxy", NULL, LF_REOPEN);
    rtpp_log_write(RTPP_LOG_INFO, cf.glog, "rtpproxy started, pid %d", getpid());

    i = open(pid_file, O_WRONLY | O_CREAT | O_TRUNC, DEFFILEMODE);
    if (i >= 0) {
	len = sprintf(buf, "%u\n", getpid());
	write(i, buf, len);
	close(i);
    } else {
	rtpp_log_ewrite(RTPP_LOG_ERR, cf.glog, "can't open pidfile for writing");
    }

    signal(SIGHUP, fatsignal);
    signal(SIGINT, fatsignal);
    signal(SIGKILL, fatsignal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, fatsignal);
    signal(SIGXCPU, fatsignal);
    signal(SIGXFSZ, fatsignal);
    signal(SIGVTALRM, fatsignal);
    signal(SIGPROF, fatsignal);
    signal(SIGUSR1, fatsignal);
    signal(SIGUSR2, fatsignal);

    cf.pfds[0].fd = controlfd;
    cf.pfds[0].events = POLLIN;
    cf.pfds[0].revents = 0;

    cf.sessions[0] = NULL;
    cf.nsessions = 1;
    cf.rtp_nsessions = 0;

    sptime = 0;
    last_tick_time = 0;
    for (;;) {
	if (cf.rtp_nsessions > 0 || cf.nsessions > 1)
	    timeout = RTPS_TICKS_MIN;
	else
	    timeout = TIMETICK * 1000;
	eptime = getctime();
	delay = (eptime - sptime) * 1000000.0;
	if (delay < (1000000 / POLL_LIMIT)) {
	    usleep((1000000 / POLL_LIMIT) - delay);
	    sptime = getctime();
	} else {
	    sptime = eptime;
	}
	i = poll(cf.pfds, cf.nsessions, timeout);
	if (i < 0 && errno == EINTR)
	    continue;
	eptime = getctime();
	if (cf.rtp_nsessions > 0) {
	    process_rtp_servers(&cf, eptime);
	}
	process_rtp(&cf, eptime);
	if (i > 0) {
	    process_commands(&cf);
	}
	if (eptime > last_tick_time + TIMETICK) {
	    alarmhandler(&cf);
	    last_tick_time = eptime;
	}
    }

    exit(0);
}
