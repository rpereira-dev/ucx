/**
* Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2019. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "tcp_sockcm_ep.h"
#include "tcp.h"
#include <ucs/sys/sock.h>
#include <ucs/async/async.h>
#include <ucs/arch/bitops.h>
#include <ucs/sys/string.h>


#define UCT_TCP_SOCKCM_EP_MAX_DEVICE_ADDR_LEN (sizeof(uct_tcp_device_addr_t) + \
                                               sizeof(struct in6_addr))


const char *uct_tcp_sockcm_cm_ep_peer_addr_str(uct_tcp_sockcm_ep_t *cep,
                                               char *buf, size_t max)
{
    struct sockaddr_storage remote_dev_addr = {0}; /* Suppress Clang false-positive */
    socklen_t remote_dev_addr_len;
    ucs_status_t status;

    /* get the device address of the remote peer associated with the connected fd */
    status = ucs_socket_getpeername(cep->fd, &remote_dev_addr, &remote_dev_addr_len);
    if (status != UCS_OK) {
        ucs_snprintf_safe(buf, max, "<%s>", ucs_status_string(status));
        return buf;
    }

    return ucs_sockaddr_str((const struct sockaddr*)&remote_dev_addr, buf, max);
}

void uct_tcp_sockcm_ep_close_fd(int *fd)
{
    ucs_async_remove_handler(*fd, 1);
    ucs_close_fd(fd);
}

static int uct_tcp_sockcm_ep_is_connected(uct_tcp_sockcm_ep_t *cep)
{
    return cep->state & (UCT_TCP_SOCKCM_EP_CLIENT_CONNECTED_CB_INVOKED |
                         UCT_TCP_SOCKCM_EP_SERVER_NOTIFY_CB_INVOKED);
}

static void uct_tcp_sockcm_ep_client_connect_cb(uct_tcp_sockcm_ep_t *cep,
                                                uct_cm_remote_data_t *remote_data,
                                                ucs_status_t status)
{
    cep->state |= UCT_TCP_SOCKCM_EP_CLIENT_CONNECTED_CB_INVOKED;
    uct_cm_ep_client_connect_cb(&cep->super, remote_data, status);
}

static void uct_tcp_sockcm_ep_disconnect_cb(uct_tcp_sockcm_ep_t *cep)
{
    cep->state |= UCT_TCP_SOCKCM_EP_DISCONNECTED;
    uct_cm_ep_disconnect_cb(&cep->super);
}

static void uct_tcp_sockcm_ep_server_notify_cb(uct_tcp_sockcm_ep_t *cep,
                                               ucs_status_t status)
{
    cep->state |= UCT_TCP_SOCKCM_EP_SERVER_NOTIFY_CB_INVOKED;
    uct_cm_ep_server_conn_notify_cb(&cep->super, status);
}

static ucs_status_t
uct_tcp_sockcm_ep_pack_priv_data(uct_tcp_sockcm_ep_t *cep, const void *data,
                                 size_t data_length)
{
    uct_tcp_sockcm_priv_data_hdr_t *hdr =
            (uct_tcp_sockcm_priv_data_hdr_t*)cep->comm_ctx.buf;

    ucs_assert(cep->comm_ctx.offset == 0);
    ucs_assert(!(cep->state & UCT_TCP_SOCKCM_EP_PRIV_DATA_PACKED));

    if (data_length > uct_tcp_sockcm_ep_get_cm(cep)->priv_data_len) {
        cep->state |= UCT_TCP_SOCKCM_EP_PACK_CB_FAILED;
        return UCS_ERR_BUFFER_TOO_SMALL;
    }

    if (data != NULL) {
        memcpy(hdr + 1, data, data_length);
    }

    hdr->length          = data_length;
    hdr->status          = (uint8_t)UCS_OK;
    cep->comm_ctx.length = sizeof(*hdr) + hdr->length;
    cep->state          |= UCT_TCP_SOCKCM_EP_PRIV_DATA_PACKED;
    return UCS_OK;
}

ucs_status_t uct_tcp_sockcm_ep_connect(uct_ep_h ep,
                                       const uct_ep_connect_params_t *params)
{
    uct_tcp_sockcm_ep_t *cep = ucs_derived_of(ep, uct_tcp_sockcm_ep_t);
    const void *priv_data;
    size_t priv_data_length;

    uct_ep_connect_params_get(params, &priv_data, &priv_data_length);
    return uct_tcp_sockcm_ep_pack_priv_data(cep, priv_data, priv_data_length);
}

ucs_status_t uct_tcp_sockcm_ep_disconnect(uct_ep_h ep, unsigned flags)
{
    uct_tcp_sockcm_ep_t *cep     = ucs_derived_of(ep, uct_tcp_sockcm_ep_t);
    uct_tcp_sockcm_t *tcp_sockcm = uct_tcp_sockcm_ep_get_cm(cep);
    char peer_str[UCS_SOCKADDR_STRING_LEN];
    ucs_status_t status;
    int ret;

    UCS_ASYNC_BLOCK(tcp_sockcm->super.iface.worker->async);

    ucs_debug("ep %p (fd=%d state=%d) disconnecting from peer: %s", cep, cep->fd,
              cep->state, uct_tcp_sockcm_cm_ep_peer_addr_str(cep, peer_str,
                                                             UCS_SOCKADDR_STRING_LEN));

    if ((cep->state & UCT_TCP_SOCKCM_EP_FAILED) &&
        !(cep->state & UCT_TCP_SOCKCM_EP_DISCONNECTED)) {
        status = UCS_ERR_NOT_CONNECTED;
        goto out;
    }

    if (ucs_unlikely(cep->state & UCT_TCP_SOCKCM_EP_DISCONNECTING)) {
        if (cep->state & UCT_TCP_SOCKCM_EP_DISCONNECTED) {
            ucs_error("duplicate call of uct_ep_disconnect on a disconnected ep "
                      "(fd=%d state=%d peer=%s)", cep->fd, cep->state,
                      uct_tcp_sockcm_cm_ep_peer_addr_str(cep, peer_str,
                                                         UCS_SOCKADDR_STRING_LEN));
            status = UCS_ERR_NOT_CONNECTED;
            goto out;
        }

        ucs_debug("duplicate call of uct_ep_disconnect on an ep "
                  "that was not disconnected yet (fd=%d state=%d). peer %s",
                  cep->fd, cep->state,
                  uct_tcp_sockcm_cm_ep_peer_addr_str(cep, peer_str,
                                                     UCS_SOCKADDR_STRING_LEN));
        status = UCS_INPROGRESS;
        goto out;
    }

    if (!uct_tcp_sockcm_ep_is_connected(cep)) {
        ucs_debug("calling uct_ep_disconnect on an ep that is not "
                  "connected yet (fd=%d state=%d to peer %s)", cep->fd,
                  cep->state, uct_tcp_sockcm_cm_ep_peer_addr_str(cep, peer_str,
                                                                 UCS_SOCKADDR_STRING_LEN));
        status = UCS_ERR_BUSY;
        goto out;
    }

    cep->state |= UCT_TCP_SOCKCM_EP_DISCONNECTING;

    /* disables further send operations but keep receive operations to get a
     * message from the peer when it disconnects in order to invoke the disconnect_cb */
    ucs_assert(cep->fd != -1);
    ret = shutdown(cep->fd, SHUT_WR);
    if (ret == -1) {
        /* if errno is 'Transport endpoint is not connected', shutdown is expected to fail.
         * Can happen if this disconnect call was triggered from the error handling
         * flow after getting EPIPE on an error event */
        if (errno == ENOTCONN) {
            ucs_debug("ep %p: failed to shutdown on fd %d. ignoring because %m",
                      cep, cep->fd);
            status = UCS_OK;
            goto out;
        }

        ucs_error("ep %p: failed to shutdown on fd %d. %m", cep, cep->fd);
        status = UCS_ERR_IO_ERROR;
        goto out;
    }

    status = UCS_OK;

out:
    UCS_ASYNC_UNBLOCK(tcp_sockcm->super.iface.worker->async);
    return status;
}

