/*
POSIX_NET_WII.C

The sockets of the platform layer (posix.h) on the Wii, in place of
port/linux/src/posix_net.c, through libogc's network calls (network.h),
which IOS serves. The shared Winsock layer (port/linux/src/xnet.c) is the
game's over them, for system link.

- Addresses: Winsock's sockaddr_in begins with a 16-bit family, lwIP's (and
  IOS's) with a length byte and a family byte; they are converted at each
  call, the rest of the structure being the same.
- Errors: libogc's calls return a negative error code (BSD's numbering, as
  newlib's), which is kept as the Winsock error posix_socket_last_error
  gives.
- The network starts at boot without waiting for it (wii_network_start,
  src/wii_boot.c): until it is up, every call fails with WSAENETDOWN.
- Option levels: Winsock's SOL_SOCKET is 0xFFFF, lwIP's 0xFFF; the options
  the game sets have the same numbers in both.

Built as the posix_*.c units are, against newlib and libogc alone.
*/

#include <errno.h>
#include <network.h>
#include <poll.h>
#include <string.h>

#include "posix.h"

/* the Winsock errors (winsock.h's WSABASEERR + the BSD number) */
#define WSA_BASE 10000
#define WSAEWOULDBLOCK (WSA_BASE + 35)
#define WSAEINVAL (WSA_BASE + 22)
#define WSAENETDOWN (WSA_BASE + 50)
#define WSAEFAULT (WSA_BASE + 14)
#define WINSOCK_SOL_SOCKET 0xFFFF

static volatile int network_state; /* 0 starting, 1 up, -1 failed */
static int last_error;

static s32 network_started(s32 result, void *user)
{
	(void)user;
	network_state = result >= 0 ? 1 : -1;
	return 0;
}

void wii_network_start(void)
{
	network_state = 0;
	if (net_init_async(network_started, NULL) < 0)
		network_state = -1;
}

static int fail(int code)
{
	/* (IOS and lwIP answer with BSD's numbers, Winsock's less its base) */
	if (code < 0)
		code = -code;
	if (code == EAGAIN)
		code = 35; /* EWOULDBLOCK */
	last_error = code ? WSA_BASE + code : WSAEINVAL;
	return -1;
}

static int result(s32 value)
{
	if (value < 0)
		return fail(value);
	last_error = 0;
	return value;
}

static int network_down(void)
{
	if (network_state == 1)
		return 0;
	last_error = WSAENETDOWN;
	return 1;
}

int posix_socket_last_error(void)
{
	return last_error;
}

/* ---------- addresses: Winsock's to lwIP's and back */

struct winsock_sockaddr_in
{
	unsigned short family;
	unsigned short port;
	unsigned long address;
	char zero[8];
};

static int to_lwip(const void *address, int length, struct sockaddr_in *out)
{
	const struct winsock_sockaddr_in *in = address;

	if (!address || length < (int)sizeof(*in))
		return 0;
	memset(out, 0, sizeof(*out));
	out->sin_len = sizeof(*out);
	out->sin_family = (sa_family_t)in->family;
	out->sin_port = in->port;
	out->sin_addr.s_addr = in->address;
	return 1;
}

static void to_winsock(const struct sockaddr_in *in, void *address, int *length)
{
	struct winsock_sockaddr_in out;

	if (!address || !length)
		return;
	memset(&out, 0, sizeof(out));
	out.family = in->sin_family;
	out.port = in->sin_port;
	out.address = in->sin_addr.s_addr;
	memcpy(address, &out, (size_t)(*length < (int)sizeof(out) ? *length : (int)sizeof(out)));
	*length = sizeof(out);
}

/* ---------- sockets */

int posix_socket(int family, int type, int protocol)
{
	if (network_down())
		return -1;
	return result(net_socket((u32)family, (u32)type, (u32)protocol));
}

