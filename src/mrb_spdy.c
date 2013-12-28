/*
** mrb_spdy - spdy class for mruby
**
** Copyright (c) mod_mruby developers 2012-
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** "Software"), to deal in the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
** CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
** SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
** [ MIT license: http://www.opensource.org/licenses/mit-license.php ]
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <assert.h>

#include <spdylay/spdylay.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "mruby.h"
#include "mruby/data.h"
#include "mruby/variable.h"
#include "mruby/array.h"
#include "mruby/hash.h"
#include "mruby/string.h"
#include "mruby/class.h"
#include "mrb_spdy.h"


#define DONE mrb_gc_arena_restore(mrb, 0);
#define MRUBY_SPDY_NAME "mruby-spdy"
#define MRUBY_SPDY_VERSION "0.0.1"

enum {
  IO_NONE,
  WANT_READ,
  WANT_WRITE
};

struct mrb_spdy_conn_t {
  SSL *ssl;
  spdylay_session *session;
  int want_io;
  mrb_state *mrb;
  mrb_value response;
};

struct mrb_spdy_request_t {
  char *host;
  uint16_t port;
  char *path;
  char *hostport;
  int32_t stream_id;
  spdylay_gzip *inflater;
};

struct mrb_spdy_uri_t {
  const char *host;
  size_t hostlen;
  uint16_t port;
  const char *path;
  size_t pathlen;
  const char *hostport;
  size_t hostportlen;
};

static char *strcopy(const char *s, size_t len)
{
  char *dst;
  dst = malloc(len+1);
  memcpy(dst, s, len);
  dst[len] = '\0';
  return dst;
}

static void mrb_spdy_check_gzip(mrb_state *mrb, struct mrb_spdy_request_t *req, char **nv)
{
  int gzip = 0;
  size_t i;
  for(i = 0; nv[i]; i += 2) {
    if(strcmp("content-encoding", nv[i]) == 0) {
      gzip = strcmp("gzip", nv[i+1]) == 0;
      break;
    }
  }
  if(gzip) {
    int rv;
    if(req->inflater) {
      return;
    }
    rv = spdylay_gzip_inflate_new(&req->inflater);
    if(rv != 0) {
      mrb_raise(mrb, E_RUNTIME_ERROR, "Can't allocate inflate stream.");
    }
  }
}

static ssize_t send_callback(spdylay_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
  struct mrb_spdy_conn_t *conn;
  ssize_t rv;
  conn = (struct mrb_spdy_conn_t*)user_data;
  conn->want_io = IO_NONE;
  ERR_clear_error();
  rv = SSL_write(conn->ssl, data, length);
  if(rv < 0) {
    int err = SSL_get_error(conn->ssl, rv);
    if(err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
      conn->want_io = (err == SSL_ERROR_WANT_READ ?
                             WANT_READ : WANT_WRITE);
      rv = SPDYLAY_ERR_WOULDBLOCK;
    } else {
      rv = SPDYLAY_ERR_CALLBACK_FAILURE;
    }
  }
  return rv;
}

static ssize_t recv_callback(spdylay_session *session, uint8_t *buf, size_t length, int flags, void *user_data)
{
  struct mrb_spdy_conn_t *conn;
  ssize_t rv;
  conn = (struct mrb_spdy_conn_t*)user_data;
  conn->want_io = IO_NONE;
  ERR_clear_error();
  rv = SSL_read(conn->ssl, buf, length);
  if(rv < 0) {
    int err = SSL_get_error(conn->ssl, rv);
    if(err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
      conn->want_io = (err == SSL_ERROR_WANT_READ ?
                             WANT_READ : WANT_WRITE);
      rv = SPDYLAY_ERR_WOULDBLOCK;
    } else {
      rv = SPDYLAY_ERR_CALLBACK_FAILURE;
    }
  } else if(rv == 0) {
    rv = SPDYLAY_ERR_EOF;
  }
  return rv;
}

static void before_ctrl_send_callback(spdylay_session *session, spdylay_frame_type type, spdylay_frame *frame, void *user_data)
{   
  struct mrb_spdy_conn_t *conn;
  conn = (struct mrb_spdy_conn_t*)user_data;

  if(type == SPDYLAY_SYN_STREAM) {
    struct mrb_spdy_request_t *req;
    int stream_id = frame->syn_stream.stream_id;
    req = spdylay_session_get_stream_user_data(session, stream_id);
    if(req && req->stream_id == -1) {
      req->stream_id = stream_id;
      mrb_hash_set(conn->mrb, conn->response, mrb_symbol_value(mrb_intern_cstr(conn->mrb, "stream_id")), mrb_fixnum_value(stream_id));
    }
  }
}

static void on_ctrl_send_callback(spdylay_session *session, spdylay_frame_type type, spdylay_frame *frame, void *user_data)
{
  struct mrb_spdy_conn_t *conn;
  conn = (struct mrb_spdy_conn_t*)user_data;
  mrb_value syn_stream;
  char **nv;
  const char *name = NULL;
  int32_t stream_id;
  size_t i;
  switch(type) {
  case SPDYLAY_SYN_STREAM:
    nv = frame->syn_stream.nv;
    name = "SYN_STREAM";
    stream_id = frame->syn_stream.stream_id;
    break;
  default:
    break;
  }
  if(name && spdylay_session_get_stream_user_data(session, stream_id)) {
    syn_stream = mrb_hash_new(conn->mrb);
    for(i = 0; nv[i]; i += 2) {
      mrb_hash_set(conn->mrb, syn_stream, mrb_symbol_value(mrb_intern_cstr(conn->mrb, nv[i])), mrb_str_new_cstr(conn->mrb, nv[i+1]));
    }
    mrb_hash_set(conn->mrb, conn->response, mrb_symbol_value(mrb_intern_cstr(conn->mrb, "syn_stream")), syn_stream);
  }
}

static void on_ctrl_recv_callback(spdylay_session *session, spdylay_frame_type type, spdylay_frame *frame, void *user_data)
{
  struct mrb_spdy_conn_t *conn;
  conn = (struct mrb_spdy_conn_t*)user_data;
  mrb_value syn_reply;
  struct mrb_spdy_request_t *req;
  char **nv;
  const char *name = NULL;
  int32_t stream_id;
  size_t i;
  switch(type) {
  case SPDYLAY_SYN_REPLY:
    nv = frame->syn_reply.nv;
    name = "SYN_REPLY";
    stream_id = frame->syn_reply.stream_id;
    break;
  case SPDYLAY_HEADERS:
    nv = frame->headers.nv;
    name = "HEADERS";
    stream_id = frame->headers.stream_id;
    break;
  default:
    break;
  }
  if(!name) {
    return;
  }
  req = spdylay_session_get_stream_user_data(session, stream_id);
  if(req) {
    mrb_spdy_check_gzip(conn->mrb, req, nv);
    syn_reply = mrb_hash_new(conn->mrb);
    for(i = 0; nv[i]; i += 2) {
      mrb_hash_set(conn->mrb, syn_reply, mrb_symbol_value(mrb_intern_cstr(conn->mrb, nv[i])), mrb_str_new_cstr(conn->mrb, nv[i+1]));
    }
    mrb_hash_set(conn->mrb, conn->response, mrb_symbol_value(mrb_intern_cstr(conn->mrb, "syn_reply")), syn_reply);
  }
}

static void on_stream_close_callback(spdylay_session *session, int32_t stream_id, spdylay_status_code status_code, void *user_data)
{
  struct mrb_spdy_conn_t *conn;
  conn = (struct mrb_spdy_conn_t*)user_data;
  mrb_state *mrb = conn->mrb;
  struct mrb_spdy_request_t *req;
  req = spdylay_session_get_stream_user_data(session, stream_id);
  if(req) {
    int rv;
    rv = spdylay_submit_goaway(session, SPDYLAY_GOAWAY_OK);
    if(rv != 0) {
      mrb_raisef(mrb, E_RUNTIME_ERROR, "spdylay_submit_goaway: %S", mrb_fixnum_value(rv));
    }
  }
}

#define MAX_OUTLEN 4096

static void on_data_chunk_recv_callback(spdylay_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len, void *user_data)
{
  struct mrb_spdy_conn_t *conn;
  conn = (struct mrb_spdy_conn_t*)user_data;
  struct mrb_spdy_request_t *req;
  char *body;
  req = spdylay_session_get_stream_user_data(session, stream_id);
  if(req) {
    mrb_hash_set(conn->mrb, conn->response, mrb_symbol_value(mrb_intern_cstr(conn->mrb, "recieve_bytes")), mrb_float_value(conn->mrb, (float)len));
    body = NULL;
    if(req->inflater) {
      while(len > 0) {
        uint8_t out[MAX_OUTLEN];
        size_t outlen = MAX_OUTLEN;
        size_t tlen = len;
        int rv;
        rv = spdylay_gzip_inflate(req->inflater, out, &outlen, data, &tlen);
        if(rv == -1) {
          spdylay_submit_rst_stream(session, stream_id, SPDYLAY_INTERNAL_ERROR);
          break;
        }
        char *merge_body = strcopy((char *)out, outlen);
        if (body == NULL) {
          body = merge_body;
        }
        else {
          strcat(body, merge_body);
        }
        data += tlen;
        len -= tlen;
      }
    } else {
      body = strcopy((char *)data, len);
    }
    mrb_value body_data = mrb_hash_get(conn->mrb, conn->response, mrb_symbol_value(mrb_intern_cstr(conn->mrb, "body")));
    mrb_value body_len;
    if (!mrb_nil_p(body_data)) {
      mrb_str_concat(conn->mrb, body_data, mrb_str_new_cstr(conn->mrb, (char *)body));
    }
    else {
      body_data = mrb_str_new_cstr(conn->mrb, (char *)body);
    }
    body_len = mrb_fixnum_value(strlen(body));
    mrb_hash_set(conn->mrb, conn->response, mrb_symbol_value(mrb_intern_cstr(conn->mrb, "body")), body_data);
    mrb_hash_set(conn->mrb, conn->response, mrb_symbol_value(mrb_intern_cstr(conn->mrb, "body_length")), body_len);
  }
}

static void mrb_spdy_setup_spdylay_callbacks(mrb_state *mrb, spdylay_session_callbacks *callbacks)
{
  memset(callbacks, 0, sizeof(spdylay_session_callbacks));
  callbacks->send_callback = send_callback;
  callbacks->recv_callback = recv_callback;
  callbacks->before_ctrl_send_callback = before_ctrl_send_callback;
  callbacks->on_ctrl_send_callback = on_ctrl_send_callback;
  callbacks->on_ctrl_recv_callback = on_ctrl_recv_callback;
  callbacks->on_stream_close_callback = on_stream_close_callback;
  callbacks->on_data_chunk_recv_callback = on_data_chunk_recv_callback;
}

static int select_next_proto_cb(SSL* ssl, unsigned char **out, unsigned char *outlen, const unsigned char *in, unsigned int inlen, void *arg)
{
  int rv;
  uint16_t *spdy_proto_version;
  rv = spdylay_select_next_protocol(out, outlen, in, inlen);
  if(rv <= 0) {
    fprintf(stderr, "FATAL: %s\n", "Server did not advertise spdy/2 or spdy/3 protocol.");
    exit(EXIT_FAILURE);
  }
  spdy_proto_version = (uint16_t*)arg;
  *spdy_proto_version = rv;
  return SSL_TLSEXT_ERR_OK;
}

static void mrb_spdy_init_ssl_ctx(mrb_state *mrb, SSL_CTX *ssl_ctx, uint16_t *spdy_proto_version)
{
  SSL_CTX_set_options(ssl_ctx, SSL_OP_ALL|SSL_OP_NO_SSLv2);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
  SSL_CTX_set_next_proto_select_cb(ssl_ctx, select_next_proto_cb,
                                   spdy_proto_version);
}

static void mrb_spdy_ssl_handshake(mrb_state *mrb, SSL *ssl, int fd)
{
  int rv;
  if(SSL_set_fd(ssl, fd) == 0) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "SSL_set_fd: %S", mrb_str_new_cstr(mrb, ERR_error_string(ERR_get_error(), NULL)));
  }
  ERR_clear_error();
  rv = SSL_connect(ssl);
  if(rv <= 0) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "SSL_connect: %S", mrb_str_new_cstr(mrb, ERR_error_string(ERR_get_error(), NULL)));
  }
}

static int mrb_spdy_connect_to(mrb_state *mrb, const char *host, uint16_t port)
{ 
  struct addrinfo hints;
  int fd = -1;
  int rv;
  char service[NI_MAXSERV];
  struct addrinfo *res, *rp;
  snprintf(service, sizeof(service), "%u", port);
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  rv = getaddrinfo(host, service, &hints, &res);
  if(rv != 0) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "getaddrinfo: %S", mrb_str_new_cstr(mrb, gai_strerror(rv)));
  }
  for(rp = res; rp; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if(fd == -1) {
      continue;
    }
    while((rv = connect(fd, rp->ai_addr, rp->ai_addrlen)) == -1 &&
          errno == EINTR);
    if(rv == 0) {
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

static void mrb_spdy_make_non_block(mrb_state *mrb, int fd)
{ 
  int flags, rv;
  while((flags = fcntl(fd, F_GETFL, 0)) == -1 && errno == EINTR);
  if(flags == -1) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "fcntl: %S", mrb_str_new_cstr(mrb, strerror(errno)));
  }
  while((rv = fcntl(fd, F_SETFL, flags | O_NONBLOCK)) == -1 && errno == EINTR);
  if(rv == -1) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "fcntl: %S", mrb_str_new_cstr(mrb, strerror(errno)));
  }
}

static void mrb_spdy_set_tcp_nodelay(mrb_state *mrb, int fd)
{ 
  int val = 1;
  int rv;
  rv = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, (socklen_t)sizeof(val));
  if(rv == -1) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "setsockopt: %S", mrb_str_new_cstr(mrb, strerror(errno)));
  }
}

static void mrb_spdy_ctl_poll(mrb_state *mrb, struct pollfd *pollfd, struct mrb_spdy_conn_t *conn)
{
  pollfd->events = 0;
  if(spdylay_session_want_read(conn->session) ||
     conn->want_io == WANT_READ) {
    pollfd->events |= POLLIN;
  }
  if(spdylay_session_want_write(conn->session) ||
     conn->want_io == WANT_WRITE) {
    pollfd->events |= POLLOUT;
  }
}

static void mrb_spdy_submit_request(mrb_state *mrb, struct mrb_spdy_conn_t *conn, struct mrb_spdy_request_t *req)
{
  int pri = 0;
  int rv;
  const char *nv[15];
  nv[0] = ":method";     nv[1] = "GET";
  nv[2] = ":path";       nv[3] = req->path;
  nv[4] = ":version";    nv[5] = "HTTP/1.1";
  nv[6] = ":scheme";     nv[7] = "https";
  nv[8] = ":host";       nv[9] = req->hostport;
  nv[10] = "accept";     nv[11] = "*/*";
  nv[12] = "user-agent"; nv[13] = MRUBY_SPDY_NAME"/"MRUBY_SPDY_VERSION;
  nv[14] = NULL;
  rv = spdylay_submit_request(conn->session, pri, nv, NULL, req);
  if(rv != 0) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "spdylay_submit_request: %S", mrb_fixnum_value(rv));
  }
}

