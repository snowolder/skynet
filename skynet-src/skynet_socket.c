#include "skynet.h"

#include "skynet_socket.h"
#include "socket_server.h"
#include "skynet_server.h"
#include "skynet_mq.h"
#include "skynet_harbor.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static struct socket_server * SOCKET_SERVER = NULL;

void 
skynet_socket_init() {
	SOCKET_SERVER = socket_server_create(skynet_now());
}

void
skynet_socket_exit() {
	socket_server_exit(SOCKET_SERVER);
}

void
skynet_socket_free() {
	socket_server_release(SOCKET_SERVER);
	SOCKET_SERVER = NULL;
}

void
skynet_socket_updatetime() {
	socket_server_updatetime(SOCKET_SERVER, skynet_now());
}

// mainloop thread
static void
forward_message(int type, bool padding, struct socket_message * result) {
	struct skynet_socket_message *sm;
	size_t sz = sizeof(*sm);
	if (padding) {
		if (result->data) {
			size_t msg_sz = strlen(result->data);
			if (msg_sz > 128) {
				msg_sz = 128;
			}
			sz += msg_sz;
		} else {
			result->data = "";
		}
	}
	sm = (struct skynet_socket_message *)skynet_malloc(sz);
	sm->type = type;
	sm->id = result->id;
	sm->ud = result->ud;
	if (padding) {
		sm->buffer = NULL;
		memcpy(sm+1, result->data, sz - sizeof(*sm));
	} else {
		sm->buffer = result->data;
	}

	struct skynet_message message;
	message.source = 0;
	message.session = 0;
	message.data = sm;
	message.sz = sz | ((size_t)PTYPE_SOCKET << MESSAGE_TYPE_SHIFT);
	
	if (skynet_context_push((uint32_t)result->opaque, &message)) {
		// todo: report somewhere to close socket
		// don't call skynet_socket_close here (It will block mainloop)
		skynet_free(sm->buffer);
		skynet_free(sm);
	}
}

int 
skynet_socket_poll() {
	struct socket_server *ss = SOCKET_SERVER;
	assert(ss);
	struct socket_message result;
	int more = 1;
	int type = socket_server_poll(ss, &result, &more);
	switch (type) {
	case SOCKET_EXIT:
		return 0;
	case SOCKET_DATA:
		forward_message(SKYNET_SOCKET_TYPE_DATA, false, &result);
		break;
	case SOCKET_CLOSE:
		forward_message(SKYNET_SOCKET_TYPE_CLOSE, false, &result);
		break;
	case SOCKET_OPEN:
		forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
		break;
	case SOCKET_ERR:
		forward_message(SKYNET_SOCKET_TYPE_ERROR, true, &result);
		break;
	case SOCKET_ACCEPT:
		forward_message(SKYNET_SOCKET_TYPE_ACCEPT, true, &result);
		break;
	case SOCKET_UDP:
		forward_message(SKYNET_SOCKET_TYPE_UDP, false, &result);
		break;
	case SOCKET_WARNING:
		forward_message(SKYNET_SOCKET_TYPE_WARNING, false, &result);
		break;
	default:
		skynet_error(NULL, "Unknown socket message type %d.",type);
		return -1;
	}
	if (more) {
		return -1;
	}
	return 1;
}

int
skynet_socket_sendbuffer(struct skynet_context *ctx, struct socket_sendbuffer *buffer) {
	return socket_server_send(SOCKET_SERVER, buffer);
}

int
skynet_socket_sendbuffer_lowpriority(struct skynet_context *ctx, struct socket_sendbuffer *buffer) {
	return socket_server_send_lowpriority(SOCKET_SERVER, buffer);
}

int 
skynet_socket_listen(struct skynet_context *ctx, const char *host, int port, int backlog) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_listen(SOCKET_SERVER, source, host, port, backlog);
}

int 
skynet_socket_connect(struct skynet_context *ctx, const char *host, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_connect(SOCKET_SERVER, source, host, port);
}

