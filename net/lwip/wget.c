// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2024 Linaro Ltd. */

#include <command.h>
#include <console.h>
#include <display_options.h>
#include <efi_loader.h>
#include <image.h>
#include <lwip/apps/http_client.h>
#include <lwip/timeouts.h>
#include <mapmem.h>
#include <net.h>
#include <time.h>

#define SERVER_NAME_SIZE 200
#define HTTP_PORT_DEFAULT 80
#define PROGRESS_PRINT_STEP_BYTES (100 * 1024)

enum done_state {
        NOT_DONE = 0,
        SUCCESS = 1,
        FAILURE = 2
};

struct wget_ctx {
	char *path;
	ulong daddr;
	ulong saved_daddr;
	ulong size;
	ulong prevsize;
	ulong start_time;
	enum done_state done;
};

static int parse_url(char *url, char *host, u16 *port, char **path)
{
	char *p, *pp;
	long lport;

	p = strstr(url, "http://");
	if (!p) {
		log_err("only http:// is supported\n");
		return -EINVAL;
	}

	p += strlen("http://");

	/* Parse hostname */
	pp = strchr(p, ':');
	if (!pp)
		pp = strchr(p, '/');
	if (!pp)
		return -EINVAL;

	if (p + SERVER_NAME_SIZE <= pp)
		return -EINVAL;

	memcpy(host, p, pp - p);
	host[pp - p] = '\0';

	if (*pp == ':') {
		/* Parse port number */
		p = pp + 1;
		lport = simple_strtol(p, &pp, 10);
		if (pp && *pp != '/')
			return -EINVAL;
		if (lport > 65535)
			return -EINVAL;
		*port = (u16)lport;
	} else {
		*port = HTTP_PORT_DEFAULT;
	}
	if (*pp != '/')
		return -EINVAL;
	*path = pp;

	return 0;
}

/*
 * Legacy syntax support
 * Convert [<server_name_or_ip>:]filename into a URL if needed
 */
static int parse_legacy_arg(char *arg, char *nurl, size_t rem)
{
	char *p = nurl;
	size_t n;
	char *col = strchr(arg, ':');
	char *env;
	char *server;
	char *path;

	if (strstr(arg, "http") == arg) {
		n = snprintf(nurl, rem, "%s", arg);
		if (n < 0 || n > rem)
			return -1;
		return 0;
	}

	n = snprintf(p, rem, "%s", "http://");
	if (n < 0 || n > rem)
		return -1;
	p += n;
	rem -= n;

	if (col) {
		n = col - arg;
		server = arg;
		path = col + 1;
	} else {
		env = env_get("httpserverip");
		if (!env)
			env = env_get("serverip");
		if (!env) {
			log_err("error: httpserver/serverip has to be set\n");
			return -1;
		}
		n = strlen(env);
		server = env;
		path = arg;
	}

	if (rem < n)
		return -1;
	strncpy(p, server, n);
	p += n;
	rem -= n;
	if (rem < 1)
		return -1;
	*p = '/';
	p++;
	rem--;
	n = strlen(path);
	if (rem < n)
		return -1;
	strncpy(p, path, n);
	p += n;
	rem -= n;
	if (rem < 1)
		return -1;
	*p = '\0';

	return 0;
}

static err_t httpc_recv_cb(void *arg, struct altcp_pcb *pcb, struct pbuf *pbuf,
			   err_t err)
{
	struct wget_ctx *ctx = arg;
	struct pbuf *buf;

	if (!pbuf)
		return ERR_BUF;

	if (!ctx->start_time)
		ctx->start_time = get_timer(0);

	for (buf = pbuf; buf; buf = buf->next) {
		memcpy((void *)ctx->daddr, buf->payload, buf->len);
		ctx->daddr += buf->len;
		ctx->size += buf->len;
		if (ctx->size - ctx->prevsize > PROGRESS_PRINT_STEP_BYTES) {
			printf("#");
			ctx->prevsize = ctx->size;
		}
	}

	altcp_recved(pcb, pbuf->tot_len);
	pbuf_free(pbuf);
	return ERR_OK;
}

