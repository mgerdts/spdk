/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *   Copyright (c) 2020, 2021 Mellanox Technologies LTD. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"

#if defined(__FreeBSD__)
#include <sys/event.h>
#define SPDK_KEVENT
#else
#include <sys/epoll.h>
#define SPDK_EPOLL
#endif

#if defined(__linux__)
#include <linux/errqueue.h>
#endif

#include <dlfcn.h>

#include <infiniband/verbs.h>
#include <mellanox/xlio_extra.h>

#include "spdk/log.h"
#include "spdk/pipe.h"
#include "spdk/sock.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk_internal/sock.h"

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32
#define VMA_PACKETS_BUF_SIZE 128
/* @todo: make pool sizes configurable */
#define RECV_ZCOPY_PACKETS_POOL_SIZE 1024
#define RECV_ZCOPY_BUFFERS_POOL_SIZE 4096

#if defined(SO_ZEROCOPY) && defined(MSG_ZEROCOPY)
#define SPDK_ZEROCOPY
#endif

#ifndef SPDK_ZEROCOPY
#define SPDK_ZEROCOPY
#endif

#ifndef SPDK_ZEROCOPY
#error "VMA requires zcopy"
#endif

#if 0
//TODO: remove when vma headers added
typedef enum {
	VMA_PDA_GET = 0,
	VMA_PDA_FREE = 1
} vma_pd_t;

struct vma_pd_attr {
	struct ibv_pd *pd_ptr;
	vma_pd_t	 vma_pd_oper;
};
#endif

enum {
	IOCTL_USER_ALLOC_TX = (1 << 0),
	IOCTL_USER_ALLOC_RX = (1 << 1),
	IOCTL_USER_ALLOC_TX_ZC = (1 << 2)
};

struct vma_sock_packet {
	struct vma_packet_t *vma_packet;
	int refs;
	void *vma_packet_id;
	STAILQ_ENTRY(vma_sock_packet) link;
};

struct vma_sock_buf {
	struct spdk_sock_buf sock_buf;
	struct vma_sock_packet *packet;
};

struct spdk_vma_sock {
	struct spdk_sock	base;
	int			fd;
	uint32_t		sendmsg_idx;
	struct ibv_pd *pd;
	bool			pending_recv;
	bool			zcopy;
	bool			recv_zcopy;
	int			so_priority;

	char			vma_packets_buf[VMA_PACKETS_BUF_SIZE];
	struct vma_sock_packet	*packets;
	STAILQ_HEAD(, vma_sock_packet)	free_packets;
	STAILQ_HEAD(, vma_sock_packet)	received_packets;
	size_t			cur_iov_idx;
	size_t			cur_offset;
	struct vma_sock_buf	*buffers;
	struct spdk_sock_buf	*free_buffers;

	TAILQ_ENTRY(spdk_vma_sock)	link;
};

struct spdk_vma_sock_group_impl {
	struct spdk_sock_group_impl	base;
	int				fd;
	TAILQ_HEAD(, spdk_vma_sock)	pending_recv;
};

static struct spdk_sock_impl_opts g_spdk_vma_sock_impl_opts = {
	.recv_buf_size = MIN_SO_RCVBUF_SIZE,
	.send_buf_size = MIN_SO_SNDBUF_SIZE,
	.enable_recv_pipe = false,
	.enable_zerocopy_send = true,
	.enable_quickack = false,
	.enable_placement_id = false,
	.enable_zerocopy_recv = false,
};

static int _sock_flush_ext(struct spdk_sock *sock);
static struct vma_api_t *g_vma_api;

