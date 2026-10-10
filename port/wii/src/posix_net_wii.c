/*
POSIX_NET_WII.C

The sockets of the platform layer (posix.h) on the Wii, in place of
port/linux/src/posix_net.c, over WiiLibs' network layer (wl/platform/net.h),
which talks to IOS through libogc. The shared Winsock layer
(port/linux/src/xnet.c) is the game's over them, for system link.

- Addresses: Winsock's sockaddr_in (a 16-bit family, the port and the address
  in network byte order) to WiiLibs' WlNetAddr (the address and port as
  numbers) and back. The Wii is big-endian, so a number and its network byte
  order are the same bytes and the fields are copied as they are. The family
  is not looked at: the game's sockets are all IPv4.
- Errors: WiiLibs' calls return a WlResult, which is turned into the Winsock
  error posix_socket_last_error gives (the table below). A result with no
  Winsock equivalent gives the errno IOS answered, as Winsock's number.
- The network starts at boot without waiting for it (wii_network_start,
  src/wii_boot.c): until it is up, every call fails with WSAENETDOWN.
- Options: Winsock's SOL_SOCKET is 0xFFFF, lwIP's 0xFFF. The ones WiiLibs
  sets (SO_REUSEADDR, SO_BROADCAST, SO_SNDBUF, SO_RCVBUF, TCP_NODELAY) are
  set; the others (SO_KEEPALIVE, SO_LINGER, and what only the Xbox has) are
  accepted and ignored, as the Linux build does for the Xbox's; SO_TYPE and
  SO_ERROR are read from WiiLibs (the socket's kind, and how a connect ended).
- Not on the Wii: the number of bytes waiting (IOS answers only FIONBIO) and
  getpeername (IOS has none; the game asks only of the connections it made,
  whose other end it already knows). The send and receive flags are not
  passed on; the game gives none.

Built as the posix_*.c units are, against newlib and WiiLibs' headers.
*/

#include <string.h>

#include "wl/platform/net.h"

#include "posix.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#error "the addresses are copied as they are: this file is for the big-endian Wii"
#endif

/* the Winsock errors (winsock.h's WSABASEERR + the BSD number) */
#define WSA_BASE 10000
#define WSAEFAULT (WSA_BASE + 14)
#define WSAEINVAL (WSA_BASE + 22)
#define WSAEWOULDBLOCK (WSA_BASE + 35)
#define WSAEINPROGRESS (WSA_BASE + 36)
#define WSAEALREADY (WSA_BASE + 37)
#define WSAENOTSOCK (WSA_BASE + 38)
#define WSAEMSGSIZE (WSA_BASE + 40)
#define WSAENOPROTOOPT (WSA_BASE + 42)
#define WSAESOCKTNOSUPPORT (WSA_BASE + 44)
#define WSAEAFNOSUPPORT (WSA_BASE + 47)
#define WSAEADDRINUSE (WSA_BASE + 48)
#define WSAEADDRNOTAVAIL (WSA_BASE + 49)
#define WSAENETDOWN (WSA_BASE + 50)
#define WSAENETUNREACH (WSA_BASE + 51)
#define WSAECONNRESET (WSA_BASE + 54)
#define WSAENOBUFS (WSA_BASE + 55)
#define WSAEISCONN (WSA_BASE + 56)
#define WSAENOTCONN (WSA_BASE + 57)
#define WSAETIMEDOUT (WSA_BASE + 60)
#define WSAECONNREFUSED (WSA_BASE + 61)
#define WSAEACCES (WSA_BASE + 13)
#define WINSOCK_SOL_SOCKET 0xFFFF

/* Winsock's, which the SDK headers here leave out (and the same numbers as lwIP's) */
#define WINSOCK_AF_INET 2
#define WINSOCK_SOCK_STREAM 1
#define WINSOCK_SOCK_DGRAM 2
#define WINSOCK_IPPROTO_TCP 6
#define WINSOCK_TCP_NODELAY 1
#define WINSOCK_SO_REUSEADDR 0x0004
#define WINSOCK_SO_BROADCAST 0x0020
#define WINSOCK_SO_SNDBUF 0x1001
#define WINSOCK_SO_RCVBUF 0x1002
#define WINSOCK_SO_ERROR 0x1007
#define WINSOCK_SO_TYPE 0x1008

enum { MAXIMUM_SELECT = WL_NET_POLL_MAX_SOCKETS };

static int last_error;

void wii_network_start(void)
{
	wl_net_start();
}

