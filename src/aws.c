// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>
#include <time.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static const char double_crlf[] = "\r\n\r\n";

static const char HTTP_ok[] = "HTTP/1.1 200 OK\r\n";
static const char HTTP_length[] = "Content-Length: %zu\r\n";
static const char HTTP_type[] = "Content-Type: text/plain\r\n";
static const char HTTP_conn_close[] = "Connection: close\r\n\r\n";
static const char HTTP_404_response[] =
"HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 0\r\nConnection: close\r\nNot found\r\n\r\n";
struct timespec time_out = {0, 0};

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */
	memset(conn->send_buffer, 0, BUFSIZ);
	conn->send_len = 0;

	memcpy(conn->send_buffer, HTTP_ok, strlen(HTTP_ok));
	conn->send_len += strlen(HTTP_ok);

	conn->send_len += snprintf(conn->send_buffer + conn->send_len, BUFSIZ, HTTP_length, conn->file_size);

	memcpy(conn->send_buffer + conn->send_len, HTTP_type, strlen(HTTP_type));
	conn->send_len += strlen(HTTP_type);

	memcpy(conn->send_buffer + conn->send_len, HTTP_conn_close, strlen(HTTP_conn_close));
	conn->send_len += strlen(HTTP_conn_close);
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */
	memset(conn->send_buffer, 0, BUFSIZ);
	conn->send_len = 0;
	memcpy(conn->send_buffer, HTTP_404_response, strlen(HTTP_404_response));
	conn->send_len += strlen(HTTP_404_response);
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */
	char full_path_static[BUFSIZ];

	strcpy(full_path_static, "/");
	strcat(full_path_static, AWS_REL_STATIC_FOLDER);

	if (strncmp(full_path_static, conn->request_path, strlen(full_path_static)) == 0)
		return RESOURCE_TYPE_STATIC;

	char full_path_dynamic[BUFSIZ];

	strcpy(full_path_dynamic, "/");
	strcat(full_path_dynamic, AWS_REL_DYNAMIC_FOLDER);
	if (strncmp(full_path_dynamic, conn->request_path, strlen(full_path_dynamic)) == 0)
		return RESOURCE_TYPE_DYNAMIC;

	return RESOURCE_TYPE_NONE;
}


struct connection *connection_create(int sockfd)
{
	/* TODO: Initialize connection structure on given socket. */
	struct connection *new_connection = calloc(1, sizeof(struct connection));

	new_connection->fd = -1;
	memset(new_connection->filename, 0, BUFSIZ);
	new_connection->eventfd = eventfd(0, EFD_NONBLOCK);
	new_connection->sockfd = sockfd;

	io_setup(1, &new_connection->ctx);
	memset(&new_connection->iocb, 0, sizeof(struct iocb));
	new_connection->piocb[0] = &new_connection->iocb;
	new_connection->file_size = 0;

	memset(new_connection->recv_buffer, 0, BUFSIZ);
	new_connection->recv_len = 0;

	memset(new_connection->send_buffer, 0, BUFSIZ);
	new_connection->send_len = 0;
	new_connection->send_pos = 0;
	new_connection->file_pos = 0;
	new_connection->async_read_len = 0;

	new_connection->have_path = 0;
	memset(new_connection->request_path, 0, BUFSIZ);

	new_connection->res_type = RESOURCE_TYPE_NONE;
	new_connection->state = STATE_INITIAL;

	http_parser_init(&new_connection->request_parser, HTTP_REQUEST);
	new_connection->request_parser.data = new_connection;

	return new_connection;
}

int submit_read_asynchronous(struct connection *conn, int remsize)
{
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, remsize, conn->file_pos);
	conn->iocb.data = conn;
	conn->piocb[0] = &conn->iocb;

	if (io_submit(conn->ctx, 1, conn->piocb) <= -1)
		return -1;
	conn->file_pos += remsize;
	return 0;
}

void connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
	if (!conn || conn->fd <= -1)
		return;

	int remsize = conn->file_size - conn->file_pos;

	if (remsize > BUFSIZ)
		remsize = BUFSIZ - 1;

	submit_read_asynchronous(conn, remsize);
}

int fd_is_opened(int fd)
{
	return (fd > -1);
}

void connection_remove(struct connection *conn)
{
	/* TODO: Remove connection handler. */
	if (fd_is_opened(conn->sockfd))
		close(conn->sockfd);
	if (fd_is_opened(conn->eventfd))
		close(conn->eventfd);
	if (fd_is_opened(conn->fd))
		close(conn->fd);
	free(conn);
}