static struct {
	int (*socket)(int domain, int type, int protocol);
	int (*bind)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
	int (*listen)(int sockfd, int backlog);
	int (*connect)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
	int (*accept)(int sockfd, struct sockaddr *restrict addr, socklen_t *restrict addrlen);
	int (*close)(int fd);
	ssize_t (*readv)(int fd, const struct iovec *iov, int iovcnt);
	ssize_t (*writev)(int fd, const struct iovec *iov, int iovcnt);
	ssize_t (*recv)(int sockfd, void *buf, size_t len, int flags);
	ssize_t (*recvmsg)(int sockfd, struct msghdr *msg, int flags);
	ssize_t (*sendmsg)(int sockfd, const struct msghdr *msg, int flags);
	int (*epoll_create1)(int flags);
	int (*epoll_ctl)(int epfd, int op, int fd, struct epoll_event *event);
	int (*epoll_wait)(int epfd, struct epoll_event *events, int maxevents, int timeout);
	int (*fcntl)(int fd, int cmd, ... /* arg */);
	int (*ioctl)(int fd, unsigned long request, ...);
	int (*getsockopt)(int sockfd, int level, int optname, void *restrict optval, socklen_t *restrict optlen);
	int (*setsockopt)(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
	int (*getsockname)(int sockfd, struct sockaddr *restrict addr, socklen_t *restrict addrlen);
	int (*getpeername)(int sockfd, struct sockaddr *restrict addr, socklen_t *restrict addrlen);
	int (*getaddrinfo)(const char *restrict node,
			   const char *restrict service,
			   const struct addrinfo *restrict hints,
			   struct addrinfo **restrict res);
	void (*freeaddrinfo)(struct addrinfo *res);
	const char *(*gai_strerror)(int errcode);
} g_vma_ops;

static int
vma_load(void)
{
	char *vma_path;
	void *vma_handle;

	vma_path = getenv("SPDK_VMA_PATH");
	if (!vma_path) {
		SPDK_NOTICELOG("VMA path is not specified. Not loading VMA socket implementation\n");
		return -1;
	}

	vma_handle = dlopen(vma_path, RTLD_NOW);
	if (!vma_handle) {
		SPDK_ERRLOG("Failed to load VMA library: path %s, error %s\n",
			    vma_path, dlerror());
		return -1;
	}

#define GET_SYM(sym) \
	g_vma_ops.sym = dlsym(vma_handle, #sym); \
	if (!g_vma_ops.sym) { \
		SPDK_ERRLOG("Failed to find symbol '%s'in VMA library\n", #sym); \
		dlclose(vma_handle); \
		return -1; \
	}

	GET_SYM(socket);
	GET_SYM(bind);
	GET_SYM(listen);
	GET_SYM(connect);
	GET_SYM(accept);
	GET_SYM(close);
	GET_SYM(readv);
	GET_SYM(writev);
	GET_SYM(recv);
	GET_SYM(recvmsg);
	GET_SYM(sendmsg);
	GET_SYM(epoll_create1);
	GET_SYM(epoll_ctl);
	GET_SYM(epoll_wait);
	GET_SYM(fcntl);
	GET_SYM(ioctl);
	GET_SYM(getsockopt);
	GET_SYM(setsockopt);
	GET_SYM(getsockname);
	GET_SYM(getpeername);
	GET_SYM(getaddrinfo);
	GET_SYM(freeaddrinfo);
	GET_SYM(gai_strerror);
#undef GET_SYM

	return 0;
}

static void *
spdk_vma_alloc(size_t size)
{
	return spdk_zmalloc(size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
}

static struct vma_api_t*
spdk_vma_get_api(void)
{
	struct vma_api_t *api_ptr = NULL;
	socklen_t len = sizeof(api_ptr);

	int err = g_vma_ops.getsockopt(-1, SOL_SOCKET, SO_VMA_GET_API, &api_ptr, &len);
	if (err < 0) {
		return NULL;
	}

	return api_ptr;
}

static int
vma_init(void)
{
	int rc;
#pragma pack(push, 1)
	struct {
		uint8_t flags;
		void *(*alloc_func)(size_t);
		void (*free_func)(void *);
	} data;
#pragma pack(pop)
	struct cmsghdr *cmsg;
	char cbuf[CMSG_SPACE(sizeof(data))];

	static_assert((sizeof(uint8_t) + sizeof(uintptr_t) +
		       sizeof(uintptr_t)) == sizeof(data),
		      "wrong vma ioctl data size.");

	/* Before init, g_vma_api must be NULL */
	assert(g_vma_api == NULL);

	g_vma_api = spdk_vma_get_api();
	if (!g_vma_api) {
		SPDK_ERRLOG("Failed to get VMA API\n");
		return -1;
	}
	SPDK_NOTICELOG("Got VMA API %p\n", g_vma_api);

	cmsg = (struct cmsghdr *)cbuf;
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = CMSG_XLIO_IOCTL_USER_ALLOC;
	cmsg->cmsg_len = CMSG_LEN(sizeof(data));
	data.flags = IOCTL_USER_ALLOC_RX;
	data.alloc_func = spdk_vma_alloc;
	data.free_func = spdk_free;
	memcpy(CMSG_DATA(cmsg), &data, sizeof(data));

	rc = g_vma_api->ioctl(cmsg, cmsg->cmsg_len);
	if (rc < 0) {
		SPDK_ERRLOG("vma_int rc %d (errno=%d)\n", rc, errno);
	}

	return rc;
}

static int
get_addr_str(struct sockaddr *sa, char *host, size_t hlen)
{
	const char *result = NULL;

	if (sa == NULL || host == NULL) {
		return -1;
	}

	switch (sa->sa_family) {
	case AF_INET:
		result = inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
				   host, hlen);
		break;
	case AF_INET6:
		result = inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
				   host, hlen);
		break;
	default:
		break;
	}

	if (result != NULL) {
		return 0;
	} else {
		return -1;
	}
}

#define __vma_sock(sock) (struct spdk_vma_sock *)sock
#define __vma_group_impl(group) (struct spdk_vma_sock_group_impl *)group

static int
vma_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
		 char *caddr, int clen, uint16_t *cport)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = g_vma_ops.getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return -1;
	}

	switch (sa.ss_family) {
	case AF_UNIX:
		/* Acceptable connection types that don't have IPs */
		return 0;
	case AF_INET:
	case AF_INET6:
		/* Code below will get IP addresses */
		break;
	default:
		/* Unsupported socket family */
		return -1;
	}

	rc = get_addr_str((struct sockaddr *)&sa, saddr, slen);
	if (rc != 0) {
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
		return -1;
	}

	if (sport) {
		if (sa.ss_family == AF_INET) {
			*sport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*sport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
	}

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = g_vma_ops.getpeername(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getpeername() failed (errno=%d)\n", errno);
		return -1;
	}

	rc = get_addr_str((struct sockaddr *)&sa, caddr, clen);
	if (rc != 0) {
		SPDK_ERRLOG("getnameinfo() failed (errno=%d)\n", errno);
		return -1;
	}

	if (cport) {
		if (sa.ss_family == AF_INET) {
			*cport = ntohs(((struct sockaddr_in *) &sa)->sin_port);
		} else if (sa.ss_family == AF_INET6) {
			*cport = ntohs(((struct sockaddr_in6 *) &sa)->sin6_port);
		}
	}

	return 0;
}

enum vma_sock_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

