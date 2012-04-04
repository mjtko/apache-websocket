/*
 * Copyright 2012 Flexiant Ltd
 *
 * Written by Alex Bligh, based upon the dumb_increment_protocol
 * example for apache-websocket, written by self.disconnect
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "apr_thread_proc.h"
#include "apr_base64.h"
#include "apr_strings.h"

#include "websocket_plugin.h"

#define APACHELOG(severity, handle, message...) ap_log_error(APLOG_MARK, APLOG_NOERRNO | severity, 0, handle->server, message)

module AP_MODULE_DECLARE_DATA websocket_tcp_proxy_module;

typedef struct
{
    char *location;
    const char *host;
    const char *port;
    const char *protocol;
    int base64;
    int timeout;
} websocket_tcp_proxy_config_rec;

typedef struct _TcpProxyData
{
    const WebSocketServer *server;
    apr_pool_t *pool;
    apr_thread_t *thread;
    apr_socket_t *tcpsocket;
    int active;
    int base64;
    int timeout;
    char *host;
    char *port;
    char *initialdata;
} TcpProxyData;

/**
 * Authenticate the connection. This can modify tpd to change (for instance)
 * the host or port to connect to, or set up initialdata. For now it is a stub.
 */

static apr_status_t tcp_proxy_do_authenticate(request_rec * r,
                                              TcpProxyData * tpd,
                                              apr_pool_t * mp)
{
    return APR_SUCCESS;
}

/**
 * Send the initial data - this would normally be generated by tcp_proxy_do_authenticate
 */

static apr_status_t tcp_proxy_send_initial_data(request_rec * r,
                                                TcpProxyData * tpd,
                                                apr_pool_t * mp)
{
    if (!tpd->initialdata)
        return APR_SUCCESS;

    apr_size_t len = strlen(tpd->initialdata);
    return apr_socket_send(tpd->tcpsocket, tpd->initialdata, &len);
}

/**
 * Shutdown the tcpsocket which will cause further read/writes
 * in either direction to fail
 */

static void tcp_proxy_shutdown_socket(TcpProxyData * tpd)
{
    if (tpd && tpd->tcpsocket)
        apr_socket_shutdown(tpd->tcpsocket, APR_SHUTDOWN_READWRITE);
}

/**
 * Connect to the remote host
 */
static apr_status_t tcp_proxy_do_tcp_connect(request_rec * r,
                                             TcpProxyData * tpd,
                                             apr_pool_t * mp)
{
    apr_sockaddr_t *sa;
    apr_socket_t *s;
    apr_status_t rv;

    APACHELOG(APLOG_DEBUG, r,
              "tcp_proxy_do_tcp_connect: connect to host %s port %s",
              tpd->host, tpd->port);

    int port = atoi(tpd->port);
    rv = apr_sockaddr_info_get(&sa, tpd->host, APR_INET, port, 0, mp);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    if (!port) {
        rv = apr_getservbyname(sa, tpd->port);
        if (rv != APR_SUCCESS) {
            return rv;
        }
    }

    rv = apr_socket_create(&s, sa->family, SOCK_STREAM, APR_PROTO_TCP, mp);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    /* it is a good idea to specify socket options explicitly.
     * in this case, we make a blocking socket with timeout. */
    apr_socket_opt_set(s, APR_SO_NONBLOCK, 0);
    apr_socket_opt_set(s, APR_SO_KEEPALIVE, 1);
    if (tpd->timeout)
        apr_socket_timeout_set(s,
                               ((apr_interval_time_t) tpd->timeout) *
                               APR_USEC_PER_SEC);

    rv = apr_socket_connect(s, sa);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    /* see the tutorial about the reason why we have to specify options again */
    apr_socket_opt_set(s, APR_SO_NONBLOCK, 0);
    apr_socket_opt_set(s, APR_SO_KEEPALIVE, 1);
    if (tpd->timeout)
        apr_socket_timeout_set(s,
                               ((apr_interval_time_t) tpd->timeout) *
                               APR_USEC_PER_SEC);

    tpd->tcpsocket = s;
    return APR_SUCCESS;
}

/* This function READS from the tcp socket and WRITES to the web socket */