void uct_tcp_sockcm_close_ep(uct_tcp_sockcm_ep_t *ep)
{
    ucs_assert(!(ep->state & UCT_TCP_SOCKCM_EP_SERVER_CONN_REQ_CB_INVOKED));
    ucs_list_del(&ep->list);
    UCS_CLASS_DELETE(uct_tcp_sockcm_ep_t, ep);
}

static ucs_status_t
uct_tcp_sockcm_ep_invoke_resolve_cb(uct_tcp_sockcm_ep_t *cep,
                                    const char *ifname,
                                    ucs_status_t resolve_status)
{
    uct_cm_ep_resolve_args_t resolve_args;
    ucs_status_t status;

    resolve_args.field_mask = UCT_CM_EP_RESOLVE_ARGS_FIELD_DEV_NAME |
                              UCT_CM_EP_RESOLVE_ARGS_FIELD_STATUS;
    resolve_args.status     = resolve_status;
    ucs_strncpy_safe(resolve_args.dev_name, ifname, UCT_DEVICE_NAME_MAX);
    status      = uct_cm_ep_resolve_cb(&cep->super, &resolve_args);
    cep->state |= UCT_TCP_SOCKCM_EP_RESOLVE_CB_INVOKED;
    if (status != UCS_OK) {
        cep->state |= UCT_TCP_SOCKCM_EP_RESOLVE_CB_FAILED;
    }

    return status;
}

static ucs_status_t
uct_tcp_sockcm_ep_invoke_pack_cb(uct_tcp_sockcm_ep_t *cep,
                                 const char *ifname)
{
    uct_cm_ep_priv_data_pack_args_t pack_args;
    uct_tcp_sockcm_priv_data_hdr_t *hdr;
    ucs_status_t status;

    pack_args.field_mask = UCT_CM_EP_PRIV_DATA_PACK_ARGS_FIELD_DEVICE_NAME;
    ucs_strncpy_safe(pack_args.dev_name, ifname, UCT_DEVICE_NAME_MAX);

    ucs_assert(cep->comm_ctx.offset == 0);
    hdr = (uct_tcp_sockcm_priv_data_hdr_t*)cep->comm_ctx.buf;
    status = uct_cm_ep_pack_cb(&cep->super, cep->super.user_data, &pack_args,
                               hdr + 1,
                               uct_tcp_sockcm_ep_get_cm(cep)->priv_data_len,
                               &hdr->length);
    if (status != UCS_OK) {
        cep->state |= UCT_TCP_SOCKCM_EP_PACK_CB_FAILED;
        return status;
    }

    hdr->status          = (uint8_t)UCS_OK;
    cep->comm_ctx.length = sizeof(*hdr) + hdr->length;
    cep->state          |= UCT_TCP_SOCKCM_EP_PRIV_DATA_PACKED;
    return UCS_OK;
}

static ucs_status_t uct_tcp_sockcm_ep_resolve(uct_tcp_sockcm_ep_t *cep,
                                              ucs_status_t resolve_status)
{
    char ifname_str[UCT_DEVICE_NAME_MAX];
    ucs_status_t status;

    /* get interface name associated with the connected client fd */
    status = ucs_sockaddr_get_ifname(cep->fd, ifname_str, sizeof(ifname_str));
    if (status != UCS_OK) {
        goto out;
    }

    if (cep->super.resolve_cb != NULL) {
        status = uct_tcp_sockcm_ep_invoke_resolve_cb(cep, ifname_str,
                                                     resolve_status);
    } else {
        ucs_assert(cep->super.priv_pack_cb != NULL);
        status = uct_tcp_sockcm_ep_invoke_pack_cb(cep, ifname_str);
    }

out:
    return status;
}

static void uct_tcp_sockcm_ep_invoke_error_cb(uct_tcp_sockcm_ep_t *cep,
                                              ucs_status_t status)
{
    uct_cm_remote_data_t remote_data;

    ucs_assert(status != UCS_OK);

    /* no errors should happen after the ep was set to failed, since its ep's fd
     * was removed from the async handlers */
    ucs_assert(!(cep->state & UCT_TCP_SOCKCM_EP_FAILED));

    if (uct_tcp_sockcm_ep_is_connected(cep)) {
        /* ep is already connected, call disconnect callback */
        uct_tcp_sockcm_ep_disconnect_cb(cep);
    } else if (cep->state & UCT_TCP_SOCKCM_EP_ON_CLIENT) {
        if (cep->state & UCT_TCP_SOCKCM_EP_RESOLVE_CB_INVOKED) {
            remote_data.field_mask = 0;
            uct_tcp_sockcm_ep_client_connect_cb(cep, &remote_data, status);
        } else {
            uct_tcp_sockcm_ep_resolve(cep, status);
        }
    } else {
        ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_ON_SERVER);
        /* the server might not have a valid ep yet. in this case the notify_cb
         * is an empty function */
        uct_tcp_sockcm_ep_server_notify_cb(cep, status);
    }
}