/* WiiLibs' results as Winsock's errors */
static int winsock_error(WlResult result)
{
	int error;

	switch (result)
	{
	case WL_ERR_AGAIN: return WSAEWOULDBLOCK;
	case WL_ERR_NET_INPROGRESS: return WSAEINPROGRESS;
	case WL_ERR_NET_REFUSED: return WSAECONNREFUSED;
	case WL_ERR_NET_RESET: return WSAECONNRESET;
	case WL_ERR_NET_TIMEDOUT: return WSAETIMEDOUT;
	case WL_ERR_NET_NOTCONN: return WSAENOTCONN;
	case WL_ERR_NET_ISCONN: return WSAEISCONN;
	case WL_ERR_NET_ALREADY: return WSAEALREADY;
	case WL_ERR_NET_ADDRINUSE: return WSAEADDRINUSE;
	case WL_ERR_NET_ADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
	case WL_ERR_NET_UNREACH: return WSAENETUNREACH;
	case WL_ERR_NET_MSGSIZE: return WSAEMSGSIZE;
	case WL_ERR_NET_ACCESS: return WSAEACCES;
	case WL_ERR_NET_NOTSOCK: return WSAENOTSOCK;
	case WL_ERR_ARG: return WSAEINVAL;
	case WL_ERR_NOMEM: return WSAENOBUFS;
	case WL_ERR_NET_DOWN: return WSAENETDOWN;
	default:
		/* (what IOS answered, in BSD's numbering, which is Winsock's less its base) */
		error = wl_net_last_errno();
		return error > 0 ? WSA_BASE + error : WSAEINVAL;
	}
}

static int fail(int error)
{
	last_error = error;
	return -1;
}

static int failed(WlResult result)
{
	return fail(winsock_error(result));
}

/* a WlResult as the call's return value */
static int done(WlResult result)
{
	if (result < 0)
		return failed(result);
	last_error = 0;
	return 0;
}

/* the count a send or receive returned, or the failure */
static int counted(int32_t count)
{
	if (count < 0)
		return failed(count);
	last_error = 0;
	return count;
}

int posix_socket_last_error(void)
{
	return last_error;
}

/* ---------- addresses: Winsock's to WiiLibs' and back */

struct winsock_sockaddr_in
{
	unsigned short family;
	unsigned short port;
	unsigned int address;
	char zero[8];
};

static int to_wl(const void *address, int length, WlNetAddr *out)
{
	const struct winsock_sockaddr_in *in = address;

	if (!address || length < (int)sizeof(*in))
		return 0;
	out->address = in->address;
	out->port = in->port;
	return 1;
}

static void to_winsock(const WlNetAddr *in, void *address, int *length)
{
	struct winsock_sockaddr_in out;

	if (!address || !length)
		return;
	memset(&out, 0, sizeof(out));
	out.family = WINSOCK_AF_INET;
	out.port = in->port;
	out.address = in->address;
	memcpy(address, &out, (size_t)(*length < (int)sizeof(out) ? *length : (int)sizeof(out)));
	*length = sizeof(out);
}

/* ---------- sockets */

int posix_socket(int family, int type, int protocol)
{
	WlSocket socket;
	WlResult result;

	(void)protocol;
	if (family != WINSOCK_AF_INET)
		return fail(WSAEAFNOSUPPORT);
	if (type != WINSOCK_SOCK_STREAM && type != WINSOCK_SOCK_DGRAM)
		return fail(WSAESOCKTNOSUPPORT);
	result = wl_net_socket(type == WINSOCK_SOCK_STREAM ? WL_NET_TCP : WL_NET_UDP, &socket);
	if (result < 0)
		return failed(result);
	last_error = 0;
	return socket;
}

