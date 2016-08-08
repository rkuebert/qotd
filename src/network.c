/*
 * network.c
 *
 * qotd - A simple QOTD daemon.
 * Copyright (c) 2015-2016 Ammon Smith
 *
 * qotd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * qotd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with qotd.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <string.h>

#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "daemon.h"
#include "journal.h"
#include "network.h"
#include "quotes.h"
#include "standard.h"

/*
 * If the returned error is one of these, then you
 * should not attempt to remake the socket.
 * Getting one of these errors should be fatal.
 */
#define CHECK_SOCKET_ERROR(error)   		\
	do { 					\
		switch (error) {		\
		case EBADF:			\
		case EFAULT:			\
		case EINVAL:			\
		case EMFILE:			\
		case ENFILE:			\
		case ENOBUFS:			\
		case ENOMEM:			\
		case ENOTSOCK:			\
		case EOPNOTSUPP:		\
		case EPROTO:			\
		case EPERM:			\
		case ENOSR:			\
		case ESOCKTNOSUPPORT:		\
		case EPROTONOSUPPORT:		\
			cleanup(EXIT_IO, true);	\
		}				\
	} while(0)

#define TCP_CONNECTION_BACKLOG		50

/* Static member declarations */
static int sockfd = -1;

void set_up_ipv4_socket(const struct options *opt)
{
	struct sockaddr_in serv_addr;
	int ret, one = 1;

	if (opt->tproto == PROTOCOL_TCP) {
		journal("Setting up IPv4 socket over TCP...\n");
		sockfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	} else {
		journal("Setting up IPv4 socket over UDP...\n");
		sockfd = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	}

	if (sockfd < 0) {
		journal("Unable to create IPv4 socket: %s.\n", strerror(errno));
		cleanup(EXIT_IO, true);
	}

	ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void *)(&one), sizeof(one));
	if (ret < 0) {
		journal("Unable to set the socket to allow address reuse: %s.\n", strerror(errno));
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(opt->port);

	ret = bind(sockfd, (const struct sockaddr *)(&serv_addr), sizeof(struct sockaddr_in));
	if (ret < 0) {
		journal("Unable to bind to IPv4 socket: %s.\n", strerror(errno));
		cleanup(EXIT_IO, true);
	}
}

void set_up_ipv6_socket(const struct options *opt)
{
	struct sockaddr_in6 serv_addr;
	int ret, one = 1;

	if (opt->tproto == PROTOCOL_TCP) {
		journal("Setting up IPv%s6 socket over TCP...\n",
				((opt->iproto == PROTOCOL_BOTH) ? "4/" : ""));
		sockfd = socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP);
	} else {
		journal("Setting up IPv%s6 socket over UDP...\n",
				((opt->iproto == PROTOCOL_BOTH) ? "4/" : ""));
		sockfd = socket(PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	}

	if (sockfd < 0) {
		journal("Unable to create IPv6 socket: %s.\n", strerror(errno));
		cleanup(EXIT_IO, true);
	}

	if (opt->iproto == PROTOCOL_IPv6) {
		ret = setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, (void *)(&one), sizeof(one));
		if (ret < 0) {
			journal("Unable to set IPv4 compatibility option: %s.\n", strerror(errno));
			cleanup(EXIT_IO, true);
		}
	}

	ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void *)(&one), sizeof(one));
	if (ret < 0) {
		journal("Unable to set the socket to allow address reuse: %s.\n", strerror(errno));
	}

	serv_addr.sin6_family = AF_INET6;
	serv_addr.sin6_addr = in6addr_any;
	serv_addr.sin6_port = htons(opt->port);
	serv_addr.sin6_flowinfo = 0;
	serv_addr.sin6_scope_id = 0;

	ret = bind(sockfd, (const struct sockaddr *)(&serv_addr), sizeof(struct sockaddr_in6));
	if (ret < 0) {
		journal("Unable to bind to socket: %s.\n", strerror(errno));
		cleanup(EXIT_IO, true);
	}
}

void close_socket(void)
{
	int ret;

	if (sockfd < 0) {
		return;
	}

	ret = close(sockfd);
	if (ret) {
		journal("Unable to close socket file descriptor %d: %s.\n",
			sockfd, strerror(errno));
	}
}

void tcp_accept_connection(void)
{
	struct sockaddr_in cli_addr;
	socklen_t cli_len;
	int consockfd;
	char *buffer;
	size_t length;
	int ret;

	journal("Listening for connection...\n");
	listen(sockfd, TCP_CONNECTION_BACKLOG);

	cli_len = sizeof(cli_addr);
	consockfd = accept(sockfd, (struct sockaddr *)(&cli_addr), &cli_len);
	if (consockfd < 0) {
		int errsave = errno;
		journal("Unable to accept connection: %s.\n", strerror(errsave));
		CHECK_SOCKET_ERROR(errsave);
		return;
	}

	ret = get_quote_of_the_day(&buffer, &length);
	if (unlikely(ret)) {
		return;
	}

	ret = write(consockfd, buffer, length);
	if (unlikely(ret < 0)) {
		ERR_TRACE();
		journal("Unable to wrtie to TCP connection socket: %s.\n", strerror(errno));
		return;
	}

	ret = close(consockfd);
	if (ret) {
		journal("Unable to close connection: %s.\n", strerror(errno));
		return;
	}
}

void udp_accept_connection(void)
{
	struct sockaddr_in cli_addr;
	socklen_t cli_len;
	char *buffer;
	size_t length;
	int ret;

	journal("Listening for connection...\n");
	ret = recvfrom(sockfd, NULL, 0, 0, (struct sockaddr *)(&cli_addr), &cli_len);
	if (unlikely(ret)) {
		int errsave = errno;
		ERR_TRACE();
		journal("Unable to read from socket: %s.\n", strerror(errsave));
		CHECK_SOCKET_ERROR(errsave);
	}

	cli_len = sizeof(cli_addr);

	ret = get_quote_of_the_day(&buffer, &length);
	if (unlikely(ret)) {
		return;
	}

	ret = sendto(sockfd, buffer, length, 0, (struct sockaddr *)(&cli_addr), cli_len);
	if (unlikely(ret < 0)) {
		ERR_TRACE();
		journal("Unable to write to UDP socket: %s.\n", strerror(errno));
		return;
	}
}