void uct_tcp_sockcm_ep_handle_event_status(uct_tcp_sockcm_ep_t *ep,
                                           ucs_status_t status,
                                           ucs_event_set_types_t events,
                                           const char *reason)
{
    ucs_status_t async_status;

    ucs_assert(UCS_STATUS_IS_ERR(status));
    ucs_assert(!(ep->state & UCT_TCP_SOCKCM_EP_FAILED));

    ucs_trace("handling error on %s ep %p (fd=%d state=%d events=%d) because %s: %s ",
              ((ep->state & UCT_TCP_SOCKCM_EP_ON_SERVER) ? "server" : "client"),
              ep, ep->fd, ep->state, events, reason, ucs_status_string(status));

    /* if the ep is on the server side but uct_ep_create wasn't called yet and
     * connection request wasn't provided to a user, destroy the ep here since
     * uct_ep_destroy won't be called either */
    if ((ep->state & (UCT_TCP_SOCKCM_EP_ON_SERVER |
                      UCT_TCP_SOCKCM_EP_SERVER_CREATED |
                      UCT_TCP_SOCKCM_EP_SERVER_CONN_REQ_CB_INVOKED)) ==
        UCT_TCP_SOCKCM_EP_ON_SERVER) {
        ucs_trace("closing server's internal ep %p (state=%d)", ep, ep->state);
        uct_tcp_sockcm_close_ep(ep);
    } else {
        ucs_assert(!(ep->state & UCT_TCP_SOCKCM_EP_SERVER_REJECT_CALLED));
        ucs_trace("removing ep %p (fd=%d state=%d) async events handler. %s ",
                  ep, ep->fd, ep->state, ucs_status_string(status));

        async_status = ucs_async_remove_handler(ep->fd, 1);
        if (async_status != UCS_OK) {
            ucs_warn("failed to remove fd %d from the async handlers: %s",
                     ep->fd, ucs_status_string(async_status));
        }

        /* if the resolve or pack callback failed, then the upper layer already
         * knows about it since it failed in it. in this case, no need to invoke
         * another upper layer callback. */
        if (!(ep->state & (UCT_TCP_SOCKCM_EP_RESOLVE_CB_FAILED |
                           UCT_TCP_SOCKCM_EP_PACK_CB_FAILED)) &&
            (ep->state & (UCT_TCP_SOCKCM_EP_SERVER_CREATED |
                          UCT_TCP_SOCKCM_EP_ON_CLIENT))) {
            uct_tcp_sockcm_ep_invoke_error_cb(ep, status);
        }

        ep->state |= UCT_TCP_SOCKCM_EP_FAILED;
    }
}

static void uct_tcp_sockcm_ep_reset_comm_ctx(uct_tcp_sockcm_ep_t *cep)
{
    cep->comm_ctx.offset = 0;
    cep->comm_ctx.length = 0;
}

static ucs_status_t uct_tcp_sockcm_ep_handle_remote_disconnect(uct_tcp_sockcm_ep_t *cep,
                                                               ucs_status_t status)
{
    char peer_str[UCS_SOCKADDR_STRING_LEN];
    ucs_status_t cb_status;

    /* remote peer disconnected */
    ucs_debug("ep %p (fd=%d state=%d): remote peer (%s) disconnected/rejected (%s)",
              cep, cep->fd, cep->state,
              uct_tcp_sockcm_cm_ep_peer_addr_str(cep, peer_str, UCS_SOCKADDR_STRING_LEN),
              ucs_status_string(status));

    /* if the server started sending any data that the client received, then
     * it means that the server accepted the client's connection request and
     * created an ep to it. therefore, the server did not reject the request
     * and there was no reject from the network either, and so if we got here
     * then the status should be UCS_ERR_CONNECTION_RESET.
     * otherwise, if we got here due to a network reject, we set the status
     * to UCS_ERR_UNREACHABLE to distinguish between a network reject and
     * a user's reject (that is done through an explicit message from the server)
     * which calls the upper layer callback with UCS_ERR_REJECTED. */
    if (ucs_test_all_flags(cep->state, UCT_TCP_SOCKCM_EP_ON_CLIENT |
                                       UCT_TCP_SOCKCM_EP_DATA_SENT) &&
        !(cep->state & (UCT_TCP_SOCKCM_EP_HDR_RECEIVED |
                        UCT_TCP_SOCKCM_EP_DATA_RECEIVED))) {
        cb_status   = UCS_ERR_NOT_CONNECTED;
        cep->state |= UCT_TCP_SOCKCM_EP_CLIENT_GOT_REJECT;
    } else {
        cb_status   = UCS_ERR_CONNECTION_RESET;
    }

    uct_tcp_sockcm_ep_reset_comm_ctx(cep);
    return cb_status;
}

static int uct_tcp_sockcm_ep_is_tx_rx_done(uct_tcp_sockcm_ep_t *cep)
{
    ucs_assert((cep->comm_ctx.length != 0));
    return (cep->comm_ctx.offset == cep->comm_ctx.length);
}

static void uct_tcp_sockcm_ep_mark_tx_completed(uct_tcp_sockcm_ep_t *cep)
{
    /* on the client side - if completed sending a message after the notify
     * call was invoked, then this message is the notify message */
    if (cep->state & UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_CALLED) {
        ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_ON_CLIENT);
        cep->state |= UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_SENT;
    }

    /* on the server side - if completed sending a message after the reject
     * call was invoked, then this message is the reject message */
    if (cep->state & UCT_TCP_SOCKCM_EP_SERVER_REJECT_CALLED) {
        ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_ON_SERVER);
        cep->state |= UCT_TCP_SOCKCM_EP_SERVER_REJECT_SENT;
    }
}

ucs_status_t uct_tcp_sockcm_ep_progress_send(uct_tcp_sockcm_ep_t *cep)
{
    uct_tcp_sockcm_t UCS_V_UNUSED *tcp_sockcm = uct_tcp_sockcm_ep_get_cm(cep);
    ucs_status_t status;
    size_t sent_length;
    ucs_event_set_types_t events;

    ucs_assert(ucs_async_is_blocked(tcp_sockcm->super.iface.worker->async));
    ucs_assert(ucs_test_all_flags(cep->state, UCT_TCP_SOCKCM_EP_ON_CLIENT      |
                                              UCT_TCP_SOCKCM_EP_PRIV_DATA_PACKED) ||
               ucs_test_all_flags(cep->state, UCT_TCP_SOCKCM_EP_ON_SERVER      |
                                              UCT_TCP_SOCKCM_EP_SERVER_CREATED |
                                              UCT_TCP_SOCKCM_EP_DATA_RECEIVED)    ||
               (cep->state & UCT_TCP_SOCKCM_EP_SERVER_REJECT_CALLED));

    ucs_assertv(cep->comm_ctx.offset < cep->comm_ctx.length, "ep state %d offset %zu length %zu",
                cep->state, cep->comm_ctx.offset, cep->comm_ctx.length);

    sent_length = cep->comm_ctx.length - cep->comm_ctx.offset;

    status = ucs_socket_send_nb(cep->fd,
                                UCS_PTR_BYTE_OFFSET(cep->comm_ctx.buf,
                                                    cep->comm_ctx.offset),
                                &sent_length);
    if ((status != UCS_OK) && (status != UCS_ERR_NO_PROGRESS)) {
        if (status != UCS_ERR_CONNECTION_RESET) { /* UCS_ERR_NOT_CONNECTED cannot return from send() */
            uct_cm_ep_peer_error(&cep->super, "ep %p failed to send %s's data "
                                "(len=%zu offset=%zu status=%s)", cep,
                                (cep->state & UCT_TCP_SOCKCM_EP_ON_SERVER) ?
                                "server" : "client",
                                cep->comm_ctx.length, cep->comm_ctx.offset,
                                ucs_status_string(status));
        }

        /* treat all send errors as if they are disconnect from the remote peer -
         * i.e. stop sending and receiving on this endpoint and invoke the upper
         * layer callback */
        status = uct_tcp_sockcm_ep_handle_remote_disconnect(cep, status);
        goto out;
    }

    cep->comm_ctx.offset += sent_length;
    ucs_assert(cep->comm_ctx.offset <= cep->comm_ctx.length);

    if (uct_tcp_sockcm_ep_is_tx_rx_done(cep)) {
        ucs_assert(status == UCS_OK);
        cep->state |= UCT_TCP_SOCKCM_EP_DATA_SENT;

        uct_tcp_sockcm_ep_mark_tx_completed(cep);
        uct_tcp_sockcm_ep_reset_comm_ctx(cep);

        if (cep->state & UCT_TCP_SOCKCM_EP_SERVER_REJECT_SENT) {
            UCS_CLASS_DELETE(uct_tcp_sockcm_ep_t, cep);
            goto out;
        }

        /* wait for a message from the peer */
        events = UCS_EVENT_SET_EVREAD;
    } else {
        /* continue the sending when possible, and handle potential disconnect */
        events = UCS_EVENT_SET_EVREAD | UCS_EVENT_SET_EVWRITE;
    }

    status = ucs_async_modify_handler(cep->fd, events);
    if (status != UCS_OK) {
        ucs_error("failed to modify %d event handler to %d: %s",
                  cep->fd, events, ucs_status_string(status));
    }

out:
    return status;
}

