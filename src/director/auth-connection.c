/* Copyright (c) 2010-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "net.h"
#include "llist.h"
#include "safe-memset.h"
#include "auth-client-interface.h"
#include "director.h"
#include "auth-connection.h"

#include <unistd.h>

struct auth_connection {
	struct auth_connection *prev, *next;

	struct director *dir;
	char *path;
	int fd;
	struct io *io;
	struct istream *input;
	struct ostream *output;

	auth_input_callback *callback;
	void *context;
};

static struct auth_connection *auth_connections;

static void auth_connection_disconnected(struct auth_connection **conn);

static void auth_connection_input(struct auth_connection *conn)
{
	char *line;

	switch (i_stream_read(conn->input)) {
	case 0:
		return;
	case -1:
		/* disconnected */
		e_error(conn->dir->event, "Auth server disconnected unexpectedly");
		auth_connection_disconnected(&conn);
		return;
	case -2:
		/* buffer full */
		e_error(conn->dir->event,
			"BUG: Auth server sent us more than %d bytes",
			(int)AUTH_CLIENT_MAX_LINE_LENGTH);
		auth_connection_disconnected(&conn);
		return;
	}

	while ((line = i_stream_next_line(conn->input)) != NULL) {
		T_BEGIN {
			conn->callback(line, conn->context);
			safe_memset(line, 0, strlen(line));
		} T_END;
	}
}

struct auth_connection *
auth_connection_init(struct director *dir, const char *path)
{
	struct auth_connection *conn;
 
	conn = i_new(struct auth_connection, 1);
	conn->dir = dir;
	conn->fd = -1;
	conn->path = i_strdup(path);
	DLLIST_PREPEND(&auth_connections, conn);
	return conn;
}

void auth_connection_set_callback(struct auth_connection *conn,
				  auth_input_callback *callback, void *context)
{
	conn->callback = callback;
	conn->context = context;
}

int auth_connection_connect(struct auth_connection *conn)
{
	i_assert(conn->fd == -1);

	conn->fd = net_connect_unix_with_retries(conn->path, 1000);
	if (conn->fd == -1) {
		e_error(conn->dir->event, "connect(%s) failed: %m", conn->path);
		return -1;
	}

	conn->input = i_stream_create_fd(conn->fd, AUTH_CLIENT_MAX_LINE_LENGTH);
	conn->output = o_stream_create_fd(conn->fd, SIZE_MAX);
	o_stream_set_no_error_handling(conn->output, TRUE);
	conn->io = io_add(conn->fd, IO_READ, auth_connection_input, conn);
	return 0;
}

void auth_connection_deinit(struct auth_connection **_conn)
{
	struct auth_connection *conn = *_conn;

	*_conn = NULL;

	DLLIST_REMOVE(&auth_connections, conn);
	if (conn->fd != -1) {
		io_remove(&conn->io);
		i_stream_unref(&conn->input);
		o_stream_unref(&conn->output);

		if (close(conn->fd) < 0)
			e_error(conn->dir->event, "close(auth connection) failed: %m");
	}
	i_free(conn->path);
	i_free(conn);
}

static void auth_connection_disconnected(struct auth_connection **_conn)
{
	struct auth_connection *conn = *_conn;

	*_conn = NULL;
	/* notify callback. it should deinit this connection */
	conn->callback(NULL, conn->context);
}

struct ostream *auth_connection_get_output(struct auth_connection *conn)
{
	i_assert(conn->output != NULL);
	return conn->output;
}

void auth_connections_deinit(void)
{
	while (auth_connections != NULL) {
		struct auth_connection *conn = auth_connections;

		auth_connection_disconnected(&conn);
	}
}