static int
vma_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	int rc;

	assert(sock != NULL);

	/* Set kernel buffer size to be at least MIN_SO_RCVBUF_SIZE */
	if (sz < MIN_SO_RCVBUF_SIZE) {
		sz = MIN_SO_RCVBUF_SIZE;
	}

	rc = g_vma_ops.setsockopt(sock->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static int
vma_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	int rc;

	assert(sock != NULL);

	if (sz < MIN_SO_SNDBUF_SIZE) {
		sz = MIN_SO_SNDBUF_SIZE;
	}

	rc = g_vma_ops.setsockopt(sock->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static inline struct ibv_pd *vma_get_pd(int fd)
{
	struct vma_pd_attr pd_attr_ptr = {};
	socklen_t len = sizeof(pd_attr_ptr);

	int err = g_vma_ops.getsockopt(fd, SOL_SOCKET, SO_VMA_PD, &pd_attr_ptr, &len);
	if (err < 0) {
		return NULL;
	}
	return pd_attr_ptr.ib_pd;
}

static struct spdk_vma_sock *
vma_sock_alloc(int fd, bool enable_zero_copy)
{
	struct spdk_vma_sock *sock;
#if defined(SPDK_ZEROCOPY) || defined(__linux__)
	int flag;
	int rc;
#endif

	sock = calloc(1, sizeof(*sock));
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	sock->fd = fd;

#if defined(SPDK_ZEROCOPY)
	flag = 1;

	if (enable_zero_copy && g_spdk_vma_sock_impl_opts.enable_zerocopy_send) {

		/* Try to turn on zero copy sends */
		rc = g_vma_ops.setsockopt(sock->fd, SOL_SOCKET, SO_ZEROCOPY, &flag, sizeof(flag));
		if (rc == 0) {
			sock->zcopy = true;
		} else {
			SPDK_ERRLOG("zcopy is not supported\n");
		}
	}

	SPDK_NOTICELOG("getting VMA pd\n");
	sock->pd = vma_get_pd(fd);
	if (!sock->pd) {
		SPDK_ERRLOG("Failed to get pd\n");
	} else {
//		assert(0);
		SPDK_NOTICELOG("!!!pd %p, context %p, dev %s handle %u\n", sock->pd, sock->pd->context,
			       sock->pd->context->device->name, sock->pd->handle);
	}

#endif

	if (enable_zero_copy && g_spdk_vma_sock_impl_opts.enable_zerocopy_recv) {
		int i;

		SPDK_NOTICELOG("Sock %d: use zerocopy recv\n", sock->fd);
		sock->recv_zcopy = true;

		sock->packets = calloc(RECV_ZCOPY_PACKETS_POOL_SIZE,
				       sizeof(struct vma_sock_packet));
		if (!sock->packets) {
			SPDK_ERRLOG("Failed to allocated packets pool for socket %d\n", fd);
			goto err_free_sock;
		}

		STAILQ_INIT(&sock->free_packets);
		for (i = 0; i < RECV_ZCOPY_PACKETS_POOL_SIZE; ++i) {
			STAILQ_INSERT_TAIL(&sock->free_packets, &sock->packets[i], link);
		}

		STAILQ_INIT(&sock->received_packets);

		sock->buffers = calloc(RECV_ZCOPY_BUFFERS_POOL_SIZE,
				       sizeof(struct vma_sock_buf));
		if (!sock->buffers) {
			SPDK_ERRLOG("Failed to allocated buffers pool for socket %d\n", fd);
			goto err_free_packets;
		}

		sock->free_buffers = &sock->buffers[0].sock_buf;
		for (i = 1; i < RECV_ZCOPY_BUFFERS_POOL_SIZE; ++i) {
			sock->buffers[i - 1].sock_buf.next = &sock->buffers[i].sock_buf;
		}
	}

#if defined(__linux__)
	flag = 1;

	if (g_spdk_vma_sock_impl_opts.enable_quickack) {
		rc = g_vma_ops.setsockopt(sock->fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
		if (rc != 0) {
			SPDK_ERRLOG("quickack was failed to set\n");
		}
	}
#endif

	return sock;

 err_free_packets:
	free(sock->packets);
 err_free_sock:
	free(sock);
	return NULL;
}

static bool
sock_is_loopback(int fd)
{
	struct ifaddrs *addrs, *tmp;
	struct sockaddr_storage sa = {};
	socklen_t salen;
	struct ifreq ifr = {};
	char ip_addr[256], ip_addr_tmp[256];
	int rc;
	bool is_loopback = false;

	salen = sizeof(sa);
	rc = g_vma_ops.getsockname(fd, (struct sockaddr *)&sa, &salen);
	if (rc != 0) {
		return is_loopback;
	}

	memset(ip_addr, 0, sizeof(ip_addr));
	rc = get_addr_str((struct sockaddr *)&sa, ip_addr, sizeof(ip_addr));
	if (rc != 0) {
		return is_loopback;
	}

	getifaddrs(&addrs);
	for (tmp = addrs; tmp != NULL; tmp = tmp->ifa_next) {
		if (tmp->ifa_addr && (tmp->ifa_flags & IFF_UP) &&
		    (tmp->ifa_addr->sa_family == sa.ss_family)) {
			memset(ip_addr_tmp, 0, sizeof(ip_addr_tmp));
			rc = get_addr_str(tmp->ifa_addr, ip_addr_tmp, sizeof(ip_addr_tmp));
			if (rc != 0) {
				continue;
			}

			if (strncmp(ip_addr, ip_addr_tmp, sizeof(ip_addr)) == 0) {
				memcpy(ifr.ifr_name, tmp->ifa_name, sizeof(ifr.ifr_name));
				g_vma_ops.ioctl(fd, SIOCGIFFLAGS, &ifr);
				if (ifr.ifr_flags & IFF_LOOPBACK) {
					is_loopback = true;
				}
				goto end;
			}
		}
	}

end:
	freeifaddrs(addrs);
	return is_loopback;
}

static struct spdk_sock *
vma_sock_create(const char *ip, int port,
		enum vma_sock_create_type type,
		struct spdk_sock_opts *opts)
{
	struct spdk_vma_sock *sock;
	char buf[MAX_TMPBUF];
	char portnum[PORTNUMLEN];
	char *p;
	struct addrinfo hints, *res, *res0;
	int fd, flag;
	int val = 1;
	int rc, sz;
	bool enable_zero_copy = true;

	assert(opts != NULL);

	if (ip == NULL) {
		return NULL;
	}
	if (ip[0] == '[') {
		snprintf(buf, sizeof(buf), "%s", ip + 1);
		p = strchr(buf, ']');
		if (p != NULL) {
			*p = '\0';
		}
		ip = (const char *) &buf[0];
	}

	snprintf(portnum, sizeof portnum, "%d", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICSERV;
	hints.ai_flags |= AI_PASSIVE;
	hints.ai_flags |= AI_NUMERICHOST;
	rc = g_vma_ops.getaddrinfo(ip, portnum, &hints, &res0);
	if (rc != 0) {
		SPDK_ERRLOG("getaddrinfo() failed %s (%d)\n", g_vma_ops.gai_strerror(rc), rc);
		return NULL;
	}

	/* try listen */
	fd = -1;
	for (res = res0; res != NULL; res = res->ai_next) {
retry:
		fd = g_vma_ops.socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (fd < 0) {
			/* error */
			continue;
		}

		sz = g_spdk_vma_sock_impl_opts.recv_buf_size;
		rc = g_vma_ops.setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
		if (rc) {
			/* Not fatal */
		}

		sz = g_spdk_vma_sock_impl_opts.send_buf_size;
		rc = g_vma_ops.setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		if (rc) {
			/* Not fatal */
		}

		rc = g_vma_ops.setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof val);
		if (rc != 0) {
			g_vma_ops.close(fd);
			/* error */
			continue;
		}
		rc = g_vma_ops.setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof val);
		if (rc != 0) {
			g_vma_ops.close(fd);
			/* error */
			continue;
		}

#if defined(SO_PRIORITY)
		if (opts->priority) {
			rc = g_vma_ops.setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &opts->priority, sizeof val);
			if (rc != 0) {
				g_vma_ops.close(fd);
				/* error */
				continue;
			}
		}
#endif

		if (res->ai_family == AF_INET6) {
			rc = g_vma_ops.setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val);
			if (rc != 0) {
				g_vma_ops.close(fd);
				/* error */
				continue;
			}
		}

		if (type == SPDK_SOCK_CREATE_LISTEN) {
			rc = g_vma_ops.bind(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("bind() failed at port %d, errno = %d\n", port, errno);
				switch (errno) {
				case EINTR:
					/* interrupted? */
					g_vma_ops.close(fd);
					goto retry;
				case EADDRNOTAVAIL:
					SPDK_ERRLOG("IP address %s not available. "
						    "Verify IP address in config file "
						    "and make sure setup script is "
						    "run before starting spdk app.\n", ip);
				/* FALLTHROUGH */
				default:
					/* try next family */
					g_vma_ops.close(fd);
					fd = -1;
					continue;
				}
			}
			/* bind OK */
			rc = g_vma_ops.listen(fd, 512);
			if (rc != 0) {
				SPDK_ERRLOG("listen() failed, errno = %d\n", errno);
				g_vma_ops.close(fd);
				fd = -1;
				break;
			}
		} else if (type == SPDK_SOCK_CREATE_CONNECT) {
			rc = g_vma_ops.connect(fd, res->ai_addr, res->ai_addrlen);
			if (rc != 0) {
				SPDK_ERRLOG("connect() failed, errno = %d\n", errno);
				/* try next family */
				g_vma_ops.close(fd);
				fd = -1;
				continue;
			}
		}

		flag = g_vma_ops.fcntl(fd, F_GETFL);
		if (g_vma_ops.fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0) {
			SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", fd, errno);
			g_vma_ops.close(fd);
			fd = -1;
			break;
		}
		break;
	}
	g_vma_ops.freeaddrinfo(res0);

	if (fd < 0) {
		return NULL;
	}

	/* Only enable zero copy for non-loopback sockets. */
	enable_zero_copy = opts->zcopy && !sock_is_loopback(fd);
	SPDK_NOTICELOG("allocating vma sock, zcopy %d\n", enable_zero_copy);

	sock = vma_sock_alloc(fd, enable_zero_copy);
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		g_vma_ops.close(fd);
		return NULL;
	}

	if (opts != NULL) {
		sock->so_priority = opts->priority;
	}
	return &sock->base;
}