ucs_status_t uct_tcp_sockcm_cm_ep_conn_notify(uct_ep_h ep)
{
    uct_tcp_sockcm_ep_t *cep            =
            ucs_derived_of(ep, uct_tcp_sockcm_ep_t);
    uct_tcp_sockcm_t *tcp_sockcm        = uct_tcp_sockcm_ep_get_cm(cep);
    uct_tcp_sockcm_priv_data_hdr_t *hdr =
            (uct_tcp_sockcm_priv_data_hdr_t*)cep->comm_ctx.buf;
    char peer_str[UCS_SOCKADDR_STRING_LEN];
    ucs_status_t status;

    UCS_ASYNC_BLOCK(tcp_sockcm->super.iface.worker->async);

    if (cep->state & (UCT_TCP_SOCKCM_EP_DISCONNECTING |
                      UCT_TCP_SOCKCM_EP_FAILED)) {
        status = UCS_ERR_NOT_CONNECTED;
        goto out;
    }

    ucs_assert(ucs_test_all_flags(cep->state, UCT_TCP_SOCKCM_EP_ON_CLIENT     |
                                              UCT_TCP_SOCKCM_EP_DATA_SENT     |
                                              UCT_TCP_SOCKCM_EP_DATA_RECEIVED |
                                              UCT_TCP_SOCKCM_EP_CLIENT_CONNECTED_CB_INVOKED));
    ucs_assert(!(cep->state & UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_CALLED));

    /* sending only the header in the notify message */
    hdr->length          = 0;
    hdr->status          = (uint8_t)UCS_OK;
    cep->comm_ctx.length = sizeof(*hdr);

    ucs_trace("ep %p sending conn notification to server: %s", cep,
              uct_tcp_sockcm_cm_ep_peer_addr_str(cep, peer_str, UCS_SOCKADDR_STRING_LEN));

    cep->state |= UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_CALLED;
    status = uct_tcp_sockcm_ep_progress_send(cep);

out:
    UCS_ASYNC_UNBLOCK(tcp_sockcm->super.iface.worker->async);
    return status;
}

static int uct_tcp_sockcm_ep_send_skip_event(uct_tcp_sockcm_ep_t *cep)
{
    /* if the ep got a disconnect notice from the peer or had an internal local
     * error, it should have removed its fd from the async handlers.
     * therefore, no send events should get here afterwards */
    ucs_assert(!(cep->state & (UCT_TCP_SOCKCM_EP_DISCONNECTED |
                               UCT_TCP_SOCKCM_EP_FAILED)));

    if (cep->state & UCT_TCP_SOCKCM_EP_DISCONNECTING) {
        return 1;
    } else if (cep->state & UCT_TCP_SOCKCM_EP_ON_SERVER) {
        return cep->state & UCT_TCP_SOCKCM_EP_DATA_SENT;
    } else {
        ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_ON_CLIENT);
        /* if data already sent or not packed yet, then skip event */
        return (cep->state & (UCT_TCP_SOCKCM_EP_CLIENT_NOTIFY_SENT |
                              UCT_TCP_SOCKCM_EP_DATA_SENT)) ||
               !(cep->state & UCT_TCP_SOCKCM_EP_PRIV_DATA_PACKED);
    }
}

ucs_status_t uct_tcp_sockcm_ep_send(uct_tcp_sockcm_ep_t *cep)
{
    if (!(cep->state & (UCT_TCP_SOCKCM_EP_RESOLVE_CB_INVOKED |
                        UCT_TCP_SOCKCM_EP_PRIV_DATA_PACKED   |
                        UCT_TCP_SOCKCM_EP_ON_SERVER))) {
        ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_ON_CLIENT);
        return uct_tcp_sockcm_ep_resolve(cep, UCS_OK);
    }

    if (uct_tcp_sockcm_ep_send_skip_event(cep)) {
        ucs_assert(!(cep->state & UCT_TCP_SOCKCM_EP_DISCONNECTING));
        return UCS_OK;
    }

    return uct_tcp_sockcm_ep_progress_send(cep);
}

static ssize_t
uct_tcp_sockcm_ep_get_remote_device_addr(const uct_tcp_sockcm_ep_t *cep,
                                         struct sockaddr_storage *saddr,
                                         socklen_t *saddr_len_p,
                                         uct_tcp_device_addr_t *remote_dev_addr,
                                         size_t max_remote_dev_addr_len)
{
    ucs_status_t status;
    size_t in_addr_len;
    size_t remote_dev_addr_len;

    /* Get the device address of the remote peer associated with the connected
     * fd */
    status = ucs_socket_getpeername(cep->fd, saddr, saddr_len_p);
    if (status != UCS_OK) {
        return status;
    }

    status = ucs_sockaddr_inet_addr_sizeof((struct sockaddr*)saddr,
                                           &in_addr_len);
    if (status != UCS_OK) {
        return status;
    }

    remote_dev_addr_len = sizeof(*remote_dev_addr) + in_addr_len;
    if (remote_dev_addr_len > max_remote_dev_addr_len) {
        return UCS_ERR_BUFFER_TOO_SMALL;
    }

    remote_dev_addr->flags     = 0u;
    remote_dev_addr->sa_family = saddr->ss_family;

    memcpy(remote_dev_addr + 1,
           ucs_sockaddr_get_inet_addr((struct sockaddr*)saddr), in_addr_len);

    return remote_dev_addr_len;
}