void *APR_THREAD_FUNC tcp_proxy_run(apr_thread_t * thread, void *data)
{
    char buffer[64];
    TcpProxyData *tpd = (TcpProxyData *) data;

    if (tpd != NULL) {
        request_rec *r = (tpd->server)->request(tpd->server);

        APACHELOG(APLOG_DEBUG, r, "tcp_proxy_run start");

        /* Keep sending messages as long as the connection is active */
        while (tpd->active && tpd->tcpsocket) {
#define WSTCPBUFSIZ 1024
#define WSTCPCBUFSIZ ((WSTCPBUFSIZ*4/3)+5)
            char buf[WSTCPBUFSIZ];
            char cbuf[WSTCPCBUFSIZ];
            apr_size_t len = sizeof(buf);

            apr_status_t rv;
            rv = apr_socket_recv(tpd->tcpsocket, buf, &len);
			
            if ( APR_STATUS_IS_TIMEUP(rv)) {
                /* nothing was read this time round. */
                continue;
            }
            else if (rv != APR_SUCCESS && !APR_STATUS_IS_EOF(rv)) {
                /* we've received an unexpected result value, so we want
                   to log that. */
                char s[1024];
                apr_strerror(rv, s, sizeof(s));
                APACHELOG(APLOG_DEBUG, r,
                          "tcp_proxy_range_run apr_socket_recv failed len=%lu rv=%d, %s",
                          (unsigned long) len, rv, s);
            }
            
            if ( rv != APR_SUCCESS ) {
                /* our read was not successful, shutdown now! */
                tcp_proxy_range_shutdown_socket(tpd);
                tpd->server->close(tpd->server);
                break;
            }

            size_t towrite = len;

            char *wbuf = buf;

            /* Base64 encode it if necessary */
            if (tpd->base64) {
                towrite = apr_base64_encode(cbuf, buf, towrite);
                wbuf = cbuf;
            }

            size_t written =
                tpd->server->send(tpd->server, MESSAGE_TYPE_TEXT /* FIXME */ ,
                                  (unsigned char *) wbuf, towrite);
            if (written != towrite) {
                APACHELOG(APLOG_DEBUG, r,
                          "tcp_proxy_run send failed, wrote %lu bytes of %lu",
                          (unsigned long) written, (unsigned long) len);
                tcp_proxy_shutdown_socket(tpd);
                tpd->server->close(tpd->server);
                break;
            }
        }

        APACHELOG(APLOG_DEBUG, r, "tcp_proxy_run stop");

    }
    return NULL;
}

/* this routine takes data FROM the web socket and writes it to the tcp socket */

static size_t CALLBACK tcp_proxy_on_message(void *plugin_private,
                                            const WebSocketServer * server,
                                            const int type,
                                            unsigned char *buffer,
                                            const size_t buffer_size)
{
    TcpProxyData *tpd = (TcpProxyData *) plugin_private;

    request_rec *r = server->request(server);

    if (tpd && tpd->tcpsocket) {
        apr_size_t len = buffer_size;
        apr_status_t rv;
        unsigned char *towrite = buffer;

        if (tpd->base64) {
            /* Unfortunately we cannot guarantee our buffer is 0 terminated, which irritatingly
             * means we have to copy it
             */
            towrite = NULL;
            unsigned char *ztbuf = calloc(1, len + 1);
            if (!ztbuf)
                goto fail;
            towrite = calloc(1, len + 1);
            if (!towrite) {
                free(ztbuf);
                goto fail;
            }
            memcpy(ztbuf, buffer, len);
            len = apr_base64_decode_binary(towrite, ztbuf);
            free(ztbuf);
            if (len <= 0) {
                free(towrite);
                towrite = NULL;
            }
          fail:
            if (!towrite) {
                APACHELOG(APLOG_DEBUG, r,
                          "tcp_proxy_on_message: apr_base64_decode_binary failed");
                tcp_proxy_shutdown_socket(tpd);
                tpd->server->close(tpd->server);
                return 0;
            }
        }

        rv = apr_socket_send(tpd->tcpsocket, towrite, &len);
        if (tpd->base64)
            free(towrite);

        if (rv != APR_SUCCESS) {
            char s[1024];
            apr_strerror(rv, s, sizeof(s));
            APACHELOG(APLOG_DEBUG, r,
                      "tcp_proxy_on_message: apr_socket_send failed, rv=%d, sent=%lu, %s",
                      rv, (unsigned long) len, s);
            tcp_proxy_shutdown_socket(tpd);
            tpd->server->close(tpd->server);
            return 0;
        }

        /*
           len = 2;
           rv = apr_socket_send(tpd->tcpsocket, "\r\n", &len);
         */
    }

    return 0;
}