static void mrb_spdy_exec_io(mrb_state *mrb, struct mrb_spdy_conn_t *conn)
{
  int rv;
  rv = spdylay_session_recv(conn->session);
  if(rv != 0) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "spdylay_session_recv: %S", mrb_fixnum_value(rv));
  }
  rv = spdylay_session_send(conn->session);
  if(rv != 0) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "spdylay_session_send: %S", mrb_fixnum_value(rv));
  }
}

static void mrb_spdy_request_init(mrb_state *mrb, struct mrb_spdy_request_t *req, const struct mrb_spdy_uri_t *uri)
{
  req->host = strcopy(uri->host, uri->hostlen);
  req->port = uri->port;
  req->path = strcopy(uri->path, uri->pathlen);
  req->hostport = strcopy(uri->hostport, uri->hostportlen);
  req->stream_id = -1;
  req->inflater = NULL;
}

static void mrb_spdy_request_free(mrb_state *mrb, struct mrb_spdy_request_t *req)
{
  free(req->host);
  free(req->path);
  free(req->hostport);
  spdylay_gzip_inflate_del(req->inflater);
}

static mrb_value mrb_spdy_fetch_uri(mrb_state *mrb, const struct mrb_spdy_uri_t *uri)
{
  spdylay_session_callbacks callbacks;
  int fd;
  SSL_CTX *ssl_ctx;
  SSL *ssl;
  struct mrb_spdy_request_t req;
  struct mrb_spdy_conn_t conn;
  int rv;
  nfds_t npollfds = 1;
  struct pollfd pollfds[1];
  uint16_t spdy_proto_version;
  mrb_value response = mrb_hash_new(mrb);

  mrb_spdy_request_init(mrb, &req, uri);

  mrb_spdy_setup_spdylay_callbacks(mrb, &callbacks);

  fd = mrb_spdy_connect_to(mrb, req.host, req.port);
  if(fd == -1) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "Could not open file descriptor");
  }
  ssl_ctx = SSL_CTX_new(SSLv23_client_method());
  if(ssl_ctx == NULL) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "SSL_CTX_new: %S", mrb_str_new_cstr(mrb, ERR_error_string(ERR_get_error(), NULL)));
  }
  mrb_spdy_init_ssl_ctx(mrb, ssl_ctx, &spdy_proto_version);
  ssl = SSL_new(ssl_ctx);
  if(ssl == NULL) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "SSL_new: %S", mrb_str_new_cstr(mrb, ERR_error_string(ERR_get_error(), NULL)));
  }
  mrb_spdy_ssl_handshake(mrb, ssl, fd);

  conn.ssl = ssl;
  conn.want_io = IO_NONE;

  mrb_spdy_make_non_block(mrb, fd);
  mrb_spdy_set_tcp_nodelay(mrb, fd);

  mrb_hash_set(mrb, response, mrb_symbol_value(mrb_intern_cstr(mrb, "spdy_proto_version")), mrb_fixnum_value(spdy_proto_version));
  conn.mrb = mrb;
  conn.response = response;
  rv = spdylay_session_client_new(&conn.session, spdy_proto_version, &callbacks, &conn);
  if(rv != 0) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "spdylay_session_client_new: %S", mrb_fixnum_value(rv));
  }

  mrb_spdy_submit_request(mrb, &conn, &req);

  pollfds[0].fd = fd;
  mrb_spdy_ctl_poll(mrb, pollfds, &conn);

  while(spdylay_session_want_read(conn.session) || spdylay_session_want_write(conn.session)) {
    int nfds = poll(pollfds, npollfds, -1);
    if(nfds == -1) {
      mrb_raisef(mrb, E_RUNTIME_ERROR, "poll: %S", mrb_str_new_cstr(mrb, strerror(errno)));
    } 
    if(pollfds[0].revents & (POLLIN | POLLOUT)) {
      mrb_spdy_exec_io(mrb, &conn);
    }
    if((pollfds[0].revents & POLLHUP) || (pollfds[0].revents & POLLERR)) {
      mrb_raise(mrb, E_RUNTIME_ERROR, "connection error");
    }
    mrb_spdy_ctl_poll(mrb, pollfds, &conn);
  }

  spdylay_session_del(conn.session);
  SSL_shutdown(ssl);
  SSL_free(ssl);
  SSL_CTX_free(ssl_ctx);
  shutdown(fd, SHUT_WR);
  close(fd);
  mrb_spdy_request_free(mrb, &req);

  return response;
}

