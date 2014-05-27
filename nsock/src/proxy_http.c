/***************************************************************************
 * proxy_http.c -- HTTP Connect proxying.                                  *
 *                                                                         *
 ***********************IMPORTANT NSOCK LICENSE TERMS***********************
 *                                                                         *
 * The nsock parallel socket event library is (C) 1999-2013 Insecure.Com   *
 * LLC This library is free software; you may redistribute and/or          *
 * modify it under the terms of the GNU General Public License as          *
 * published by the Free Software Foundation; Version 2.  This guarantees  *
 * your right to use, modify, and redistribute this software under certain *
 * conditions.  If this license is unacceptable to you, Insecure.Com LLC   *
 * may be willing to sell alternative licenses (contact                    *
 * sales@insecure.com ).                                                   *
 *                                                                         *
 * As a special exception to the GPL terms, Insecure.Com LLC grants        *
 * permission to link the code of this program with any version of the     *
 * OpenSSL library which is distributed under a license identical to that  *
 * listed in the included docs/licenses/OpenSSL.txt file, and distribute   *
 * linked combinations including the two. You must obey the GNU GPL in all *
 * respects for all of the code used other than OpenSSL.  If you modify    *
 * this file, you may extend this exception to your version of the file,   *
 * but you are not obligated to do so.                                     *
 *                                                                         *
 * If you received these files with a written license agreement stating    *
 * terms other than the (GPL) terms above, then that alternative license   *
 * agreement takes precedence over this comment.                           *
 *                                                                         *
 * Source is provided to this software because we believe users have a     *
 * right to know exactly what a program is going to do before they run it. *
 * This also allows you to audit the software for security holes (none     *
 * have been found so far).                                                *
 *                                                                         *
 * Source code also allows you to port Nmap to new platforms, fix bugs,    *
 * and add new features.  You are highly encouraged to send your changes   *
 * to the dev@nmap.org mailing list for possible incorporation into the    *
 * main distribution.  By sending these changes to Fyodor or one of the    *
 * Insecure.Org development mailing lists, or checking them into the Nmap  *
 * source code repository, it is understood (unless you specify otherwise) *
 * that you are offering the Nmap Project (Insecure.Com LLC) the           *
 * unlimited, non-exclusive right to reuse, modify, and relicense the      *
 * code.  Nmap will always be available Open Source, but this is important *
 * because the inability to relicense code has caused devastating problems *
 * for other Free Software projects (such as KDE and NASM).  We also       *
 * occasionally relicense the code to third parties as discussed above.    *
 * If you wish to specify special license conditions of your               *
 * contributions, just say so when you send them.                          *
 *                                                                         *
 * This program is distributed in the hope that it will be useful, but     *
 * WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU       *
 * General Public License v2.0 for more details                            *
 * (http://www.gnu.org/licenses/gpl-2.0.html).                             *
 *                                                                         *
 ***************************************************************************/

/* $Id $ */

#define _GNU_SOURCE
#include <stdio.h>

#include "nsock.h"
#include "nsock_internal.h"
#include "nsock_log.h"
#include <string.h>

#define DEFAULT_PROXY_PORT_HTTP 8080


extern struct timeval nsock_tod;
extern const struct proxy_spec ProxySpecHttp;


static int proxy_http_node_new(struct proxy_node **node, const struct uri *uri) {
  int rc;
  struct proxy_node *proxy;

  proxy = (struct proxy_node *)safe_zalloc(sizeof(struct proxy_node));
  proxy->spec = &ProxySpecHttp;

  rc = proxy_resolve(uri->host, (struct sockaddr *)&proxy->ss, &proxy->sslen);
  if (rc < 0) {
    free(proxy);
    *node = NULL;
    return -1;
  }

  if (uri->port == -1)
    proxy->port = DEFAULT_PROXY_PORT_HTTP;
  else
    proxy->port = (unsigned short)uri->port;

  rc = asprintf(&proxy->nodestr, "http://%s:%d", uri->host, proxy->port);
  if (rc < 0) {
    /* asprintf() failed for some reason but this is not a disaster (yet).
     * Set nodestr to NULL and try to keep on going. */
    proxy->nodestr = NULL;
  }

  *node = proxy;

  return 1;
}