void *CALLBACK tcp_proxy_on_connect(const WebSocketServer * server)
{
    TcpProxyData *tpd = NULL;

    if ((server != NULL) && (server->version == WEBSOCKET_SERVER_VERSION_1)) {
        /* Get access to the request_rec strucure for this connection */
        request_rec *r = server->request(server);

        APACHELOG(APLOG_DEBUG, r, "tcp_proxy_on_connect starting");

        if (r != NULL) {

            apr_pool_t *pool = NULL;
            size_t i = 0, count = server->protocol_count(server);

            websocket_tcp_proxy_config_rec *conf =
                (websocket_tcp_proxy_config_rec *) ap_get_module_config(r->
                                                                        per_dir_config,
                                                                        &websocket_tcp_proxy_module);
            const char *requiredprotocol = conf ? conf->protocol : NULL;

            if (requiredprotocol) {
                for (i = 0; i < count; i++) {
                    const char *protocol = server->protocol_index(server, i);

                    if (protocol && (strcmp(protocol, requiredprotocol) == 0)) {
                        /* If the client can speak the protocol, set it in the response */
                        server->protocol_set(server, protocol);
                        break;
                    }
                }
            }
            else {
                count = 1;      /* ensure i<count */
            }

            /* If the protocol negotiation worked, create a new memory pool */
            if ((i < count) &&
                (apr_pool_create(&pool, r->pool) == APR_SUCCESS)) {

                APACHELOG(APLOG_DEBUG, r,
                          "tcp_proxy_on_connect protocol correct");

                /* Allocate memory to hold the tcp proxy state */
                if ((tpd =
                     (TcpProxyData *) apr_palloc(pool,
                                                 sizeof(TcpProxyData))) !=
                    NULL) {
                    apr_thread_t *thread = NULL;
                    apr_threadattr_t *thread_attr = NULL;

                    tpd->server = server;
                    tpd->pool = pool;
                    tpd->thread = NULL;
                    tpd->tcpsocket = NULL;
                    tpd->active = 1;
                    tpd->base64 = 0;
                    tpd->timeout = 30;
                    tpd->port = "echo";
                    tpd->host = "127.0.0.1";
                    tpd->initialdata = NULL;

                    websocket_tcp_proxy_config_rec *conf =
                        (websocket_tcp_proxy_config_rec *)
                        ap_get_module_config(r->per_dir_config,
                                             &websocket_tcp_proxy_module);
                    if (conf) {
                        tpd->base64 = conf->base64;
                        tpd->timeout = conf->timeout;
                        if (conf->host)
                            tpd->host = apr_pstrdup(pool, conf->host);
                        if (conf->port)
                            tpd->port = apr_pstrdup(pool, conf->port);
                        APACHELOG(APLOG_DEBUG, r,
                                  "tcp_proxy_on_connect: base64 is %d",
                                  conf->base64);
                    }
                    else {
                        APACHELOG(APLOG_DEBUG, r,
                                  "tcp_proxy_on_connect: no config");
                    }

                    /* Check we can authenticate the incoming user (this is a hook for others to add to)
                     * Check we can connect
                     * And if we have initial data to send, then send that
                     */
                    if ((APR_SUCCESS ==
                         tcp_proxy_do_authenticate(r, tpd, pool))
                        && (APR_SUCCESS ==
                            tcp_proxy_do_tcp_connect(r, tpd, pool))
                        && (APR_SUCCESS ==
                            tcp_proxy_send_initial_data(r, tpd, pool))) {

                        /* Create a non-detached thread that will perform the work */
                        if ((apr_threadattr_create(&thread_attr, pool) ==
                             APR_SUCCESS)
                            && (apr_threadattr_detach_set(thread_attr, 0) ==
                                APR_SUCCESS)
                            &&
                            (apr_thread_create
                             (&thread, thread_attr, tcp_proxy_run, tpd,
                              pool) == APR_SUCCESS)) {
                            tpd->thread = thread;
                            /* Success */
                            pool = NULL;
                        }
                        else {
                            tpd = NULL;
                        }
                    }
                    else
                        tpd = NULL;
                }
                if (pool != NULL) {
                    apr_pool_destroy(pool);
                }
            }
        }
    }
    return tpd;
}

void CALLBACK tcp_proxy_on_disconnect(void *plugin_private,
                                      const WebSocketServer * server)
{
    TcpProxyData *tpd = (TcpProxyData *) plugin_private;

    request_rec *r = server->request(server);
    APACHELOG(APLOG_DEBUG, r, "tcp_proxy_on_disconnect");

    if (tpd != 0) {
        /* When disconnecting, inform the thread that it is time to stop */
        tpd->active = 0;
        tcp_proxy_shutdown_socket(tpd);
        if (tpd->thread) {
            apr_status_t status;

            /* Wait for the thread to finish */
            status = apr_thread_join(&status, tpd->thread);
        }
        tcp_proxy_shutdown_socket(tpd);

        if (tpd->tcpsocket) {
            apr_socket_close(tpd->tcpsocket);
            tpd->tcpsocket = NULL;
        }

        apr_pool_destroy(tpd->pool);
    }
}

/*
 * Since we are returning a pointer to static memory, there is no need for a
 * "destroy" function.
 */