static struct spdk_sock *
vma_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return vma_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN, opts);
}

static struct spdk_sock *
vma_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return vma_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT, opts);
}

static struct spdk_sock *
vma_sock_accept(struct spdk_sock *_sock)
{
	struct spdk_vma_sock		*sock = __vma_sock(_sock);
	struct sockaddr_storage		sa;
	socklen_t			salen;
	int				rc, fd;
	struct spdk_vma_sock		*new_sock;
	int				flag;

	memset(&sa, 0, sizeof(sa));
	salen = sizeof(sa);

	assert(sock != NULL);

	rc = g_vma_ops.accept(sock->fd, (struct sockaddr *)&sa, &salen);

	if (rc == -1) {
		return NULL;
	}

	fd = rc;

	flag = g_vma_ops.fcntl(fd, F_GETFL);
	if ((!(flag & O_NONBLOCK)) && (g_vma_ops.fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0)) {
		SPDK_ERRLOG("fcntl can't set nonblocking mode for socket, fd: %d (%d)\n", fd, errno);
		g_vma_ops.close(fd);
		return NULL;
	}

#if defined(SO_PRIORITY)
	/* The priority is not inherited, so call this function again */
	if (sock->base.opts.priority) {
		rc = g_vma_ops.setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &sock->base.opts.priority, sizeof(int));
		if (rc != 0) {
			g_vma_ops.close(fd);
			return NULL;
		}
	}
#endif

	/* Inherit the zero copy feature from the listen socket */
	new_sock = vma_sock_alloc(fd, sock->zcopy);
	if (new_sock == NULL) {
		g_vma_ops.close(fd);
		return NULL;
	}
	new_sock->so_priority = sock->base.opts.priority;

	return &new_sock->base;
}

static int
vma_sock_close(struct spdk_sock *_sock)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);

	assert(TAILQ_EMPTY(&_sock->pending_reqs));

	/* If the socket fails to close, the best choice is to
	 * leak the fd but continue to free the rest of the sock
	 * memory. */
	g_vma_ops.close(sock->fd);

	free(sock->packets);
	free(sock->buffers);
	free(sock);

	return 0;
}

#ifdef SPDK_ZEROCOPY
static int
_sock_check_zcopy(struct spdk_sock *sock)
{
	struct spdk_vma_sock *vsock = __vma_sock(sock);
	struct spdk_vma_sock_group_impl *group = __vma_group_impl(sock->group_impl);
	struct msghdr msgh = {};
	uint8_t buf[sizeof(struct cmsghdr) + sizeof(struct sock_extended_err)];
	ssize_t rc;
	struct sock_extended_err *serr;
	struct cmsghdr *cm;
	uint32_t idx;
	struct spdk_sock_request *req, *treq;
	bool found;

	msgh.msg_control = buf;
	msgh.msg_controllen = sizeof(buf);

	while (true) {
		rc = g_vma_ops.recvmsg(vsock->fd, &msgh, MSG_ERRQUEUE);

		if (rc < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) {
				return 0;
			}

			if (!TAILQ_EMPTY(&sock->pending_reqs)) {
				SPDK_ERRLOG("Attempting to receive from ERRQUEUE yielded error, but pending list still has orphaned entries\n");
			} else {
				SPDK_WARNLOG("Recvmsg yielded an error!\n");
			}
			return 0;
		}

		cm = CMSG_FIRSTHDR(&msgh);
		if (!cm || cm->cmsg_level != SOL_IP || cm->cmsg_type != IP_RECVERR) {
			SPDK_WARNLOG("Unexpected cmsg level or type!\n");
			return 0;
		}

		serr = (struct sock_extended_err *)CMSG_DATA(cm);
		if (serr->ee_errno != 0 || serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
			SPDK_WARNLOG("Unexpected extended error origin\n");
			return 0;
		}

		/* Most of the time, the pending_reqs array is in the exact
		 * order we need such that all of the requests to complete are
		 * in order, in the front. It is guaranteed that all requests
		 * belonging to the same sendmsg call are sequential, so once
		 * we encounter one match we can stop looping as soon as a
		 * non-match is found.
		 */
		for (idx = serr->ee_info; idx <= serr->ee_data; idx++) {
			found = false;

			TAILQ_FOREACH_SAFE(req, &sock->pending_reqs, internal.link, treq) {
				if (req->internal.offset == idx) {
					found = true;

					rc = spdk_sock_request_put(sock, req, 0);
					if (rc < 0) {
						return rc;
					}

				} else if (found) {
					break;
				}
			}

			/* If we reaped buffer reclaim notification and sock is not in pending_recv list yet,
			 * add it now. It allows to call socket callback and process completions */
			if (found && !vsock->pending_recv && group) {
				vsock->pending_recv = true;
				TAILQ_INSERT_TAIL(&group->pending_recv, vsock, link);
			}
		}
	}

	return 0;
}
#endif


static int
vma_sock_flush(struct spdk_sock *sock)
{
#ifdef SPDK_ZEROCOPY
	struct spdk_vma_sock *vsock = __vma_sock(sock);

	if (vsock->zcopy && !TAILQ_EMPTY(&sock->pending_reqs)) {
		_sock_check_zcopy(sock);
	}
#endif

	return _sock_flush_ext(sock);
}

static inline struct vma_packet_t *
next_packet(struct vma_packet_t *packet)
{
	return (struct vma_packet_t *)((char *)packet +
				       sizeof(struct vma_packet_t) +
				       packet->sz_iov * sizeof(struct iovec));
}

#ifdef DEBUG
static void
dump_packet(struct vma_sock_packet *packet)
{
	size_t i;

	for (i = 0; i < packet->vma_packet->sz_iov; ++i) {
		SPDK_DEBUGLOG(vma, "Packet %p: id %p, iov[%lu].len %lu\n",
			      packet, packet->vma_packet_id, i,
			      packet->vma_packet->iov[i].iov_len);
	}
}
#endif