void handle_new_connection(void)
{
	/* TODO: Handle a new connection request on the server socket. */
	struct sockaddr_in client_address;
	socklen_t client_address_len = sizeof(client_address);


	/* TODO: Accept new connection. */
	int client_server_sockfd = accept(listenfd,
				(struct sockaddr *)&client_address, &client_address_len);

	/* TODO: Set socket to be non-blocking. */
	fcntl(client_server_sockfd, F_SETFL,
					fcntl(client_server_sockfd, F_GETFL, 0) | O_NONBLOCK);
	/* TODO: Instantiate new connection handler. */
	struct connection *new_connection = connection_create(client_server_sockfd);

	/* TODO: Add socket to epoll. */
	w_epoll_add_ptr_in(epollfd, client_server_sockfd, new_connection);

	/* TODO: Initialize HTTP_REQUEST parser. */
	http_parser_init(&new_connection->request_parser, HTTP_REQUEST);
	new_connection->request_parser.data = new_connection;
}

void update_received_data(struct connection *conn, char *buf, int buf_size)
{
	memcpy(conn->recv_buffer + conn->recv_len, buf, buf_size);
	conn->recv_len += buf_size;
}

int is_header_end(struct connection *conn)
{
	return (strstr(conn->recv_buffer, double_crlf) != NULL);
}

void receive_data(struct connection *conn)
{
	/* TODO: Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	if (!conn)
		return;

	int curr_received_bytes;
	char buffer[BUFSIZ];

	curr_received_bytes = recv(conn->sockfd, buffer, BUFSIZ, 0);

	if (curr_received_bytes < 0) {
		connection_remove(conn);
		return;
	}

	// keep calling this function till we receive all the header
	// will keep popping as event with EPOLLIN flag

	if (curr_received_bytes + conn->recv_len < BUFSIZ) {
		update_received_data(conn, buffer, curr_received_bytes);
		if (is_header_end(conn)) {
			conn->state = STATE_REQUEST_RECEIVED;
			w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		}
	}
}

int connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */
	if (!conn || !conn->have_path)
		return -1;

	char full_path[BUFSIZ] = "";

	memcpy(full_path, AWS_DOCUMENT_ROOT, strlen(AWS_DOCUMENT_ROOT));
	memcpy(full_path + strlen(AWS_DOCUMENT_ROOT), conn->request_path + 1, strlen(conn->request_path));

	conn->fd = open(full_path, O_RDONLY);

	if (conn->fd <= -1)
		return -1;

	struct stat filestat;
	int rc;

	rc = fstat(conn->fd, &filestat);
	if (rc < 0)
		return -1;
	conn->file_size = filestat.st_size;
	conn->file_pos = 0;

	return 1;
}



void connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
	if (conn == NULL) {
		perror("Invalid connection");
		return;
	}

	struct io_event events[1];

	int num_events = io_getevents(conn->ctx, 1, 1, events, &time_out);

	if (num_events == 0)
		return;

	struct connection *event_conn = (struct connection *)events[0].data;

	if (event_conn != conn)
		return;

	conn->async_read_len = events[0].res;
}

int parse_header(struct connection *conn)
{
	/* TODO: Parse the HTTP header and extract the file path. */
	/* Use mostly null settings except for on_path callback. */
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};
	if (http_parser_execute(&conn->request_parser, &settings_on_path,
	conn->recv_buffer, conn->recv_len) != conn->recv_len)
		return -1;
	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* TODO: Send static data using sendfile(2). */

	if (!conn || conn->fd <= -1)
		return STATE_NO_STATE;

	off_t offset = conn->file_pos;
	int no_bytes_sent = sendfile(conn->sockfd, conn->fd, &offset, conn->file_size - offset);

	if (no_bytes_sent < 1)
		return STATE_CONNECTION_CLOSED;
	conn->file_pos = offset;

	if (conn->file_pos == conn->file_size)
		return STATE_DATA_SENT;

	return STATE_SENDING_DATA;
}

int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	int no_bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_pos, conn->send_len - conn->send_pos, 0);

	if (no_bytes_sent <= -1)
		return -1;

	conn->send_pos += no_bytes_sent;
	if (conn->send_pos == conn->send_len) {
		conn->send_pos = 0;
		conn->send_len = 0;
	}

	return no_bytes_sent;
}


int connection_send_dynamic(struct connection *conn)
{
	/* TODO: Read data asynchronously.
	 * Returns 0 on success and -1 on error.
	 */

	int no_bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_pos, conn->async_read_len - conn->send_pos, 0);

	if (no_bytes_sent <= -1)
		return -1;
	conn->send_pos += no_bytes_sent;
	if (conn->send_pos == conn->async_read_len) {
		conn->send_pos = 0;
		conn->send_len = 0;
		conn->state = STATE_HEADER_SENT;
		if (conn->file_pos == conn->file_size) {
			conn->file_pos = 0;
			conn->file_size = 0;
			conn->state = STATE_DATA_SENT;
		}
	}
	return 0;
}