int posix_socket_close(int socket)
{
	return done(wl_net_close(socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	WlNetAddr wl;

	if (!to_wl(address, address_length, &wl))
		return fail(WSAEFAULT);
	return done(wl_net_bind(socket, &wl));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	WlNetAddr wl;

	if (!to_wl(address, address_length, &wl))
		return fail(WSAEFAULT);
	return done(wl_net_connect(socket, &wl));
}

int posix_socket_listen(int socket, int backlog)
{
	return done(wl_net_listen(socket, backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	WlNetAddr peer;
	WlSocket accepted;
	WlResult result = wl_net_accept(socket, &accepted, &peer);

	if (result < 0)
		return failed(result);
	to_winsock(&peer, address, address_length);
	last_error = 0;
	return accepted;
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	(void)flags;
	if (length < 0)
		return fail(WSAEINVAL);
	return counted(wl_net_send(socket, buffer, (uint32_t)length));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	WlNetAddr wl;

	(void)flags;
	if (length < 0)
		return fail(WSAEINVAL);
	if (!to_wl(address, address_length, &wl))
		return fail(WSAEFAULT);
	return counted(wl_net_sendto(socket, buffer, (uint32_t)length, &wl));
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	(void)flags;
	if (length < 0)
		return fail(WSAEINVAL);
	return counted(wl_net_recv(socket, buffer, (uint32_t)length));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	WlNetAddr from;
	int32_t received;

	(void)flags;
	if (length < 0)
		return fail(WSAEINVAL);
	received = wl_net_recvfrom(socket, buffer, (uint32_t)length, &from);
	if (received >= 0)
		to_winsock(&from, address, address_length);
	return counted(received);
}

int posix_socket_shutdown(int socket, int how)
{
	/* (Winsock's SD_RECEIVE, SD_SEND and SD_BOTH are WiiLibs' 0, 1 and 2) */
	if (how < WL_NET_SHUT_READ || how > WL_NET_SHUT_BOTH)
		return fail(WSAEINVAL);
	return done(wl_net_shutdown(socket, (WlNetShutdown)how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	return done(wl_net_set_nonblocking(socket, nonblocking != 0));
}

/* (IOS answers only FIONBIO: a Winsock error, and no count) */
int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	uint32_t available = 0;
	WlResult result = wl_net_bytes_available(socket, &available);

	*count = result < 0 ? 0 : available;
	return done(result);
}

int posix_socket_set_nodelay(int socket)
{
	return done(wl_net_set_option(socket, WL_NET_OPT_NODELAY, 1));
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	int setting;

	if (!value || length < 1)
		return fail(WSAEFAULT);
	/* (a BOOL or a DWORD; a one-byte value is taken as it is) */
	setting = length >= (int)sizeof(int) ? *(const int *)value : *(const unsigned char *)value;
	if (level == WINSOCK_SOL_SOCKET)
	{
		switch (name)
		{
		case WINSOCK_SO_REUSEADDR: return done(wl_net_set_option(socket, WL_NET_OPT_REUSEADDR, setting));
		case WINSOCK_SO_BROADCAST: return done(wl_net_set_option(socket, WL_NET_OPT_BROADCAST, setting));
		case WINSOCK_SO_SNDBUF: return done(wl_net_set_option(socket, WL_NET_OPT_SNDBUF, setting));
		case WINSOCK_SO_RCVBUF: return done(wl_net_set_option(socket, WL_NET_OPT_RCVBUF, setting));
		default: break;
		}
	}
	else if (level == WINSOCK_IPPROTO_TCP && name == WINSOCK_TCP_NODELAY)
	{
		return done(wl_net_set_option(socket, WL_NET_OPT_NODELAY, setting));
	}
	/* (the others have nothing to do here) */
	last_error = 0;
	return 0;
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	int answer;

	if (!value || !length || *length < (int)sizeof(int))
		return fail(WSAEFAULT);
	if (level != WINSOCK_SOL_SOCKET)
		return fail(WSAENOPROTOOPT);
	switch (name)
	{
	case WINSOCK_SO_TYPE:
	{
		WlNetProto proto;
		WlResult result = wl_net_socket_proto(socket, &proto);

		if (result < 0)
			return failed(result);
		answer = proto == WL_NET_TCP ? WINSOCK_SOCK_STREAM : WINSOCK_SOCK_DGRAM;
		break;
	}
	case WINSOCK_SO_ERROR:
	{
		WlResult ended;
		WlResult result = wl_net_connect_result(socket, &ended);

		if (result < 0)
			return failed(result);
		answer = ended < 0 ? winsock_error(ended) : 0;
		break;
	}
	default:
		return fail(WSAENOPROTOOPT);
	}
	*(int *)value = answer;
	*length = sizeof(int);
	last_error = 0;
	return 0;
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	WlNetAddr local;
	WlResult result = wl_net_local_addr(socket, &local);

	if (result < 0)
		return failed(result);
	to_winsock(&local, address, address_length);
	last_error = 0;
	return 0;
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	(void)socket;
	(void)address;
	(void)address_length;
	return fail(WSAEINVAL);
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	static const uint32_t events[3] = { WL_NET_POLL_READ, WL_NET_POLL_WRITE, WL_NET_POLL_PRIORITY };
	static const uint32_t ready[3] =
	{
		WL_NET_POLL_READ | WL_NET_POLL_HUP | WL_NET_POLL_ERROR,
		WL_NET_POLL_WRITE | WL_NET_POLL_ERROR,
		WL_NET_POLL_PRIORITY | WL_NET_POLL_ERROR
	};
	WlNetPoll items[MAXIMUM_SELECT];
	int *lists[3] = { read, write, error };
	int *counts[3] = { read_count, write_count, error_count };
	int total = 0, list, index, ready_count = 0;
	int32_t polled;
	long long milliseconds = (long long)timeout_seconds * 1000 + ((long long)timeout_microseconds + 999) / 1000;

	for (list = 0; list < 3; list++)
	{
		if (!lists[list] || !counts[list])
			continue;
		for (index = 0; index < *counts[list] && total < MAXIMUM_SELECT; index++)
		{
			items[total].socket = lists[list][index];
			items[total].events = events[list];
			items[total].revents = 0;
			total++;
		}
	}
	polled = wl_net_poll(items, (uint32_t)total,
		infinite ? WL_NET_TIMEOUT_FOREVER : (int32_t)(milliseconds > 0x7FFFFFFF ? 0x7FFFFFFF : milliseconds));
	if (polled < 0)
		return failed(polled);
	total = 0;
	for (list = 0; list < 3; list++)
	{
		int kept = 0;

		if (!lists[list] || !counts[list])
			continue;
		for (index = 0; index < *counts[list] && total < MAXIMUM_SELECT; index++, total++)
		{
			if (items[total].revents & ready[list])
				lists[list][kept++] = items[total].socket;
		}
		*counts[list] = kept;
		ready_count += kept;
	}
	last_error = 0;
	return ready_count;
}

/* (network byte order, which on the big-endian Wii is the number) */
posix_ulong posix_local_ipv4_address(void)
{
	return wl_net_local_address();
}