static ssize_t
vma_sock_recvfrom_zcopy(struct spdk_vma_sock *sock)
{
	struct vma_packets_t *vma_packets;
	struct vma_packet_t *vma_packet;
	int flags = 0;
	int ret;
	size_t i;

	ret = g_vma_api->recvfrom_zcopy(sock->fd, sock->vma_packets_buf,
					sizeof(sock->vma_packets_buf), &flags, NULL, NULL);
	if (ret < 0) {
		if (spdk_unlikely(errno != EAGAIN && errno != EWOULDBLOCK)) {
			SPDK_ERRLOG("recvfrom_zcopy failed, errno %d\n", errno);
		}

		return ret;
	} else if (ret == 0) {
		SPDK_DEBUGLOG(vma, "recvfrom_zcopy failed, ret %d\n", ret);
		/* @todo: temporary workaround.
		 * Normally, 0 returned by recv function means that socket was disconnected.
		 * Looks like VMA does not follow this rule for zero copy receive.
		 * Need to check with VMA team
		 */
		errno = EAGAIN;
		return -1;
	}

	if (!(flags & MSG_VMA_ZCOPY)) {
		SPDK_WARNLOG("Zcopy receive was not performed. Got %d bytes.\n", ret);
		return -1;
	}

	vma_packets = (struct vma_packets_t *)sock->vma_packets_buf;
	SPDK_DEBUGLOG(vma, "Sock %d: got %lu packets, total %d bytes\n",
		      sock->fd, vma_packets->n_packet_num, ret);

	/* Wrap all VMA packets and link to received packets list */
	vma_packet = &vma_packets->pkts[0];
	for (i = 0; i < vma_packets->n_packet_num; ++i) {
		struct vma_sock_packet *packet = STAILQ_FIRST(&sock->free_packets);
		size_t j, len = 0;

		/* @todo: Filter out zero length packets.
		 * Check with VMA team why it happens.
		 */
		for (j = 0; j < vma_packet->sz_iov; ++j) {
			len += vma_packet->iov[j].iov_len;
		}

		if (len == 0) {
			int rc;

			SPDK_DEBUGLOG(vma, "Dropping zero length packet: id %p\n", vma_packet->packet_id);
			rc = g_vma_api->free_packets(sock->fd, vma_packet, 1);
			if (rc < 0) {
				SPDK_ERRLOG("Free VMA packets failed, ret %d, errno %d\n",
					    rc, errno);
			}
			vma_packet = next_packet(vma_packet);
			continue;
		}

		/* @todo: handle lack of free packets */
		assert(packet);
		STAILQ_REMOVE_HEAD(&sock->free_packets, link);
		/*
		 * @todo: VMA packet pointer is only valid till next
		 * recvfrom_zcopy. We should be done with iovs by that
		 * time, but we need packet_id to free the packet
		 * later. Need to save it somewhere.It is not clear if
		 * iovs are required to free the packet.
		 */
		packet->vma_packet = vma_packet;
		packet->vma_packet_id = vma_packet->packet_id;
		/*
		 * While the packet is in received list there is data
		 * to read from it.  To avoid free of packets with
		 * unread data we intialize reference counter to 1.
		 */
		packet->refs = 1;
		STAILQ_INSERT_TAIL(&sock->received_packets, packet, link);
#ifdef DEBUG
		SPDK_DEBUGLOG(vma, "Sock %d: packet %lu\n", sock->fd, i);
		dump_packet(packet);
#endif
		vma_packet = next_packet(vma_packet);
	}

	sock->cur_iov_idx = 0;
	sock->cur_offset = 0;

	return ret;
}

static void
vma_sock_free_packet(struct spdk_vma_sock *sock, struct vma_sock_packet *packet) {
	int ret;
	struct vma_packet_t vma_packet;

	SPDK_DEBUGLOG(vma, "Sock %d: free VMA packet %p\n",
		      sock->fd, packet->vma_packet->packet_id);
	assert(packet->refs == 0);
	/* @todo: How heavy is free_packets()? Maybe batch packets to free? */
	vma_packet.packet_id = packet->vma_packet_id;
	vma_packet.sz_iov = 0;
	ret = g_vma_api->free_packets(sock->fd, &vma_packet, 1);
	if (ret < 0) {
		SPDK_ERRLOG("Free VMA packets failed, ret %d, errno %d\n",
			    ret, errno);
	}

	STAILQ_INSERT_HEAD(&sock->free_packets, packet, link);
}

static void
packets_advance(struct spdk_vma_sock *sock, size_t len)
{
	SPDK_DEBUGLOG(vma, "Sock %d: advance packets by %lu bytes\n", sock->fd, len);
	while (len > 0) {
		struct vma_sock_packet *cur_packet = STAILQ_FIRST(&sock->received_packets);
		/* We don't allow to advance by more than we have data in packets */
		assert(cur_packet != NULL);
		struct iovec *iov = &cur_packet->vma_packet->iov[sock->cur_iov_idx];
		int iov_len = iov->iov_len - sock->cur_offset;

		if ((int)len < iov_len) {
			sock->cur_offset += len;
			len = 0;
		} else {
			len -= iov_len;

			/* Next iov */
			sock->cur_offset = 0;
			sock->cur_iov_idx++;
			if (sock->cur_iov_idx >= cur_packet->vma_packet->sz_iov) {
				/* Next packet */
				sock->cur_iov_idx = 0;
				STAILQ_REMOVE_HEAD(&sock->received_packets, link);
				if (--cur_packet->refs == 0) {
					vma_sock_free_packet(sock, cur_packet);
				}
			}
		}
	}

	assert(len == 0);
}

static size_t
packets_next_chunk(struct spdk_vma_sock *sock,
		   void **buf,
		   struct vma_sock_packet **packet,
		   size_t max_len)
{
	struct vma_sock_packet *cur_packet = STAILQ_FIRST(&sock->received_packets);

	while (cur_packet) {
		struct iovec *iov = &cur_packet->vma_packet->iov[sock->cur_iov_idx];
		size_t len = iov->iov_len - sock->cur_offset;

		if (len == 0) {
			/* VMA may return zero length iov. Skip to next in this case */
			sock->cur_offset = 0;
			sock->cur_iov_idx++;
			assert(sock->cur_iov_idx <= cur_packet->vma_packet->sz_iov);
			if (sock->cur_iov_idx >= cur_packet->vma_packet->sz_iov) {
				/* Next packet */
				sock->cur_iov_idx = 0;
				cur_packet = STAILQ_NEXT(cur_packet, link);
			}
			continue;
		}

		assert(max_len > 0);
		assert(len > 0);
		len = spdk_min(len, max_len);
		*buf = iov->iov_base + sock->cur_offset;
		*packet = cur_packet;
		return len;
	}

	return 0;
}

static int
readv_wrapper(struct spdk_vma_sock *sock, struct iovec *iovs, int iovcnt)
{
	int ret;

	if (sock->recv_zcopy) {
		int i;
		size_t offset = 0;

		if (STAILQ_EMPTY(&sock->received_packets)) {
			ret = vma_sock_recvfrom_zcopy(sock);
			if (ret <= 0) {
				SPDK_DEBUGLOG(vma, "Sock %d: readv_wrapper ret %d, errno %d\n",
					      sock->fd, ret, errno);
				return ret;
			}
		}

		assert(!STAILQ_EMPTY(&sock->received_packets));
		ret = 0;
		i = 0;
		while (i < iovcnt) {
			void *buf;
			size_t len;
			struct iovec *iov = &iovs[i];
			size_t iov_len = iov->iov_len - offset;
			struct vma_sock_packet *packet;

			len = packets_next_chunk(sock, &buf, &packet, iov_len);
			if (len == 0) {
				/* No more data */
				SPDK_DEBUGLOG(vma, "Sock %d: readv_wrapper ret %d\n", sock->fd, ret);
				return ret;
			}

			memcpy(iov->iov_base + offset, buf, len);
			packets_advance(sock, len);
			ret += len;
			offset += len;
			assert(offset <= iov->iov_len);
			if (offset == iov->iov_len) {
				offset = 0;
				i++;
			}
		}

		SPDK_DEBUGLOG(vma, "Sock %d: readv_wrapper ret %d\n", sock->fd, ret);
	} else {
		ret = g_vma_ops.readv(sock->fd, iovs, iovcnt);
	}

	return ret;
}