void handle_input(struct connection *conn)
{
	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */

	switch (conn->state) {
	case STATE_INITIAL:

		conn->state = STATE_RECEIVING_DATA;
		break;
	case STATE_RECEIVING_DATA:
		receive_data(conn);
		break;
	default:
		printf("shouldn't get here hehe %d\n", conn->state);
	}
}

void handle_output(struct connection *conn)
{
	/* TODO: Handle output information: may be a new valid requests or notification of
	 * completion of an asynchronous I/O operation or invalid requests.
	 */
	int rc;

	switch (conn->state) {
	case STATE_REQUEST_RECEIVED:
		rc = parse_header(conn);

		if (rc <= -1) {
			connection_remove(conn);
			break;
		}

		conn->res_type = connection_get_resource_type(conn);

		conn->state = STATE_SENDING_404;
		switch (conn->res_type) {
		case RESOURCE_TYPE_STATIC:

			rc = connection_open_file(conn);
			if (rc < 0)
				break;

			connection_prepare_send_reply_header(conn);
			conn->state = STATE_SENDING_HEADER;
			break;
		case RESOURCE_TYPE_DYNAMIC:
			rc = connection_open_file(conn);
			if (rc < 0)
				break;

			connection_prepare_send_reply_header(conn);
			conn->state = STATE_SENDING_HEADER;
			break;

		default:
			break;
		}

		break;
	case STATE_SENDING_HEADER:
		if (connection_send_data(conn) <= -1) {
			connection_remove(conn);
			break;
		}
		if (conn->send_len == 0)
			conn->state = STATE_HEADER_SENT;
		break;
	case STATE_SENDING_404:
		connection_prepare_send_404(conn);
		if (connection_send_data(conn) <= -1)
			connection_remove(conn);

		if (conn->send_len == 0)
			conn->state = STATE_404_SENT;
		break;
	case STATE_HEADER_SENT:
		conn->state = (conn->res_type == RESOURCE_TYPE_STATIC) ? STATE_SENDING_DATA : STATE_ASYNC_ONGOING;
		if (conn->state == STATE_ASYNC_ONGOING)
			connection_start_async_io(conn);
		break;
	case STATE_404_SENT:
		connection_remove(conn);
		break;
	case STATE_CONNECTION_CLOSED:
		break;
	case STATE_ASYNC_ONGOING:
		connection_complete_async_io(conn);
		conn->state = STATE_SENDING_DATA;
		break;
	case STATE_SENDING_DATA:
		switch (conn->res_type) {
		case RESOURCE_TYPE_STATIC:
			rc = connection_send_static(conn);
			if (rc == STATE_DATA_SENT) {
				conn->state = STATE_DATA_SENT;
				connection_remove(conn);
			}
			if (rc == STATE_CONNECTION_CLOSED)
				connection_remove(conn);
			break;
		case RESOURCE_TYPE_DYNAMIC:
			rc = connection_send_dynamic(conn);
			if (rc <= -1)
				connection_remove(conn);
			break;
		default:
			break;
		}
		break;
	case STATE_DATA_SENT:
		conn->state = STATE_CONNECTION_CLOSED;
		break;
	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

int is_output_event(struct epoll_event event)
{
	return (event.events & EPOLLOUT);
}

int is_input_event(struct epoll_event event)
{
	return (event.events & EPOLLIN);
}

void handle_client(struct epoll_event event)
{
	/* TODO: Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */
	if (event.data.fd == listenfd && is_input_event(event)) {
		handle_new_connection();
		return;
	}
	if (is_input_event(event)) {
		handle_input(event.data.ptr);
		return;
	}
	if (is_output_event(event))
		handle_output(event.data.ptr);
}

int main(void)
{
	/* TODO: Initialize asynchronous operations. */
	io_setup(10, &ctx);
	/* TODO: Initialize multiplexing. */
	epollfd = w_epoll_create();
	/* TODO: Create server socket. */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);

	/* TODO: Add server socket to epoll object*/
	w_epoll_add_fd_in(epollfd, listenfd);
	/* Uncomment the following line for debugging. */
	//dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		memset(&rev, 0, sizeof(struct epoll_event));

		/* TODO: Wait for events. */
		w_epoll_wait_infinite(epollfd, &rev);
		handle_client(rev);
		/* TODO: Switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
	}

	return 0;
}