static WebSocketPlugin s_plugin = {
    sizeof(WebSocketPlugin),
    WEBSOCKET_PLUGIN_VERSION_0,
    NULL,                       /* destroy */
    tcp_proxy_on_connect,
    tcp_proxy_on_message,
    tcp_proxy_on_disconnect
};

extern EXPORT WebSocketPlugin *CALLBACK tcp_proxy_init()
{
    return &s_plugin;
}

static const char *mod_websocket_tcp_proxy_conf_base64(cmd_parms * cmd,
                                                       void *config, int flag)
{
    websocket_tcp_proxy_config_rec *cfg =
        (websocket_tcp_proxy_config_rec *) config;
    cfg->base64 = flag;
    return NULL;
}

static const char *mod_websocket_tcp_proxy_conf_host(cmd_parms * cmd,
                                                     void *config,
                                                     const char *arg)
{
    websocket_tcp_proxy_config_rec *cfg =
        (websocket_tcp_proxy_config_rec *) config;
    cfg->host = arg;
    return NULL;
}

static const char *mod_websocket_tcp_proxy_conf_port(cmd_parms * cmd,
                                                     void *config,
                                                     const char *arg)
{
    websocket_tcp_proxy_config_rec *cfg =
        (websocket_tcp_proxy_config_rec *) config;
    cfg->port = arg;
    return NULL;
}

static const char *mod_websocket_tcp_proxy_conf_protocol(cmd_parms * cmd,
                                                         void *config,
                                                         const char *arg)
{
    websocket_tcp_proxy_config_rec *cfg =
        (websocket_tcp_proxy_config_rec *) config;
    cfg->protocol = strcmp(arg, "any") ? arg : NULL;
    return NULL;
}

static const char *mod_websocket_tcp_proxy_conf_timeout(cmd_parms * cmd,
                                                        void *config,
                                                        const char *arg)
{
    websocket_tcp_proxy_config_rec *cfg =
        (websocket_tcp_proxy_config_rec *) config;
    cfg->timeout = atoi(arg);
    return NULL;
}

static const command_rec mod_websocket_tcp_proxy_cmds[] = {
    AP_INIT_FLAG("WebSocketTcpProxyBase64",
                 mod_websocket_tcp_proxy_conf_base64, NULL, OR_AUTHCFG,
                 "Flag to indicate use of base64 encoding; defaults to off"),
    AP_INIT_TAKE1("WebSocketTcpProxyHost", mod_websocket_tcp_proxy_conf_host,
                  NULL, OR_AUTHCFG,
                  "Host to connect WebSockets TCP proxy to; default 127.0.0.1"),
    AP_INIT_TAKE1("WebSocketTcpProxyPort", mod_websocket_tcp_proxy_conf_port,
                  NULL, OR_AUTHCFG,
                  "Port to connect WebSockets TCP proxy to; default echo"),
    AP_INIT_TAKE1("WebSocketTcpProxyProtocol",
                  mod_websocket_tcp_proxy_conf_protocol, NULL, OR_AUTHCFG,
                  "WebSockets protocols to accept, or 'any'; default 'any'"),
    AP_INIT_TAKE1("WebSocketTcpProxyTimeout",
                  mod_websocket_tcp_proxy_conf_timeout, NULL, OR_AUTHCFG,
                  "WebSockets proxy connection timeout in seconds; default 30"),
    {NULL}
};

static void *mod_websocket_tcp_proxy_create_dir_config(apr_pool_t * p,
                                                       char *path)
{
    websocket_tcp_proxy_config_rec *conf = NULL;

    if (path != NULL) {
        conf = apr_pcalloc(p, sizeof(websocket_tcp_proxy_config_rec));
        if (conf != NULL) {
            conf->location = apr_pstrdup(p, path);
            conf->base64 = 0;
            conf->host = apr_pstrdup(p, "127.0.0.1");
            conf->port = apr_pstrdup(p, "echo");
            conf->protocol = NULL;
            conf->timeout = 30;
        }
    }
    return (void *) conf;
}

static int mod_websocket_tcp_proxy_method_handler(request_rec * r)
{
    return DECLINED;
}

static void mod_websocket_tcp_proxy_register_hooks(apr_pool_t * p)
{
    ap_hook_handler(mod_websocket_tcp_proxy_method_handler, NULL, NULL,
                    APR_HOOK_LAST);
}

module AP_MODULE_DECLARE_DATA websocket_tcp_proxy_module = {
    STANDARD20_MODULE_STUFF,
    mod_websocket_tcp_proxy_create_dir_config,  /* create per-directory config structure */
    NULL,                       /* merge per-directory config structures */
    NULL,                       /* create server config structure */
    NULL,                       /* merge server config structures */
    mod_websocket_tcp_proxy_cmds,       /* command table */
    mod_websocket_tcp_proxy_register_hooks,     /* hooks */
};