static ssize_t
vma_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);

	return readv_wrapper(sock, iov, iovcnt);
}

static ssize_t
vma_sock_recv(struct spdk_sock *sock, void *buf, size_t len)
{
	struct iovec iov[1];

	iov[0].iov_base = buf;
	iov[0].iov_len = len;

	return vma_sock_readv(sock, iov, 1);
}

static ssize_t
vma_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	int rc;

	/* In order to process a writev, we need to flush any asynchronous writes
	 * first. */
	rc = _sock_flush_ext(_sock);
	if (rc < 0) {
		return rc;
	}

	if (!TAILQ_EMPTY(&_sock->queued_reqs)) {
		/* We weren't able to flush all requests */
		errno = EAGAIN;
		return -1;
	}

	return g_vma_ops.writev(sock->fd, iov, iovcnt);
}

static inline size_t
vma_sock_prep_reqs(struct spdk_sock *_sock, struct iovec *iovs, struct vma_pd_key *mkeys)
{
	size_t iovcnt = 0;
	int i;
	struct spdk_sock_request *req;
	unsigned int offset;

	req = TAILQ_FIRST(&_sock->queued_reqs);

	while (req) {
		assert(req->mkeys);

		offset = req->internal.offset;

		for (i = 0; i < req->iovcnt; i++) {
			/* Consume any offset first */
			if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) {
				offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
				continue;
			}

			iovs[iovcnt].iov_base = SPDK_SOCK_REQUEST_IOV(req, i)->iov_base + offset;
			iovs[iovcnt].iov_len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset;
			assert(req->mkeys);
			if (mkeys) {
				mkeys[iovcnt].mkey = req->mkeys[i];
				mkeys[iovcnt].flags = 0;
			}

//			SPDK_NOTICELOG("req %p, buffer[%zu]: %p, %zu. mkey %u\n", req, iovcnt, iovs[iovcnt].iov_base, iovs[iovcnt].iov_len,
//						   mkeys[iovcnt].mkey);
			iovcnt++;

			offset = 0;

			if (iovcnt >= IOV_BATCH_SIZE) {
				break;
			}
		}

		if (iovcnt >= IOV_BATCH_SIZE) {
			break;
		}

		req = TAILQ_NEXT(req, internal.link);
	}

	return iovcnt;
}

static int
_sock_flush_ext(struct spdk_sock *sock)
{
	struct spdk_vma_sock *vsock = __vma_sock(sock);
	struct msghdr msg = {};
	struct cmsghdr *cmsg;
	int flags;
	struct iovec iovs[IOV_BATCH_SIZE];
	union {
		char buf[CMSG_SPACE(sizeof(struct vma_pd_key) * IOV_BATCH_SIZE)];
		struct cmsghdr align;
	} mkeys_container;
	struct vma_pd_key *mkeys;
	size_t iovcnt;
	int retval;
	struct spdk_sock_request *req;
	int i;
	ssize_t rc;
	unsigned int offset;
	size_t len;

	/* Can't flush from within a callback or we end up with recursive calls */
	if (sock->cb_cnt > 0) {
		return 0;
	}

	if (spdk_unlikely(TAILQ_EMPTY(&sock->queued_reqs))) {
		return 0;
	}

	if (vsock->zcopy && vsock->pd) {

		msg.msg_control = mkeys_container.buf;
		msg.msg_controllen = sizeof(mkeys_container.buf);
		cmsg = CMSG_FIRSTHDR(&msg);

		cmsg->cmsg_len = CMSG_LEN(sizeof(struct vma_pd_key) * IOV_BATCH_SIZE);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_VMA_PD;
		flags = MSG_ZEROCOPY;

		mkeys = (struct vma_pd_key *)CMSG_DATA(cmsg);
		iovcnt = vma_sock_prep_reqs(sock, iovs, mkeys);

		msg.msg_controllen = CMSG_SPACE(sizeof(struct vma_pd_key) * iovcnt);
		cmsg->cmsg_len = CMSG_LEN(sizeof(struct vma_pd_key) * iovcnt);

	} else {
		if (vsock->zcopy) {
			flags = MSG_ZEROCOPY;
		} else {
			flags = 0;
		}
		msg.msg_control = NULL;
		msg.msg_controllen = 0;

		iovcnt = vma_sock_prep_reqs(sock, iovs, NULL);
	}

	assert(iovcnt);

	/* Perform the vectored write */
	msg.msg_iov = iovs;
	msg.msg_iovlen = iovcnt;

	rc = g_vma_ops.sendmsg(vsock->fd, &msg, flags);
	if (rc <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || (errno == ENOBUFS && vsock->zcopy)) {
			return 0;
		}

		fprintf(stderr, "sendmsg rc %zd, errno %d\n", rc, errno);
		SPDK_NOTICELOG("sendmsg error %zd\n", rc);
		return rc;
	}

	if (vsock->zcopy && vsock->pd) {
//		printf("------use ctx idx %u \t sendmsg_idx %u \t\n", ctx->internal_idx, vsock->sendmsg_idx);
	}

	/* Handling overflow case, because we use vsock->sendmsg_idx - 1 for the
	 * req->internal.offset, so sendmsg_idx should not be zero  */
	if (spdk_unlikely(vsock->sendmsg_idx == UINT32_MAX)) {
		vsock->sendmsg_idx = 1;
	} else {
		vsock->sendmsg_idx++;
	}

	/* Consume the requests that were actually written */
	req = TAILQ_FIRST(&sock->queued_reqs);
	while (req) {
		offset = req->internal.offset;

		for (i = 0; i < req->iovcnt; i++) {
			/* Advance by the offset first */
			if (offset >= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len) {
				offset -= SPDK_SOCK_REQUEST_IOV(req, i)->iov_len;
				continue;
			}

			/* Calculate the remaining length of this element */
			len = SPDK_SOCK_REQUEST_IOV(req, i)->iov_len - offset;

			if (len > (size_t)rc) {
				/* This element was partially sent. */
				req->internal.offset += rc;
				return 0;
			}

			offset = 0;
			req->internal.offset += len;
			rc -= len;
		}

		/* Handled a full request. */
		spdk_sock_request_pend(sock, req);

		if (!vsock->zcopy) {
			/* The sendmsg syscall above isn't currently asynchronous,
			* so it's already done. */
			retval = spdk_sock_request_put(sock, req, 0);
			if (retval) {
				break;
			}
		} else {
			/* Re-use the offset field to hold the sendmsg call index. The
			 * index is 0 based, so subtract one here because we've already
			 * incremented above. */
			req->internal.offset = vsock->sendmsg_idx - 1;
		}

		if (rc == 0) {
			break;
		}

		req = TAILQ_FIRST(&sock->queued_reqs);
	}

	return 0;
}


static void
vma_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	int rc;

	spdk_sock_request_queue(sock, req);

	assert(req->mkeys);

	/* If there are a sufficient number queued, just flush them out immediately. */
	if (sock->queued_iovcnt >= IOV_BATCH_SIZE) {
		rc = _sock_flush_ext(sock);
		if (rc) {
			spdk_sock_abort_requests(sock);
		}
	}
}