int posix_socket_close(int socket)
{
	return result(net_close(socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	struct sockaddr_in lwip;

	if (!to_lwip(address, address_length, &lwip))
		return fail(EFAULT);
	return result(net_bind(socket, (struct sockaddr *)&lwip, sizeof(lwip)));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	struct sockaddr_in lwip;

	if (!to_lwip(address, address_length, &lwip))
		return fail(EFAULT);
	return result(net_connect(socket, (struct sockaddr *)&lwip, sizeof(lwip)));
}

int posix_socket_listen(int socket, int backlog)
{
	return result(net_listen(socket, (u32)backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	struct sockaddr_in lwip;
	socklen_t length = sizeof(lwip);
	s32 accepted = net_accept(socket, (struct sockaddr *)&lwip, &length);

	if (accepted >= 0)
		to_winsock(&lwip, address, address_length);
	return result(accepted);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	return result(net_send(socket, buffer, length, (u32)flags));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	struct sockaddr_in lwip;

	if (!to_lwip(address, address_length, &lwip))
		return fail(EFAULT);
	return result(net_sendto(socket, buffer, length, (u32)flags, (struct sockaddr *)&lwip, sizeof(lwip)));
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	return result(net_recv(socket, buffer, length, (u32)flags));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	struct sockaddr_in lwip;
	socklen_t lwip_length = sizeof(lwip);
	s32 received = net_recvfrom(socket, buffer, length, (u32)flags, (struct sockaddr *)&lwip, &lwip_length);

	if (received >= 0)
		to_winsock(&lwip, address, address_length);
	return result(received);
}

int posix_socket_shutdown(int socket, int how)
{
	return result(net_shutdown(socket, (u32)how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	u32 on = nonblocking ? 1 : 0;

	return result(net_ioctl(socket, FIONBIO, &on));
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	u32 available = 0;
	int value = result(net_ioctl(socket, FIONREAD, &available));

	*count = value < 0 ? 0 : available;
	return value < 0 ? -1 : 0;
}

int posix_socket_set_nodelay(int socket)
{
	int on = 1;

	return result(net_setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)));
}

static u32 level_of(int level)
{
	return level == WINSOCK_SOL_SOCKET ? SOL_SOCKET : (u32)level;
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	return result(net_setsockopt(socket, level_of(level), (u32)name, value, (socklen_t)length));
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	socklen_t lwip_length = (socklen_t)*length;
	int value_result = result(net_getsockopt(socket, level_of(level), (u32)name, value, &lwip_length));

	*length = (int)lwip_length;
	return value_result;
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	struct sockaddr_in lwip;
	socklen_t length = sizeof(lwip);
	s32 value = net_getsockname(socket, (struct sockaddr *)&lwip, &length);

	if (value >= 0)
		to_winsock(&lwip, address, address_length);
	return result(value);
}

/* (IOS has no getpeername: the game asks only of the connections it made,
whose other end it already knows) */
int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	(void)socket;
	(void)address;
	(void)address_length;
	return fail(EINVAL);
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	enum { MAXIMUM = 64 };
	static const u32 events[3] = { POLLIN, POLLOUT, POLLPRI };
	static const u32 ready[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI | POLLERR };
	struct pollsd descriptors[MAXIMUM];
	int *lists[3] = { read, write, error };
	int *counts[3] = { read_count, write_count, error_count };
	int total = 0, list, index, polled, ready_count = 0;
	long long milliseconds = (long long)timeout_seconds * 1000 + ((long long)timeout_microseconds + 999) / 1000;

	if (network_down())
		return -1;
	for (list = 0; list < 3; list++)
	{
		if (!lists[list] || !counts[list])
			continue;
		for (index = 0; index < *counts[list] && total < MAXIMUM; index++)
		{
			descriptors[total].socket = lists[list][index];
			descriptors[total].events = events[list];
			descriptors[total].revents = 0;
			total++;
		}
	}
	polled = net_poll(descriptors, total, infinite ? -1 : (s32)(milliseconds > 0x7FFFFFFF ? 0x7FFFFFFF : milliseconds));
	if (polled < 0)
		return fail(polled);
	total = 0;
	for (list = 0; list < 3; list++)
	{
		int kept = 0;

		if (!lists[list] || !counts[list])
			continue;
		for (index = 0; index < *counts[list] && total < MAXIMUM; index++, total++)
		{
			if (descriptors[total].revents & ready[list])
				lists[list][kept++] = descriptors[total].socket;
		}
		*counts[list] = kept;
		ready_count += kept;
	}
	last_error = 0;
	return ready_count;
}

posix_ulong posix_local_ipv4_address(void)
{
	return network_state == 1 ? net_gethostip() : 0;
}
