#include "libtrading/proto/fix_message.h"
#include "libtrading/proto/fix_session.h"

#include "libtrading/array.h"
#include "libtrading/die.h"

#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>

struct protocol_info {
	const char		*name;
	void			(*session_accept)(int incoming_fd);
};

static unsigned long fix_execution_report_fields(struct fix_field *fields)
{
	char fmt[64], buf[64];
	unsigned long nr = 0;
	struct timeval tv;
	struct tm *tm;

	gettimeofday(&tv, NULL);
	tm = gmtime(&tv.tv_sec);
	strftime(fmt, sizeof fmt, "%Y%m%d-%H:%M:%S", tm);
	snprintf(buf, sizeof buf, "%s.%03ld", fmt, (long)tv.tv_usec / 1000);

	fields[nr++] = FIX_STRING_FIELD(OrderID, "OrderID");
	fields[nr++] = FIX_STRING_FIELD(Symbol, "Symbol");
	fields[nr++] = FIX_STRING_FIELD(ExecID, "ExecID");
	fields[nr++] = FIX_STRING_FIELD(OrdStatus, "2");
	fields[nr++] = FIX_STRING_FIELD(ExecType, "0");
	fields[nr++] = FIX_FLOAT_FIELD(LeavesQty, 0);
	fields[nr++] = FIX_FLOAT_FIELD(CumQty, 100);
	fields[nr++] = FIX_FLOAT_FIELD(AvgPx, 100);
	fields[nr++] = FIX_STRING_FIELD(Side, "1");

	return nr;

}

static void fix_session_accept(int incoming_fd)
{
	struct fix_message logon_msg;
	struct fix_session *session;
	struct fix_field *fields;
	struct fix_message *msg;
	unsigned long nr;

	session		= fix_session_new(incoming_fd, FIX_4_4, "BUYSIDE", "SELLSIDE");

	msg = fix_session_recv(session, 0);

	logon_msg	= (struct fix_message) {
		.type		= FIX_MSG_TYPE_LOGON,
	};
	fix_session_send(session, &logon_msg, 0);

	fields = calloc(FIX_MAX_FIELD_NUMBER, sizeof(struct fix_field));
	if (!fields)
		return;

	nr = fix_execution_report_fields(fields);

	for (;;) {
		struct fix_message logout_msg;

		fix_session_test_request(session);

		msg = fix_session_recv(session, 0);
		if (!msg)
			continue;
		else if (fix_message_type_is(msg, FIX_MSG_TYPE_NEW_ORDER_SINGLE)) {
			fix_session_execution_report(session, fields, nr);
			continue;
		}
		else if (!fix_message_type_is(msg, FIX_MSG_TYPE_LOGOUT))
			continue;

		logout_msg	= (struct fix_message) {
			.type		= FIX_MSG_TYPE_LOGOUT,
		};
		fix_session_send(session, &logout_msg, 0);
		break;
	}

	free(fields);
	fix_session_free(session);
}

static const struct protocol_info protocols[] = {
	{ "fix",		fix_session_accept },
};

static const struct protocol_info *lookup_protocol_info(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(protocols); i++) {
		const struct protocol_info *proto_info = &protocols[i];

		if (!strcmp(proto_info->name, name))
			return proto_info;
	}
	return NULL;
}

static void usage(void)
{
	printf("\n  usage: trade server -p [port] -c [protocol]\n\n");
}

static int socket_setopt(int sockfd, int level, int optname, int optval)
{
	return setsockopt(sockfd, level, optname, (void *) &optval, sizeof(optval));
}

int main(int argc, char *argv[])
{
	const struct protocol_info *proto_info;
	const char *proto = NULL;
	struct sockaddr_in sa;
	int port = 0;
	int sockfd;
	int opt;

	while ((opt = getopt(argc, argv, "p:c:")) != -1) {
		switch (opt) {
		case 'p':
			port = atoi(optarg);
			break;
		case 'c':
			proto = optarg;
			break;
		default: /* '?' */
			usage();
			exit(EXIT_FAILURE);
		}
	}

	if (!port || !proto) {
		usage();
		exit(EXIT_FAILURE);
	}

	proto_info = lookup_protocol_info(proto);
	if (!proto_info) {
		printf("Unsupported protocol '%s'\n", proto);
		exit(EXIT_FAILURE);
	}

	sockfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sockfd < 0)
		die("cannot create socket");

	if (socket_setopt(sockfd, IPPROTO_TCP, TCP_NODELAY, 1) < 0)
		die("cannot set socket option TCP_NODELAY");

	if (socket_setopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 1) < 0)
		die("cannot set socket option SO_REUSEADDR");

	sa = (struct sockaddr_in) {
		.sin_family		= AF_INET,
		.sin_port		= htons(port),
		.sin_addr		= (struct in_addr) {
			.s_addr			= INADDR_ANY,
		},
	};

	if (bind(sockfd, (const struct sockaddr *)&sa, sizeof(struct sockaddr_in)) < 0)
		die("bind failed");

	printf("Server is listening to port %d using '%s' protocol...\n", port, proto);

	if (listen(sockfd, 10) < 0)
		die("listen failed");

	for (;;) {
		int incoming_fd;

		incoming_fd = accept(sockfd, NULL, NULL);
		if (incoming_fd < 0)
			die("accept failed");

		if (socket_setopt(incoming_fd, IPPROTO_TCP, TCP_NODELAY, 1) < 0)
			die("cannot set socket option TCP_NODELAY");

		proto_info->session_accept(incoming_fd);

		shutdown(incoming_fd, SHUT_RDWR);

		close(incoming_fd);
	}

	close(sockfd);

	return 0;
}