static int
vma_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	int val;
	int rc;

	assert(sock != NULL);

	val = nbytes;
	rc = g_vma_ops.setsockopt(sock->fd, SOL_SOCKET, SO_RCVLOWAT, &val, sizeof val);
	if (rc != 0) {
		return -1;
	}
	return 0;
}

static bool
vma_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = g_vma_ops.getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET6);
}

static bool
vma_sock_is_ipv4(struct spdk_sock *_sock)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	struct sockaddr_storage sa;
	socklen_t salen;
	int rc;

	assert(sock != NULL);

	memset(&sa, 0, sizeof sa);
	salen = sizeof sa;
	rc = g_vma_ops.getsockname(sock->fd, (struct sockaddr *) &sa, &salen);
	if (rc != 0) {
		SPDK_ERRLOG("getsockname() failed (errno=%d)\n", errno);
		return false;
	}

	return (sa.ss_family == AF_INET);
}

static bool
vma_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	uint8_t byte;
	int rc;

	rc = g_vma_ops.recv(sock->fd, &byte, 1, MSG_PEEK);
	if (rc == 0) {
		return false;
	}

	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return true;
		}

		return false;
	}

	return true;
}

static struct spdk_sock_group_impl *
vma_sock_group_impl_create(void)
{
	struct spdk_vma_sock_group_impl *group_impl;
	int fd;

#if defined(SPDK_EPOLL)
	fd = g_vma_ops.epoll_create1(0);
#elif defined(SPDK_KEVENT)
	fd = kqueue();
#endif
	if (fd == -1) {
		return NULL;
	}

	group_impl = calloc(1, sizeof(*group_impl));
	if (group_impl == NULL) {
		SPDK_ERRLOG("group_impl allocation failed\n");
		g_vma_ops.close(fd);
		return NULL;
	}

	group_impl->fd = fd;
	TAILQ_INIT(&group_impl->pending_recv);

	return &group_impl->base;
}

static int
vma_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_vma_sock_group_impl *group = __vma_group_impl(_group);
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	int rc;

#if defined(SPDK_EPOLL)
	struct epoll_event event;

	memset(&event, 0, sizeof(event));
	/* EPOLLERR is always on even if we don't set it, but be explicit for clarity */
	event.events = EPOLLIN | EPOLLERR;
	event.data.ptr = sock;

	rc = g_vma_ops.epoll_ctl(group->fd, EPOLL_CTL_ADD, sock->fd, &event);
#elif defined(SPDK_KEVENT)
	struct kevent event;
	struct timespec ts = {0};

	EV_SET(&event, sock->fd, EVFILT_READ, EV_ADD, 0, 0, sock);

	rc = kevent(group->fd, &event, 1, NULL, 0, &ts);
#endif

	return rc;
}

static int
vma_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_vma_sock_group_impl *group = __vma_group_impl(_group);
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	int rc;

#if defined(SPDK_EPOLL)
	struct epoll_event event;

	/* Event parameter is ignored but some old kernel version still require it. */
	rc = g_vma_ops.epoll_ctl(group->fd, EPOLL_CTL_DEL, sock->fd, &event);
#elif defined(SPDK_KEVENT)
	struct kevent event;
	struct timespec ts = {0};

	EV_SET(&event, sock->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);

	rc = kevent(group->fd, &event, 1, NULL, 0, &ts);
	if (rc == 0 && event.flags & EV_ERROR) {
		rc = -1;
		errno = event.data;
	}
#endif

	spdk_sock_abort_requests(_sock);

	return rc;
}

static int
vma_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
			 struct spdk_sock **socks)
{
	struct spdk_vma_sock_group_impl *group = __vma_group_impl(_group);
	struct spdk_sock *sock, *tmp;
	int num_events, i, rc;
	struct spdk_vma_sock *vsock, *ptmp;
#if defined(SPDK_EPOLL)
	struct epoll_event events[MAX_EVENTS_PER_POLL];
#elif defined(SPDK_KEVENT)
	struct kevent events[MAX_EVENTS_PER_POLL];
	struct timespec ts = {0};
#endif

	/* This must be a TAILQ_FOREACH_SAFE because while flushing,
	 * a completion callback could remove the sock from the
	 * group. */
	TAILQ_FOREACH_SAFE(sock, &_group->socks, link, tmp) {
		rc = _sock_flush_ext(sock);
		if (rc) {
			spdk_sock_abort_requests(sock);
		}
	}

#if defined(SPDK_EPOLL)
	num_events = g_vma_ops.epoll_wait(group->fd, events, max_events, 0);
#elif defined(SPDK_KEVENT)
	num_events = kevent(group->fd, NULL, 0, events, max_events, &ts);
#endif

	if (num_events == -1) {
		return -1;
	} else if (num_events == 0 && !TAILQ_EMPTY(&_group->socks)) {
		uint8_t byte;

		sock = TAILQ_FIRST(&_group->socks);
		vsock = __vma_sock(sock);
		/* a recv is done here to busy poll the queue associated with
		 * first socket in list and potentially reap incoming data.
		 */
		if (vsock->so_priority) {
			g_vma_ops.recv(vsock->fd, &byte, 1, MSG_PEEK);
		}
	}

	for (i = 0; i < num_events; i++) {
#if defined(SPDK_EPOLL)
		sock = events[i].data.ptr;
		vsock = __vma_sock(sock);

#ifdef SPDK_ZEROCOPY
		if (events[i].events & EPOLLERR) {
			rc = _sock_check_zcopy(sock);
			/* If the socket was closed or removed from
			 * the group in response to a send ack, don't
			 * add it to the array here. */
			if (rc || sock->cb_fn == NULL) {
				continue;
			}
		}
#endif
		if ((events[i].events & EPOLLIN) == 0) {
			continue;
		}

#elif defined(SPDK_KEVENT)
		sock = events[i].udata;
		vsock = __vma_sock(sock);
#endif

		/* If the socket does not already have recv pending, add it now */
		if (!vsock->pending_recv) {
			vsock->pending_recv = true;
			TAILQ_INSERT_TAIL(&group->pending_recv, vsock, link);
		}
	}

	num_events = 0;

	TAILQ_FOREACH_SAFE(vsock, &group->pending_recv, link, ptmp) {
		if (num_events == max_events) {
			break;
		}

		/* If the socket's cb_fn is NULL, just remove it from the
		 * list and do not add it to socks array */
		if (spdk_unlikely(vsock->base.cb_fn == NULL)) {
			vsock->pending_recv = false;
			TAILQ_REMOVE(&group->pending_recv, vsock, link);
			continue;
		}

		socks[num_events++] = &vsock->base;
	}

	/* Cycle the pending_recv list so that each time we poll things aren't
	 * in the same order. */
	for (i = 0; i < num_events; i++) {
		vsock = __vma_sock(socks[i]);

		TAILQ_REMOVE(&group->pending_recv, vsock, link);
		vsock->pending_recv = false;
	}

	return num_events;
}

static int
vma_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_vma_sock_group_impl *group = __vma_group_impl(_group);
	int rc;

	rc = g_vma_ops.close(group->fd);
	free(group);
	return rc;
}