int 
skynet_socket_bind(struct skynet_context *ctx, int fd) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_bind(SOCKET_SERVER, source, fd);
}

void 
skynet_socket_close(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_close(SOCKET_SERVER, source, id);
}

void 
skynet_socket_shutdown(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_shutdown(SOCKET_SERVER, source, id);
}

void 
skynet_socket_start(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_start(SOCKET_SERVER, source, id);
}

void
skynet_socket_pause(struct skynet_context *ctx, int id) {
	uint32_t source = skynet_context_handle(ctx);
	socket_server_pause(SOCKET_SERVER, source, id);
}


void
skynet_socket_nodelay(struct skynet_context *ctx, int id) {
	socket_server_nodelay(SOCKET_SERVER, id);
}

int 
skynet_socket_udp(struct skynet_context *ctx, const char * addr, int port) {
	uint32_t source = skynet_context_handle(ctx);
	return socket_server_udp(SOCKET_SERVER, source, addr, port);
}

int 
skynet_socket_udp_connect(struct skynet_context *ctx, int id, const char * addr, int port) {
	return socket_server_udp_connect(SOCKET_SERVER, id, addr, port);
}

int 
skynet_socket_udp_sendbuffer(struct skynet_context *ctx, const char * address, struct socket_sendbuffer *buffer) {
	return socket_server_udp_send(SOCKET_SERVER, (const struct socket_udp_address *)address, buffer);
}

const char *
skynet_socket_udp_address(struct skynet_socket_message *msg, int *addrsz) {
	if (msg->type != SKYNET_SOCKET_TYPE_UDP) {
		return NULL;
	}
	struct socket_message sm;
	sm.id = msg->id;
	sm.opaque = 0;
	sm.ud = msg->ud;
	sm.data = msg->buffer;
	return (const char *)socket_server_udp_address(SOCKET_SERVER, &sm, addrsz);
}

struct socket_info *
skynet_socket_info() {
	return socket_server_info(SOCKET_SERVER);
}


// ---------------------------------------------
struct socket_server_pool {
	struct socket_server **list;
	int count;
	int size;
};

static struct socket_server_pool ss_pool = {NULL, 0, 1};

static int
socket_poll(struct socket_server *ss)
{
	assert(ss);
	struct socket_message result;
	int more = 1;
	int type = socket_server_poll(ss, &result, &more);
	switch (type) {
		case SOCKET_EXIT:
			return 0;
		case SOCKET_DATA:
			forward_message(SKYNET_SOCKET_TYPE_DATA, false, &result);
			break;
		case SOCKET_CLOSE:
			forward_message(SKYNET_SOCKET_TYPE_CLOSE, false, &result);
			break;
		case SOCKET_OPEN:
			forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
			break;
		case SOCKET_ERR:
			forward_message(SKYNET_SOCKET_TYPE_ERROR, true, &result);
			break;
		case SOCKET_ACCEPT:
			forward_message(SKYNET_SOCKET_TYPE_ACCEPT, true, &result);
			break;
		case SOCKET_UDP:
			forward_message(SKYNET_SOCKET_TYPE_UDP, false, &result);
			break;
		default:
			skynet_error(NULL, "unknown socket message type: %d.", type);
			return -1;
	}
	if (more)
	{
		return -1;
	}
	return 1;
}

static void *
thread_qs_socket(void *p)
{
	struct socket_server *ss = p;
	for (;;)
	{
		int r = socket_poll(ss);
		if (r == 0)
			break;
		if (r < 0 && skynet_context_total() == 0)
			break;
	}
	return NULL;
}

int
new_socket_server(struct skynet_context *ctx) 
{
	pthread_t pid;
	struct socket_server *ss = socket_server_create(skynet_now());
	if (pthread_create(&pid, NULL, thread_qs_socket, ss))
	{
		socket_server_release(ss);
		return 0;
	}
	if (ss_pool.count + 1 == ss_pool.size)
	{
		struct socket_server **list = skynet_malloc(sizeof(struct socket_server *) * ss_pool.size);
		int i;
		for (i = 0; i < ss_pool.count; i++)
		{
			list[i] = ss_pool.list[i];
		}
		ss_pool.size *= 2;
		ss_pool.list = list;
	}
	ss_pool.list[ss_pool.count++] = ss;
	return ss_pool.count;
}