//TODO: use mruby-http
static int parse_uri(struct mrb_spdy_uri_t *res, const char *uri)
{
  size_t len, i, offset;
  int ipv6addr = 0;
  memset(res, 0, sizeof(struct mrb_spdy_uri_t));
  len = strlen(uri);
  if(len < 9 || memcmp("https://", uri, 8) != 0) {
    return -1;
  }
  offset = 8;
  res->host = res->hostport = &uri[offset];
  res->hostlen = 0;
  if(uri[offset] == '[') {
    ++offset;
    ++res->host;
    ipv6addr = 1;
    for(i = offset; i < len; ++i) {
      if(uri[i] == ']') {
        res->hostlen = i-offset;
        offset = i+1;
        break;
      }
    }
  } else {
    const char delims[] = ":/?#";
    for(i = offset; i < len; ++i) {
      if(strchr(delims, uri[i]) != NULL) {
        break;
      }
    }
    res->hostlen = i-offset;
    offset = i;
  }
  if(res->hostlen == 0) {
    return -1;
  }
  res->port = 443;
  if(offset < len) {
    if(uri[offset] == ':') {
      const char delims[] = "/?#";
      int port = 0;
      ++offset;
      for(i = offset; i < len; ++i) {
        if(strchr(delims, uri[i]) != NULL) {
          break;
        }
        if('0' <= uri[i] && uri[i] <= '9') {
          port *= 10;
          port += uri[i]-'0';
          if(port > 65535) {
            return -1;
          }
        } else {
          return -1;
        }
      }
      if(port == 0) {
        return -1;
      }
      offset = i;
      res->port = port;
    }
  }
  res->hostportlen = uri+offset+ipv6addr-res->host;
  for(i = offset; i < len; ++i) {
    if(uri[i] == '#') {
      break;
    }
  }
  if(i-offset == 0) {
    res->path = "/";
    res->pathlen = 1;
  } else {
    res->path = &uri[offset];
    res->pathlen = i-offset;
  }
  return 0;
}

mrb_value mrb_spdy_client_get(mrb_state *mrb, mrb_value self)
{
  char *uri;
  struct mrb_spdy_uri_t uri_data;
  struct sigaction act;
  int rv;

  mrb_get_args(mrb, "z", &uri);
  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &act, 0);

  SSL_load_error_strings();
  SSL_library_init();

  rv = parse_uri(&uri_data, uri);
  if(rv != 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "parse_uri failed");
  }
  return mrb_spdy_fetch_uri(mrb, &uri_data);
}

void mrb_mruby_spdy_gem_init(mrb_state *mrb)
{
  struct RClass *spdy, *client;

  spdy = mrb_define_module(mrb, "SPDY");
  client = mrb_define_class_under(mrb, spdy, "Client", mrb->object_class);
  mrb_define_class_method(mrb, client, "get", mrb_spdy_client_get, ARGS_REQ(1));

  DONE;
}

void mrb_mruby_spdy_gem_final(mrb_state *mrb)
{
}