static int
vma_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	if (!opts || !len) {
		errno = EINVAL;
		return -1;
	}
	memset(opts, 0, *len);

#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(opts->field) <= *len

#define GET_FIELD(field) \
	if (FIELD_OK(field)) { \
		opts->field = g_spdk_vma_sock_impl_opts.field; \
	}

	GET_FIELD(recv_buf_size);
	GET_FIELD(send_buf_size);
	GET_FIELD(enable_recv_pipe);
	GET_FIELD(enable_zerocopy_send);
	GET_FIELD(enable_quickack);
	GET_FIELD(enable_placement_id);
	GET_FIELD(enable_zerocopy_recv);

#undef GET_FIELD
#undef FIELD_OK

	*len = spdk_min(*len, sizeof(g_spdk_vma_sock_impl_opts));
	return 0;
}

static int
vma_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	if (!opts) {
		errno = EINVAL;
		return -1;
	}

#define FIELD_OK(field) \
	offsetof(struct spdk_sock_impl_opts, field) + sizeof(opts->field) <= len

#define SET_FIELD(field) \
	if (FIELD_OK(field)) { \
		g_spdk_vma_sock_impl_opts.field = opts->field; \
	}

	SET_FIELD(recv_buf_size);
	SET_FIELD(send_buf_size);
	SET_FIELD(enable_recv_pipe);
	SET_FIELD(enable_zerocopy_send);
	SET_FIELD(enable_quickack);
	SET_FIELD(enable_placement_id);
	SET_FIELD(enable_zerocopy_recv);

#undef SET_FIELD
#undef FIELD_OK

	return 0;
}

static int
vma_sock_get_caps(struct spdk_sock *sock, struct spdk_sock_caps *caps)
{
	struct spdk_vma_sock *vsock = __vma_sock(sock);

	caps->zcopy_send = vsock->zcopy;
	caps->ibv_pd = vsock->pd;

	return 0;
}

static struct vma_sock_buf *
vma_sock_get_buf(struct spdk_vma_sock *sock)
{
	struct spdk_sock_buf *sock_buf = sock->free_buffers;
	/* @todo: we don't handle lack of buffers yet */
	assert(sock_buf);
	sock->free_buffers = sock_buf->next;
	return SPDK_CONTAINEROF(sock_buf, struct vma_sock_buf, sock_buf);
}

static void
vma_sock_free_buf(struct spdk_vma_sock *sock, struct vma_sock_buf *buf)
{
	buf->sock_buf.next = sock->free_buffers;
	sock->free_buffers = &buf->sock_buf;
}

static ssize_t
vma_sock_recv_zcopy(struct spdk_sock *_sock, size_t len, struct spdk_sock_buf **sock_buf)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);
	struct vma_sock_buf *prev_buf = NULL;
	int ret;

	SPDK_DEBUGLOG(vma, "Sock %d: zcopy recv %lu bytes\n", sock->fd, len);
	assert(sock->recv_zcopy);
	*sock_buf = NULL;

	if (STAILQ_EMPTY(&sock->received_packets)) {
		ret = vma_sock_recvfrom_zcopy(sock);
		if (ret <= 0) {
			SPDK_DEBUGLOG(vma, "Sock %d: recv_zcopy ret %d, errno %d\n",
				      sock->fd, ret, errno);
			return ret;
		}
	}

	assert(!STAILQ_EMPTY(&sock->received_packets));
	ret = 0;
	while (len > 0) {
		void *data;
		size_t chunk_len;
		struct vma_sock_buf *buf;
		struct vma_sock_packet *packet;

		chunk_len = packets_next_chunk(sock, &data, &packet, len);
		if (chunk_len == 0) {
			/* No more data */
			break;
		}

		assert(chunk_len <= len);
		buf = vma_sock_get_buf(sock);
		/* @todo: we don't handle lack of buffers yet */
		assert(buf);
		buf->sock_buf.iov.iov_base = data;
		buf->sock_buf.iov.iov_len = chunk_len;
		buf->sock_buf.next = NULL;
		buf->packet = packet;
		packet->refs++;
		if (prev_buf) {
			prev_buf->sock_buf.next = &buf->sock_buf;
		} else {
			*sock_buf = &buf->sock_buf;
		}

		packets_advance(sock, chunk_len);
		len -= chunk_len;
		ret += chunk_len;
		prev_buf = buf;
		SPDK_DEBUGLOG(vma, "Sock %d: add buffer %p, len %lu, total_len %d\n",
			      sock->fd, buf, buf->sock_buf.iov.iov_len, ret);
	}

	SPDK_DEBUGLOG(vma, "Sock %d: recv_zcopy ret %d\n", sock->fd, ret);
	return ret;
}

static int
vma_sock_free_bufs(struct spdk_sock *_sock, struct spdk_sock_buf *sock_buf)
{
	struct spdk_vma_sock *sock = __vma_sock(_sock);

	while (sock_buf) {
		struct vma_sock_buf *buf = SPDK_CONTAINEROF(sock_buf,
							    struct vma_sock_buf,
							    sock_buf);
		struct vma_sock_packet *packet = buf->packet;
		struct spdk_sock_buf *next = buf->sock_buf.next;

		vma_sock_free_buf(sock, buf);
		if (--packet->refs == 0) {
			vma_sock_free_packet(sock, packet);
		}

		sock_buf = next;
	}

	return 0;
}

static struct spdk_net_impl g_vma_net_impl = {
	.name		= "vma",
	.getaddr	= vma_sock_getaddr,
	.connect	= vma_sock_connect,
	.listen		= vma_sock_listen,
	.accept		= vma_sock_accept,
	.close		= vma_sock_close,
	.recv		= vma_sock_recv,
	.readv		= vma_sock_readv,
	.writev		= vma_sock_writev,
	.writev_async	= vma_sock_writev_async,
	.flush		= vma_sock_flush,
	.set_recvlowat	= vma_sock_set_recvlowat,
	.set_recvbuf	= vma_sock_set_recvbuf,
	.set_sendbuf	= vma_sock_set_sendbuf,
	.is_ipv6	= vma_sock_is_ipv6,
	.is_ipv4	= vma_sock_is_ipv4,
	.is_connected	= vma_sock_is_connected,
	.group_impl_create	= vma_sock_group_impl_create,
	.group_impl_add_sock	= vma_sock_group_impl_add_sock,
	.group_impl_remove_sock = vma_sock_group_impl_remove_sock,
	.group_impl_poll	= vma_sock_group_impl_poll,
	.group_impl_close	= vma_sock_group_impl_close,
	.get_opts	= vma_sock_impl_get_opts,
	.set_opts	= vma_sock_impl_set_opts,
	.get_caps	= vma_sock_get_caps,
	.recv_zcopy	= vma_sock_recv_zcopy,
	.free_bufs	= vma_sock_free_bufs
};

static void __attribute__((constructor)) 
 spdk_net_impl_register_vma(void)
{
	if (vma_load() == 0 &&
	    vma_init() == 0) {
		spdk_net_impl_register(&g_vma_net_impl, DEFAULT_SOCK_PRIORITY);
	}
}

SPDK_LOG_REGISTER_COMPONENT(vma)