static void httpc_result_cb(void *arg, httpc_result_t httpc_result,
			    u32_t rx_content_len, u32_t srv_res, err_t err)
{
	struct wget_ctx *ctx = arg;
	ulong elapsed;

	if (httpc_result != HTTPC_RESULT_OK) {
		log_err("\nHTTP client error %d\n", httpc_result);
		ctx->done = FAILURE;
		return;
	}
	if (srv_res != 200) {
		log_err("\nHTTP server error %d\n", srv_res);
		ctx->done = FAILURE;
		return;
	}

	elapsed = get_timer(ctx->start_time);
	if (!elapsed)
		elapsed = 1;
	if (rx_content_len > PROGRESS_PRINT_STEP_BYTES)
		printf("\n");
	printf("%u bytes transferred in %lu ms (", rx_content_len, elapsed);
	print_size(rx_content_len / elapsed * 1000, "/s)\n");
	printf("Bytes transferred = %lu (%lx hex)\n", ctx->size, ctx->size);
	efi_set_bootdev("Net", "", ctx->path, map_sysmem(ctx->saved_daddr, 0),
			rx_content_len);
	if (env_set_hex("filesize", rx_content_len) ||
	    env_set_hex("fileaddr", ctx->saved_daddr)) {
		log_err("Could not set filesize or fileaddr\n");
		ctx->done = FAILURE;
		return;
	}

	ctx->done = SUCCESS;
}

static int wget_loop(struct udevice *udev, ulong dst_addr, char *uri)
{
	char server_name[SERVER_NAME_SIZE];
	httpc_connection_t conn;
	httpc_state_t *state;
	struct netif *netif;
	struct wget_ctx ctx;
	char *path;
	u16 port;

	ctx.daddr = dst_addr;
	ctx.saved_daddr = dst_addr;
	ctx.done = NOT_DONE;
	ctx.size = 0;
	ctx.prevsize = 0;
	ctx.start_time = 0;

	if (parse_url(uri, server_name, &port, &path))
		return CMD_RET_USAGE;

	netif = net_lwip_new_netif(udev);
	if (!netif)
		return -1;

	memset(&conn, 0, sizeof(conn));
	conn.result_fn = httpc_result_cb;
	ctx.path = path;
	if (httpc_get_file_dns(server_name, port, path, &conn, httpc_recv_cb,
			       &ctx, &state)) {
		net_lwip_remove_netif(netif);
		return CMD_RET_FAILURE;
	}

	while (!ctx.done) {
		net_lwip_rx(udev, netif);
		sys_check_timeouts();
		if (ctrlc())
			break;
	}

	net_lwip_remove_netif(netif);

	if (ctx.done == SUCCESS)
		return 0;

	return -1;
}

int wget_with_dns(ulong dst_addr, char *uri)
{
	eth_set_current();

	return wget_loop(eth_get_dev(), dst_addr, uri);
}

int do_wget(struct cmd_tbl *cmdtp, int flag, int argc, char * const argv[])
{
	char *end;
	char *url;
	ulong dst_addr;
	char nurl[1024];

	if (argc < 2 || argc > 3)
		return CMD_RET_USAGE;

	dst_addr = hextoul(argv[1], &end);
        if (end == (argv[1] + strlen(argv[1]))) {
		if (argc < 3)
			return CMD_RET_USAGE;
		url = argv[2];
	} else {
		dst_addr = image_load_addr;
		url = argv[1];
	}

	if (parse_legacy_arg(url, nurl, sizeof(nurl)))
	    return CMD_RET_FAILURE;

	if (wget_with_dns(dst_addr, nurl))
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}

/**
 * wget_validate_uri() - validate the uri for wget
 *
 * @uri:	uri string
 *
 * This function follows the current U-Boot wget implementation.
 * scheme: only "http:" is supported
 * authority:
 *   - user information: not supported
 *   - host: supported
 *   - port: not supported(always use the default port)
 *
 * Uri is expected to be correctly percent encoded.
 * This is the minimum check, control codes(0x1-0x19, 0x7F, except '\0')
 * and space character(0x20) are not allowed.
 *
 * TODO: stricter uri conformance check
 *
 * Return:	true on success, false on failure
 */
bool wget_validate_uri(char *uri)
{
	char c;
	bool ret = true;
	char *str_copy, *s, *authority;

	for (c = 0x1; c < 0x21; c++) {
		if (strchr(uri, c)) {
			log_err("invalid character is used\n");
			return false;
		}
	}
	if (strchr(uri, 0x7f)) {
		log_err("invalid character is used\n");
		return false;
	}

	if (strncmp(uri, "http://", 7)) {
		log_err("only http:// is supported\n");
		return false;
	}
	str_copy = strdup(uri);
	if (!str_copy)
		return false;

	s = str_copy + strlen("http://");
	authority = strsep(&s, "/");
	if (!s) {
		log_err("invalid uri, no file path\n");
		ret = false;
		goto out;
	}
	s = strchr(authority, '@');
	if (s) {
		log_err("user information is not supported\n");
		ret = false;
		goto out;
	}

out:
	free(str_copy);

	return ret;
}