static ucs_status_t
uct_tcp_sockcm_ep_server_invoke_conn_req_cb(uct_tcp_sockcm_ep_t *cep)
{
    uct_tcp_sockcm_priv_data_hdr_t *hdr    = (uct_tcp_sockcm_priv_data_hdr_t*)
                                                     cep->comm_ctx.buf;
    struct sockaddr_storage saddr          = {0};
    uct_tcp_device_addr_t *remote_dev_addr =
            ucs_alloca(UCT_TCP_SOCKCM_EP_MAX_DEVICE_ADDR_LEN);
    ssize_t remote_dev_addr_len;
    uct_cm_listener_conn_request_args_t conn_req_args;
    char peer_str[UCS_SOCKADDR_STRING_LEN];
    char ifname_str[UCT_DEVICE_NAME_MAX];
    uct_cm_remote_data_t remote_data;
    socklen_t saddr_len;
    ucs_sock_addr_t client_saddr;
    ucs_status_t status;

    /* Get the local interface name associated with the connected fd */
    status = ucs_sockaddr_get_ifname(cep->fd, ifname_str, UCT_DEVICE_NAME_MAX);
    if (UCS_OK != status) {
        return status;
    }

    remote_dev_addr_len = uct_tcp_sockcm_ep_get_remote_device_addr(
            cep, &saddr, &saddr_len, remote_dev_addr,
            UCT_TCP_SOCKCM_EP_MAX_DEVICE_ADDR_LEN);
    if (remote_dev_addr_len < 0) {
        return (ucs_status_t)remote_dev_addr_len;
    }

    remote_data.field_mask            = UCT_CM_REMOTE_DATA_FIELD_DEV_ADDR        |
                                        UCT_CM_REMOTE_DATA_FIELD_DEV_ADDR_LENGTH |
                                        UCT_CM_REMOTE_DATA_FIELD_CONN_PRIV_DATA  |
                                        UCT_CM_REMOTE_DATA_FIELD_CONN_PRIV_DATA_LENGTH;
    remote_data.dev_addr              = (uct_device_addr_t*)remote_dev_addr;
    remote_data.dev_addr_length       = remote_dev_addr_len;
    remote_data.conn_priv_data        = hdr + 1;
    remote_data.conn_priv_data_length = hdr->length;

    client_saddr.addr    = (struct sockaddr*)&saddr;
    client_saddr.addrlen = saddr_len;

    conn_req_args.field_mask     = UCT_CM_LISTENER_CONN_REQUEST_ARGS_FIELD_DEV_NAME     |
                                   UCT_CM_LISTENER_CONN_REQUEST_ARGS_FIELD_CONN_REQUEST |
                                   UCT_CM_LISTENER_CONN_REQUEST_ARGS_FIELD_REMOTE_DATA  |
                                   UCT_CM_LISTENER_CONN_REQUEST_ARGS_FIELD_CLIENT_ADDR;
    conn_req_args.conn_request   = cep;
    conn_req_args.remote_data    = &remote_data;
    conn_req_args.client_address = client_saddr;
    ucs_strncpy_safe(conn_req_args.dev_name, ifname_str, UCT_DEVICE_NAME_MAX);

    status = ucs_sockaddr_get_ipstr(client_saddr.addr, peer_str,
                                    UCS_SOCKADDR_STRING_LEN);
    ucs_assert(status == UCS_OK);
    ucs_debug("fd %d, dev_addr: flags 0x%x length %zu %s %s,"
              " conn_priv_data_length=%zu",
              cep->fd, remote_dev_addr->flags, remote_data.dev_addr_length,
              ucs_sockaddr_address_family_str(remote_dev_addr->sa_family),
              peer_str, remote_data.conn_priv_data_length);

    /* the endpoint, passed as the conn_request to the callback, will be passed
     * to uct_ep_create() which will be invoked by the user and therefore moving
     * over to its responsibility. */
    ucs_list_del(&cep->list);
    cep->state |= UCT_TCP_SOCKCM_EP_SERVER_CONN_REQ_CB_INVOKED;
    cep->listener->conn_request_cb(&cep->listener->super, cep->listener->user_data,
                                   &conn_req_args);

    return UCS_OK;
}

static ucs_status_t
uct_tcp_sockcm_ep_client_invoke_connect_cb(uct_tcp_sockcm_ep_t *cep)
{
    uct_tcp_sockcm_priv_data_hdr_t *hdr    = (uct_tcp_sockcm_priv_data_hdr_t*)
                                                     cep->comm_ctx.buf;
    struct sockaddr_storage saddr          = {0};
    uct_tcp_device_addr_t *remote_dev_addr =
            ucs_alloca(UCT_TCP_SOCKCM_EP_MAX_DEVICE_ADDR_LEN);
    ssize_t remote_dev_addr_len;
    uct_cm_remote_data_t remote_data;
    socklen_t saddr_len;

    remote_dev_addr_len = uct_tcp_sockcm_ep_get_remote_device_addr(
            cep, &saddr, &saddr_len, remote_dev_addr,
            UCT_TCP_SOCKCM_EP_MAX_DEVICE_ADDR_LEN);
    if (remote_dev_addr_len < 0) {
        return (ucs_status_t)remote_dev_addr_len;
    }

    remote_data.field_mask            = UCT_CM_REMOTE_DATA_FIELD_DEV_ADDR        |
                                        UCT_CM_REMOTE_DATA_FIELD_DEV_ADDR_LENGTH |
                                        UCT_CM_REMOTE_DATA_FIELD_CONN_PRIV_DATA  |
                                        UCT_CM_REMOTE_DATA_FIELD_CONN_PRIV_DATA_LENGTH;
    remote_data.dev_addr              = (uct_device_addr_t*)remote_dev_addr;
    remote_data.dev_addr_length       = remote_dev_addr_len;
    remote_data.conn_priv_data        = hdr + 1;
    remote_data.conn_priv_data_length = hdr->length;

    uct_tcp_sockcm_ep_client_connect_cb(cep, &remote_data, (ucs_status_t)hdr->status);

    return UCS_OK;
}

static ucs_status_t uct_tcp_sockcm_ep_server_handle_data_received(uct_tcp_sockcm_ep_t *cep)
{
    uct_tcp_sockcm_priv_data_hdr_t *hdr = (uct_tcp_sockcm_priv_data_hdr_t *)
                                           cep->comm_ctx.buf;
    ucs_status_t status;

    if (cep->state & UCT_TCP_SOCKCM_EP_DATA_SENT) {
        ucs_assert(ucs_test_all_flags(cep->state, UCT_TCP_SOCKCM_EP_SERVER_CREATED |
                                                  UCT_TCP_SOCKCM_EP_DATA_RECEIVED));
        ucs_assert(hdr->length == 0);

        uct_tcp_sockcm_ep_server_notify_cb(cep, (ucs_status_t)hdr->status);

        /* don't access the endpoint after calling an upper layer callback since
         * it might have destroyed it.
         * if not destroyed, the server's handler should already be waiting to
         * EVREAD events */
        status = UCS_OK;
    } else if ((cep->state & UCT_TCP_SOCKCM_EP_DATA_RECEIVED) &&
               !(cep->state & UCT_TCP_SOCKCM_EP_SERVER_CREATED)) {
        status = uct_tcp_sockcm_ep_server_invoke_conn_req_cb(cep);
    } else {
        ucs_error("unexpected state on the server endpoint: %d", cep->state);
        status = UCS_ERR_IO_ERROR;
    }

    return status;
}