static void proxy_http_node_delete(struct proxy_node *node) {
  if (!node)
    return;

  if (node->nodestr)
    free(node->nodestr);

  free(node);
}

static int handle_state_initial(mspool *nsp, msevent *nse, void *udata) {
  struct proxy_chain_context *px_ctx = nse->iod->px_ctx;
  struct sockaddr_storage *ss;
  size_t sslen;
  unsigned short port;
  struct proxy_node *next;
  int timeout;

  px_ctx->px_state = PROXY_STATE_HTTP_TCP_CONNECTED;

  next = proxy_ctx_node_next(px_ctx);
  if (next) {
    ss    = &next->ss;
    sslen = next->sslen;
    port  = next->port;
  } else {
    ss    = &px_ctx->target_ss;
    sslen = px_ctx->target_sslen;
    port  = px_ctx->target_port;
  }

  timeout = TIMEVAL_MSEC_SUBTRACT(nse->timeout, nsock_tod);

  nsock_printf(nsp, (nsock_iod)nse->iod, nsock_proxy_ev_dispatch,
               timeout, udata, "CONNECT %s:%d HTTP/1.1\r\n\r\n",
               inet_ntop_ez(ss, sslen), (int)port);

  nsock_readlines(nsp, (nsock_iod)nse->iod, nsock_proxy_ev_dispatch,
                  timeout, udata, 1);

  return 0;
}

static int handle_state_tcp_connected(mspool *nsp, msevent *nse, void *udata) {
  struct proxy_chain_context *px_ctx = nse->iod->px_ctx;
  char *res;
  int reslen;

  res = nse_readbuf(nse, &reslen);

  /* TODO string check!! */
  if (!((reslen >= 15) && strstr(res, "200 OK"))) {
    struct proxy_node *node = px_ctx->px_current;

    nsock_log_debug(nsp, "Connection refused from proxy %s", node->nodestr);
    return -EINVAL;
  }

  px_ctx->px_state = PROXY_STATE_HTTP_TUNNEL_ESTABLISHED;

  if (proxy_ctx_node_next(px_ctx) == NULL) {
    forward_event(nsp, nse, udata);
  } else {
    px_ctx->px_current = proxy_ctx_node_next(px_ctx);
    px_ctx->px_state   = PROXY_STATE_INITIAL;
    nsock_proxy_ev_dispatch(nsp, nse, udata);
  }
  return 0;
}

static void proxy_http_handler(nsock_pool nspool, nsock_event nsevent, void *udata) {
  int rc = 0;
  mspool *nsp = (mspool *)nspool;
  msevent *nse = (msevent *)nsevent;

  switch (nse->iod->px_ctx->px_state) {
    case PROXY_STATE_INITIAL:
      rc = handle_state_initial(nsp, nse, udata);
      break;

    case PROXY_STATE_HTTP_TCP_CONNECTED:
      if (nse->type == NSE_TYPE_READ)
        rc = handle_state_tcp_connected(nsp, nse, udata);
      break;

    case PROXY_STATE_HTTP_TUNNEL_ESTABLISHED:
      forward_event(nsp, nse, udata);
      break;

    default:
      fatal("Invalid proxy state!");
  }

  if (rc) {
    nse->status = NSE_STATUS_PROXYERROR;
    forward_event(nsp, nse, udata);
  }
}


/* ---- PROXY DEFINITION ---- */
static const struct proxy_op ProxyOpsHttp = {
  proxy_http_node_new,
  proxy_http_node_delete,
  proxy_http_handler,
};

const struct proxy_spec ProxySpecHttp = {
  "http://",
  PROXY_TYPE_HTTP,
  &ProxyOpsHttp,
};