int count_socket_server()
{
	return ss_pool.count;
}

struct socket_server *
get_socket_server(const int id)
{
	if (id == 0)
		return SOCKET_SERVER;
	if (id < 1 || id > ss_pool.count)
		return NULL;
	return ss_pool.list[id - 1];
}

int qs_socket_send(struct skynet_context *ctx, int ssid, void *buffer)
{
	struct socket_server *ss = get_socket_server(ssid);
	return socket_server_send(ss, buffer);
}

void qs_socket_send_lowpriority(struct skynet_context *ctx, int ssid, void *buffer)
{
	struct socket_server *ss = get_socket_server(ssid);
	socket_server_send_lowpriority(ss, buffer);
}

int qs_socket_listen(struct skynet_context *ctx, int ssid, const char * host, int port, int backlog)
{
	uint32_t source = skynet_context_handle(ctx);
	struct socket_server *ss = get_socket_server(ssid);
	return socket_server_listen(ss, source, host, port, backlog);
}

int qs_socket_connect(struct skynet_context *ctx, int ssid, const char * host, int port)
{
	uint32_t source = skynet_context_handle(ctx);
	struct socket_server *ss = get_socket_server(ssid);
	return socket_server_connect(ss, source, host, port);
}

int qs_socket_bind(struct skynet_context * ctx, int ssid, int fd)
{
	uint32_t source = skynet_context_handle(ctx);
	struct socket_server *ss = get_socket_server(ssid);
	return socket_server_bind(ss, source, fd);
}

void qs_socket_close(struct skynet_context *ctx, int ssid, int id)
{
	uint32_t source = skynet_context_handle(ctx);
	struct socket_server *ss = get_socket_server(ssid);
	socket_server_close(ss, source, id);
}

void qs_socket_shutdown(struct skynet_context * ctx, int ssid, int id)
{
	uint32_t source = skynet_context_handle(ctx);
	struct socket_server *ss = get_socket_server(ssid);
	socket_server_shutdown(ss, source, id);
}

void qs_socket_start(struct skynet_context * ctx, int ssid, int id)
{
	uint32_t source = skynet_context_handle(ctx);
	struct socket_server *ss = get_socket_server(ssid);
	socket_server_start(ss, source, id);
}

void qs_socket_nodelay(struct skynet_context * ctx, int ssid, int id)
{
	struct socket_server *ss = get_socket_server(ssid);
	socket_server_nodelay(ss, id);
}

int qs_socket_udp(struct skynet_context *ctx, int ssid, const char * addr, int port)
{
	uint32_t source = skynet_context_handle(ctx);
	struct socket_server *ss = get_socket_server(ssid);
	return socket_server_udp(ss, source, addr, port);
}

int qs_socket_udp_connect(struct skynet_context *ctx, int ssid, int id, const char * addr, int port)
{
	struct socket_server *ss = get_socket_server(ssid);
	return socket_server_udp_connect(ss, id, addr, port);
}

int qs_socket_udp_send(struct skynet_context * ctx, int ssid, const char * addr, const void * buffer)
{
	struct socket_server *ss = get_socket_server(ssid);
	return socket_server_udp_send(ss, (const struct socket_udp_address *)addr, buffer);
}

const char * qs_socket_udp_address(int ssid, struct skynet_socket_message *msg, int *addrsz)
{
	if (msg->type != SKYNET_SOCKET_TYPE_UDP)
	{
		return NULL;
	}
	struct socket_message sm;
	sm.id = msg->id;
	sm.opaque = 0;
	sm.ud = msg->ud;
	sm.data = msg->buffer;
	struct socket_server *ss = get_socket_server(ssid);
	return (const char *)socket_server_udp_address(ss, &sm, addrsz);
}