ucs_status_t uct_tcp_sockcm_ep_handle_data_received(uct_tcp_sockcm_ep_t *cep)
{
    const uct_tcp_sockcm_priv_data_hdr_t *hdr;
    ucs_status_t status;

    ucs_assert(!(cep->state & UCT_TCP_SOCKCM_EP_DISCONNECTED));

    cep->state |= UCT_TCP_SOCKCM_EP_DATA_RECEIVED;
    /* if the data was received, drop the header_received flag to receive new messages */
    cep->state &= ~UCT_TCP_SOCKCM_EP_HDR_RECEIVED;
    uct_tcp_sockcm_ep_reset_comm_ctx(cep);

    if (cep->state & UCT_TCP_SOCKCM_EP_ON_SERVER) {
        status = uct_tcp_sockcm_ep_server_handle_data_received(cep);
    } else {
        ucs_assert(cep->state & UCT_TCP_SOCKCM_EP_ON_CLIENT);

        hdr = (const uct_tcp_sockcm_priv_data_hdr_t *)cep->comm_ctx.buf;
        if ((ucs_status_t)hdr->status == UCS_ERR_REJECTED) {
            ucs_assert(!(cep->state & UCT_TCP_SOCKCM_EP_CLIENT_CONNECTED_CB_INVOKED));
            cep->state |= UCT_TCP_SOCKCM_EP_CLIENT_GOT_REJECT;
            status = UCS_ERR_REJECTED;
        } else {
            status = uct_tcp_sockcm_ep_client_invoke_connect_cb(cep);
        }

        /* next, unless disconnected, if the client did not send
         * a connection establishment notification to the server from the connect_cb,
         * he will send it from the main thread */
    }

    return status;
}

static ucs_status_t uct_tcp_sockcm_ep_recv_nb(uct_tcp_sockcm_ep_t *cep)
{
    size_t recv_length;
    ucs_status_t status;

    recv_length = uct_tcp_sockcm_ep_get_cm(cep)->priv_data_len +
                  sizeof(uct_tcp_sockcm_priv_data_hdr_t) - cep->comm_ctx.offset;
    status = ucs_socket_recv_nb(cep->fd,
                                UCS_PTR_BYTE_OFFSET(cep->comm_ctx.buf,
                                                    cep->comm_ctx.offset),
                                0, &recv_length);
    if ((status != UCS_OK) && (status != UCS_ERR_NO_PROGRESS)) {
        if (status != UCS_ERR_NOT_CONNECTED) {  /* ECONNRESET cannot return from recv() */
            uct_cm_ep_peer_error(&cep->super,
                                 "ep %p (fd=%d) failed to recv client's data "
                                 "(offset=%zu status=%s)", cep, cep->fd,
                                 cep->comm_ctx.offset, ucs_status_string(status));
        }

        /* treat all recv errors as if they are disconnect/reject from the remote peer -
         * i.e. stop sending and receiving on this endpoint */
        status = uct_tcp_sockcm_ep_handle_remote_disconnect(cep, status);
        goto out;
    }

    cep->comm_ctx.offset += recv_length;
    ucs_assertv((cep->comm_ctx.length ?
                 cep->comm_ctx.offset <= cep->comm_ctx.length : 1), "%zu > %zu",
                cep->comm_ctx.offset, cep->comm_ctx.length);

out:
    return status;
}

ucs_status_t uct_tcp_sockcm_ep_recv(uct_tcp_sockcm_ep_t *cep)
{
    uct_tcp_sockcm_priv_data_hdr_t *hdr;
    ucs_status_t status;

    /* if the ep got a disconnect notice from the peer, had an internal local
     * error or the client received a reject from the server, it should have
     * removed its fd from the async handlers.
     * therefore, no recv events should get here afterwards */
    ucs_assert(!(cep->state & (UCT_TCP_SOCKCM_EP_DISCONNECTED |
                               UCT_TCP_SOCKCM_EP_CLIENT_GOT_REJECT |
                               UCT_TCP_SOCKCM_EP_FAILED)));

    if (cep->state & UCT_TCP_SOCKCM_EP_SERVER_REJECT_CALLED) {
        return UCS_OK;
    }

    status = uct_tcp_sockcm_ep_recv_nb(cep);
    if (status != UCS_OK) {
        goto out;
    }

    if (!(cep->state & UCT_TCP_SOCKCM_EP_HDR_RECEIVED)) {
        if (cep->comm_ctx.offset < sizeof(*hdr)) {
            goto out;
        }

        hdr                  = (uct_tcp_sockcm_priv_data_hdr_t *)cep->comm_ctx.buf;
        cep->comm_ctx.length = sizeof(*hdr) + hdr->length;
        ucs_assertv(cep->comm_ctx.offset <= cep->comm_ctx.length , "%zu > %zu",
                    cep->comm_ctx.offset, cep->comm_ctx.length);

        cep->state          |= UCT_TCP_SOCKCM_EP_HDR_RECEIVED;
    }

    if (uct_tcp_sockcm_ep_is_tx_rx_done(cep)) {
        status = uct_tcp_sockcm_ep_handle_data_received(cep);
    }

out:
    return (status == UCS_ERR_NO_PROGRESS) ? UCS_OK : status;
}

ucs_status_t uct_tcp_sockcm_ep_set_sockopt(uct_tcp_sockcm_ep_t *ep)
{
    uct_tcp_sockcm_t *tcp_sockcm = uct_tcp_sockcm_ep_get_cm(ep);
    ucs_status_t status;

    status = ucs_socket_set_buffer_size(ep->fd, tcp_sockcm->sockopt_sndbuf,
                                        tcp_sockcm->sockopt_rcvbuf);
    if (status != UCS_OK) {
        return status;
    }

    return ucs_tcp_base_set_syn_cnt(ep->fd, tcp_sockcm->syn_cnt);
}

static ucs_status_t uct_tcp_sockcm_ep_client_init(uct_tcp_sockcm_ep_t *cep,
                                                  const uct_ep_params_t *params)
{
    uct_tcp_sockcm_t *tcp_sockcm = uct_tcp_sockcm_ep_get_cm(cep);
    uct_cm_base_ep_t *cm_ep      = &cep->super;
    char ip_port_str[UCS_SOCKADDR_STRING_LEN];
    const struct sockaddr *server_addr;
    ucs_async_context_t *async_ctx;
    ucs_status_t status;
    int ret;

    cep->state |= UCT_TCP_SOCKCM_EP_ON_CLIENT;

    status = UCT_CM_SET_CB(params, UCT_EP_PARAM_FIELD_SOCKADDR_CONNECT_CB_CLIENT,
                           cm_ep->client.connect_cb, params->sockaddr_cb_client,
                           uct_cm_ep_client_connect_callback_t,
                           ucs_empty_function);
    if (status != UCS_OK) {
        goto err;
    }

    server_addr = params->sockaddr->addr;
    status = ucs_socket_create(server_addr->sa_family, SOCK_STREAM, 0, &cep->fd);
    if (status != UCS_OK) {
        goto err;
    }

    /* Set the fd to non-blocking mode. (so that connect() won't be blocking) */
    status = ucs_sys_fcntl_modfl(cep->fd, O_NONBLOCK, 0);
    if (status != UCS_OK) {
        status = UCS_ERR_IO_ERROR;
        goto err_close_socket;
    }

    status = uct_tcp_sockcm_ep_set_sockopt(cep);
    if (status != UCS_OK) {
        goto err_close_socket;
    }

    if (params->field_mask & UCT_EP_PARAM_FIELD_LOCAL_SOCKADDR) {
        ret = bind(cep->fd, (const struct sockaddr *)params->local_sockaddr->addr,
                   params->local_sockaddr->addrlen);
        if (ret < 0) {
            ucs_diag("bind socket to %s failed (%d): %m",
                     ucs_sockaddr_str(params->local_sockaddr->addr,
                                      ip_port_str, UCS_SOCKADDR_STRING_LEN),
                     errno);
            status = UCS_ERR_IO_ERROR;
            goto err_close_socket;
        }
    }

    /* try to connect to the server */
    status = ucs_socket_connect(cep->fd, server_addr);
    if (UCS_STATUS_IS_ERR(status)) {
        goto err_close_socket;
    }
    ucs_assert((status == UCS_OK) || (status == UCS_INPROGRESS));

    async_ctx = tcp_sockcm->super.iface.worker->async;
    status    = ucs_async_set_event_handler(async_ctx->mode, cep->fd,
                                            UCS_EVENT_SET_EVWRITE,
                                            uct_tcp_sa_data_handler, cep,
                                            async_ctx);
    if (status != UCS_OK) {
        goto err_close_socket;
    }

    ucs_debug("created a TCP SOCKCM endpoint (fd=%d) on tcp cm %p, "
              "remote addr: %s", cep->fd, tcp_sockcm,
              ucs_sockaddr_str(server_addr, ip_port_str, UCS_SOCKADDR_STRING_LEN));

    return status;

err_close_socket:
    uct_tcp_sockcm_ep_close_fd(&cep->fd);
err:
    return status;
}

static ssize_t uct_tcp_sockcm_ep_pack_cb(uct_tcp_sockcm_ep_t *tcp_ep,
                                         void *data_buf)
{
    uct_tcp_sockcm_t *tcp_sockcm = uct_tcp_sockcm_ep_get_cm(tcp_ep);
    uct_cm_ep_priv_data_pack_args_t pack_args;
    size_t priv_data_ret;
    char ifname_str[UCT_DEVICE_NAME_MAX];
    ucs_status_t status;

    status = ucs_sockaddr_get_ifname(tcp_ep->fd, ifname_str,
                                     sizeof(ifname_str));
    if (status != UCS_OK) {
        return status;
    }

    pack_args.field_mask = UCT_CM_EP_PRIV_DATA_PACK_ARGS_FIELD_DEVICE_NAME;
    ucs_strncpy_safe(pack_args.dev_name, ifname_str, UCT_DEVICE_NAME_MAX);
    status = uct_cm_ep_pack_cb(&tcp_ep->super, tcp_ep->super.user_data,
                               &pack_args, data_buf, tcp_sockcm->priv_data_len,
                               &priv_data_ret);
    if (status != UCS_OK) {
        tcp_ep->state |= UCT_TCP_SOCKCM_EP_PACK_CB_FAILED;
        return status;
    }

    return priv_data_ret;
}

/**
 * The caller has to block async.
 */
static ucs_status_t uct_tcp_sockcm_ep_server_create(uct_tcp_sockcm_ep_t *tcp_ep,
                                                    const uct_ep_params_t *params,
                                                    uct_ep_h *ep_p)
{
    uct_tcp_sockcm_t *tcp_sockcm = uct_tcp_sockcm_ep_get_cm(tcp_ep);
    void *data_buf               = NULL;
    uct_tcp_sockcm_t *params_tcp_sockcm;
    const void *priv_data;
    ssize_t priv_data_length;
    ucs_async_context_t *new_async_ctx;
    ucs_status_t status;

    if (!(params->field_mask & UCT_EP_PARAM_FIELD_CM)) {
        ucs_error("UCT_EP_PARAM_FIELD_CM is not set. field_mask 0x%"PRIx64,
                  params->field_mask);
        status = UCS_ERR_INVALID_PARAM;
        goto err;
    }

    if (params->cm == NULL) {
        ucs_error("cm cannot be NULL (ep=%p fd=%d)", tcp_ep, tcp_ep->fd);
        status = UCS_ERR_INVALID_PARAM;
        goto err;
    }

    if (tcp_ep->state & UCT_TCP_SOCKCM_EP_FAILED) {
        status = UCS_ERR_CONNECTION_RESET;
        goto err;
    }

    /* check if the server opened this ep, to the client, on a CM that is
     * different from the one it created its internal ep on earlier, when it
     * received the connection request from the client (the cm used by its listener) */
    if (&tcp_sockcm->super != params->cm) {
        status = ucs_async_remove_handler(tcp_ep->fd, 1);
        if (status != UCS_OK) {
            ucs_error("failed to remove fd %d from the async handlers: %s",
                      tcp_ep->fd, ucs_status_string(status));
            goto err;
        }
    }

    /* set the server's ep to use the cm from params.
     * (it could be the previous one it had - the one used by the listener or
     * a new one set by the user) */
    status = uct_cm_ep_set_common_data(&tcp_ep->super, params);
    if (status != UCS_OK) {
        ucs_error("failed to set common data for a uct_cm_base_ep_t endpoint");
        goto err;
    }

    status = UCT_CM_SET_CB(params, UCT_EP_PARAM_FIELD_SOCKADDR_NOTIFY_CB_SERVER,
                           tcp_ep->super.server.notify_cb, params->sockaddr_cb_server,
                           uct_cm_ep_server_conn_notify_callback_t,
                           ucs_empty_function);
    if (status != UCS_OK) {
        goto err;
    }

    /* the server's endpoint was already created by the listener, return it */
    *ep_p             = &tcp_ep->super.super.super;
    params_tcp_sockcm = ucs_derived_of(params->cm, uct_tcp_sockcm_t);

    if (&tcp_sockcm->super != params->cm) {
        new_async_ctx = params_tcp_sockcm->super.iface.worker->async;
        status = ucs_async_set_event_handler(new_async_ctx->mode, tcp_ep->fd,
                                             UCS_EVENT_SET_EVREAD |
                                             UCS_EVENT_SET_EVERR,
                                             uct_tcp_sa_data_handler,
                                             tcp_ep, new_async_ctx);
        if (status != UCS_OK) {
            ucs_error("failed to set event handler (fd %d): %s",
                      tcp_ep->fd, ucs_status_string(status));
            goto err;
        }

        /* set the server's ep to use the iface from the cm in params */
        uct_ep_set_iface(&tcp_ep->super.super.super, &params->cm->iface.super);

        status = uct_base_ep_stats_reset(&tcp_ep->super.super, &params->cm->iface);
        if (status != UCS_OK) {
            ucs_error("failed to reset the stats on ep %p: %s",
                      tcp_ep, ucs_status_string(status));
            goto err;
        }

        ucs_trace("moved tcp_sockcm ep %p from cm %p to cm %p", tcp_ep,
                  tcp_sockcm, params_tcp_sockcm);
    }

    ucs_assert(uct_tcp_sockcm_ep_get_cm(tcp_ep) == params_tcp_sockcm);
    ucs_trace("server completed endpoint creation (fd=%d cm=%p state=%d)",
              tcp_ep->fd, params_tcp_sockcm, tcp_ep->state);

    /* now that the server's ep was created, can try to send data */
    ucs_async_modify_handler(tcp_ep->fd, UCS_EVENT_SET_EVWRITE |
                                         UCS_EVENT_SET_EVREAD);

    if (ucs_test_all_flags(params->field_mask,
                           UCT_EP_PARAM_FIELD_PRIV_DATA |
                           UCT_EP_PARAM_FIELD_PRIV_DATA_LENGTH)) {
        priv_data        = params->private_data;
        priv_data_length = params->private_data_length;
    } else if (params->field_mask & UCT_EP_PARAM_FIELD_SOCKADDR_PACK_CB) {
        data_buf = ucs_malloc(tcp_sockcm->priv_data_len, "tcp_priv_data");
        if (data_buf == NULL) {
            status = UCS_ERR_NO_MEMORY;
            goto err;
        }

        priv_data        = data_buf;
        priv_data_length = uct_tcp_sockcm_ep_pack_cb(tcp_ep, data_buf);
        if (priv_data_length < 0) {
            status = (ucs_status_t)priv_data_length;
            goto err;
        }
    } else {
        priv_data        = NULL;
        priv_data_length = 0;
    }

    status = uct_tcp_sockcm_ep_pack_priv_data(tcp_ep, priv_data,
                                              priv_data_length);
    if (status == UCS_OK) {
        tcp_ep->state |= UCT_TCP_SOCKCM_EP_SERVER_CREATED;
    }

err:
    ucs_free(data_buf);
    return status;
}

UCS_CLASS_INIT_FUNC(uct_tcp_sockcm_ep_t, const uct_ep_params_t *params)
{
    ucs_status_t status;

    UCS_CLASS_CALL_SUPER_INIT(uct_cm_base_ep_t, params);

    uct_tcp_sockcm_ep_reset_comm_ctx(self);
    self->state        = 0;
    self->comm_ctx.buf = ucs_calloc(1, uct_tcp_sockcm_ep_get_cm(self)->priv_data_len +
                                    sizeof(uct_tcp_sockcm_priv_data_hdr_t),
                                    "tcp_sockcm priv data");
    if (self->comm_ctx.buf == NULL) {
        ucs_error("failed to allocate memory for the ep's send/recv buf");
        status = UCS_ERR_NO_MEMORY;
        goto out;
    }

    if (params->field_mask & UCT_EP_PARAM_FIELD_SOCKADDR) {
        status = uct_tcp_sockcm_ep_client_init(self, params);
        if (status != UCS_OK) {
            ucs_free(self->comm_ctx.buf);
            goto out;
        }
    } else {
        self->state |= UCT_TCP_SOCKCM_EP_ON_SERVER;
        status = UCS_OK;
    }

    ucs_debug("%s created an endpoint on tcp_sockcm %p id: %d state: %d",
              (self->state & UCT_TCP_SOCKCM_EP_ON_SERVER) ? "server" : "client",
              uct_tcp_sockcm_ep_get_cm(self), self->fd, self->state);

out:
    return status;
}

ucs_status_t uct_tcp_sockcm_ep_create(const uct_ep_params_t *params, uct_ep_h *ep_p)
{
    uct_tcp_sockcm_ep_t *tcp_ep;
    ucs_async_context_t *async;
    ucs_status_t status;

    if (params->field_mask & UCT_EP_PARAM_FIELD_SOCKADDR) {
        /* create a new endpoint for the client side */
        return UCS_CLASS_NEW(uct_tcp_sockcm_ep_t, ep_p, params);
    } else if (params->field_mask & UCT_EP_PARAM_FIELD_CONN_REQUEST) {
        tcp_ep = (uct_tcp_sockcm_ep_t*)params->conn_request;
        async  = uct_tcp_sockcm_ep_get_cm(tcp_ep)->super.iface.worker->async;

        UCS_ASYNC_BLOCK(async);
        status = uct_tcp_sockcm_ep_server_create(tcp_ep, params, ep_p);
        if (status != UCS_OK) {
            UCS_CLASS_DELETE(uct_tcp_sockcm_ep_t, tcp_ep);
        }

        UCS_ASYNC_UNBLOCK(async);
        return status;
    } else {
        ucs_error("either UCT_EP_PARAM_FIELD_SOCKADDR or UCT_EP_PARAM_FIELD_CONN_REQUEST "
                  "has to be provided");
        return UCS_ERR_INVALID_PARAM;
    }
}

UCS_CLASS_CLEANUP_FUNC(uct_tcp_sockcm_ep_t)
{
    uct_tcp_sockcm_t *tcp_sockcm = uct_tcp_sockcm_ep_get_cm(self);

    UCS_ASYNC_BLOCK(tcp_sockcm->super.iface.worker->async);

    ucs_trace("%s destroy ep %p (state=%d) on cm %p",
              (self->state & UCT_TCP_SOCKCM_EP_ON_SERVER) ? "server" : "client",
              self, self->state, tcp_sockcm);

    ucs_free(self->comm_ctx.buf);

    uct_tcp_sockcm_ep_close_fd(&self->fd);
    UCS_ASYNC_UNBLOCK(tcp_sockcm->super.iface.worker->async);
}

UCS_CLASS_DEFINE(uct_tcp_sockcm_ep_t, uct_cm_base_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_tcp_sockcm_ep_t, uct_ep_t, const uct_ep_params_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_tcp_sockcm_ep_t, uct_ep_t);
