/*
 * Copyright (c) 2007-2010, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 * Copyright (c) 2010-2015, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name OmniTI Computer Consulting, Inc. nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mtev_defines.h"

#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <dirent.h>
#include <stdint.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif
#include <sys/wait.h>
#include <sys/utsname.h>
#include <zlib.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/tree.h>
#include <libxml/HTMLparser.h>
#include <openssl/md5.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <libtz.h>

#include "mtev_conf.h"
#include "mtev_reverse_socket.h"
#include "mtev_xml.h"
#include "mtev_log.h"
#include "mtev_str.h"
#include "mtev_b32.h"
#include "mtev_b64.h"
#include "mtev_getip.h"
#include "mtev_lockfile.h"
#include "mtev_mkdir.h"
#include "eventer/eventer.h"
#include "mtev_json.h"
#include "mtev_watchdog.h"
#include "mtev_cluster.h"
#include "mtev_thread.h"
#include "mtev_websocket_client.h"

#define LUA_COMPAT_MODULE
#include "lua_mtev.h"

extern mtev_log_stream_t mtev_lua_error_ls;
extern mtev_log_stream_t mtev_lua_debug_ls;
#define nldeb mtev_lua_debug_ls
#define nlerr mtev_lua_error_ls

static mtev_hash_table shared_queues = MTEV_HASH_EMPTY;
static pthread_mutex_t shared_queues_mutex;
static mtev_hash_table shared_table = MTEV_HASH_EMPTY;
static pthread_mutex_t shared_table_mutex;
static mtev_hash_table *shared_seq_table;

static mtev_hash_table *semaphore_table;

typedef struct {
  mtev_hash_table string_keys;
  mtev_hash_table int_keys;
} mtev_lua_table_t;

typedef struct {
  int lua_type; // see lua.h (LUA_TNIL, LUA_TNUMBER, LUA_TBOOLEAN, LUA_TSTRING, LUA_TTABLE, LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD, and LUA_TLIGHTUSERDATA)
  union {
    lua_Number number;
    mtev_boolean boolean;
    char *string;
    mtev_lua_table_t* table;
  } value;
} lua_data_t;

static lua_data_t* mtev_lua_serialize(lua_State *L, int index);
void mtev_lua_deserialize(lua_State *L, const lua_data_t *data);
static void mtev_lua_free_data(void *vdata);

#define DEFLATE_CHUNK_SIZE 32768
#define ON_STACK_LUA_STRLEN 2048

#define LUA_DISPATCH(n, f) \
     if(!strcmp(k, #n)) { \
       lua_pushlightuserdata(L, udata); \
       lua_pushcclosure(L, f, 1); \
       return 1; \
     }
#define LUA_RETSTRING(n, g) \
     if(!strcmp(k, #n)) { \
       lua_pushstring(L, g); \
       return 1; \
     }
#define LUA_RETINTEGER(n, g) \
     if(!strcmp(k, #n)) { \
       lua_pushinteger(L, g); \
       return 1; \
     }

typedef struct {
  mtev_json_tokener *tok;
  mtev_json_object *root;
} json_crutch;

static int
nl_mtev_timeval_new_ex(lua_State *L, struct timeval src) {
  struct timeval *tgt = (struct timeval *)lua_newuserdata(L, sizeof(*tgt));
  *tgt = src;
  luaL_getmetatable(L, "mtev.timeval");
  lua_setmetatable(L, -2);
  return 1;
}

static int nl_mtev_timeval_now(lua_State *L) {
  struct timeval now;
  mtev_gettimeofday(&now, NULL);
  return nl_mtev_timeval_new_ex(L, now);
}

static void double_to_timeval(double in, struct timeval *out) {
  out->tv_sec = floor(in);
  double unused_inf;
  double tus = modf(in, &unused_inf);
  if(tus < 0) tus += 1;
  out->tv_usec = round(1000000.0 * tus);
}
static int nl_mtev_timeval_new(lua_State *L) {
  if(lua_gettop(L) == 1) {
    struct timeval tv;
    double_to_timeval(lua_tonumber(L,1), &tv);
    return nl_mtev_timeval_new_ex(L, tv);
  }
  struct timeval tv = { .tv_sec = lua_tointeger(L,1), .tv_usec = lua_tointeger(L,2) };
  return nl_mtev_timeval_new_ex(L, tv);
}

#define TVDOUBLE(tv) ((double)(tv)->tv_sec + (double)(tv)->tv_usec / 1000000.0)
static int
mtev_lua_timeval_tostring(lua_State *L) {
  char buf[128];
  struct timeval *tv = luaL_checkudata(L, 1, "mtev.timeval");
  snprintf(buf, sizeof(buf), "%lu.%06lu", tv->tv_sec, tv->tv_usec);
  lua_pushstring(L, buf);
  return 1;
}
static int
mtev_lua_timeval_sub(lua_State *L) {
  struct timeval *tv = luaL_checkudata(L, 1, "mtev.timeval");
  struct timeval *o = luaL_checkudata(L, 2, "mtev.timeval");
  struct timeval n;
  sub_timeval(*tv, *o, &n);
  return nl_mtev_timeval_new_ex(L, n);
}
static int
mtev_lua_timeval_add(lua_State *L) {
  struct timeval *tv = luaL_checkudata(L, 1, "mtev.timeval");
  struct timeval *o, stacko;
  if(lua_isnumber(L,2)) {
    double_to_timeval(lua_tonumber(L,2), &stacko);
    o = &stacko;
  }
  else o = luaL_checkudata(L, 2, "mtev.timeval");
  struct timeval n;
  add_timeval(*tv, *o, &n);
  return nl_mtev_timeval_new_ex(L, n);
}
static int
mtev_lua_timeval_eq(lua_State *L) {
  struct timeval *left = luaL_checkudata(L, 1, "mtev.timeval");
  struct timeval *right, right_st;
  if(lua_isnumber(L,2)) {
    double_to_timeval(lua_tonumber(L,2), &right_st);
    right = &right_st;
  } else {
    right = luaL_checkudata(L, 2, "mtev.timeval");
  }
  lua_pushboolean(L, compare_timeval(*left,*right) == 0);
  return 1;
}
static int
mtev_lua_timeval_lt(lua_State *L) {
  struct timeval *left = luaL_checkudata(L, 1, "mtev.timeval");
  struct timeval *right, right_st;
  if(lua_isnumber(L,2)) {
    double_to_timeval(lua_tonumber(L,2), &right_st);
    right = &right_st;
  } else {
    right = luaL_checkudata(L, 2, "mtev.timeval");
  }
  lua_pushboolean(L, compare_timeval(*left,*right) < 0);
  return 1;
}
static int
mtev_lua_timeval_le(lua_State *L) {
  struct timeval *left = luaL_checkudata(L, 1, "mtev.timeval");
  struct timeval *right, right_st;
  if(lua_isnumber(L,2)) {
    double_to_timeval(lua_tonumber(L,2), &right_st);
    right = &right_st;
  } else {
    right = luaL_checkudata(L, 2, "mtev.timeval");
  }
  lua_pushboolean(L, compare_timeval(*left,*right) <= 0);
  return 1;
}
static int
mtev_lua_timeval_seconds(lua_State *L) {
  struct timeval *tv = luaL_checkudata(L, 1, "mtev.timeval");
  lua_pushnumber(L, TVDOUBLE(tv));
  return 1;
}
#undef TVDOUBLE

static int
mtev_lua_timeval_index_func(lua_State *L) {
  if(lua_gettop(L) != 2) {
    return luaL_error(L, "Illegal use of lua timeval");
  }
  struct timeval *tv = luaL_checkudata(L, 1, "mtev.timeval");
  const char* k = luaL_checkstring(L, 2);
  if(!strcmp(k, "seconds")) {
    lua_pushcclosure(L, mtev_lua_timeval_seconds, 0);
    return 1;
  }
  if(!strcmp(k, "sec")) {
    lua_pushinteger(L, tv->tv_sec);
    return 1;
  }
  if(!strcmp(k, "usec")) {
    lua_pushinteger(L, tv->tv_usec);
    return 1;
  }
  return 0;
}

static const luaL_Reg mtevtimevallib[] = {
  { "now", nl_mtev_timeval_now },
  { "new", nl_mtev_timeval_new },
  { "seconds", mtev_lua_timeval_seconds },
  { NULL, NULL }
};

static void
mtev_lua_eventer_cl_cleanup(eventer_t e) {
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl = eventer_get_closure(e);
  int newmask;
  if(cl) {
    if(cl->L) {
      ci = mtev_lua_find_resume_info(cl->L, mtev_false);
      if(ci) mtev_lua_deregister_event(ci, e, 0);
    }
    if(eventer_get_mask(e) & (EVENTER_EXCEPTION|EVENTER_READ|EVENTER_WRITE)) {
      eventer_remove_fde(e);
    }
    eventer_close(e, &newmask);
    if(cl->free) cl->free(cl);
    eventer_set_closure(e, NULL);
  }
}
static void
nl_extended_free(void *vcl) {
  struct nl_slcl *cl = vcl;
  if(cl->inbuff) free(cl->inbuff);
  if(cl->eptr) *cl->eptr = NULL;
  free(cl);
}
static void
lua_timeout_callback_ref_free(void* cb) {
  lua_timeout_callback_ref *callback_ref = (lua_timeout_callback_ref*) cb;
  luaL_unref(callback_ref->L, LUA_REGISTRYINDEX, callback_ref->callback_reference);
  free(callback_ref);
}
static void
inbuff_addlstring(struct nl_slcl *cl, const char *b, int l) {
  int newsize = 0;
  char *newbuf;

  if (cl->inbuff_len < 0 || l < 0) {
    mtevFatal(mtev_error, "Error (inbuff_addlstring): Invalid Argument to inbuff_addlstring: An argument was negative (ci->inbuff_len: %d, l: %d)\n",
            cl->inbuff_len, l);
  }
  if (cl->inbuff_len + l < 0) {
    mtevFatal(mtev_error, "Error (inbuff_addlstring): Addition Overflow im inbuff_addlstring (ci->inbuff_len: %d, l: %d, sum: %d\n",
            cl->inbuff_len, l, cl->inbuff_len+l);
  }

  if(cl->inbuff_len + l > cl->inbuff_allocd)
    newsize = cl->inbuff_len + l;
  if(newsize) {
    newbuf = cl->inbuff_allocd ? realloc(cl->inbuff, newsize) : malloc(newsize);
    if (!newbuf) {
      mtevFatal(mtev_error, "Error (inbuff_addlstring): Couldn't allocate newbuf: %d (%s) - inbuff_allocd %d, newsize %d\n",
              errno, strerror(errno), cl->inbuff_allocd, newsize);
    }
    cl->inbuff = newbuf;
    cl->inbuff_allocd = newsize;
  }
  memcpy(cl->inbuff + cl->inbuff_len, b, l);
  cl->inbuff_len += l;
}

static int
nl_resume(eventer_t e, int mask, void *vcl, struct timeval *now) {
    (void) mask;
    (void) now;
    lua_State *L = vcl;
    mtev_lua_resume_info_t *ci = mtev_lua_find_resume_info(L, mtev_false);
    if(!ci) {
        mtev_lua_eventer_cl_cleanup(e);
        return 0;
    }
    mtev_lua_deregister_event(ci, e, 0);
    mtev_lua_lmc_resume(ci->lmc, ci, 0);
    return 0;
}

static int
mtev_lua_socket_close(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  eventer_t *eptr, e;
  struct nl_slcl *cl;
  int newmask;

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if (e == NULL) return 0;

  /* Simply null it out so if we try to use it, we'll notice */
  *eptr = NULL;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  mtev_lua_deregister_event(ci, e, 0);
  eventer_remove_fde(e);
  eventer_close(e, &newmask);
  cl = eventer_get_closure(e);
  eventer_free(e);
  if(cl && cl->free) cl->free(cl);

 /* socket:close() is unsafe and can cause deadlocks: We could be closing a socket here,then
  * attempting to create a new socket in another thread with the same fd which would need out
  * lock... you have that inverted in the other thread and you have deadlock.
  * Solution: yield to the eventer.
  */

  e = eventer_in_s_us(nl_resume, (void*) L, 0, 0);
  mtev_lua_register_event(ci, e);
  eventer_add(e);
  return mtev_lua_yield(ci, 0);
}

static int
mtev_lua_socket_connect_complete(eventer_t e, int mask, void *vcl,
                                 struct timeval *now) {
  (void)now;
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl = vcl;
  int args = 0, aerrno;
  socklen_t aerrno_len = sizeof(aerrno);

  ci = mtev_lua_find_resume_info(cl->L, mtev_true);
  mtevAssert(ci);

  if(cl->timeout_event) {
    eventer_remove_timed(cl->timeout_event);
    mtev_lua_deregister_event(ci, cl->timeout_event, 1);
    cl->timeout_event = NULL;
  }

  eventer_remove_fde(e);
  mtev_lua_deregister_event(ci, e, 0);

  *(cl->eptr) = eventer_alloc_copy(e);
  eventer_set_mask(*cl->eptr, 0);
  mtev_lua_register_event(ci, *cl->eptr);

  if(getsockopt(eventer_get_fd(e),SOL_SOCKET,SO_ERROR, &aerrno, &aerrno_len) == 0)
    if(aerrno != 0) goto connerr;

  if(!(mask & EVENTER_EXCEPTION) &&
     mask & EVENTER_WRITE) {
    /* Connect completed successfully */
    lua_pushinteger(cl->L, 0);
    args = 1;
  }
  else {
    aerrno = errno;
   connerr:
    lua_pushinteger(cl->L, -1);
    lua_pushstring(cl->L, strerror(aerrno));
    args = 2;
  }
  mtev_lua_lmc_resume(ci->lmc, ci, args);
  return 0;
}
static int
mtev_lua_socket_recv_complete(eventer_t e, int mask, void *vcl,
                              struct timeval *now) {
  (void)now;
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl = vcl;
  int rv, args = 0;
  void *inbuff = NULL;
  socklen_t alen;

  ci = mtev_lua_find_resume_info(cl->L, mtev_false);
  if(!ci) { mtev_lua_eventer_cl_cleanup(e); return 0; }

  if(mask & EVENTER_EXCEPTION) {
    lua_pushinteger(cl->L, -1);
    args = 1;
    goto alldone;
  }

  inbuff = malloc(cl->read_goal);
  if(!inbuff) {
    lua_pushinteger(cl->L, -1);
    args = 1;
    goto alldone;
  }

  alen = sizeof(cl->address);
  while((rv = recvfrom(eventer_get_fd(e), inbuff, cl->read_goal, 0,
                       (struct sockaddr *)&cl->address, &alen)) == -1 &&
        errno == EINTR);
  if(rv < 0) {
    if(errno == EAGAIN) {
      free(inbuff);
      return EVENTER_READ | EVENTER_EXCEPTION;
    }
    lua_pushinteger(cl->L, rv);
    lua_pushstring(cl->L, strerror(errno));
    args = 2;
  }
  else {
    lua_pushinteger(cl->L, rv);
    lua_pushlstring(cl->L, inbuff, rv);
    args = 2;
    args += mtev_lua_push_inet_ntop(cl->L, (struct sockaddr *)&cl->address);
  }

 alldone:
  if(inbuff) free(inbuff);
  eventer_remove_fde(e);
  mtev_lua_deregister_event(ci, e, 0);
  *(cl->eptr) = eventer_alloc_copy(e);
  eventer_set_mask(*cl->eptr, 0);
  mtev_lua_register_event(ci, *cl->eptr);
  mtev_lua_lmc_resume(ci->lmc, ci, args);
  return 0;
}
static int
mtev_lua_socket_recv(lua_State *L) {
  int args, rv;
  struct nl_slcl *cl;
  mtev_lua_resume_info_t *ci;
  eventer_t e, *eptr;
  void *inbuff;
  socklen_t alen;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
  cl = eventer_get_closure(e);
  cl->read_goal = lua_tointeger(L, 2);
  inbuff = malloc(cl->read_goal);

  alen = sizeof(cl->address);
  while((rv = recvfrom(eventer_get_fd(e), inbuff, cl->read_goal, 0,
                       (struct sockaddr *)&cl->address, &alen)) == -1 &&
        errno == EINTR);
  if(rv < 0) {
    if(errno == EAGAIN) {
      eventer_remove_fde(e);
      eventer_set_callback(e, mtev_lua_socket_recv_complete);
      eventer_set_mask(e, EVENTER_READ | EVENTER_EXCEPTION);
      eventer_add(e);
      *eptr = NULL;
      free(inbuff);
      return mtev_lua_yield(ci, 0);
    }
    lua_pushinteger(cl->L, rv);
    lua_pushstring(cl->L, strerror(errno));
    args = 2;
  }
  else {
    lua_pushinteger(cl->L, rv);
    lua_pushlstring(cl->L, inbuff, rv);
    args = 2;
    args += mtev_lua_push_inet_ntop(cl->L, (struct sockaddr *)&cl->address);
  }
  free(inbuff);
  return args;
}
static int
mtev_lua_socket_send_complete(eventer_t e, int mask, void *vcl,
                              struct timeval *now) {
  (void)now;
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl = vcl;
  int sbytes;
  int args = 0;

  ci = mtev_lua_find_resume_info(cl->L, mtev_false);
  if(!ci) { mtev_lua_eventer_cl_cleanup(e); return 0; }

  if(mask & EVENTER_EXCEPTION) {
    lua_pushinteger(cl->L, -1);
    args = 1;
    goto alldone;
  }
  if(cl->sendto) {
    while((sbytes = sendto(eventer_get_fd(e), cl->outbuff, cl->write_goal, 0,
                           (struct sockaddr *)&cl->address,
                           cl->address.sin4.sin_family==AF_INET ?
                               sizeof(cl->address.sin4) :
                               sizeof(cl->address.sin6))) == -1 &&
          errno == EINTR);
  }
  else {
    while((sbytes = send(eventer_get_fd(e), cl->outbuff, cl->write_goal, 0)) == -1 &&
          errno == EINTR);
  }
  if(sbytes > 0) {
    lua_pushinteger(cl->L, sbytes);
    args = 1;
  }
  else if(sbytes == -1 && errno == EAGAIN) {
    return EVENTER_WRITE | EVENTER_EXCEPTION;
  }
  else {
    lua_pushinteger(cl->L, sbytes);
    args = 1;
    if(sbytes == -1) {
      lua_pushstring(cl->L, strerror(errno));
      args++;
    }
  }

 alldone:
  eventer_remove_fde(e);
  mtev_lua_deregister_event(ci, e, 0);
  *(cl->eptr) = eventer_alloc_copy(e);
  eventer_set_mask(*cl->eptr, 0);
  mtev_lua_register_event(ci, *cl->eptr);
  mtev_lua_lmc_resume(ci->lmc, ci, args);
  return 0;
}
static int
mtev_lua_socket_send(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  eventer_t e, *eptr;
  const void *bytes;
  size_t nbytes;
  ssize_t sbytes;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
  if(lua_gettop(L) != 2)
    luaL_error(L, "mtev.socket.send with bad arguments");
  bytes = lua_tolstring(L, 2, &nbytes);

  while((sbytes = send(eventer_get_fd(e), bytes, nbytes, 0)) == -1 && errno == EINTR);
  if(sbytes < 0 && errno == EAGAIN) {
    struct nl_slcl *cl;
    /* continuation */
    cl = eventer_get_closure(e);
    cl->write_sofar = 0;
    cl->outbuff = bytes;
    cl->write_goal = nbytes;
    cl->sendto = 0;
    eventer_set_callback(e, mtev_lua_socket_send_complete);
    eventer_set_mask(e, EVENTER_WRITE | EVENTER_EXCEPTION);
    eventer_add(e);
    *eptr = NULL;
    return mtev_lua_yield(ci, 0);
  }
  lua_pushinteger(L, sbytes);
  if(sbytes < 0) {
    lua_pushstring(L, strerror(errno));
    return 2;
  }
  return 1;
}

static int
mtev_lua_socket_sendto(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  eventer_t e, *eptr;
  const char *target;
  unsigned short port;
  int8_t family;
  int rv;
  const void *bytes;
  size_t nbytes;
  ssize_t sbytes;
  union {
    struct sockaddr_in sin4;
    struct sockaddr_in6 sin6;
  } a;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
  if(lua_gettop(L) != 4)
    luaL_error(L, "mtev.socket.sendto with bad arguments");
  bytes = lua_tolstring(L, 2, &nbytes);
  target = lua_tostring(L, 3);
  if(!target) target = "";
  port = lua_tointeger(L, 4);

  family = AF_INET;
  rv = inet_pton(family, target, &a.sin4.sin_addr);
  if(rv != 1) {
    family = AF_INET6;
    rv = inet_pton(family, target, &a.sin6.sin6_addr);
    if(rv != 1) {
      memset(&a, 0, sizeof(a));
      lua_pushinteger(L, -1);
      lua_pushfstring(L, "Cannot translate '%s' to IP\n", target);
      return 2;
    }
    else {
      /* We've IPv6 */
      a.sin6.sin6_family = AF_INET6;
      a.sin6.sin6_port = htons(port);
    }
  }
  else {
    a.sin4.sin_family = family;
    a.sin4.sin_port = htons(port);
  }

  while((sbytes = sendto(eventer_get_fd(e), bytes, nbytes,
                         0, (struct sockaddr *)&a,
                         family==AF_INET ? sizeof(a.sin4)
                                         : sizeof(a.sin6))) == -1 &&
        errno == EINTR);
  if(sbytes < 0 && errno == EAGAIN) {
    struct nl_slcl *cl;
    /* continuation */
    cl = eventer_get_closure(e);
    cl->write_sofar = 0;
    cl->outbuff = bytes;
    cl->write_goal = nbytes;
    cl->sendto = 1;
    eventer_set_callback(e, mtev_lua_socket_send_complete);
    eventer_set_mask(e, EVENTER_WRITE | EVENTER_EXCEPTION);
    eventer_add(e);
    *eptr = NULL;
    return mtev_lua_yield(ci, 0);
  }
  lua_pushinteger(L, sbytes);
  if(sbytes < 0) {
    lua_pushstring(L, strerror(errno));
    return 2;
  }
  return 1;
}
static int
mtev_lua_socket_bind(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  eventer_t e, *eptr;
  const char *target;
  unsigned short port;
  int8_t family;
  int rv;
  union {
    struct sockaddr_in sin4;
    struct sockaddr_in6 sin6;
  } a;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
  target = lua_tostring(L, 2);
  if(!target) target = "";
  port = lua_tointeger(L, 3);
  memset(&a, 0, sizeof(a));

  family = AF_INET;
  rv = inet_pton(family, target, &a.sin4.sin_addr);
  if(rv != 1) {
    family = AF_INET6;
    rv = inet_pton(family, target, &a.sin6.sin6_addr);
    if(rv != 1) {
      lua_pushinteger(L, -1);
      lua_pushfstring(L, "Cannot translate '%s' to IP\n", target);
      return 2;
    }
    else {
      /* We've IPv6 */
      a.sin6.sin6_family = AF_INET6;
      a.sin6.sin6_port = htons(port);
    }
  }
  else {
    a.sin4.sin_family = family;
    a.sin4.sin_port = htons(port);
    a.sin4.sin_addr.s_addr = INADDR_ANY;
    memset (a.sin4.sin_zero, 0, sizeof (a.sin4.sin_zero));
  }

  rv = bind(eventer_get_fd(e), (struct sockaddr *)&a,
            family==AF_INET ? sizeof(a.sin4) : sizeof(a.sin6));
  if(rv == 0) {
    lua_pushinteger(L, 0);
    return 1;
  }
  lua_pushinteger(L, -1);
  lua_pushstring(L, strerror(errno));
  return 2;
}

static eventer_t *
mtev_lua_event(lua_State *L, eventer_t e) {
  eventer_t *addr;
  addr = (eventer_t *)lua_newuserdata(L, sizeof(e));
  *addr = e;
  luaL_getmetatable(L, "mtev.eventer");
  lua_setmetatable(L, -2);
  return addr;
}

static int
mtev_lua_socket_accept_complete(eventer_t e, int mask, void *vcl,
                                struct timeval *now) {
  (void)mask;
  (void)now;
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl = vcl, *newcl;
  eventer_t newe;
  int fd, nargs = 0, newmask;
  union {
    struct sockaddr in;
    struct sockaddr_in in4;
    struct sockaddr_in6 in6;
  } addr;
  socklen_t inlen, optlen;

  ci = mtev_lua_find_resume_info(cl->L, mtev_false);
  if(!ci) { mtev_lua_eventer_cl_cleanup(e); return 0; }

  inlen = sizeof(addr.in);
  fd = eventer_accept(e, &addr.in, &inlen, &newmask);
  if(fd <= 0 && errno == EAGAIN) return newmask | EVENTER_EXCEPTION;
  if(fd < 0) {
    lua_pushnil(cl->L);
    goto alldone;
  }

  if(eventer_set_fd_nonblocking(fd)) {
    close(fd);
    lua_pushnil(cl->L);
    goto alldone;
  }

  newcl = calloc(1, sizeof(*cl));
  newcl->free = nl_extended_free;
  newcl->L = cl->L;

  optlen = sizeof(newcl->send_size);
  if(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &newcl->send_size, &optlen) != 0)
    newcl->send_size = 4096;

  newe = eventer_alloc_fd(NULL, newcl, fd, 0);
  newcl->eptr = mtev_lua_event(cl->L, newe);

  mtev_lua_register_event(ci, newe);
  nargs = 1;

 alldone:
  eventer_remove_fde(e);
  mtev_lua_deregister_event(ci, e, 0);
  *(cl->eptr) = eventer_alloc_copy(e);
  eventer_set_mask(*cl->eptr, 0);
  mtev_lua_register_event(ci, *cl->eptr);
  mtev_lua_lmc_resume(ci->lmc, ci, nargs);
  return 0;
}

static int
mtev_lua_socket_listen(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  eventer_t e, *eptr;
  int rv;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");

  if((rv = listen(eventer_get_fd(e), lua_tointeger(L, 2))) < 0) {
    lua_pushinteger(L, rv);
    lua_pushinteger(L, errno);
    lua_pushstring(L, strerror(errno));
    return 3;
  }
  lua_pushinteger(L, rv);
  return 1;
}

static int
mtev_lua_socket_own(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl;
  eventer_t e, *eptr;

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  *eptr = NULL;
  cl = eventer_get_closure(e);
  if(cl->L == L) return 0;

  ci = mtev_lua_find_resume_info(cl->L, mtev_false);
  if(ci) mtev_lua_deregister_event(ci, e, 0);
  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);
  cl->L = L;
  cl->eptr = mtev_lua_event(L, e);
  mtev_lua_register_event(ci, e);
  return 1;
}

static int
mtev_lua_socket_accept(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl;
  eventer_t e, *eptr;
  socklen_t optlen;
  union {
    struct sockaddr in;
    struct sockaddr_in in4;
    struct sockaddr_in6 in6;
  } addr;
  socklen_t inlen;
  int fd, newmask;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  mtevL(nldeb, "accept starting\n");
  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
  cl = eventer_get_closure(e);

  if(cl->L != L) {
    mtevL(nlerr, "cross-coroutine socket call: use event:own()\n");
    luaL_error(L, "cross-coroutine socket call: use event:own()");
  }

  inlen = sizeof(addr.in);
  fd = eventer_accept(e, &addr.in, &inlen, &newmask);
  if(fd < 0) {
    if(errno == EAGAIN) {
      /* Need completion */
      eventer_set_callback(e, mtev_lua_socket_accept_complete);
      eventer_set_mask(e, newmask | EVENTER_EXCEPTION);
      eventer_add(e);
      *eptr = NULL;
      mtevL(nldeb, "accept rescheduled\n");
      return mtev_lua_yield(ci, 0);
    }
    mtevL(nldeb, "accept error: %s\n", strerror(errno));
    lua_pushnil(L);
    return 1;
  }
  if(eventer_set_fd_nonblocking(fd)) {
    close(fd);
    lua_pushnil(L);
    return 1;
  }

  cl = calloc(1, sizeof(*cl));
  cl->free = nl_extended_free;
  cl->L = L;

  optlen = sizeof(cl->send_size);
  if(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cl->send_size, &optlen) != 0)
    cl->send_size = 4096;

  e = eventer_alloc_fd(NULL, cl, fd, 0);
  cl->eptr = mtev_lua_event(L, e);

  mtev_lua_register_event(ci, e);
  return 1;
}
static int
mtev_lua_socket_setsockopt(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  eventer_t e, *eptr;
  const char *type;
  int type_val;
  int value;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  if(lua_gettop(L) != 3) {
    lua_pushinteger(L, -1);
    lua_pushfstring(L, "setsockopt(type, value) wrong arguments");
    return 2;
  }
  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
  type = lua_tostring(L, 2);
  value = lua_tointeger(L, 3);

  if (strcmp(type, "SO_BROADCAST") == 0)
    type_val = SO_BROADCAST;
  else if (strcmp(type, "SO_REUSEADDR") == 0)
    type_val = SO_REUSEADDR;
#ifdef SO_REUSEPORT
  else if (strcmp(type, "SO_REUSEPORT") == 0)
    type_val = SO_REUSEPORT;
#endif
  else if (strcmp(type, "SO_KEEPALIVE") == 0)
    type_val = SO_KEEPALIVE;
  else if (strcmp(type, "SO_LINGER") == 0)
    type_val = SO_LINGER;
  else if (strcmp(type, "SO_OOBINLINE") == 0)
    type_val = SO_OOBINLINE;
  else if (strcmp(type, "SO_SNDBUF") == 0)
    type_val = SO_SNDBUF;
  else if (strcmp(type, "SO_RCVBUF") == 0)
    type_val = SO_RCVBUF;
  else if (strcmp(type, "SO_DONTROUTE") == 0)
    type_val = SO_DONTROUTE;
  else if (strcmp(type, "SO_RCVLOWAT") == 0)
    type_val = SO_RCVLOWAT;
  else if (strcmp(type, "SO_RCVTIMEO") == 0)
    type_val = SO_RCVTIMEO;
  else if (strcmp(type, "SO_SNDLOWAT") == 0)
    type_val = SO_SNDLOWAT;
  else if (strcmp(type, "SO_SNDTIMEO") == 0)
    type_val = SO_SNDTIMEO;
  else {
    lua_pushinteger(L, -1);
    lua_pushfstring(L, "Socket  operation '%s' not supported\n", type);
    return 2;
  }

  if (setsockopt(eventer_get_fd(e), SOL_SOCKET, type_val,
                 (char*)&value, sizeof(value)) < 0) {
    lua_pushinteger(L, -1);
    lua_pushfstring(L, strerror(errno));
    return 2;
  }
  lua_pushinteger(L, 0);
  return 1;
}
static int
mtev_lua_socket_gen_name(lua_State *L,
                         int (*namefunc)(int, struct sockaddr *, socklen_t *)) {
  mtev_lua_resume_info_t *ci;
  eventer_t e, *eptr;
  char ip[INET6_ADDRSTRLEN];
  union {
    struct sockaddr a;
    struct sockaddr_in ip4;
    struct sockaddr_in6 ip6;
  } addr;
  socklen_t addrlen;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
 
  if(namefunc(eventer_get_fd(e), &addr.a, &addrlen) == 0) {
    switch(addr.a.sa_family) {
      case AF_INET:
        if(inet_ntop(AF_INET, &addr.ip4.sin_addr, ip, sizeof(ip))) {
          lua_pushstring(L, ip);
          lua_pushinteger(L, ntohs(addr.ip4.sin_port));
          return 2;
        }
        break;
      case AF_INET6:
        if(inet_ntop(AF_INET6, &addr.ip6.sin6_addr, ip, sizeof(ip))) {
          lua_pushstring(L, ip);
          lua_pushinteger(L, ntohs(addr.ip6.sin6_port));
          return 2;
        }
        break;
      default: break;
    }
  }
  return 0;
}
static int
mtev_lua_socket_peer_name(lua_State *L) {
  return mtev_lua_socket_gen_name(L, getpeername);
}
static int
mtev_lua_socket_sock_name(lua_State *L) {
  return mtev_lua_socket_gen_name(L, getsockname);
}

static int
socket_connect_timeout(eventer_t e, int mask, void *closure, struct timeval *now) {
  (void)mask;
  (void)now;
  mtev_lua_resume_info_t *ci;
  lua_timeout_callback_ref* cb_ref;
  lua_State *L;
  struct nl_slcl *cl;

  cb_ref = (lua_timeout_callback_ref*) closure;
  L = cb_ref->L;
  ci = mtev_lua_find_resume_info(L, mtev_true);
  assert(ci);

  // Remove the original event from the event loop, and close the socket.
  eventer_remove_fde(cb_ref->timed_out_eventer);
  mtev_lua_deregister_event(ci, cb_ref->timed_out_eventer, 0);

  // When we return to the event loop `timed_out_eventer` will be freeed.
  // Make a copy so that the lua socket object has something to work with:
  cl = eventer_get_closure(cb_ref->timed_out_eventer);
  *(cl->eptr) = eventer_alloc_copy(cb_ref->timed_out_eventer);
  mtev_lua_register_event(ci, *cl->eptr);

  // The socket will be closed when either
  // (a) e:close() is called explicitly on the lua object (recommended),
  // (b) the lua object is GCed.
  // No need to explicitly close it here.

  // return into the original Lua call which spawned this timeout
  lua_pushinteger(L, -1);
  lua_pushstring(L, "timeout");
  mtev_lua_lmc_resume(ci->lmc, ci, 2);

  mtev_lua_deregister_event(ci, e, 1);
  return 0;
}


static int
mtev_lua_socket_connect(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl;
  eventer_t e, *eptr;
  const char *target;
  unsigned short port;
  int8_t family;
  int rv;
  union {
    struct sockaddr_in sin4;
    struct sockaddr_in6 sin6;
  } a;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
  cl = eventer_get_closure(e);
  if(cl->L != L) {
    mtevL(nlerr, "cross-coroutine socket call: use event:own()\n");
    luaL_error(L, "cross-coroutine socket call: use event:own()");
  }
  target = lua_tostring(L, 2);

  if(target && !strncmp(target, "reverse:", 8)) {
    int fd = mtev_reverse_socket_connect(target+8, -1);
    mtevL(nldeb, "e:connect %s -> %d\n", target, fd);
    if(fd < 0) {
      lua_pushinteger(L, -1);
      lua_pushfstring(L, "Reverse connection unavailable");
      return 2;
    }
    if(dup2(fd, eventer_get_fd(e)) < 0) {
      close(fd);
      lua_pushinteger(L, -1);
      lua_pushfstring(L, "Reverse connection dup2 failed");
      return 2;
    }
    close(fd);
    lua_pushinteger(L, 0);
    return 1;
  }

  if(!target) target = "";
  port = lua_tointeger(L, 3);

  family = AF_INET;
  rv = inet_pton(family, target, &a.sin4.sin_addr);
  if(rv != 1) {
    family = AF_INET6;
    rv = inet_pton(family, target, &a.sin6.sin6_addr);
    if(rv != 1) {
      memset(&a, 0, sizeof(a));
      lua_pushinteger(L, -1);
      lua_pushfstring(L, "Cannot translate '%s' to IP\n", target);
      return 2;
    }
    else {
      /* We've IPv6 */
      a.sin6.sin6_family = AF_INET6;
      a.sin6.sin6_port = htons(port);
    }
  }
  else {
    a.sin4.sin_family = family;
    a.sin4.sin_port = htons(port);
  }

  rv = connect(eventer_get_fd(e), (struct sockaddr *)&a,
               family==AF_INET ? sizeof(a.sin4) : sizeof(a.sin6));
  mtevL(nldeb, "e:connect %s -> %d\n", target, rv);
  if(rv == 0) {
    lua_pushinteger(L, 0);
    return 1;
  }
  if(rv == -1 && errno == EINPROGRESS) {
    /* Need completion */
    /* setup timeout handler */
    if (lua_gettop(L) >= 4 && lua_isnumber(L, 4)) {
      double timeout_user = lua_tonumber(L, 4);
      int timeout_s = floor(timeout_user);
      int timeout_us = (timeout_user - timeout_s) * 1000000;
      lua_timeout_callback_ref* cb_ref = calloc(1, sizeof(lua_timeout_callback_ref));
      cb_ref->free = free;
      cb_ref->L = L;
      cb_ref->timed_out_eventer = e;
      eventer_t timeout_eventer = eventer_in_s_us(socket_connect_timeout, cb_ref, timeout_s, timeout_us);
      mtev_lua_register_event(ci, timeout_eventer);
      cl->timeout_event = timeout_eventer;
      eventer_add_timed(timeout_eventer);
    }
    /* setup completion handler */
    eventer_set_callback(e, mtev_lua_socket_connect_complete);
    eventer_set_mask(e, EVENTER_READ | EVENTER_WRITE | EVENTER_EXCEPTION);
    eventer_add(e);
    *eptr = NULL;
    return mtev_lua_yield(ci, 0);
  }
  lua_pushinteger(L, -1);
  lua_pushstring(L, strerror(errno));
  return 2;
}
static int
mtev_lua_ssl_upgrade(eventer_t e, int mask, void *vcl,
                     struct timeval *now) {
  (void)now;
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl = vcl;
  int rv, nargs;

  mtevL(nldeb, "ssl_upgrade attempt\n");
  rv = eventer_SSL_connect(e, &mask);
  if(rv <= 0 && errno == EAGAIN) return mask | EVENTER_EXCEPTION;

  ci = mtev_lua_find_resume_info(cl->L, mtev_false);
  if(!ci) { mtev_lua_eventer_cl_cleanup(e); return 0; }

  /* Upgrade completed (successfully???) */
  nargs = 1;
  lua_pushinteger(cl->L, (rv > 0) ? 0 : -1);
  if(rv <= 0) {
    eventer_ssl_ctx_t *ctx;
    const char *err = NULL;
    ctx = eventer_get_eventer_ssl_ctx(e);
    if(ctx) err = eventer_ssl_get_last_error(ctx);
    lua_pushinteger(cl->L, -1);
    if(err) {
      lua_pushlstring(cl->L, err, strlen(err));
      nargs++;
    }
    mtevL(nldeb, "ssl_upgrade failed: %s\n", err ? err : "unknown");
  }
  else {
    mtevL(nldeb, "ssl_upgrade completed\n");
  }

  eventer_remove_fde(e);
  mtev_lua_deregister_event(ci, e, 0);
  *(cl->eptr) = eventer_alloc_copy(e);
  eventer_set_mask(*cl->eptr, 0);
  mtev_lua_register_event(ci, *cl->eptr);

  mtev_lua_lmc_resume(ci->lmc, ci, nargs);
  return 0;
}
static int
mtev_lua_socket_connect_ssl(lua_State *L) {
  const char *snihost = NULL, *layer, *err;
  eventer_ssl_ctx_t *sslctx = NULL;
  mtev_lua_resume_info_t *ci;
  eventer_t e, *eptr;
  int tmpmask, rv, nargs = 1;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
  if(lua_istable(L, 2)) {
    mtev_hash_table *sslconfig = mtev_lua_table_to_hash(L, 2);
    if(sslconfig) {
      snihost = lua_tostring(L, 3);
      if(!snihost) snihost = mtev_hash_dict_get(sslconfig, "snihost");
      layer = lua_tostring(L, 4);
      if(layer) mtev_hash_replace(sslconfig, "layer", strlen("layer"), layer, NULL, NULL);
      sslctx = eventer_ssl_ctx_new_ex(SSL_CLIENT, sslconfig);
      free(sslconfig);
    }
  } else {
    /* This is the legacy call, we leave it here as to be silently
     * backward compatible.
     */
    const char *ca, *ciphers, *cert, *key;
    cert = lua_tostring(L, 2);
    key = lua_tostring(L, 3);
    ca = lua_tostring(L, 4);
    ciphers = lua_tostring(L, 5);
    snihost = lua_tostring(L, 6);
    layer = lua_tostring(L, 7);

    sslctx = eventer_ssl_ctx_new(SSL_CLIENT, layer, cert, key, ca, ciphers);
  }
  if(!sslctx) {
    lua_pushinteger(L, -1);
    lua_pushstring(L, "ssl_client context creation failed");
    mtevL(nldeb, "ssl_client context creation failed\n");
    return 2;
  }

  if (snihost != NULL && strlen(snihost) >= 1) {
    eventer_ssl_ctx_set_sni(sslctx, snihost);
  }

  eventer_ssl_ctx_set_verify(sslctx, eventer_ssl_verify_cert, NULL);
  EVENTER_ATTACH_SSL(e, sslctx);

  /* We need do the ssl connect and register a completion if
   * it comes back with an EAGAIN.
   */
  tmpmask = EVENTER_READ|EVENTER_WRITE;
  mtevL(nldeb, "ssl_connect attempt\n");
  rv = eventer_SSL_connect(e, &tmpmask);
  if(rv <= 0 && errno == EAGAIN) {
    /* Need completion */
    eventer_remove_fde(e);
    eventer_set_mask(e, tmpmask | EVENTER_EXCEPTION);
    eventer_set_callback(e, mtev_lua_ssl_upgrade);
    eventer_add(e);
    *eptr = NULL;
    return mtev_lua_yield(ci, 0);
  }
  lua_pushinteger(L, (rv > 0) ? 0 : -1);
  if(rv <= 0) {
    err = eventer_ssl_get_last_error(sslctx);
    if(err) {
      lua_pushstring(L, err);
      nargs++;
    }
    mtevL(nldeb, "ssl_connect failed: %s\n", err ? err : "unknown");
  }
  else {
    mtevL(nldeb, "ssl_connect completed\n");
  }
  return nargs;
}

static int
mtev_lua_socket_do_read(eventer_t e, int *mask, struct nl_slcl *cl,
                        int *read_complete) {
  char buff[4096];
  int len;
  *read_complete = 0;
  while((len = eventer_read(e, buff, sizeof(buff), mask)) > 0) {
    if(cl->read_goal) {
      int remaining = cl->read_goal - cl->read_sofar;
      /* copy up to the goal into the inbuff */
      inbuff_addlstring(cl, buff, MIN(len, remaining));
      cl->read_sofar += len;
      if(cl->read_sofar >= cl->read_goal) { /* We're done */
        lua_pushlstring(cl->L, cl->inbuff, cl->read_goal);
        *read_complete = 1;
        cl->read_sofar -= cl->read_goal;
        cl->inbuff_len = 0;
        if(cl->read_sofar > 0) {  /* We have to buffer this for next read */
          inbuff_addlstring(cl, buff + remaining, cl->read_sofar);
        }
        break;
      }
    }
    else if(cl->read_terminator) {
      const char *cp;
      int remaining = len;
      cp = mtev_memmem(buff, len, cl->read_terminator, cl->read_terminator_len);
      if(cp) remaining = cp - buff + cl->read_terminator_len;
      inbuff_addlstring(cl, buff, MIN(len, remaining));
      cl->read_sofar += len;
      if(cp) {
        lua_pushlstring(cl->L, cl->inbuff, cl->inbuff_len);
        *read_complete = 1;

        cl->read_sofar = len - remaining;
        cl->inbuff_len = 0;
        if(cl->read_sofar > 0) { /* We have to buffer this for next read */
          inbuff_addlstring(cl, buff + remaining, cl->read_sofar);
        }
        break;
      }
    }
  }
  if((((len < 0) && (errno != EAGAIN)) || (len == 0)) && cl->inbuff_len) {
    /* EOF */
    *read_complete = 1;
    lua_pushlstring(cl->L, cl->inbuff, cl->inbuff_len);
    cl->inbuff_len = 0;
    return 0;
  }
  return len;
}
static int
mtev_lua_socket_read_complete(eventer_t e, int mask, void *vcl,
                              struct timeval *now) {
  (void)now;
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl = vcl;
  int len;
  int args = 0;

  ci = mtev_lua_find_resume_info(cl->L, mtev_false);
  if(!ci) { mtev_lua_eventer_cl_cleanup(e); return 0; }

  if(cl->timeout_event) {
    eventer_remove_timed(cl->timeout_event);
    mtev_lua_deregister_event(ci, cl->timeout_event, 1);
    cl->timeout_event = NULL;
  }

  len = mtev_lua_socket_do_read(e, &mask, cl, &args);
  if(len >= 0) {
    /* We broke out, cause we read enough... */
  }
  else if(len == -1 && errno == EAGAIN) {
    return mask | EVENTER_EXCEPTION;
  }
  else {
    lua_pushnil(cl->L);
    args = 1;
  }
  eventer_remove_fde(e);
  mtev_lua_deregister_event(ci, e, 0);
  *(cl->eptr) = eventer_alloc_copy(e);
  mtev_lua_register_event(ci, *cl->eptr);
  mtev_lua_lmc_resume(ci->lmc, ci, args);
  return 0;
}

static int
socket_read_timeout(eventer_t e, int mask, void *closure, struct timeval *now) {
  (void)mask;
  (void)now;
  struct nl_slcl *cl;
  mtev_lua_resume_info_t *ci;
  lua_timeout_callback_ref* cb_ref;
  lua_State *L;

  cb_ref = (lua_timeout_callback_ref*)closure;
  L = cb_ref->L;

  // run the timeout callback
  lua_rawgeti( L, LUA_REGISTRYINDEX, cb_ref->callback_reference );
  lua_call(L, 0, 0);

  cl = eventer_get_closure(cb_ref->timed_out_eventer);
  ci = mtev_lua_find_resume_info(L, mtev_true);
  assert(ci);

  // remove the original read event
  eventer_remove_fde(cb_ref->timed_out_eventer);
  mtev_lua_deregister_event(ci, cb_ref->timed_out_eventer, 0);
  *(cl->eptr) = eventer_alloc_copy(cb_ref->timed_out_eventer);
  mtev_lua_register_event(ci, *cl->eptr);

  // return into the original Lua call which spawned this timeout
  lua_pushnil(L);
  mtev_lua_lmc_resume(ci->lmc, ci, 1);

  mtev_lua_deregister_event(ci, e, 1);

  return 0;
}

static int
mtev_lua_socket_read(lua_State *L) {
  int args, mask, len;
  struct nl_slcl *cl;
  mtev_lua_resume_info_t *ci;
  eventer_t e, *eptr;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
  cl = eventer_get_closure(e);
  cl->read_goal = 0;
  cl->read_terminator = NULL;
  cl->read_terminator_len = 0;

  if(cl->L != L) {
    mtevL(nlerr, "cross-coroutine socket call: use event:own()\n");
    luaL_error(L, "cross-coroutine socket call: use event:own()");
  }

  if(lua_isnumber(L, 2)) {
    cl->read_goal = lua_tointeger(L, 2);
    if(cl->read_goal <= cl->read_sofar) {
     i_know_better:
      /* We have enough, we can service this right here */
      lua_pushlstring(L, cl->inbuff, cl->read_goal);
      cl->read_sofar -= cl->read_goal;
      if(cl->read_sofar) {
        memmove(cl->inbuff, cl->inbuff + cl->read_goal, cl->read_sofar);
      }
      cl->inbuff_len = cl->read_sofar;
      return 1;
    }
  }
  else {
    cl->read_terminator = lua_tolstring(L, 2, &cl->read_terminator_len);
    if(cl->read_sofar) {
      const char *cp;
      /* Ugh... inernalism */
      cp = mtev_memmem(cl->inbuff, cl->read_sofar,
                       cl->read_terminator, cl->read_terminator_len);
      if(cp) {
        /* Here we matched... and we _know_ that someone actually wants:
         * cl->read_terminator_len + cp - cl->inbuff.buffer bytes...
         * give it to them.
         */
        cl->read_goal = cl->read_terminator_len + cp - cl->inbuff;
        cl->read_terminator = NULL;
        cl->read_terminator_len = 0;
        mtevAssert(cl->read_goal <= cl->read_sofar);
        goto i_know_better;
      }
    }
  }

  len = mtev_lua_socket_do_read(e, &mask, cl, &args);
  if(args == 1) return 1; /* completed read, return result */
  if(len == -1 && errno == EAGAIN) {
    /* we need to drop into eventer */
    eventer_remove_fde(e);
    eventer_set_callback(e, mtev_lua_socket_read_complete);
    eventer_set_mask(e, mask | EVENTER_EXCEPTION);
    eventer_add(e);
    *eptr = NULL;

    if (lua_gettop(L) == 5 && lua_isfunction(L, 5)) {
      double timeout_user;
      int timeout_s;
      int timeout_us;
      timeout_s = 10;
      timeout_us = 0;
      if(lua_isnumber(L, 4)) {
        timeout_user = lua_tonumber(L, 4);
        timeout_s = floor(timeout_user);
        timeout_us = (timeout_user - timeout_s) * 1000000;
      }

      lua_timeout_callback_ref* cb_ref = malloc(sizeof(lua_timeout_callback_ref));
      cb_ref->free = lua_timeout_callback_ref_free;
      cb_ref->L = L;
      cb_ref->callback_reference = luaL_ref( L, LUA_REGISTRYINDEX );
      cb_ref->timed_out_eventer = e;

      eventer_t timeout_eventer =
        eventer_in_s_us(socket_read_timeout, cb_ref, timeout_s, timeout_us);
      mtev_lua_register_event(ci, timeout_eventer);
      cl->timeout_event = timeout_eventer;
      eventer_add_timed(timeout_eventer);
    }

    return mtev_lua_yield(ci, 0);
  }
  else {
    lua_pushnil(cl->L);
    args = 1;
    return args;
  }
}
static int
mtev_lua_socket_write_complete(eventer_t e, int mask, void *vcl,
                               struct timeval *now) {
  (void)now;
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl = vcl;
  int rv;
  int args = 0;

  ci = mtev_lua_find_resume_info(cl->L, mtev_false);
  if(!ci) { mtev_lua_eventer_cl_cleanup(e); return 0; }

  if(mask & EVENTER_EXCEPTION) {
    lua_pushinteger(cl->L, -1);
    args = 1;
    goto alldone;
  }
  while((rv = eventer_write(e,
                            cl->outbuff + cl->write_sofar,
                            MIN((size_t)cl->send_size,
                                (cl->write_goal - cl->write_sofar)),
                            &mask)) > 0) {
    cl->write_sofar += rv;
    mtevAssert(cl->write_sofar <= cl->write_goal);
    if(cl->write_sofar == cl->write_goal) break;
  }
  if(rv > 0) {
    lua_pushinteger(cl->L, cl->write_goal);
    args = 1;
  }
  else if(rv == -1 && errno == EAGAIN) {
    return mask | EVENTER_EXCEPTION;
  }
  else {
    lua_pushinteger(cl->L, -1);
    args = 1;
    if(rv == -1) {
      lua_pushstring(cl->L, strerror(errno));
      args++;
    }
  }

 alldone:
  eventer_remove_fde(e);
  mtev_lua_deregister_event(ci, e, 0);
  *(cl->eptr) = eventer_alloc_copy(e);
  eventer_set_mask(*cl->eptr, 0);
  mtev_lua_register_event(ci, *cl->eptr);
  mtev_lua_lmc_resume(ci->lmc, ci, args);
  return 0;
}
static int
mtev_lua_socket_write(lua_State *L) {
  int rv, mask;
  struct nl_slcl *cl;
  mtev_lua_resume_info_t *ci;
  eventer_t e, *eptr;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");
  cl = eventer_get_closure(e);
  cl->write_sofar = 0;
  cl->outbuff = lua_tolstring(L, 2, &cl->write_goal);

  if(cl->L != L) {
    mtevL(nlerr, "cross-coroutine socket call: use event:own()\n");
    luaL_error(L, "cross-coroutine socket call: use event:own()");
  }

  while((rv = eventer_write(e,
                            cl->outbuff + cl->write_sofar,
                            MIN((size_t)cl->send_size,
                                (cl->write_goal - cl->write_sofar)),
                            &mask)) > 0) {
    cl->write_sofar += rv;
    mtevAssert(cl->write_sofar <= cl->write_goal);
    if(cl->write_sofar == cl->write_goal) break;
  }
  if(rv > 0) {
    lua_pushinteger(L, cl->write_goal);
    return 1;
  }
  if(rv == -1 && errno == EAGAIN) {
    eventer_remove_fde(e);
    eventer_set_callback(e, mtev_lua_socket_write_complete);
    eventer_set_mask(e, mask | EVENTER_EXCEPTION);
    eventer_add(e);
    *eptr = NULL;
    return mtev_lua_yield(ci, 0);
  }
  lua_pushinteger(L, -1);
  return 1;
}
static int
mtev_lua_socket_ssl_ctx(lua_State *L) {
  eventer_t *eptr, e;
  eventer_ssl_ctx_t **ssl_ctx_holder, *ssl_ctx;

  eptr = lua_touserdata(L, lua_upvalueindex(1));
  if(eptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  e = *eptr;
  if(e == NULL) luaL_error(L, "invalid event");

  ssl_ctx = eventer_get_eventer_ssl_ctx(e);
  if(!ssl_ctx) {
    lua_pushnil(L);
    return 1;
  }

  ssl_ctx_holder = (eventer_ssl_ctx_t **)lua_newuserdata(L, sizeof(ssl_ctx));
  *ssl_ctx_holder = ssl_ctx;
  luaL_getmetatable(L, "mtev.eventer.ssl_ctx");
  lua_setmetatable(L, -2);
  return 1;
}
/*! \lua a,... = mtev.timezone:extract(time, field1, ...)
\param time is the offset in seconds from UNIX epoch.
\param field1 is a field to extract in the time local to the timezone object.
\return The value of each each requested field.

Valid fields are "second", "minute", "hour", "monthday", "month", "weekday",
"yearday", "year", "dst", "offset", and "zonename."
*/
static int
mtev_lua_timezone_extract(lua_State *L) {
  struct tm *res, stack_buf;
  tzinfo_t **zi = lua_touserdata(L, lua_upvalueindex(1));
  const tzzone_t *tz = NULL;
  const char *field = NULL;
  int n = lua_gettop(L);
  if(n < 3) luaL_error(L, "mtev.timezone:extract(time, ...)");
  if(lua_touserdata(L,1) != zi) luaL_error(L, "must be called as method");
  time_t seconds = luaL_checkinteger(L,2);
  res = libtz_zonetime(*zi, &seconds, &stack_buf, &tz);
  if(!res) luaL_error(L, "Failed timezone conversion");
  int i, nresults = 0;
  for(i=3;i<=n;i++) {
    field = luaL_checkstring(L,i);
    if(!strcmp(field, "second")) lua_pushinteger(L, res->tm_sec);
    else if(!strcmp(field, "minute")) lua_pushinteger(L, res->tm_min);
    else if(!strcmp(field, "hour")) lua_pushinteger(L, res->tm_hour);
    else if(!strcmp(field, "monthday")) lua_pushinteger(L, res->tm_mday);
    else if(!strcmp(field, "weekday")) lua_pushinteger(L, res->tm_wday+1);
    else if(!strcmp(field, "yearday")) lua_pushinteger(L, res->tm_yday+1);
    else if(!strcmp(field, "month")) lua_pushinteger(L, res->tm_mon+1);
    else if(!strcmp(field, "year")) lua_pushinteger(L, res->tm_year+1900);
    else if(!strcmp(field, "dst")) lua_pushboolean(L, res->tm_isdst);
    else if(!strcmp(field, "offset")) lua_pushnumber(L, libtz_tzzone_offset(tz));
    else if(!strcmp(field, "zonename")) lua_pushstring(L, libtz_tzzone_name(tz));
    else luaL_error(L, "unknown extraction '%s'", field);
    nresults++;
  }
  return nresults;
}
static int
mtev_timezone_index_func(lua_State *L) {
  const char *k;
  tzinfo_t **udata;
  int n = lua_gettop(L);
  mtevAssert(n == 2);
  udata = (tzinfo_t**) luaL_testudata(L, 1, "mtev.timezone");
  if(udata == NULL) {
    luaL_error(L, "metatable error, arg1 not a mtev.timezone!");
  }
  if(!lua_isstring(L, 2)) {
    luaL_error(L, "metatable error, arg2 not a string!");
  }
  k = lua_tostring(L, 2);
  switch(*k) {
    case 'e':
      LUA_DISPATCH(extract, mtev_lua_timezone_extract);
      break;
    default:
      break;
  }
  luaL_error(L, "mtev.eventer no such element: %s", k);
  return 0;
}
static int
mtev_lua_timezone_gc(lua_State *L) {
  tzinfo_t **zip = lua_touserdata(L, 1);
  libtz_free_tzinfo(*zip);
  return 0;
}
/*! \lua mtev.timezone = mtev.timezone(zonename)
 * \param zonename is the name of the timezone (e.g. "UTC" or "US/Eastern")
 * \return an mtev.timezone object.
 */
static int
nl_timezone(lua_State *L) {
  const char *err;
  if(lua_gettop(L) != 1) luaL_error(L, "timezone(<tzname>)");
  const char *zonename = luaL_checkstring(L,1);
  tzinfo_t *zi = libtz_open(zonename, &err);
  if(!zi) luaL_error(L, err);
  tzinfo_t **zip = lua_newuserdata(L, sizeof(tzinfo_t *));
  *zip = zi;
  luaL_getmetatable(L, "mtev.timezone");
  lua_setmetatable(L, -2);
  return 1;
}
static int
mtev_eventer_index_func(lua_State *L) {
  int n;
  const char *k;
  eventer_t *udata;
  n = lua_gettop(L); /* number of arguments */
  mtevAssert(n == 2);
  if(!luaL_checkudata(L, 1, "mtev.eventer")) {
    luaL_error(L, "metatable error, arg1 not a mtev.eventer!");
  }
  udata = lua_touserdata(L, 1);
  if(!lua_isstring(L, 2)) {
    luaL_error(L, "metatable error, arg2 not a string!");
  }
  k = lua_tostring(L, 2);
  switch(*k) {
    case 'a':
/*! \lua mtev.eventer = mtev.eventer:accept()
\brief Accept a new connection.
\return a new eventer object representing the new connection.
*/
     LUA_DISPATCH(accept, mtev_lua_socket_accept);
     break;
    case 'b':
/*! \lua rv, err = mtev.eventer:bind(address, port)
\brief Bind a socket to an address.
\param address the IP address to which to bind.
\param port the port to which to bind.
\return rv is 0 on success, on error rv is non-zero and err contains an error message.
*/
     LUA_DISPATCH(bind, mtev_lua_socket_bind);
     break;
    case 'c':
/*! \lua rv = mtev.eventer:close()
\brief Closes the socket.
*/
     LUA_DISPATCH(close, mtev_lua_socket_close);
/*! \lua rv, err = mtev.eventer:connect(target[, port][, timeout])
\brief Request a connection on a socket.
\param target the target address for a connection.  Either an IP address (in which case a port is required), or a `reverse:` connection for reverse tunnelled connections.
\param timeout for connect operation
\return rv is 0 on success, non-zero on failure with err holding the error message.
*/
     LUA_DISPATCH(connect, mtev_lua_socket_connect);
     break;
    case 'l':
/*! \lua rv, errno, err = mtev.eventer:listen(backlog)
\brief Listen on a socket.
\param backlog the listen backlog.
\return rv is 0 on success, on failure rv is non-zero and errno and err contain error information.
*/
     LUA_DISPATCH(listen, mtev_lua_socket_listen);
     break;
    case 'o':
/*! \lua ev = mtev.eventer:own()
\brief Declare ownership of an event within a spawned co-routine.
\return New eventer object 'ev' that is owed by the calling co-routine

The old eventer object will be disowned and invalid for use!
*/
     LUA_DISPATCH(own, mtev_lua_socket_own);
     break;
    case 'p':
/*! \lua address, port = mtev.eventer:peer_name()
\brief Get details of the remote side of a socket.
\return local address, local port
*/
     LUA_DISPATCH(peer_name, mtev_lua_socket_peer_name);
     break;
    case 'r':
/*! \lua payload = mtev.eventer:read(stop)
\brief Read data from a socket.
\param stop is either an integer describing a number of bytes to read or a string describing an inclusive read terminator.
\return the payload read, or nothing on error.
*/
     LUA_DISPATCH(read, mtev_lua_socket_read);
/*! \lua rv, payload, address, port = mtev.eventer:recv(nbytes)
\brief Receive bytes from a socket.
\param nbytes the number of bytes to receive.
\return rv is the return of the `recvfrom` libc call, < 0 if error, otherwise it represents the number of bytes received. payload is a lua string representing the data received. address and port are those of the sender of the packet.
*/
     LUA_DISPATCH(recv, mtev_lua_socket_recv);
     break;
    case 's':
/*! \lua nbytes, err = mtev.eventer:send(payload)
\brief Send data over a socket.
\param payload the payload to send as a lua string.
\return bytes is -1 on error, otherwise the number of bytes sent. err contains error messages.
*/
     LUA_DISPATCH(send, mtev_lua_socket_send);
/*! \lua nbytes, err = mtev.eventer:sendto(payload, address, port)
\brief Send data over a disconnected socket.
\param payload the payload to send as a lua string.
\param address is the destination address for the payload.
\param port is the destination port for the payload.
\return bytes is -1 on error, otherwise the number of bytes sent. err contains error messages.
*/
     LUA_DISPATCH(sendto, mtev_lua_socket_sendto);
/*! \lua rv, err = mtev.eventer:setsockopt(feature, value)
\brief Set a socket option.
\param feature is on the the OS `SO_` parameters as a string.
\param value is the value to which `feature` should be set.
\return rv is 0 on success, -1 on failure. err contains error messages.
*/
     LUA_DISPATCH(setsockopt, mtev_lua_socket_setsockopt);
/*! \lua rv, err = mtev.eventer:ssl_upgrade_socket(cert, sslconfig[, snihost[, layer]]]])
\brief Upgrade a normal TCP socket to SSL.
\param sslconfig is a table that looks like mtev's sslconfig
\param snihost the host name to which we're connecting (SNI).
\param layer a desired SSL layer.
\return rv is 0 on success, -1 on failure. err contains error messages.
*/
     LUA_DISPATCH(ssl_upgrade_socket, mtev_lua_socket_connect_ssl);
/*! \lua mtev.eventer.ssl_ctx = mtev.eventer:ssl_ctx()
\brief Gets the SSL context associated with an SSL-upgraded event.
\return an mtev.eventer.ssl_ctx object.
*/
     LUA_DISPATCH(ssl_ctx, mtev_lua_socket_ssl_ctx);
/*! \lua address, port = mtev.eventer:sock_name()
\brief Get details of the local side of a socket.
\return local address, local port
*/
     LUA_DISPATCH(sock_name, mtev_lua_socket_sock_name);
     break;
    case 'w':
/*! \lua nbytes = mtev.eventer:write(data)
\brief Writes data to a socket.
\param data a lua string that contains the data to write.
\return the number of bytes written.
*/
     LUA_DISPATCH(write, mtev_lua_socket_write);
     break;
    default:
      break;
  }
  luaL_error(L, "mtev.eventer no such element: %s", k);
  return 0;
}

static int
mtev_ssl_ctx_index_func(lua_State *L) {
  int n;
  const char *k;
  eventer_ssl_ctx_t **udata, *ssl_ctx;
  n = lua_gettop(L); /* number of arguments */
  mtevAssert(n == 2);
  if(!luaL_checkudata(L, 1, "mtev.eventer.ssl_ctx")) {
    luaL_error(L, "metatable error, arg1 not a mtev.eventer.ssl_ctx!");
  }
  udata = lua_touserdata(L, 1);
  ssl_ctx = *udata;
  if(!lua_isstring(L, 2)) {
    luaL_error(L, "metatable error, arg2 not a string!");
  }
  k = lua_tostring(L, 2);
  switch(*k) {
    case 'c':
      if(!strcmp(k,"ciphers")) {
        int i = 0;
        const char *ciphername;
        lua_newtable(L);
        while(NULL != (ciphername = eventer_ssl_get_cipher_list(ssl_ctx,i))) {
          lua_pushnumber(L, ++i);
          lua_pushstring(L, ciphername);
          lua_settable(L,-3);
        }
        return 1;
      }
      LUA_RETSTRING(current_cipher, eventer_ssl_get_current_cipher(ssl_ctx));
      break;
    case 'e':
      LUA_RETSTRING(error, eventer_ssl_get_peer_error(ssl_ctx));
      LUA_RETINTEGER(end_time, eventer_ssl_get_peer_end_time(ssl_ctx));
      break;
    case 'i':
      LUA_RETSTRING(issuer, eventer_ssl_get_peer_issuer(ssl_ctx));
      break;
    case 'm':
      LUA_RETINTEGER(method, eventer_ssl_get_method(ssl_ctx));
      break;
    case 'p':
      if(!strcmp(k,"peer_certificate")) {
        X509 *x509 = eventer_ssl_get_peer_certificate(ssl_ctx);
        return mtev_lua_crypto_newx509(L, x509);
      }
      break;
    case 's':
      LUA_RETSTRING(sni_name, eventer_ssl_get_sni_name(ssl_ctx));
      LUA_RETSTRING(san_list, eventer_ssl_get_peer_san_list(ssl_ctx));
      LUA_RETSTRING(subject, eventer_ssl_get_peer_subject(ssl_ctx));
      LUA_RETINTEGER(start_time, eventer_ssl_get_peer_start_time(ssl_ctx));
      if(!strcmp(k,"ssl_session")) {
        SSL_SESSION *ssl_session = eventer_ssl_get_session(ssl_ctx);
        return mtev_lua_crypto_new_ssl_session(L, ssl_session);
      }
      break;
    default:
      break;
  }
  luaL_error(L, "mtev.eventer.ssl_ctx no such element: %s", k);
  return 0;
}

struct websocket_lua_t {
  mtev_websocket_client_t *client;
  lua_State *L;
  int self_idx;
  int cb_ready, cb_message, cb_cleanup;
};

/*! \lua mtev.websocket_client:send(opcode, payload)
    \param opcode The websocket opcode.
    \param payload The payload.
    \brief Send a message over a websocket client.

    The client object has fields exposing: `CONTINUATION`, `TEXT`,
    `BINARY`, `CONNECTION_CLOSE`, `PING`, and `PONG`.
*/
static int
mtev_lua_ws_send(lua_State *L) {
  struct websocket_lua_t *udata = lua_touserdata(L, lua_upvalueindex(1));
  int n = lua_gettop(L);
  if(n != 3) luaL_error(L, "mtev.websocket_client:send(opcode, buffer)");
  if(lua_touserdata(L,1) != udata) luaL_error(L, "must be called as method");
  int opcode = lua_tointeger(L,2);
  size_t len;
  const char *data = lua_tolstring(L,3,&len);
  lua_pushboolean(L, mtev_websocket_client_send(udata->client, opcode, (void *)data, len));
  return 1;
}
static void
mtev_lua_ws_unref_bits(lua_State *L, struct websocket_lua_t *udata) {
  if(udata->self_idx >= 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, udata->self_idx);
    udata->self_idx = -1;
  }
  if(udata->cb_ready >= 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, udata->cb_ready);
    udata->cb_ready = -1;
  }
  if(udata->cb_message >= 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, udata->cb_message);
    udata->cb_message = -1;
  }
  if(udata->cb_cleanup >= 0) {
    luaL_unref(L, LUA_REGISTRYINDEX, udata->cb_cleanup);
    udata->cb_cleanup = -1;
  }
}
static void
mtev_lua_ws_close_internal(lua_State *L, struct websocket_lua_t *udata) {
  mtev_lua_ws_unref_bits(L, udata);
  if(udata->client) {
    mtev_websocket_client_t *client = udata->client;
    udata->client = NULL;
    mtev_websocket_client_free(client);
  }
}
/*! \lua mtev.websocket_client:close()
    \brief Close a websocket client.
*/
static int
mtev_lua_ws_close(lua_State *L) {
  struct websocket_lua_t *udata = lua_touserdata(L, lua_upvalueindex(1));
  int n = lua_gettop(L);
  if(n != 1) luaL_error(L, "mtev.websocket_client:close()");
  if(lua_touserdata(L,1) != udata) luaL_error(L, "must be called as method");
  return 1;
}
static int
mtev_lua_ws_gc(lua_State *L) {
  struct websocket_lua_t *udata = lua_touserdata(L,1);
  mtev_lua_ws_close_internal(L, udata);
  mtev_lua_ws_unref_bits(L, udata);
  mtev_lua_resume_info_t *ci = mtev_lua_find_resume_info(L, mtev_false);
  if(ci) mtev_lua_cancel_coro(ci);
  return 0;
}
static int
mtev_websocket_client_index_func(lua_State *L) {
  int n;
  const char *k;
  struct websocket_lua_t *udata;
  n = lua_gettop(L); /* number of arguments */
  mtevAssert(n == 2);
  if(!luaL_checkudata(L, 1, "mtev.websocket_client")) {
    luaL_error(L, "metatable error, arg1 not a mtev.websocket_client!");
  }
  udata = lua_touserdata(L, 1);
  if(!lua_isstring(L, 2)) {
    luaL_error(L, "metatable error, arg2 not a string!");
  }
  k = lua_tostring(L, 2);
  switch(*k) {
    /* Values from the websocket RFC */
    case 'B':
      if(!strcmp(k, "BINARY")) { lua_pushinteger(L, 2); return 1; }
      break;
    case 'C':
      if(!strcmp(k, "CONTINUATION")) { lua_pushinteger(L, 0); return 1; }
      if(!strcmp(k, "CONNECTION_CLOSE")) { lua_pushinteger(L, 8); return 1; }
      break;
    case 'P':
      if(!strcmp(k, "PING")) { lua_pushinteger(L, 9); return 1; }
      if(!strcmp(k, "PONG")) { lua_pushinteger(L, 10); return 1; }
      break;
    case 'T':
      if(!strcmp(k, "TEXT")) { lua_pushinteger(L, 1); return 1; }
      break;
    case 'c':
      LUA_DISPATCH(close, mtev_lua_ws_close);
      break;
    case 's':
      LUA_DISPATCH(send, mtev_lua_ws_send);
      break;
  }
  luaL_error(L, "mtev.websocket_client no such element: %s\n", k);
  return 0;
}

static mtev_boolean nl_ws_cb_ready(mtev_websocket_client_t *client,
                                   void *closure) {
  struct websocket_lua_t *udata = closure;
  if(udata->client == NULL) return mtev_false;
  if(udata->L == NULL) return mtev_false;
  mtevAssert(udata->client == client);
  mtev_boolean rv = mtev_true;
  lua_State *L = udata->L;

  if(udata->cb_ready >= 0) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, udata->cb_ready);
    lua_rawgeti(L, LUA_REGISTRYINDEX, udata->self_idx);
    int lrv = mtev_lua_pcall(L, 1, 1, 0);
    if(lrv) {
      int i;
      mtevL(nlerr, "lua: ws ready handler failed\n");
      mtev_lua_traceback(L);
      i = lua_gettop(L);
      if(i>0 && lua_isstring(L, i)) mtevL(nlerr, "lua: %s\n", lua_tostring(L, i));
      lua_pop(L,i);
      rv = mtev_false;
    }
    else {
      rv = lua_toboolean(L,1);
    }
    lua_pop(L,lua_gettop(L));
  }
  return rv;
}
static mtev_boolean nl_ws_cb_message(mtev_websocket_client_t *client,
                                     int opcode, const unsigned char *msg,
                                     size_t msg_len, void *closure) {
  struct websocket_lua_t *udata = closure;
  if(udata->client == NULL) return mtev_false;
  if(udata->L == NULL) return mtev_false;
  mtevAssert(udata->client == client);
  mtev_boolean rv = mtev_true;
  lua_State *L = udata->L;

  if(udata->cb_message >= 0) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, udata->cb_message);
    lua_rawgeti(L, LUA_REGISTRYINDEX, udata->self_idx);
    lua_pushinteger(L, opcode);
    lua_pushlstring(L, (const char *)msg, msg_len);
    int lrv = mtev_lua_pcall(L, 3, 1, 0);
    if(lrv) {
      int i;
      mtevL(nlerr, "lua: ws ready handler failed\n");
      mtev_lua_traceback(L);
      i = lua_gettop(L);
      if(i>0 && lua_isstring(L, i)) mtevL(nlerr, "lua: %s\n", lua_tostring(L, i));
      lua_pop(L,i);
      rv = mtev_false;
    }
    else {
      rv = lua_toboolean(L,1);
    }
    lua_pop(L,lua_gettop(L));
  }
  return rv;
}
static void nl_ws_cb_cleanup(mtev_websocket_client_t *client,
                             void *closure) {
  (void)client;
  struct websocket_lua_t *udata = closure;
  lua_State *L = udata->L;
  if(udata->L == NULL) return;

  if(udata->cb_cleanup >= 0) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, udata->cb_cleanup);
    if(udata->client == NULL) lua_pushnil(L);
    else lua_rawgeti(L, LUA_REGISTRYINDEX, udata->self_idx);
    int lrv = mtev_lua_pcall(L, 1, 0, 0);
    if(lrv) {
      int i;
      mtevL(nlerr, "lua: ws ready handler failed\n");
      mtev_lua_traceback(L);
      i = lua_gettop(L);
      if(i>0 && lua_isstring(L, i)) mtevL(nlerr, "lua: %s\n", lua_tostring(L, i));
    }
    lua_pop(L,lua_gettop(L));
  }
  mtev_lua_ws_close_internal(L, udata);
}
static mtev_websocket_client_callbacks lua_ws_callbacks = {
  nl_ws_cb_ready,
  nl_ws_cb_message,
  nl_ws_cb_cleanup
};

/*! \lua success = mtev.websocket_client_connect(host, port, uri, service, callbacks, sslconfig)
    \brief Create a new web socket client.
    \param host The host
    \param port The port
    \param uri The uri
    \param service The service
    \param callbacks A table of callbacks
    \param sslconfig An optional non-empty table of ssl configuration.
    \return True or false for success.

    Callbacks may include:
      * ready = function(mtev.websocket_client) return boolean
      * message = function(mtev.websocket_client, opcode, payload) return boolean
      * cleanup = function(mtev.websocket_client)
    
    If callbacks returning boolean return false, the connection will shutdown.
    sslconfig can contain `ca_chain` `key` `cert` `layer` `ciphers` just as
    with other SSL functions.
*/
static int
nl_websocket_client_connect(lua_State *L) {
  int n = lua_gettop(L);
  if(n < 5 || n > 6) luaL_error(L, "wrong args");
  const char *host = lua_tostring(L,1);
  int port = lua_tointeger(L,2);
  const char *path = lua_tostring(L,3);
  const char *service = lua_tostring(L,4);
  mtev_hash_table *sslconfig = NULL;
  if(host == NULL) luaL_error(L, "bad host argument");
  if(path == NULL) luaL_error(L, "bad path argument");
  if(!lua_istable(L,5)) luaL_error(L, "bad callback table");
  
  if(n == 6) {
    sslconfig = mtev_lua_table_to_hash(L, 6);
  }

  mtev_lua_resume_info_t *ci = mtev_lua_find_resume_info(L, mtev_true);
  mtev_lua_resume_info_t *ri = ci->new_ri_f(ci->lmc);

  struct websocket_lua_t *state = lua_newuserdata(ri->coro_state, sizeof(*state));
  state->L = ri->coro_state;
  luaL_getmetatable(state->L, "mtev.websocket_client");
  lua_setmetatable(state->L, -2);
  state->self_idx = luaL_ref(state->L, LUA_REGISTRYINDEX);
  lua_rawgeti(state->L, LUA_REGISTRYINDEX, state->self_idx);
#define SET_CB(name) do { \
  lua_getfield(L,5,#name); \
  if(lua_isnil(L,-1)) { state->cb_##name = -1; break; } \
  if(!lua_isfunction(L,-1)) luaL_error(L, "callback " #name " not function"); \
  lua_xmove(L, state->L, 1); \
  state->cb_##name = luaL_ref(state->L, LUA_REGISTRYINDEX); \
} while(0)
  SET_CB(ready);
  SET_CB(message);
  SET_CB(cleanup);
  state->client =
    mtev_websocket_client_new(host, port, path, service,
                              &lua_ws_callbacks, state, NULL, sslconfig);
  if(sslconfig) mtev_hash_destroy(sslconfig, NULL, NULL);
  free(sslconfig);
  if(state->client == NULL) {
    lua_pushboolean(L,mtev_false);
    return 1;
  }
  lua_pushboolean(L,mtev_true);
  return 1;
}

struct nl_wn_queue_node {
  int *refs;
  int nrefs;
  struct nl_wn_queue_node *next;
};
struct nl_wn_queue {
  char *key;
  lua_State *L;
  eventer_t pending_event;
  eventer_t ping_event;
  struct nl_wn_queue_node *head, *tail;
};
static void
nl_wn_queue_push(struct nl_wn_queue *q, lua_State *L, int nargs) {
  struct nl_wn_queue_node *toinsert = calloc(1, sizeof(*toinsert));
  toinsert->nrefs = nargs;
  toinsert->refs = calloc(nargs, sizeof(int));
  for(int i=1;i<=nargs;i++) {
    lua_pushvalue(L,i);
    toinsert->refs[i-1] = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  if(q->tail) q->tail->next = toinsert;
  else q->head = toinsert;
  q->tail = toinsert;
}
static int
nl_wn_queue_pop(struct nl_wn_queue *q, lua_State *L) {
  struct nl_wn_queue_node *n;
  n = q->head;
  if(!n) return 0;
  q->head = q->head->next;
  if(q->head == NULL) q->tail = NULL;

  /* use n */
  int nargs = 0;
  for(int i=0; i<n->nrefs; i++) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, n->refs[i]);
    luaL_unref(L, LUA_REGISTRYINDEX, n->refs[i]);
    nargs++;
  }
  free(n->refs);
  free(n);
  return nargs;
}

static int
nl_waitfor_ping(eventer_t e, int mask, void *vcl, struct timeval *now) {
  (void)mask;
  (void)now;
  mtev_lua_resume_info_t *ci;
  struct nl_wn_queue *q = vcl;

  ci = mtev_lua_find_resume_info(q->L, mtev_false);
  if(!ci) return 0;
  mtevAssert(e == q->ping_event);
  q->ping_event = NULL;
  mtev_lua_deregister_event(ci, e, 0);

  int available_nargs = 0;
  if(q->L) {
    available_nargs = nl_wn_queue_pop(q, q->L);
  }
  /* There could be nothing... */
  if(available_nargs == 0) return 0;

  if(q->pending_event) {
    eventer_t te = eventer_remove(q->pending_event);
    q->pending_event = NULL;
    q->L = NULL;
    if(te) mtev_lua_deregister_event(ci, te, 0);
  }

  if(q->head == NULL) {
    mtev_hash_delete(ci->lmc->pending, q->key, strlen(q->key), free, free);
  }

  mtev_lua_lmc_resume(ci->lmc, ci, available_nargs);
  return 0;
}

/*! \lua mtev.notify(key, ...)
\brief Send notification message on given key, to be received by mtev.waitfor(key)
\param key key specifying notification channel
\param ... additional args to be included in the message
*/
static int
nl_waitfor_notify(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  struct nl_wn_queue *q;
  void *vptr;
  const char *key;
  int nargs;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);
  nargs = lua_gettop(L);
  if(nargs < 1) {
    return 0;
  }
  key = lua_tostring(L, 1);
  if(!key) luaL_error(L, "notify called without key");
  if(!mtev_hash_retrieve(ci->lmc->pending, key, strlen(key), &vptr)) {
    q = calloc(1, sizeof(*q));
    q->key = strdup(key);
    /* This should never fail.... if it does, we have multiple threads interacting,
     * which is unexpected behavior */
    if (mtev_hash_store(ci->lmc->pending, q->key, strlen(q->key), q) == 0) {
      free(q->key);
      free(q);
      luaL_error(L, "error storing nl_wn_queue object in hash in nl_waitfor_notify");
    }
  } else {
    q = vptr;
  }
  if(q->L) {
    ci = mtev_lua_find_resume_info(q->L, mtev_false);
    if(!ci) q->L = NULL;
  }
  if(lua_isyieldable(L) && q->L) {
    lua_xmove(L, q->L, nargs);
    q->L = NULL;

    mtevAssert(ci);
    if(q->pending_event) {
      if(eventer_remove(q->pending_event)) {
        mtev_lua_deregister_event(ci, q->pending_event, 0);
        eventer_free(q->pending_event);
      }
      q->pending_event = NULL;
    }
    mtev_lua_lmc_resume(ci->lmc, ci, nargs);
    return 0;
  }
  else {
    nl_wn_queue_push(q, L, nargs);
    /* There's a chance that they are actively waiting but we
     * can't directly resume them due to us not being yieldable.
     * In this case, we should ping them to wake up.
     */
    if(q->L) {
      if(q->ping_event == NULL) {
        eventer_t newe = eventer_in_s_us(nl_waitfor_ping, q, 0, 0);
        q->ping_event = newe;
        mtev_lua_register_event(ci, newe);
        eventer_add(newe);
      }
    }
  }
  return 0;
}

static int
nl_waitfor_timeout(eventer_t e, int mask, void *vcl, struct timeval *now) {
  (void)mask;
  (void)now;
  mtev_lua_resume_info_t *ci;
  struct nl_wn_queue *q = vcl;

  ci = mtev_lua_find_resume_info(q->L, mtev_false);
  if(!ci) return 0;
  mtevAssert(e == q->pending_event);

  int available_nargs = 0;
  if(q->L) {
    available_nargs = nl_wn_queue_pop(q, q->L);
  }

  if(q->ping_event) {
    eventer_t removed = eventer_remove(q->ping_event);
    if(removed) {
      mtev_lua_deregister_event(ci, removed, 0);
      eventer_free(removed);
    }
    q->ping_event = NULL;
  }
  q->pending_event = NULL;
  q->L = NULL;

  mtev_lua_deregister_event(ci, e, 0);

  if(q->head == NULL) {
    mtev_hash_delete(ci->lmc->pending, q->key, strlen(q->key), free, free);
  }

  mtev_lua_lmc_resume(ci->lmc, ci, available_nargs);
  return 0;
}

/*! \lua ... = mtev.waitfor(key, [timeout])
\brief Suspend until for notification on key is received or the timeout is reached.
\return arguments passed to mtev.notify() including the key.
*/
static int
nl_waitfor(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  void *vptr;
  const char *key;
  struct nl_wn_queue *q;
  eventer_t e;
  double p_int = 0.0;
  mtev_boolean have_timeout = mtev_false;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);
  if(lua_gettop(L) < 1 || lua_gettop(L) > 2) {
    luaL_error(L, "waitfor(key, [timeout]) wrong arguments");
  }
  key = lua_tostring(L, 1);
  if(lua_gettop(L) == 2 && !lua_isnil(L, 2)) {
    p_int = lua_tonumber(L, 2);
    have_timeout = mtev_true;
  }

  if(!key) luaL_error(L, "waitfor called without key");
  if(!mtev_hash_retrieve(ci->lmc->pending, key, strlen(key), &vptr)) {
    q = calloc(1, sizeof(*q));
    q->key = strdup(key);
    q->L = L;
    /* This should never fail.... if it does, we have multiple threads interacting,
     * which is unexpected behavior */
    if (mtev_hash_store(ci->lmc->pending, q->key, strlen(q->key), q) == 0) {
      free(q->key);
      free(q);
      luaL_error(L, "error storing nl_wn_queue object in hash in nl_waitfor");
    }
  } else {
    q = vptr;
    if(q->L) luaL_error(L, "waitfor cannot be called concurrently");
    q->L = L;
  }

  int available_nargs = nl_wn_queue_pop(q, L);
  if(available_nargs > 0) {
    q->L = NULL;
    if(q->head == NULL) {
      if(q->ping_event) {
        eventer_t removed = eventer_remove(q->ping_event);
        if(removed) {
          mtev_lua_deregister_event(ci, removed, 0);
          eventer_free(removed);
        }
        q->ping_event = NULL;
      }
      mtev_hash_delete(ci->lmc->pending, q->key, strlen(q->key), free, free);
    }
    return available_nargs;
  }
  if(!have_timeout) {
    return mtev_lua_yield(ci, 0);
  }
  /* if the timeout is zero and we didn't return already, don't wait */
  if(p_int == 0.0) {
    q->L = NULL;
    return 0;
  }

  q->pending_event = e =
    eventer_in_s_us(nl_waitfor_timeout, q,
                    floor(p_int), (p_int - floor(p_int)) * 1000000);
  mtev_lua_register_event(ci, e);
  eventer_add(e);
  return mtev_lua_yield(ci, 0);
}


static int
nl_sleep_complete(eventer_t e, int mask, void *vcl, struct timeval *now) {
  (void)mask;
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl = vcl;
  struct timeval diff;

  ci = mtev_lua_find_resume_info(cl->L, mtev_false);
  if(!ci) { mtev_lua_eventer_cl_cleanup(e); return 0; }
  mtev_lua_deregister_event(ci, e, 0);

  sub_timeval(*now, cl->start, &diff);
  nl_mtev_timeval_new_ex(cl->L, diff);

  free(cl);
  mtev_lua_lmc_resume(ci->lmc, ci, 1);
  return 0;
}

/*! \lua slept = mtev.sleep(duration_s)
\param duration_s the number of sections to sleep
\return the time slept as mtev.timeval object
*/
static int
nl_sleep(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl;
  eventer_t e;
  double p_int;

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  p_int = lua_tonumber(L, 1);
  cl = calloc(1, sizeof(*cl));
  cl->free = nl_extended_free;
  cl->L = L;
  mtev_gettimeofday(&cl->start, NULL);

  e = eventer_in_s_us(nl_sleep_complete, cl,
                      floor(p_int), (p_int - floor(p_int)) * 1000000);
  mtev_lua_register_event(ci, e);
  eventer_add(e);
  return mtev_lua_yield(ci, 0);
}

#define SIMPLE_NL(func) static int \
nl_##func(lua_State *L) { \
  lua_pushinteger(L, func()); \
  return 1; \
}

SIMPLE_NL(getuid)
SIMPLE_NL(getgid)
SIMPLE_NL(geteuid)
SIMPLE_NL(getegid)
SIMPLE_NL(getpid)
SIMPLE_NL(getppid)

static int
nl_unlink(lua_State *L) {
  int rv;
  if(lua_gettop(L) != 1)
    luaL_error(L, "bad call to mtev.unlink");
  rv = unlink(lua_tostring(L,1));
  lua_pushboolean(L, rv == 0);
  if(rv >= 0) return 1;
  lua_pushinteger(L, errno);
  lua_pushstring(L, strerror(errno));
  return 3;
}

/*! \lua ok, errno, errstr = mtev.rmdir(path)
\param path string
\return boolean success flag, error number, string representation of error
 */
static int
nl_rmdir(lua_State *L) {
  int rv;
  if(lua_gettop(L) != 1)
    luaL_error(L, "bad call to mtev.rmdir");
  rv = rmdir(lua_tostring(L,1));
  lua_pushboolean(L, rv == 0);
  if(rv >= 0) return 1;
  lua_pushinteger(L, errno);
  lua_pushstring(L, strerror(errno));
  return 3;
}

/*! \lua ok, errno, errstr = mtev.mkdir(path)
\param path string
\return boolean success flag, error number, string representation of error
*/
static int
nl_mkdir(lua_State *L) {
  int rv;
  if(lua_gettop(L) != 2)
    luaL_error(L, "bad call to mtev.mkdir");
  rv = mkdir(lua_tostring(L,1), lua_tointeger(L, 2));
  lua_pushboolean(L, rv == 0);
  if(rv >= 0) return 1;
  lua_pushinteger(L, errno);
  lua_pushstring(L, strerror(errno));
  return 3;
}

/*! \lua ok, errno, errstr = mtev.mkdir_for_file(path)
  \param path string
  \return boolean success flag, error number, string representation of error
*/
static int
nl_mkdir_for_file(lua_State *L) {
  int rv;
  if(lua_gettop(L) != 2)
    luaL_error(L, "bad call to mtev.mkdir_for_file");
  rv = mkdir_for_file(lua_tostring(L,1), lua_tointeger(L, 2));
  lua_pushboolean(L, rv == 0);
  if(rv >= 0) return 1;
  lua_pushinteger(L, errno);
  lua_pushstring(L, strerror(errno));
  return 3;
}

/*! \lua path = mtev.getcwd()
\return path string or nil
*/
static int
nl_getcwd(lua_State *L) {
  char *rp, path[PATH_MAX * 4];
  rp = getcwd(path, sizeof(path));
  if(rp) lua_pushstring(L, rp);
  else lua_pushnil(L);
  return 1;
}



/*! \lua fh = mtev.open(file, flags)
  \param file to open (string)
  \param integer flag
  \return file handle

  The following flag constants are pre-defined:
  `O_RDONLY`,
  `O_WRONLY`,
  `O_RDWR`,
  `O_APPEND`,
  `O_SYNC`,
  `O_NOFOLLOW`,
  `O_CREAT`,
  `O_TRUNC`,
  `O_EXCL`
  see `man 2 open` for their semantics.
*/
static int
nl_open(lua_State *L) {
  const char *file;
  int fd, flags;
  if(lua_gettop(L) < 2 || lua_gettop(L) > 3)
    luaL_error(L, "bad call to mtev.open");
  file = lua_tostring(L, 1);
  flags = lua_tointeger(L, 2);
  if(lua_gettop(L) == 2)
    fd = open(file, flags);
  else
    fd = open(file, flags, lua_tointeger(L, 3));
  lua_pushinteger(L, fd);
  if(fd >= 0) return 1;
  lua_pushinteger(L, errno);
  lua_pushstring(L, strerror(errno));
  return 3;
}

/*! \lua mtev.write(fd, str)
*/
static int
nl_write(lua_State *L) {
  int fd, rv;
  size_t len;
  const char *str;
  if(lua_gettop(L) != 2 || !lua_isnumber(L,1) || !lua_isstring(L,2))
    luaL_error(L, "bad parameters to mtev.write(fd, str)");
  fd = lua_tointeger(L,1);
  str = lua_tolstring(L,2,&len);
  rv = write(fd, str, len);
  lua_pushinteger(L,rv);
  if(rv < 0) lua_pushinteger(L,errno);
  else lua_pushnil(L);
  return 2;
}

/*! \lua mtev.close(fd)
\brief Close a file descripto.
\param fd the integer file descriptor to close.
 */
static int
nl_close(lua_State *L) {
  if(lua_gettop(L) != 1 || !lua_isnumber(L, 1))
    luaL_error(L, "bad call to mtev.close");
  close(lua_tointeger(L,1));
  return 0;
}

/*! \lua rv = mtev.chmod(file, mode)
\brief Change the mode of a file.
\param file the path to a target file.
\param a new file mode.
\return rv is the return as documented by the `chmod` libc call.
*/
static int
nl_chmod(lua_State *L) {
  int rv;
  if(lua_gettop(L) != 2 || !lua_isstring(L, 1) || !lua_isnumber(L, 2))
    luaL_error(L, "bad call to mtev.chmod(file, mode)");
  rv = chmod(lua_tostring(L,1), lua_tointeger(L,2));
  lua_pushinteger(L, rv);
  if(rv<0) lua_pushinteger(L, errno);
  else lua_pushnil(L);
  return 2;
}

/* \lua mtev.stat()
*/
static int
nl_stat(lua_State *L) {
  struct stat st;
  int err = 0;
  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.stat");
  if(lua_isstring(L,1)) err = lstat(lua_tostring(L,1), &st);
  else if(lua_isnumber(L,1)) err = fstat(lua_tointeger(L,1), &st);
  else if(lua_isnil(L,1)) {
    lua_pushnil(L);
    return 1;
  }
  else luaL_error(L, "mtev.stat expects a filename or descriptor");
  if(err < 0) {
    lua_pushnil(L);
    lua_pushinteger(L, errno);
    lua_pushstring(L, strerror(errno));
    return 3;
  }
#define SET_STAT(attr) do { \
  lua_pushinteger(L, (int)st.st_##attr); \
  lua_setfield(L, -2, #attr); \
} while(0)
  lua_createtable(L, 0, 9);
  SET_STAT(dev);
  SET_STAT(ino);
  SET_STAT(mode);
  SET_STAT(nlink);
  SET_STAT(uid);
  SET_STAT(gid);
  SET_STAT(rdev);
  SET_STAT(size);
  return 1;
}

/* \lua mtev.readdir()
*/
static int
nl_readdir(lua_State *L) {
  const char *path;
  DIR *root;
  struct dirent *de, *entry;
  int size = 0, cnt = 1;
  int use_filter = 0;
  if(lua_gettop(L) < 1 || lua_gettop(L) > 2)
    luaL_error(L, "bad call to mtev.readdir");
  path = lua_tostring(L, 1);
  if(lua_gettop(L) == 2) {
    if(!lua_isfunction(L, 2))
      luaL_error(L, "mtev.readdir second argument must be a function");
    use_filter = 1;
  }

#ifdef _PC_NAME_MAX
  size = pathconf(path, _PC_NAME_MAX);
#endif
  size = MAX(size, PATH_MAX + 128);
  root = opendir(path);
  if(!root) {
    lua_pushnil(L);
    return 1;
  }
  de = malloc(size);
  lua_newtable(L);
  while(portable_readdir_r(root, de, &entry) == 0 && entry != NULL) {
    int use_value = 1;
    lua_pushstring(L, entry->d_name);
    if(use_filter) {
      lua_pushvalue(L,2);   /* func */
      lua_pushvalue(L,-2);  /* arg  */
      if(mtev_lua_pcall(L,1,1,0) != 0) {
        closedir(root);
        free(de);
        luaL_error(L, lua_tostring(L,-1));
      }
      use_value = lua_toboolean(L,-1);
      lua_pop(L, 1);
    }
    if(use_value) {
      lua_pushinteger(L, cnt++);
      lua_insert(L, -2);
      lua_settable(L, -3);
    }
    else {
      lua_pop(L,1);
    }
  }
  closedir(root);
  free(de);

  return 1;
}
/*! \lua path = mtev.realpath(inpath)
\brief Return the real path of a relative path.
\param inpath a relative path as a string
\return The non-relative path inpath refers to (or nil on error).
*/
static int
nl_realpath(lua_State *L) {
  char path[PATH_MAX], *rpath;
  if(lua_gettop(L) != 1 || !lua_isstring(L,1))
    luaL_error(L, "bad call to mtev.realpath");
  rpath = realpath(lua_tostring(L,1), path);
  if(rpath) lua_pushstring(L, rpath);
  else lua_pushnil(L);
  return 1;
}

// not exposed
static int
nl_log_up(lua_State *L) {
  int i, n;
  const char *log_dest, *message;
  mtev_log_stream_t ls;

  if(lua_gettop(L) < 1) luaL_error(L, "bad call to mtev.log");

  log_dest = lua_tostring(L, lua_upvalueindex(1));
  ls = mtev_log_stream_find(log_dest);
  if(!ls) {
    mtevL(mtev_stderr, "Cannot find log stream: '%s'\n", log_dest);
    return 0;
  }

  n = lua_gettop(L);
  lua_getglobal(L, "string");
  lua_pushstring(L, "format");
  lua_gettable(L, -1);
  for(i=1;i<=n;i++)
    lua_pushvalue(L, i);
  lua_call(L, n, 1);
  message = lua_tostring(L, -1);

  /* The following is an emulation of the mtevL macro,
   * but with lua magic to hijack the FILE and LINE
   * so that in debugging mode we see lua lines instead of this
   * C file which is useless.
   */
  bool mtevLT_doit = mtev_log_global_enabled() || N_L_S_ON((ls));
  if(!mtevLT_doit) {
    Zipkin_Span *mtevLT_span = mtev_zipkin_active_span(NULL);
    if(mtevLT_span != NULL) {
      mtevLT_doit = mtev_zipkin_span_logs_attached(mtevLT_span);
    }
  }
  if(mtevLT_doit) {
    luaL_where(L, 2);
    const char *where = lua_tostring(L, -1);
    int line = 0;
    char path[PATH_MAX];
    const char *file = "<unknown>";
    if(where) {
      file = strrchr(where, '/');
      file = file ? (file+1) : where;
      strlcpy(path, file, sizeof(path));
      char *lc = strrchr(path, ':');
      if(lc) {
        *lc = '\0';
        lc = strrchr(path, ':');
        if(lc) {
          *lc++ = '\0';
	  line = atoi(lc);
        }
      }
      file = path;
    }
    lua_pop(L,1); /* where */
    mtev_log((ls), NULL, file, line, "%s", message);
  }
  lua_pop(L, 1); /* formatted string */
  lua_pop(L, 1); /* "string" table */
  return 0;
}
/*! \lua len = mtev.print(format, ...)
\param format a format string see printf(3c)
\param ... arguments to be used within the specified format
\return the number of bytes written

This function is effectively the `mtev.log` function with the first argument
set to "error".  It is also aliased into the global `print` symbol such that
one cannot accidentally call the print builtin.
*/
static char *print_tgt = NULL;
static int
nl_print(lua_State *L) {
  int n = lua_gettop(L);
  lua_pushstring(L, print_tgt ? print_tgt : "error");
  lua_pushcclosure(L, nl_log_up, 1);
  lua_insert(L, 1);
  if(n == 0) {
    lua_pushstring(L,"\n");
    n = 1;
  }
  else {
    int i;
    char fmt[1024];
    fmt[0] = '\0';
    for(i=0;i<n;i++) {
      strlcat(fmt, "%s", sizeof(fmt));
      strlcat(fmt, (i==(n-1)) ? "\n" : "\t", sizeof(fmt));
    }
    lua_pushstring(L,fmt);
    lua_insert(L,2);
    n++;
  }
  lua_call(L, n, 0);
  return 0;
}
/*! \lua len = mtev.printto(facility)
\param facility is an mtev log stream

This function sets the mtev.print output facility.
*/
static int
nl_printto(lua_State *L) {
  int n = lua_gettop(L);
  if(n != 1) luaL_error(L, "mtev.printto(facility)");
  char *tofree = print_tgt;
  print_tgt = strdup(luaL_checkstring(L,1));
  free(tofree);
  return 0;
}
/*! \lua len = mtev.log(facility, format, ...)
\brief write message into the libmtev logging system
\param facility the name of the mtev_log_stream (e.g. "error")
\param format a format string see printf(3c)
\param ... arguments to be used within the specified format
\return the number of bytes written
*/
static int
nl_log(lua_State *L) {
  int n = lua_gettop(L);
  lua_pushvalue(L,1);
  lua_pushcclosure(L, nl_log_up, 1);
  lua_remove(L,1);
  lua_insert(L,1);
  lua_call(L, n-1, 0);
  return 0;
}
/*! \lua boolean = mtev.log_enabled(facility)
\brief Determine the enabled status of a log.
\param facility the name of the mtev_log_stream (e.g. "debug")
\return a boolean indicating the enabled status of the log facility
*/
static int
nl_log_enabled(lua_State *L) {
  const char *logname = luaL_checkstring(L,1);
  mtev_log_stream_t ls = mtev_log_stream_find(logname);
  if(!ls) return 0;
  lua_pushboolean(L, N_L_S_ON(ls));
  return 1;
}
/*! \lua mtev.enable_log(facility, flags = true)
\brief Enable or disable a log facility by name.
\param facility the name of the mtev_log_stream (e.g. "debug")
\param flags true enables, false disables
*/
static int
nl_enable_log(lua_State *L) {
  int n = lua_gettop(L);
  int enabled = 1;
  if(n<1) luaL_error(L, "mtev.enable_log(facility[, bool])");
  const char *logname = lua_tostring(L,1);
  if(n>1) enabled = lua_toboolean(L,2);
  mtev_log_stream_t ls = mtev_log_stream_find(logname);
  if(!ls) return 0;
  if(enabled && !N_L_S_ON(ls)) {
    mtev_log_stream_set_flags(ls, mtev_log_stream_get_flags(ls) | MTEV_LOG_STREAM_ENABLED);
  }
  else if(!enabled && N_L_S_ON(ls)) {
    mtev_log_stream_set_flags(ls, mtev_log_stream_get_flags(ls) & ~MTEV_LOG_STREAM_ENABLED);
  }
  return 0;
}
/* \lua mtev.lockfile_acquire()
*/
static int
nl_lockfile_acquire(lua_State *L) {
  mtev_lockfile_t val = -1;
  const char *filename;
  if(lua_gettop(L) != 1 || !lua_isstring(L, 1))
    luaL_error(L, "bad call to mtev.lockfile_acquire");
  filename = lua_tostring(L,1);
  if(filename) val = mtev_lockfile_acquire(filename);
  lua_pushinteger(L, val);
  return 1;
}
/* \lua mtev.lockfile_release()
*/
static int
nl_lockfile_release(lua_State *L) {
  int rv;
  if(lua_gettop(L) != 1 || !lua_isnumber(L, 1))
    luaL_error(L, "bad call to mtev.lockfile_release");
  rv = mtev_lockfile_release(lua_tointeger(L,1));
  lua_pushinteger(L, rv);
  return 1;
}
/* \lua mtev.crc32()
*/
static int
nl_crc32(lua_State *L) {
  size_t inlen;
  const char *input;
  uLong start = 0, inputidx = 1;
  if(lua_isnil(L,1)) start = crc32(0, NULL, 0);
  if(lua_gettop(L) == 2) {
    start = lua_tointeger(L, 2);
    inputidx = 2;
  }
  input = lua_tolstring(L, inputidx, &inlen);
  lua_pushnumber(L, (double)crc32(start, (Bytef *)input, inlen));
  return 1;
}
/* \lua mtev.base32_decode()
*/
static int
nl_base32_decode(lua_State *L) {
  size_t inlen, decoded_len;
  const char *message;
  unsigned char *decoded;
  unsigned char decoded_buf[ON_STACK_LUA_STRLEN];
  int needs_free = 0;

  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.decode");

  message = lua_tolstring(L, 1, &inlen);
  if(MAX(1,inlen) <= ON_STACK_LUA_STRLEN) {
    decoded = decoded_buf;
  }
  else {
    decoded = malloc(MAX(1,inlen));
    needs_free = 1;
  }
  if(!decoded) luaL_error(L, "out-of-memory");
  decoded_len = mtev_b32_decode(message, inlen, decoded, MAX(1,inlen));
  lua_pushlstring(L, (char *)decoded, decoded_len);
  if(needs_free) free(decoded);
  return 1;
}
/* \lua mtev.base32_encode()
*/
static int
nl_base32_encode(lua_State *L) {
  size_t inlen, encoded_len;
  const unsigned char *message;
  char *encoded;
  char encoded_buf[ON_STACK_LUA_STRLEN];
  int needs_free = 0;

  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.encode");

  message = (const unsigned char *)lua_tolstring(L, 1, &inlen);
  encoded_len = (((inlen + 7) / 5) * 8) + 1;
  if(encoded_len <= ON_STACK_LUA_STRLEN) {
    encoded = encoded_buf;
  }
  else {
    encoded = malloc(encoded_len);
    needs_free = 1;
  }
  if(!encoded) luaL_error(L, "out-of-memory");
  encoded_len = mtev_b32_encode(message, inlen, encoded, encoded_len);
  lua_pushlstring(L, (char *)encoded, encoded_len);
  if(needs_free) free(encoded);
  return 1;
}
/*! \lua mtev.base64_decode()
*/
static int
nl_base64_decode(lua_State *L) {
  size_t inlen, decoded_len;
  const char *message;
  unsigned char *decoded;
  unsigned char decoded_buf[ON_STACK_LUA_STRLEN];
  int needs_free = 0;

  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.decode");

  message = lua_tolstring(L, 1, &inlen);
  if(MAX(1,inlen) <= ON_STACK_LUA_STRLEN) {
    decoded = decoded_buf;
  }
  else {
    decoded = malloc(MAX(1,inlen));
    needs_free = 1;
  }
  if(!decoded) luaL_error(L, "out-of-memory");
  decoded_len = mtev_b64_decode(message, inlen, decoded, MAX(1,inlen));
  lua_pushlstring(L, (char *)decoded, decoded_len);
  if(needs_free) free(decoded);
  return 1;
}
/*! \lua mtev.base64_encode()
*/
static int
nl_base64_encode(lua_State *L) {
  size_t inlen, encoded_len;
  const unsigned char *message;
  char *encoded;
  char encoded_buf[ON_STACK_LUA_STRLEN];
  int needs_free = 0;

  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.encode");

  message = (const unsigned char *)lua_tolstring(L, 1, &inlen);
  encoded_len = (((inlen + 2) / 3) * 4) + 1;
  if(encoded_len <= ON_STACK_LUA_STRLEN) {
    encoded = encoded_buf;
  }
  else {
    encoded = malloc(encoded_len);
    needs_free = 1;
  }
  if(!encoded) luaL_error(L, "out-of-memory");
  encoded_len = mtev_b64_encode(message, inlen, encoded, encoded_len);
  lua_pushlstring(L, (char *)encoded, encoded_len);
  if(needs_free) free(encoded);
  return 1;
}
/*! \lua mtev.utf8tohtml()
*/
static int
nl_utf8tohtml(lua_State *L) {
  int in_idx = 1, tags_idx = 2;
  const unsigned char *in;
  unsigned char *out;
  unsigned char out_buf[ON_STACK_LUA_STRLEN];
  size_t in_len_size_t;
  int rv, out_len, in_len, needs_free = 0;

  if(lua_gettop(L) < 1)
    luaL_error(L, "bad arguments to mtev.utf8tohtml");
  /* We might be called a method. cope. */
  if(lua_isuserdata(L,1)) {
    in_idx++; tags_idx++;
  }
  if(lua_gettop(L) < in_idx || lua_gettop(L) > tags_idx ||
     !lua_isstring(L,in_idx))
    luaL_error(L, "bad arguments to mtev.utf8tohtml");

  in = (const unsigned char *)lua_tolstring(L, in_idx, &in_len_size_t);
  in_len = (int)in_len_size_t;
  if((size_t)in_len != (size_t)in_len_size_t)
    luaL_error(L, "overflow");
  out_len = in_len * 6 + 1;
  if(out_len <= ON_STACK_LUA_STRLEN) {
    out = out_buf;
  }
  else {
    out = malloc(out_len);
    needs_free = 1;
  }
  if(!out) luaL_error(L, "out-of-memory");
  rv = UTF8ToHtml(out, &out_len, in, &in_len);
  if(rv >= 0) {
    if(lua_toboolean(L,tags_idx)) {
      int i = 0, tagcnt = 0;
      for(i=0;i<out_len;i++) if(out[i] == '<' || out[i] == '>') tagcnt++;
      if(tagcnt) {
        unsigned char *newout, *outcp;
        /* each tag goes from 1 char to 4, (+3) */
        outcp = newout = malloc(out_len + (tagcnt * 3) + 1);
        if(!newout) {
          if(needs_free) free(out);
          luaL_error(L, "out-of-memory");
        }
        for(i=0;i<out_len;i++) {
          if(out[i] == '<') {
            memcpy(outcp, "&lt;", 4);
            outcp += 4;
          }
          else if(out[i] == '>') {
            memcpy(outcp, "&gt;", 4);
            outcp += 4;
          }
          else {
            *outcp++ = out[i];
          }
        }
        if(needs_free) free(out);
        out = newout;
        out_len += tagcnt * 3;
        needs_free = 1;
      }
    }
    lua_pushlstring(L, (const char *)out, out_len);
    if(needs_free) free(out);
    return 1;
  }
  if(needs_free) free(out);
  if(rv == -2) luaL_error(L, "utf8tohtml transcoding failure");
  luaL_error(L, "utf8tohtml failure");
  return 0;
}
/*! \lua mtev.hmac_sha1_encode()
*/
static int
nl_hmac_sha1_encode(lua_State *L) {
  size_t messagelen, keylen, encoded_len;
  const unsigned char *message, *key;
  unsigned char result[EVP_MAX_MD_SIZE+1];
  unsigned int md_len;
  char encoded[29];

  if(lua_gettop(L) != 2) luaL_error(L, "bad call to mtev.hmac_sha1_encode");
  encoded_len = 28; /* the length of the base64 encoded HMAC-SHA1 result will always be 28 */

  message = (const unsigned char *)lua_tolstring(L, 1, &messagelen);
  key = (const unsigned char *)lua_tolstring(L, 2, &keylen);

  HMAC(EVP_sha1(), key, keylen, message, messagelen, result, &md_len);
  encoded_len = mtev_b64_encode(result, md_len, encoded, encoded_len);

  lua_pushlstring(L, (char *)encoded, encoded_len);

  return 1;
}
/*! \lua mtev.hmac_sha256_encode()
*/
static int
nl_hmac_sha256_encode(lua_State *L) {
  size_t messagelen, keylen, encoded_len;
  const unsigned char *message, *key;
  unsigned char result[EVP_MAX_MD_SIZE+1];
  unsigned int md_len;
  char encoded[45];

  if(lua_gettop(L) != 2) luaL_error(L, "bad call to mtev.hmac_sha256_encode");
  encoded_len = 44; /* the length of the base64 encoded HMAC-SHA256 result will always be 44 */

  message = (const unsigned char *)lua_tolstring(L, 1, &messagelen);
  key = (const unsigned char *)lua_tolstring(L, 2, &keylen);

  HMAC(EVP_sha256(), key, keylen, message, messagelen, result, &md_len);
  encoded_len = mtev_b64_encode(result, md_len, encoded, encoded_len);

  lua_pushlstring(L, (char *)encoded, encoded_len);

  return 1;
}

/*! \lua mtev.md5_hex()
*/
static const char _hexchars[16] =
  {'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};
static int
nl_md5_hex(lua_State *L) {
  int i;
  MD5_CTX ctx;
  size_t inlen;
  const char *in;
  unsigned char md5[MD5_DIGEST_LENGTH];
  char md5_hex[MD5_DIGEST_LENGTH * 2 + 1];

  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.md5_hex");
  MD5_Init(&ctx);
  in = lua_tolstring(L, 1, &inlen);
  MD5_Update(&ctx, (const void *)in, (unsigned long)inlen);
  MD5_Final(md5, &ctx);
  for(i=0;i<MD5_DIGEST_LENGTH;i++) {
    md5_hex[i*2] = _hexchars[(md5[i] >> 4) & 0xf];
    md5_hex[i*2+1] = _hexchars[md5[i] & 0xf];
  }
  md5_hex[i*2] = '\0';
  lua_pushstring(L, md5_hex);
  return 1;
}
/*! \lua mtev.md5()
*/
static int
nl_md5(lua_State *L) {
  MD5_CTX ctx;
  size_t inlen;
  const char *in;
  unsigned char md5[MD5_DIGEST_LENGTH];

  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.md5");
  MD5_Init(&ctx);
  in = lua_tolstring(L, 1, &inlen);
  MD5_Update(&ctx, (const void *)in, (unsigned long)inlen);
  MD5_Final(md5, &ctx);
  lua_pushlstring(L, (char *)md5, sizeof(md5));
  return 1;
}
/*! \lua mtev.sha1_hex()
*/
static int
nl_sha1_hex(lua_State *L) {
  int i;
  SHA_CTX ctx;
  size_t inlen;
  const char *in;
  unsigned char sha1[SHA_DIGEST_LENGTH];
  char sha1_hex[SHA_DIGEST_LENGTH * 2 + 1];

  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.sha1_hex");
  SHA1_Init(&ctx);
  in = lua_tolstring(L, 1, &inlen);
  SHA1_Update(&ctx, (const void *)in, (unsigned long)inlen);
  SHA1_Final(sha1, &ctx);
  for(i=0;i<SHA_DIGEST_LENGTH;i++) {
    sha1_hex[i*2] = _hexchars[(sha1[i] >> 4) & 0xf];
    sha1_hex[i*2+1] = _hexchars[sha1[i] & 0xf];
  }
  sha1_hex[i*2] = '\0';
  lua_pushstring(L, sha1_hex);
  return 1;
}
/*! \lua mtev.sha1()
*/
static int
nl_sha1(lua_State *L) {
  SHA_CTX ctx;
  size_t inlen;
  const char *in;
  unsigned char sha1[SHA_DIGEST_LENGTH];

  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.sha1");
  SHA1_Init(&ctx);
  in = lua_tolstring(L, 1, &inlen);
  SHA1_Update(&ctx, (const void *)in, (unsigned long)inlen);
  SHA1_Final(sha1, &ctx);
  lua_pushlstring(L, (char *)sha1, sizeof(sha1));
  return 1;
}
/*! \lua digest_hex = mtev.sha256_hex(s)
\param s a string
\return the SHA256 digest of the input string, encoded in hexadecimal format
*/
static int
nl_sha256_hex(lua_State *L) {
  int i;
  SHA256_CTX ctx;
  size_t inlen;
  const char *in;
  unsigned char sha256[SHA256_DIGEST_LENGTH];
  char sha256_hex[SHA256_DIGEST_LENGTH * 2 + 1];

  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.sha256_hex");
  SHA256_Init(&ctx);
  in = lua_tolstring(L, 1, &inlen);
  SHA256_Update(&ctx, (const void *)in, (unsigned long)inlen);
  SHA256_Final(sha256, &ctx);
  for(i=0;i<SHA256_DIGEST_LENGTH;i++) {
    sha256_hex[i*2] = _hexchars[(sha256[i] >> 4) & 0xf];
    sha256_hex[i*2+1] = _hexchars[sha256[i] & 0xf];
  }
  sha256_hex[i*2] = '\0';
  lua_pushstring(L, sha256_hex);
  return 1;
}
/*! \lua digest = mtev.sha256(s)
\param s a string
\return the SHA256 digest of the input string
*/
static int
nl_sha256(lua_State *L) {
  SHA256_CTX ctx;
  size_t inlen;
  const char *in;
  unsigned char sha256[SHA256_DIGEST_LENGTH];

  if(lua_gettop(L) != 1) luaL_error(L, "bad call to mtev.sha256");
  SHA256_Init(&ctx);
  in = lua_tolstring(L, 1, &inlen);
  SHA256_Update(&ctx, (const void *)in, (unsigned long)inlen);
  SHA256_Final(sha256, &ctx);
  lua_pushlstring(L, (char *)sha256, sizeof(sha256));
  return 1;
}
/*! \lua sec, usec = mtev.gettimeofday()
\return the seconds and microseconds since epoch (1970 UTC)
*/
static int
nl_gettimeofday(lua_State *L) {
  struct timeval now;
  mtev_gettimeofday(&now, NULL);
  lua_pushinteger(L, now.tv_sec);
  lua_pushinteger(L, now.tv_usec);
  return 2;
}
/*! \lua mtev.uuid()
*/
static int
nl_uuid(lua_State *L) {
  uuid_t out;
  char uuid_str[UUID_STR_LEN+1];
  mtev_uuid_generate(out);
  mtev_uuid_unparse_lower(out, uuid_str);
  lua_pushstring(L, uuid_str);
  return 1;
}
static int
nl_socket_internal(lua_State *L, int family, int proto) {
  struct nl_slcl *cl;
  mtev_lua_resume_info_t *ci;
  socklen_t optlen;
  int fd;
  eventer_t e;

  fd = socket(family, proto, 0);
  if(fd < 0) {
    lua_pushnil(L);
    return 1;
  }
  if(eventer_set_fd_nonblocking(fd)) {
    close(fd);
    lua_pushnil(L);
    return 1;
  }

  ci = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ci);

  cl = calloc(1, sizeof(*cl));
  cl->free = nl_extended_free;
  cl->L = L;

  optlen = sizeof(cl->send_size);
  if(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cl->send_size, &optlen) != 0)
    cl->send_size = 4096;

  e = eventer_alloc_fd(NULL, cl, fd, EVENTER_EXCEPTION);
  cl->eptr = mtev_lua_event(L, e);

  mtev_lua_register_event(ci, e);
  return 1;
}
/*! \lua mtev.eventer = mtev.socket(address[, type])
\brief Open a socket for eventer-friendly interaction.
\param address a string 'inet', 'inet6' or an address to connect to
\param type an optional string 'tcp' or 'udp' (default is 'tcp')
\return an eventer object.

No connect() call is performed here, the address provided is only used
to ascertain the address family for the socket.
*/
static int
nl_socket(lua_State *L) {
  int n = lua_gettop(L);
  uint8_t family = AF_INET;
  union {
    struct in_addr addr4;
    struct in6_addr addr6;
  } a;

  if(n > 0 && lua_isstring(L,1)) {
    const char *fam = lua_tostring(L,1);
    if(!fam) fam = "";
    if(!strncmp(fam, "reverse:", 8)) family = AF_INET;
    else if(!strcmp(fam, "inet")) family = AF_INET;
    else if(!strcmp(fam, "inet6")) family = AF_INET6;
    else if(inet_pton(AF_INET, fam, &a) == 1) family = AF_INET;
    else if(inet_pton(AF_INET6, fam, &a) == 1) family = AF_INET6;
    else luaL_error(L, "mtev.socket family for %s unknown", fam);
  }

  if(n <= 1) return nl_socket_internal(L, family, SOCK_STREAM);
  if(n == 2 && lua_isstring(L,2)) {
    const char *type = lua_tostring(L,2);
    if(!strcmp(type, "tcp"))
      return nl_socket_internal(L, family, SOCK_STREAM);
    else if(!strcmp(type, "udp"))
      return nl_socket_internal(L, family, SOCK_DGRAM);
  }
  luaL_error(L, "mtev.socket called with invalid arguments");
  return 0;
}

struct gunzip_crutch {
  z_stream *stream;
  void *scratch_buffer;
};
static int
nl_gunzip_deflate(lua_State *L) {
  struct gunzip_crutch *crutch;
  const char *input;
  size_t inlen;
  z_stream *stream;
  Bytef *data = NULL;
  uLong outlen = 0;
  uLong newoutlen = 0;
  size_t limit = 1024*1024;
  int allow_restart = 1;
  int zerr, n = lua_gettop(L);
  enum {
    NO_ERROR,
    READ_LIMIT_EXCEEDED,
    MALLOC_FAILED,
  } internal_error = NO_ERROR;

  if(n < 1 || n > 2) {
    lua_pushnil(L);
    return 1;
  }

  crutch = lua_touserdata(L, lua_upvalueindex(1));
  stream = crutch->stream;

  input = lua_tolstring(L, 1, &inlen);
  if(!input) {
    lua_pushnil(L);
    return 1;
  }
  if(n == 2 && !lua_isnil(L, 2))
    limit = lua_tointeger(L, 2);

  stream->next_in = (Bytef *)input;
  stream->avail_in = inlen;
  while(1) {
    zerr = inflate(stream, Z_FULL_FLUSH);
    if(zerr == Z_OK || zerr == Z_STREAM_END) {
      /* got some data */
      int size_read = DEFLATE_CHUNK_SIZE - stream->avail_out;
      allow_restart = 0;
      newoutlen = outlen + size_read;
      if(limit && newoutlen > limit) {
        internal_error = READ_LIMIT_EXCEEDED;
        break;
      }
      if(newoutlen > outlen) {
        Bytef *newdata;
        if(data) newdata = realloc(data, newoutlen);
        else newdata = malloc(newoutlen);
        if(!newdata) {
          internal_error = MALLOC_FAILED;
          break;
        }
        data = newdata;
        memcpy(data + outlen, stream->next_out - size_read, size_read);
        outlen += size_read;
        stream->next_out -= size_read;
        stream->avail_out += size_read;
      }
      if(zerr == Z_STREAM_END) {
        /* Good to go */
        break;
      }
    }
    else if(allow_restart && zerr == Z_DATA_ERROR) {
      /* Rarely seen, but on the internet, some IIS servers seem
       * to not generate 'correct' deflate streams, so we use
       * inflateInit2 here to manually configure the stream.
       */
      inflateEnd(stream);
      zerr = inflateInit2(stream, -MAX_WBITS);
      if (zerr != Z_OK) {
        break;
      }
      stream->next_in = (Bytef *)input;
      stream->avail_in = inlen;
      allow_restart = 0;
      continue;
    }
    else {
      break;
    }

    if(stream->avail_in == 0) break;
  }
  switch(internal_error) {
    case NO_ERROR:
      if(zerr == Z_OK || zerr == Z_STREAM_END) {
        if(outlen > 0) lua_pushlstring(L, (char *)data, outlen);
        else lua_pushstring(L, "");
        free(data);
        return 1;
      }
      free(data);
      switch(zerr) {
        case Z_NEED_DICT: luaL_error(L, "zlib: dictionary error"); break;
        case Z_STREAM_ERROR: luaL_error(L, "zlib: stream error"); break;
        case Z_DATA_ERROR: luaL_error(L, "zlib: data error"); break;
        case Z_MEM_ERROR: luaL_error(L, "zlib: out-of-memory"); break;
        case Z_BUF_ERROR: luaL_error(L, "zlib: buffer error"); break;
        case Z_VERSION_ERROR: luaL_error(L, "zlib: version mismatch"); break;
        case Z_ERRNO: luaL_error(L, strerror(errno)); break;
      }
      break;
    case READ_LIMIT_EXCEEDED:
      free(data);
      luaL_error(L, "HTTP client internal error: download exceeded maximum read size (%d bytes)\n",
                 limit);
      break;
    case MALLOC_FAILED:
      free(data);
      luaL_error(L, "HTTP client internal error: out-of-memory"); break;
  }

  lua_pushnil(L);
  return 1;
}
/*! \lua mtev.gunzip()
*/
static int
nl_gunzip(lua_State *L) {
  struct gunzip_crutch *crutch;
  z_stream *stream;

  crutch = (struct gunzip_crutch *)lua_newuserdata(L, sizeof(*crutch));
  crutch->stream = malloc(sizeof(*stream));
  memset(crutch->stream, 0, sizeof(*crutch->stream));
  luaL_getmetatable(L, "mtev.gunzip");
  lua_setmetatable(L, -2);

  crutch->stream->next_in = NULL;
  crutch->stream->avail_in = 0;
  crutch->scratch_buffer =
    crutch->stream->next_out = malloc(DEFLATE_CHUNK_SIZE);
  crutch->stream->avail_out = crutch->stream->next_out ? DEFLATE_CHUNK_SIZE : 0;
  inflateInit2(crutch->stream, MAX_WBITS+32);

  lua_pushcclosure(L, nl_gunzip_deflate, 1);
  return 1;
}
static int
mtev_lua_gunzip_gc(lua_State *L) {
  struct gunzip_crutch *crutch;
  crutch = (struct gunzip_crutch *)lua_touserdata(L,1);
  if(crutch->scratch_buffer) free(crutch->scratch_buffer);
  inflateEnd(crutch->stream);
  free(crutch->stream);
  return 0;
}

struct pcre_global_info {
  pcre *re;
  size_t offset;
  const char *subject; /* we only use this for pointer equivalency testing */
};
static int
mtev_lua_pcre_match(lua_State *L) {
  bool vectored_return = false;
  const char *subject;
  struct pcre_global_info *pgi;
  int i, cnt, ovector[30];
  size_t inlen;
  struct pcre_extra e;
  memset(&e, 0, sizeof(e));

  pgi = (struct pcre_global_info *)lua_touserdata(L, lua_upvalueindex(1));
  subject = lua_tolstring(L,1,&inlen);
  if(lua_gettop(L) > 1) {
    if(!lua_istable(L, 2)) {
      mtevL(nldeb, "pcre match called with second argument that is not a table\n");
    }
    else {
      lua_getfield(L, -1, "limit");
      if(lua_isnumber(L, -1)) {
        e.flags |= PCRE_EXTRA_MATCH_LIMIT;
        e.match_limit = (int)lua_tonumber(L, -1);
      }
      lua_pop(L, 1);
      lua_getfield(L, -1, "limit_recurse");
      if(lua_isnumber(L, -1)) {
        e.flags |= PCRE_EXTRA_MATCH_LIMIT_RECURSION;
        e.match_limit_recursion = (int)lua_tonumber(L, -1);
      }
      lua_pop(L, 1);
      lua_getfield(L, -1, "offsets");
      if(lua_isboolean(L, -1)) {
        vectored_return = (int)lua_toboolean(L, -1);
      }
      lua_pop(L, 1);
    }
  }
  if(!subject) {
    pgi->subject = NULL;
    pgi->offset = 0;
    lua_pushboolean(L,0);
    return 1;
  }
  if(pgi->subject != subject) {
    pgi->offset = 0;
    pgi->subject = subject;
  }
  if (pgi->offset >= inlen) {
    if(vectored_return) lua_createtable(L, 0, 0);
    else lua_pushboolean(L,0);
    return 1;
  }
  cnt = pcre_exec(pgi->re, &e, subject + pgi->offset,
                  inlen - pgi->offset, 0, 0,
                  ovector, sizeof(ovector)/sizeof(*ovector));
  if(cnt <= 0) {
    if(vectored_return) lua_createtable(L, 0, 0);
    else lua_pushboolean(L,0);
    return 1;
  }
  if(vectored_return) {
    lua_createtable(L, cnt*2, 0);
    for(i = 0; i < cnt; i++) {
      lua_pushinteger(L, pgi->offset+ovector[i*2]);
      lua_rawseti(L, -2, i*2+1);
      lua_pushinteger(L, pgi->offset+ovector[i*2+1]);
      lua_rawseti(L, -2, i*2+2);
    }
  }
  else lua_pushboolean(L,1);
  for(i = 0; i < cnt; i++) {
    int start = ovector[i*2];
    int end = ovector[i*2+1];
    lua_pushlstring(L, subject+pgi->offset+start, end-start);
  }
  pgi->offset += ovector[1]; /* endof the overall match */
  return cnt+1;
}

/*! \lua matcher = mtev.pcre(pcre_expression)
\param pcre_expression a perl compatible regular expression
\return a matcher function `rv, m, ... = matcher(subject, options)`

A compiled pcre matcher function takes a string subject as the first
argument and optional options as second argument.

The matcher will return first whether there was a match (true/false).
If true, the next return value will be to entire scope of the match
followed by any capture subexpressions.  If the same subject variable
is supplied, subsequent calls will act on the remainder of the subject
past previous matches (allowing for global search emulation).  If the
subject changes, the match starting location is reset to the beginning.
The caller can force a reset by calling `matcher(nil)`.

`options` is an option table with the optional fields `limit`
(`PCRE_CONFIG_MATCH_LIMIT`) and `limit_recurse` (`PCRE_CONFIG_MATCH_LIMIT_RECURSION`).
See the pcreapi man page for more details.
*/
static int
nl_pcre(lua_State *L) {
  pcre *re;
  struct pcre_global_info *pgi;
  const char *expr;
  const char *errstr;
  int erroff;

  expr = lua_tostring(L,1);
  re = pcre_compile(expr, 0, &errstr, &erroff, NULL);
  if(!re) {
    lua_pushnil(L);
    lua_pushstring(L, errstr);
    lua_pushinteger(L, erroff);
    return 3;
  }
  pgi = (struct pcre_global_info *)lua_newuserdata(L, sizeof(*pgi));
  pgi->re = re;
  pgi->offset = 0;
  luaL_getmetatable(L, "mtev.pcre");
  lua_setmetatable(L, -2);
  lua_pushcclosure(L, mtev_lua_pcre_match, 1);
  return 1;
}
static int
mtev_lua_pcre_gc(lua_State *L) {
  struct pcre_global_info *pgi;
  pgi = (struct pcre_global_info *)lua_touserdata(L,1);
  pcre_free(pgi->re);
  return 0;
}

#define SPLIT_PATH(path, base, element) char base_buf[4096]; \
do { \
  char *endp; \
  element = NULL; \
  if(sizeof(base_buf) < strlen(path)+1) luaL_error(L, "path overflow"); \
  base = base_buf; \
  memcpy(base, path, strlen(path)+1); \
  endp = base + strlen(path); \
  while(endp > base && *endp != '/') endp--; \
  if(*endp == '/') *endp = '\0'; \
  element = endp + 1; \
} while(0)
/*! \lua mtev.conf_get_string()
*/
static int
nl_conf_get_string(lua_State *L) {
  char *val;
  const char *path = lua_tostring(L,1);
  if(path && lua_gettop(L) == 2) {
    mtev_conf_section_t section;
    char *element, *base;
    SPLIT_PATH(path, base, element);

    section = mtev_conf_get_section_write(MTEV_CONF_ROOT, base);
    if(mtev_conf_section_is_empty(section) || !element) {
      lua_pushboolean(L, 0);
    }
    else {
      mtev_conf_set_string(section, element, lua_tostring(L,2));
      lua_pushboolean(L, 1);
    }
    mtev_conf_release_section_write(section);
    return 1;
  }
  if(path &&
     mtev_conf_get_string(MTEV_CONF_ROOT, path, &val)) {
    lua_pushstring(L,val);
    free(val);
  }
  else lua_pushnil(L);
  return 1;
}
/*! \lua mtev.conf_get_string_list()
*/
static int
nl_conf_get_string_list(lua_State *L) {
  char *val;
  int n, cnt;
  n = lua_gettop(L);
  mtevAssert(n == 2);
  const char *base_path = lua_tostring(L,1);
  const char *child_path = lua_tostring(L,2);

  mtev_conf_section_t* mqs = mtev_conf_get_sections_read(MTEV_CONF_ROOT, base_path, &cnt);

  if(mqs == NULL) {
    lua_pushnil(L);
  } else {
    lua_createtable(L, cnt, 0);
    for(int i = 0; i < cnt; i++) {
      if(!mtev_conf_get_string(mqs[i], child_path, &val)) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "Unable to read option entry: %s%s", base_path, child_path);
        mtev_conf_release_sections_read(mqs, cnt);
        return luaL_error(L, msg);
      }
      lua_pushinteger(L, i + 1);
      lua_pushstring(L, val);
      lua_settable(L, -3);
      free(val);
    }
    mtev_conf_release_sections_read(mqs, cnt);
  }

  return 1;
}
/*! \lua mtev.conf_get_integer()
*/
static int
nl_conf_get_integer(lua_State *L) {
  int32_t val;
  const char *path = lua_tostring(L,1);
  if(path && lua_gettop(L) == 2) {
    mtev_conf_section_t section;
    char *element, *base;
    SPLIT_PATH(path, base, element);

    section = mtev_conf_get_section_write(MTEV_CONF_ROOT, base);
    if(mtev_conf_section_is_empty(section) || !element) {
      lua_pushboolean(L, 0);
    }
    else {
      mtev_conf_set_string(section, element, lua_tostring(L,2));
      lua_pushboolean(L, 1);
    }
    mtev_conf_release_section_write(section);
    return 1;
  }
  if(path &&
     mtev_conf_get_int32(MTEV_CONF_ROOT, path, &val)) {
    lua_pushinteger(L,val);
  }
  else lua_pushnil(L);
  return 1;
}
/*! \lua mtev.conf_get_boolean()
*/
static int
nl_conf_get_boolean(lua_State *L) {
  mtev_boolean val;
  const char *path = lua_tostring(L,1);
  if(path && lua_gettop(L) == 2) {
    mtev_conf_section_t section;
    char *element, *base;
    SPLIT_PATH(path, base, element);

    section = mtev_conf_get_section_write(MTEV_CONF_ROOT, base);
    if(mtev_conf_section_is_empty(section) || !element) {
      lua_pushboolean(L, 0);
    }
    else {
      mtev_conf_set_string(section, element, lua_toboolean(L,2) ? "true" : "false");
      lua_pushboolean(L, 1);
    }
    mtev_conf_release_section_write(section);
    return 1;
  }
  if(path &&
     mtev_conf_get_boolean(MTEV_CONF_ROOT, path, &val)) {
    lua_pushboolean(L,val);
  }
  else lua_pushnil(L);
  return 1;
}
/*! \lua mtev.conf_get_float()
*/
static int
nl_conf_get_float(lua_State *L) {
  float val;
  const char *path = lua_tostring(L,1);
  if(path && lua_gettop(L) == 2) {
    mtev_conf_section_t section;
    char *element, *base;
    SPLIT_PATH(path, base, element);

    section = mtev_conf_get_section_write(MTEV_CONF_ROOT, base);
    if(mtev_conf_section_is_empty(section) || !element) {
      lua_pushboolean(L, 0);
    }
    else {
      mtev_conf_set_string(section, element, lua_tostring(L,2));
      lua_pushboolean(L, 1);
    }
    mtev_conf_release_section_write(section);
    return 1;
  }
  if(path &&
     mtev_conf_get_float(MTEV_CONF_ROOT, path, &val)) {
    lua_pushnumber(L,val);
  }
  else lua_pushnil(L);
  return 1;
}
/*! \lua mtev.conf_replace_value()
*/
static int
nl_conf_replace_value(lua_State *L) {
  const char *path = lua_tostring(L,1);
  if (path && lua_gettop(L) == 2) {
    mtev_conf_section_t section;
    char *element, *base;
    SPLIT_PATH(path, base, element);
    while (!mtev_conf_section_is_empty(section = mtev_conf_get_section_write(MTEV_CONF_ROOT, path))) {
      mtev_conf_remove_section(section);
    }
    section = mtev_conf_get_section_write(MTEV_CONF_ROOT, base);
    if(mtev_conf_section_is_empty(section)) {
      lua_pushboolean(L, 0);
      mtev_conf_release_section_write(section);
      return 1;
    }
    mtev_conf_set_string(section, element, lua_tostring(L,2));
    lua_pushboolean(L, 1);
    mtev_conf_release_section_write(section);
  }
  else lua_pushnil(L);
  return 1;
}
/*! \lua mtev.conf_replace_boolean()
*/
static int
nl_conf_replace_boolean(lua_State *L) {
  const char *path = lua_tostring(L,1);
  if (path && lua_gettop(L) == 2) {
    mtev_conf_section_t section;
    char *element, *base;
    SPLIT_PATH(path, base, element);
    while (!mtev_conf_section_is_empty(section = mtev_conf_get_section_write(MTEV_CONF_ROOT, path))) {
      mtev_conf_remove_section(section);
    }
    section = mtev_conf_get_section_write(MTEV_CONF_ROOT, base);
    if(mtev_conf_section_is_empty(section)) {
      lua_pushboolean(L, 0);
      mtev_conf_release_section_write(section);
      return 1;
    }
    mtev_conf_set_string(section, element, lua_toboolean(L,2) ? "true" : "false");
    lua_pushboolean(L, 1);
    mtev_conf_release_section_write(section);
  }
  else lua_pushnil(L);
  return 1;
}

struct xpath_iter {
  xmlXPathContextPtr ctxt;
  xmlXPathObjectPtr pobj;
  int cnt;
  int idx;
};
static int
mtev_lua_xpath_iter(lua_State *L) {
  struct xpath_iter *xpi;
  xpi = lua_touserdata(L, lua_upvalueindex(1));
  if(xpi->pobj) {
    if(xpi->idx < xpi->cnt) {
      xmlNodePtr node, *nodeptr;
      node = xmlXPathNodeSetItem(xpi->pobj->nodesetval, xpi->idx);
      xpi->idx++;
      nodeptr = (xmlNodePtr *)lua_newuserdata(L, sizeof(node));
      *nodeptr = node;
      luaL_getmetatable(L, "mtev.xmlnode");
      lua_setmetatable(L, -2);
      return 1;
    }
  }
  return 0;
}

/*!
\lua iter mtev.xmldoc:xpath(xpath, [node])
\return iterator over mtev.xmlnode objects
*/
static int
mtev_lua_xpath(lua_State *L) {
  int n;
  const char *xpathexpr;
  xmlDocPtr *docptr, doc;
  xmlNodePtr *nodeptr = NULL;
  xmlXPathContextPtr ctxt;
  struct xpath_iter *xpi;

  n = lua_gettop(L);
  /* the first arg is implicitly self (it's a method) */
  docptr = lua_touserdata(L, lua_upvalueindex(1));
  if(docptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(n < 2 || n > 3) luaL_error(L, "expects 1 or 2 arguments, got %d", n);
  doc = *docptr;
  xpathexpr = lua_tostring(L, 2);
  if(!xpathexpr) luaL_error(L, "no xpath expression provided");
  ctxt = xmlXPathNewContext(doc);
  if(n == 3) {
    nodeptr = lua_touserdata(L, 3);
    if(nodeptr) ctxt->node = *nodeptr;
  }
  if(!ctxt) luaL_error(L, "invalid xpath");

  xpi = (struct xpath_iter *)lua_newuserdata(L, sizeof(*xpi));
  xpi->ctxt = ctxt;
  mtev_conf_xml_errors_to_debug();
  xpi->pobj = xmlXPathEval((xmlChar *)xpathexpr, xpi->ctxt);
  if(!xpi->pobj || xpi->pobj->type != XPATH_NODESET)
    xpi->cnt = 0;
  else
    xpi->cnt = xmlXPathNodeSetGetLength(xpi->pobj->nodesetval);
  xpi->idx = 0;
  luaL_getmetatable(L, "mtev.xpathiter");
  lua_setmetatable(L, -2);
  lua_pushcclosure(L, mtev_lua_xpath_iter, 1);
  return 1;
}

/*!
\lua str = mtev.xmlnode:name()
*/
static int
mtev_lua_xmlnode_name(lua_State *L) {
  xmlNodePtr *nodeptr;
  /* the first arg is implicitly self (it's a method) */
  nodeptr = lua_touserdata(L, lua_upvalueindex(1));
  if(nodeptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(lua_gettop(L) == 1) {
    xmlChar *v;
    v = (xmlChar *)(*nodeptr)->name;
    if(v) {
      lua_pushstring(L, (const char *)v);
    }
    else lua_pushnil(L);
    return 1;
  }
  luaL_error(L,"must be called with no arguments");
  return 0;
}


/*!
\lua val = mtev.xmlnode:attr(key)
*/
static int
mtev_lua_xmlnode_attr(lua_State *L) {
  xmlNodePtr *nodeptr;
  /* the first arg is implicitly self (it's a method) */
  nodeptr = lua_touserdata(L, lua_upvalueindex(1));
  if(nodeptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(lua_gettop(L) == 3 && lua_isstring(L,2)) {
    const char *attr = lua_tostring(L,2);
    if(lua_isnil(L,3))
      xmlSetProp(*nodeptr, (xmlChar *)attr, NULL);
    else
      xmlSetProp(*nodeptr, (xmlChar *)attr, (xmlChar *)lua_tostring(L,3));
    return 0;
  }
  if(lua_gettop(L) == 2 && lua_isstring(L,2)) {
    xmlChar *v;
    const char *attr = lua_tostring(L,2);
    v = xmlGetProp(*nodeptr, (xmlChar *)attr);
    if(v) {
      lua_pushstring(L, (const char *)v);
      xmlFree(v);
    }
    else lua_pushnil(L);
    return 1;
  }
  luaL_error(L,"must be called with one argument");
  return 0;
}

/*!
\lua str = mtev.xmlnode:contents()
\return content of xml node as string
*/
static int
mtev_lua_xmlnode_contents(lua_State *L) {
  xmlNodePtr *nodeptr;
  /* the first arg is implicitly self (it's a method) */
  nodeptr = lua_touserdata(L, lua_upvalueindex(1));
  if(nodeptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(lua_gettop(L) == 2 && lua_isstring(L,2)) {
    const char *data = lua_tostring(L,2);
    xmlChar *enc = xmlEncodeEntitiesReentrant((*nodeptr)->doc, (xmlChar *)data);
    xmlNodeSetContent(*nodeptr, (xmlChar *)enc);
    xmlFree(enc);
    return 0;
  }
  if(lua_gettop(L) == 1) {
    xmlChar *v;
    v = xmlNodeGetContent(*nodeptr);
    if(v) {
      lua_pushstring(L, (const char *)v);
      xmlFree(v);
    }
    else lua_pushnil(L);
    return 1;
  }
  luaL_error(L,"must be called with no arguments");
  return 0;
}

/*!
\lua sibling = mtev.xmlnode:next()
\return next sibling xml node
*/
static int
mtev_lua_xmlnode_next(lua_State *L) {
  xmlNodePtr *nodeptr;
  nodeptr = lua_touserdata(L, lua_upvalueindex(1));
  if(*nodeptr) {
    xmlNodePtr *newnodeptr;
    newnodeptr = (xmlNodePtr *)lua_newuserdata(L, sizeof(*nodeptr));
    *newnodeptr = *nodeptr;
    luaL_getmetatable(L, "mtev.xmlnode");
    lua_setmetatable(L, -2);
    *nodeptr = (*nodeptr)->next;
    return 1;
  }
  return 0;
}

/*!
\lua child = mtev.xmlnode:addchild(str)
\brief Add child to the given xml node
\return child mtev.xmlnode
*/
static int
mtev_lua_xmlnode_addchild(lua_State *L) {
  xmlNodePtr *nodeptr;
  nodeptr = lua_touserdata(L, lua_upvalueindex(1));
  if(nodeptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(lua_gettop(L) == 2 && lua_isstring(L,2)) {
    xmlNodePtr *newnodeptr;
    newnodeptr = (xmlNodePtr *)lua_newuserdata(L, sizeof(*nodeptr));
    *newnodeptr = xmlNewChild(*nodeptr, NULL,
                              (xmlChar *)lua_tostring(L,2), NULL);
    luaL_getmetatable(L, "mtev.xmlnode");
    lua_setmetatable(L, -2);
    return 1;
  }
  luaL_error(L,"must be called with one argument");
  return 0;
}

/*!
\lua iter = mtev.xmlnode:children()
\return iterator over child mtev.xmlnodes
*/
static int
mtev_lua_xmlnode_children(lua_State *L) {
  xmlNodePtr *nodeptr, node, cnode;
  /* the first arg is implicitly self (it's a method) */
  nodeptr = lua_touserdata(L, lua_upvalueindex(1));
  if(nodeptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  node = *nodeptr;
  cnode = node->children;
  nodeptr = lua_newuserdata(L, sizeof(cnode));
  *nodeptr = cnode;
  luaL_getmetatable(L, "mtev.xmlnode");
  lua_setmetatable(L, -2);
  lua_pushcclosure(L, mtev_lua_xmlnode_next, 1);
  return 1;
}

/*!
\lua str = mtev.xmldoc:tostring()
\return string representation of xmldoc
*/
static int
mtev_lua_xml_tostring(lua_State *L) {
  int n;
  xmlDocPtr *docptr;
  char *xmlstring;
  n = lua_gettop(L);
  /* the first arg is implicitly self (it's a method) */
  docptr = lua_touserdata(L, lua_upvalueindex(1));
  if(docptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(n != 1) luaL_error(L, "expects no arguments, got %d", n - 1);
  mtev_conf_xml_errors_to_debug();
  xmlstring = mtev_xmlSaveToBuffer(*docptr);
  lua_pushstring(L, xmlstring);
  free(xmlstring);
  return 1;
}

/*!
\lua node = mtev.xmldoc:root()
\return mtev.xmlnode containing root of document
*/
static int
mtev_lua_xml_docroot(lua_State *L) {
  int n;
  xmlDocPtr *docptr;
  xmlNodePtr *ptr;
  n = lua_gettop(L);
  /* the first arg is implicitly self (it's a method) */
  docptr = lua_touserdata(L, lua_upvalueindex(1));
  if(docptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(n != 1) luaL_error(L, "expects no arguments, got %d", n - 1);
  ptr = lua_newuserdata(L, sizeof(*ptr));
  *ptr = xmlDocGetRootElement(*docptr);
  luaL_getmetatable(L, "mtev.xmlnode");
  lua_setmetatable(L, -2);
  return 1;
}
static int
mtev_lua_xpathiter_gc(lua_State *L) {
  struct xpath_iter *xpi;
  xpi = lua_touserdata(L, 1);
  xmlXPathFreeContext(xpi->ctxt);
  if(xpi->pobj) xmlXPathFreeObject(xpi->pobj);
  return 0;
}
static int
mtev_xmlnode_index_func(lua_State *L) {
  int n;
  const char *k;
  xmlNodePtr *udata;
  n = lua_gettop(L); /* number of arguments */
  mtevAssert(n == 2);
  if(!luaL_checkudata(L, 1, "mtev.xmlnode")) {
    luaL_error(L, "metatable error, arg1 not a mtev.xmlnode!");
  }
  udata = lua_touserdata(L, 1);
  if(!lua_isstring(L, 2)) {
    luaL_error(L, "metatable error, arg2 not a string!");
  }
  k = lua_tostring(L, 2);
  switch(*k) {
    case 'a':
      LUA_DISPATCH(attr, mtev_lua_xmlnode_attr);
      LUA_DISPATCH(attribute, mtev_lua_xmlnode_attr);
      LUA_DISPATCH(addchild, mtev_lua_xmlnode_addchild);
      break;
    case 'c':
      LUA_DISPATCH(children, mtev_lua_xmlnode_children);
      LUA_DISPATCH(contents, mtev_lua_xmlnode_contents);
      break;
    case 'n':
      LUA_DISPATCH(name, mtev_lua_xmlnode_name);
      break;
    default:
      break;
  }
  luaL_error(L, "mtev.xmlnode no such element: %s", k);
  return 0;
}

/*!
\lua mtev.parsexml(str)
\brief Parse xml string
\return mtev.xmldoc representation
*/
static int
nl_parsexml(lua_State *L) {
  xmlDocPtr *docptr, doc;
  const char *in;
  size_t inlen;

  if(lua_gettop(L) != 1) luaL_error(L, "parsexml requires one argument");

  in = lua_tolstring(L, 1, &inlen);
  mtev_conf_xml_errors_to_debug();
  doc = xmlParseMemory(in, inlen);
  if(!doc) {
    lua_pushnil(L);
    return 1;
  }

  docptr = (xmlDocPtr *)lua_newuserdata(L, sizeof(doc));
  *docptr = doc;
  luaL_getmetatable(L, "mtev.xmldoc");
  lua_setmetatable(L, -2);
  return 1;
}
static int
mtev_lua_xmldoc_gc(lua_State *L) {
  xmlDocPtr *holder;
  holder = (xmlDocPtr *)lua_touserdata(L,1);
  xmlFreeDoc(*holder);
  return 0;
}
static int
mtev_xmldoc_index_func(lua_State *L) {
  int n;
  const char *k;
  xmlDocPtr *udata;
  n = lua_gettop(L); /* number of arguments */
  mtevAssert(n == 2);
  if(!luaL_checkudata(L, 1, "mtev.xmldoc")) {
    luaL_error(L, "metatable error, arg1 not a mtev.xmldoc!");
  }
  udata = lua_touserdata(L, 1);
  if(!lua_isstring(L, 2)) {
    luaL_error(L, "metatable error, arg2 not a string!");
  }
  k = lua_tostring(L, 2);
  switch(*k) {
    case 'r':
     LUA_DISPATCH(root, mtev_lua_xml_docroot);
     break;
    case 't':
     LUA_DISPATCH(tostring, mtev_lua_xml_tostring);
     break;
    case 'x':
     LUA_DISPATCH(xpath, mtev_lua_xpath);
     break;
    default:
     break;
  }
  luaL_error(L, "mtev.xmldoc no such element: %s", k);
  return 0;
}

struct cluster_crutch {
  char *cluster_name;
  mtev_cluster_t *cluster;
};
/* \lua bool = mtev.cluster:am_i_oldest_visible_node()
\return whether the invoking code is running on the oldest node in the cluster.
*/
static int
mtev_lua_cluster_am_i_oldest_visible_node(lua_State *L) {
  int n = lua_gettop(L);
  /* the first arg is implicitly self (it's a method) */
  void *udata = lua_touserdata(L, lua_upvalueindex(1));
  if(udata != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(n != 1) luaL_error(L, "expects no arguments, got %d", n);
  struct cluster_crutch *c = (struct cluster_crutch *)udata;
  mtev_cluster_t *cluster = c->cluster ? c->cluster : mtev_cluster_by_name(c->cluster_name);
  if(cluster == NULL) luaL_error(L, "no such cluster");
  lua_pushboolean(L, mtev_cluster_am_i_oldest_visible_node(cluster));
  return 1;
}

/* \lua cnt = mtev.cluster:size()
\return the number of nodes in the cluster.
*/
static int
mtev_lua_cluster_size(lua_State *L) {
  int n = lua_gettop(L);
  /* the first arg is implicitly self (it's a method) */
  void *udata = lua_touserdata(L, lua_upvalueindex(1));
  if(udata != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(n != 1) luaL_error(L, "expects no arguments, got %d", n);
  struct cluster_crutch *c = (struct cluster_crutch *)udata;
  mtev_cluster_t *cluster = c->cluster ? c->cluster : mtev_cluster_by_name(c->cluster_name);
  if(cluster == NULL) luaL_error(L, "no such cluster");
  lua_pushinteger(L, mtev_cluster_size(cluster));
  return 1;
}

static int
mtev_cluster_index_func(lua_State *L) {
  void *udata;
  int n;
  n = lua_gettop(L); /* number of arguments */
  mtevAssert(n == 2);
  if(!luaL_checkudata(L, 1, "mtev.cluster")) {
    luaL_error(L, "metatable error, arg1 not a mtev.xmldoc!");
  }
  udata = lua_touserdata(L, 1);
  if(!lua_isstring(L, 2)) {
    luaL_error(L, "metatable error, arg2 not a string!");
  }
  const char *k = lua_tostring(L, 2);
  switch(*k) {
    case 'a':
      LUA_DISPATCH(am_i_oldest_visible_node, mtev_lua_cluster_am_i_oldest_visible_node);
      break;
    case 's':
      LUA_DISPATCH(size, mtev_lua_cluster_size);
      break;
  }
  luaL_error(L, "mtev.cluster no such element: %s", k);
  return 0;
}

/*! \lua cluster = mtev.cluster(name)
\param name name of cluster
\return a cluster object or nil if no cluster of that name is found.
*/
static int
nl_mtev_cluster(lua_State *L) {
  const char *cluster_name = luaL_checkstring(L,1);
  mtev_cluster_t *cluster = mtev_cluster_by_name(cluster_name);
  if(cluster == NULL) return 0;
  // We store the cluster name in the lua object not the reference to the mtev_cluster_t, since this
  // might be changing over time.
  char *obj = lua_newuserdata(L, sizeof(struct cluster_crutch)+strlen(cluster_name)+1);
  memset(obj, 0, sizeof(struct cluster_crutch));
  memcpy(obj + sizeof(struct cluster_crutch), cluster_name, strlen(cluster_name)+1);
  struct cluster_crutch *c = (struct cluster_crutch *)obj;
  c->cluster_name = obj + sizeof(struct cluster_crutch);
  luaL_getmetatable(L, "mtev.cluster");
  lua_setmetatable(L, -2);
  return 1;
}

static void
nl_push_ctype_mtev_cluster(lua_State *L, va_list ap) {
  mtev_cluster_t *cluster = va_arg(ap, mtev_cluster_t *);
  struct cluster_crutch *c = lua_newuserdata(L, sizeof(struct cluster_crutch));
  memset(c, 0, sizeof(struct cluster_crutch));
  c->cluster = cluster;
  luaL_getmetatable(L, "mtev.cluster");
  lua_setmetatable(L, -2);
}


/*! \lua obj = mtev.json:tostring()
\brief return a JSON-formatted string of an `mtev.json` object
\return a lua string

Returns a JSON document (as a string) representing the underlying
`mtev.json` object.
*/
static int
mtev_lua_json_tostring(lua_State *L) {
  int n;
  json_crutch **docptr;
  const char *jsonstring;
  n = lua_gettop(L);
  /* the first arg is implicitly self (it's a method) */
  docptr = lua_touserdata(L, lua_upvalueindex(1));
  if(docptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(n != 1) luaL_error(L, "expects no arguments, got %d", n - 1);
  jsonstring = mtev_json_object_to_json_string((*docptr)->root);
  lua_pushstring(L, jsonstring);
  /* jsonstring is freed with the root object later */
  return 1;
}
static int
mtev_json_object_to_luatype(lua_State *L, const mtev_json_object *o) {
  if(!o) {
    lua_pushnil(L);
    return 1;
  }
  switch(mtev_json_object_get_type(o)) {
    case mtev_json_type_null: lua_pushnil(L); break;
    case mtev_json_type_object:
    {
      int cnt = 0;
      for(struct jl_lh_entry *e = mtev_json_object_get_object_head(o);
          e; e = mtev_json_entry_next(e)) {
        cnt++;
      }
      lua_createtable(L, 0, cnt);
      mtev_json_object_object_foreach(o,key,val) {
        mtev_json_object_to_luatype(L, val);
        lua_setfield(L, -2, key);
      }
      break;
    }
    case mtev_json_type_string:
      lua_pushstring(L, mtev_json_object_get_string(o));
      break;
    case mtev_json_type_boolean:
      lua_pushboolean(L, mtev_json_object_get_boolean(o));
      break;
    case mtev_json_type_double:
      lua_pushnumber(L, mtev_json_object_get_double(o));
      break;
    case mtev_json_type_int:
    {
      int64_t i64;
      uint64_t u64;
      char istr[64];
      /* push "double" if we can accurately represent the number in a
       * double; otherwise, push a string. doubles have 52-bit mantissas.
       */
#define REPR_LUA_INT_MAX(t) (t(1) << 52)
      switch(mtev_json_object_get_int_overflow(o)) {
        case mtev_json_overflow_int:
          /* 32-bit number, can be represented accurately. */
          lua_pushnumber(L, mtev_json_object_get_int(o)); break;
        case mtev_json_overflow_int64:
          i64 = mtev_json_object_get_int64(o);
          if (i64 > -REPR_LUA_INT_MAX(INT64_C) && i64 < REPR_LUA_INT_MAX(INT64_C))
            lua_pushnumber(L, i64);
          else {
            snprintf(istr, sizeof(istr), "%" PRId64, i64);
            lua_pushstring(L, istr);
          }
          break;
        case mtev_json_overflow_uint64:
          u64 = mtev_json_object_get_uint64(o);
          if (u64 < REPR_LUA_INT_MAX(UINT64_C))
            lua_pushnumber(L, u64);
          else {
            snprintf(istr, sizeof(istr), "%" PRIu64, u64);
            lua_pushstring(L, istr);
          }
          break;
      }
      break;
    }
    case mtev_json_type_array:
    {
      int i, cnt;
      cnt = mtev_json_object_array_length(o);
      lua_createtable(L, cnt, 0);
      for(i=0;i<cnt;i++) {
        mtev_json_object_to_luatype(L, mtev_json_object_array_get_idx(o,i));
        lua_rawseti(L, -2, i+1);
      }
      break;
    }
  }
  return 1;
}

/*! \lua obj = mtev.json:document()
\brief return a lua prepresentation of an `mtev.json` object
\return a lua object (usually a table)

Returns a fair representation of the underlying JSON document
as native lua objects.
*/
static int
mtev_lua_json_document(lua_State *L) {
  int n;
  json_crutch **docptr;
  n = lua_gettop(L);
  /* the first arg is implicitly self (it's a method) */
  docptr = lua_touserdata(L, lua_upvalueindex(1));
  if(docptr != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  if(n != 1) luaL_error(L, "expects no arguments, got %d", n - 1);
  return mtev_json_object_to_luatype(L, (*docptr)->root);
}
static mtev_boolean
mtev_lua_guess_is_array(lua_State *L, int idx) {
  mtev_boolean rv = mtev_true;
  lua_pushnil(L);  /* first key */
  while(lua_next(L,idx) != 0) {
    if(lua_type(L,-2) != LUA_TNUMBER) rv = mtev_false;
    else if(lua_tointeger(L,-2) < 1) rv = mtev_false;
    lua_pop(L,1);
  }
  return rv;
}
static mtev_json_object *
mtev_lua_thing_to_json_object(lua_State *L, int idx, int limit) {
  lua_Integer v_int;
  double v_double;
  if(idx < 0) {
    idx = lua_gettop(L) + idx + 1;
  }
  if(limit == 0) return MJ_STR("[depth elided]");
  if(limit > 0) limit--;
  switch(lua_type(L, idx)) {
    case LUA_TBOOLEAN:
      return mtev_json_object_new_boolean(lua_toboolean(L,idx));
    case LUA_TSTRING:
      return mtev_json_object_new_string(lua_tostring(L,idx));
    case LUA_TNUMBER:
      v_int = lua_tointeger(L,idx);
      v_double = lua_tonumber(L,idx);
      if((double)v_int == v_double) {
        return mtev_json_object_new_int64(v_int);
      }
      return mtev_json_object_new_double(v_double);
    case LUA_TTABLE:
      if(mtev_lua_guess_is_array(L,idx)) {
        mtev_json_object *array = mtev_json_object_new_array();
        lua_pushnil(L);
        while(lua_next(L,idx) != 0) {
          int key;
          key = lua_tointeger(L,-2);
          mtev_json_object_array_put_idx(array, key-1,
                                         mtev_lua_thing_to_json_object(L,-1,limit));
          lua_pop(L,1);
        }
        return array;
      }
      else {
        mtev_json_object *table = mtev_json_object_new_object();
        lua_pushnil(L);
        while(lua_next(L,idx) != 0) {
          const char *key;
          char numkey[32];
          if(lua_type(L,-2) == LUA_TNUMBER) {
            int idx = lua_tointeger(L,-2);
            snprintf(numkey, sizeof(numkey), "%d", idx);
            key = numkey;
          }
          else {
            key = lua_tostring(L,-2);
          }
          mtev_json_object_object_add(table, key,
                                      mtev_lua_thing_to_json_object(L,-1,limit));
          lua_pop(L,1);
        }
        return table;
      }
    case LUA_TLIGHTUSERDATA:
    case LUA_TUSERDATA:
    case LUA_TFUNCTION:
    case LUA_TTHREAD:
    case LUA_TNONE:
    case LUA_TNIL:
    default:
      break;
  }
  return NULL;
}
/*! \lua jsonobj = mtev.tojson(obj, maxdepth = -1)
\brief Convert a lua object into a json doucument.
\param obj a lua object (usually a table).
\param maxdepth if specified limits the recursion.
\return an mtev.json object.

This converts a lua object, ignoring types that do not have JSON
counterparts (like userdata, lightuserdata, functions, threads, etc.).
The return is an `mtev.json` object not a string. You must invoke
the `tostring` method to convert it to a simple string.
*/
static int
nl_tojson(lua_State *L) {
  json_crutch **docptr, *doc;
  int limit = -1;
  int n = lua_gettop(L);
  if(n < 1) luaL_error(L, "tojson requires at least one argument");
  if(n == 2) {
    limit = lua_tointeger(L,2);
    lua_pop(L,1);
  }
  doc = calloc(1, sizeof(*doc));
  doc->root = mtev_lua_thing_to_json_object(L,1,limit);
  docptr = (json_crutch **)lua_newuserdata(L, sizeof(doc));
  *docptr = doc;
  luaL_getmetatable(L, "mtev.json");
  lua_setmetatable(L, -2);
  return 1;
}

// Removes wrapping around mtev_json_object structure, and leaves a
// mtev_json_object* on the lua stack, so other C functions can make
// use of it.
// (unstable, undocumented)
static int
mtev_lua_json_unwrap(lua_State *L){
  json_crutch **docptr;
  mtev_json_object **unwrapped;
  if (lua_gettop(L) != 1) luaL_error(L, "unwrap requires one argument");
  docptr = lua_touserdata(L, lua_upvalueindex(1));
  if(docptr != lua_touserdata(L, 1))
  if(docptr == NULL || (*docptr)->root == NULL) return 0;

  unwrapped = (mtev_json_object **) lua_newuserdata(L, sizeof(mtev_json_object*));
  *unwrapped = mtev_json_object_get((*docptr)->root);
  luaL_getmetatable(L, "mtev.json_object");
  lua_setmetatable(L, -2);
  return 1;
}

/*! \lua jsonobj, err, offset = mtev.parsejson(string)
\brief Convert a JSON strint to an `mtev.json`.
\param string is a JSON formatted string.
\return an mtev.json object plus errors on failure.

This converts a JSON string to a lua object.  As lua
does not support table keys with nil values, this
implementation sets them to nil and thus elides the keys.
If parsing fails nil is returned followed by the error and
the byte offset into the string where the error occurred.
*/
static int
nl_parsejson(lua_State *L) {
  json_crutch **docptr, *doc;
  const char *in;
  size_t inlen;

  if(lua_gettop(L) != 1) luaL_error(L, "parsejson requires one argument");

  in = lua_tolstring(L, 1, &inlen);
  doc = calloc(1, sizeof(*doc));
  doc->tok = mtev_json_tokener_new();
  doc->root = mtev_json_tokener_parse_ex(doc->tok, in, inlen);
  if(doc->tok->err != mtev_json_tokener_success) {
    lua_pushnil(L);
    lua_pushstring(L, mtev_json_tokener_errors[doc->tok->err]);
    lua_pushinteger(L, doc->tok->char_offset);
    mtev_json_tokener_free(doc->tok);
    if(doc->root) mtev_json_object_put(doc->root);
    free(doc);
    return 3;
  }

  docptr = (json_crutch **)lua_newuserdata(L, sizeof(doc));
  *docptr = doc;
  luaL_getmetatable(L, "mtev.json");
  lua_setmetatable(L, -2);
  return 1;
}
static int
mtev_lua_json_gc(lua_State *L) {
  json_crutch **json;
  json = (json_crutch **)lua_touserdata(L,1);
  if((*json)->tok) mtev_json_tokener_free((*json)->tok);
  if((*json)->root) mtev_json_object_put((*json)->root);
  free(*json);
  return 0;
}
static int
mtev_lua_json_object_gc(lua_State *L) {
  struct json_object **json;
  json = (struct json_object **)lua_touserdata(L,1);
  if(*json) mtev_json_object_put(*json);
  return 0;
}
static int
mtev_json_index_func(lua_State *L) {
  int n;
  const char *k;
  json_crutch **udata;
  n = lua_gettop(L); /* number of arguments */
  mtevAssert(n == 2);
  if(!luaL_checkudata(L, 1, "mtev.json")) {
    luaL_error(L, "metatable error, arg1 not a mtev.json!");
  }
  udata = lua_touserdata(L, 1);
  if(!lua_isstring(L, 2)) {
    luaL_error(L, "metatable error, arg2 not a string!");
  }
  k = lua_tostring(L, 2);
  switch(*k) {
    case 'd':
     LUA_DISPATCH(document, mtev_lua_json_document);
     break;
    case 't':
     LUA_DISPATCH(tostring, mtev_lua_json_tostring);
     break;
    case 'u':
     LUA_DISPATCH(unwrap, mtev_lua_json_unwrap);
    default:
     break;
  }
  luaL_error(L, "mtev.json no such element: %s", k);
  return 0;
}

struct spawn_info {
  pid_t pid;
  int last_errno;
  eventer_t in;
  eventer_t out;
  eventer_t err;
};

/*! \lua mtev.process = mtev.spawn(path, argv, env)
\brief Spawn a subprocess.
\param path the path to the executable to spawn
\param argv an array of arguments (first argument is the process name)
\param env an optional array of "K=V" strings.
\return an object with the mtev.process metatable set.

This function spawns a new subprocess running the binary specified as
the first argument.
*/
int nl_spawn(lua_State *L) {
  int in[2] = {-1,-1}, out[2] = {-1,-1}, err[2] = {-1,-1};
  int arg_count = 0, rv;
  const char *path;
  const char *noargs[1] = { NULL };
  const char **argv = noargs, **envp = noargs;
  struct spawn_info *spawn_info;
  posix_spawnattr_t *attr = NULL, attr_onstack;
  posix_spawn_file_actions_t *filea = NULL, filea_onstack;
  mtev_lua_resume_info_t *ri;
  int ntop = lua_gettop(L);

  ri = mtev_lua_find_resume_info(L, mtev_true);
  mtevAssert(ri);
  spawn_info = (struct spawn_info *)lua_newuserdata(L, sizeof(*spawn_info));
  memset(spawn_info, 0, sizeof(*spawn_info));
  spawn_info->pid = -1;
  luaL_getmetatable(L, "mtev.process");
  lua_setmetatable(L, -2);

  path = lua_tostring(L,1);

  /* argv */
  if(ntop > 1) {
    if(!lua_istable(L,2)) luaL_error(L, "spawn(path [,{args} [,{env}]])");
    lua_pushnil(L);  /* first key */
    while (lua_next(L, 2) != 0) arg_count++, lua_pop(L, 1);
    argv = malloc(sizeof(*argv) * (arg_count + 1));
    lua_pushnil(L);  /* first key */
    arg_count = 0;
    while (lua_next(L, 2) != 0) {
      argv[arg_count++] = lua_tostring(L, -1);
      lua_pop(L, 1);
    }
    argv[arg_count] = NULL;
  }

  /* envp */
  arg_count = 0;
  if(ntop > 2) {
    if(!lua_istable(L,3)) luaL_error(L, "spawn(path [,{args} [,{env}]])");
    lua_pushnil(L);  /* first key */
    while (lua_next(L, 3) != 0) arg_count++, lua_pop(L, 1);
    envp = malloc(sizeof(*envp) * (arg_count + 1));
    lua_pushnil(L);  /* first key */
    arg_count = 0;
    while (lua_next(L, 3) != 0) {
      envp[arg_count++] = lua_tostring(L, -1);
      lua_pop(L, 1);
    }
    envp[arg_count] = NULL;
  }

  filea = &filea_onstack;
  if(posix_spawn_file_actions_init(filea)) {
    spawn_info->last_errno = errno;
    mtevL(nldeb, "posix_spawn_file_actions_init -> %s\n", strerror(spawn_info->last_errno));
    goto err;
  }
#define PIPE_SAFE(p, idx, tfd) do { \
  if(pipe(p) < 0) { \
    spawn_info->last_errno = errno; \
    mtevL(nldeb, "pipe -> %s\n", strerror(spawn_info->last_errno)); \
    goto err; \
  } \
  if(eventer_set_fd_nonblocking(p[idx ? 0 : 1])) { \
    spawn_info->last_errno = errno; \
    mtevL(nldeb, "set nonblocking -> %s\n", strerror(spawn_info->last_errno)); \
    goto err; \
  } \
  posix_spawn_file_actions_adddup2(filea, p[idx], tfd); \
  posix_spawn_file_actions_addclose(filea, p[idx ? 0 : 1]); \
} while(0)

  PIPE_SAFE(in, 0, 0);
  PIPE_SAFE(out, 1, 1);
  PIPE_SAFE(err, 1, 2);
  attr = &attr_onstack;
  memset(attr, 0, sizeof(*attr));
  if(posix_spawnattr_init(attr)) {
    attr = NULL;
    spawn_info->last_errno = errno;
    mtevL(nldeb, "posix_spawnattr_init(%d) -> %s\n", errno, strerror(errno));
    goto err;
  }

/* This is to ensure that we get consistent behavior between
 * Solaris and Linux - they each treat failing to launch a process
 * differently, and we want the behavior to be consistent */
#if defined(POSIX_SPAWN_NOEXECERR_NP)
  short flags;
  if (posix_spawnattr_getflags(attr, &flags)) {
    mtevFatal(mtev_error, "Couldn't get flags in posix_spawnattr_init\n");
  }
  flags |= POSIX_SPAWN_NOEXECERR_NP;
  if (posix_spawnattr_setflags(attr, flags)) {
    mtevFatal(mtev_error, "Couldn't set flags in posix_spawnattr_init\n");
  }
#endif

  rv = posix_spawnp(&spawn_info->pid, path, filea, attr,
                   (char * const *)argv, (char * const *)envp);
  if(rv != 0) {
    spawn_info->last_errno = errno;
    mtevL(nldeb, "posix_spawn(%d) -> %s\n", errno, strerror(errno));
    goto err;
  }
  /* Cleanup the parent half */
  if(argv != noargs) free(argv);
  if(envp != noargs) free(envp);
  if(filea) posix_spawn_file_actions_destroy(filea);
  if(attr) posix_spawnattr_destroy(attr);
  close(in[0]); close(out[1]); close(err[1]);

#define NEWEVENT(e, ourfd, L, ri) do { \
  struct nl_slcl *cl; \
  cl = calloc(1, sizeof(*cl)); \
  cl->free = nl_extended_free; \
  cl->L = L; \
  e = eventer_alloc_fd(NULL, cl, ourfd, EVENTER_EXCEPTION); \
  cl->eptr = mtev_lua_event(L, e); \
  cl->send_size = 4096; \
  mtev_lua_register_event(ri, e); \
} while(0)

  NEWEVENT(spawn_info->in,  in[1],  L, ri);
  NEWEVENT(spawn_info->out, out[0], L, ri);
  NEWEVENT(spawn_info->err, err[0], L, ri);
  return 4;

 err:
  mtevL(nldeb, "nl_spawn -> %s\n", strerror(spawn_info->last_errno));
  if(in[0] != -1) close(in[0]);
  if(in[1] != -1) close(in[1]);
  if(out[0] != -1) close(out[0]);
  if(out[1] != -1) close(out[1]);
  if(err[0] != -1) close(err[0]);
  if(err[1] != -1) close(err[1]);
  if(argv != noargs) free(argv);
  if(envp != noargs) free(envp);
  if(filea) posix_spawn_file_actions_destroy(filea);
  if(attr) posix_spawnattr_destroy(attr);
  lua_pushinteger(L, spawn_info->last_errno);
  return 2;
}

/*! \lua thread, tid = mtev.thread_self()
*/
static int
nl_thread_self(lua_State *L) {
  lua_module_closure_t *lmc;

  lua_getglobal(L, "mtev_internal_lmc");;
  lmc = lua_touserdata(L, lua_gettop(L));
  lua_pop(L, 1);
  lua_pushinteger(L, (int) mtev_thread_id());
  lua_pushinteger(L, (lmc) ? lmc->eventer_id : -1);
  const char *thread_name = mtev_thread_getname();
  if(!thread_name) return 2;
  lua_pushstring(L, thread_name);
  return 3;
}

/*! \lua mtev.eventer_loop_concurrency()
*/
static int
nl_eventer_loop_concurrency(lua_State *L) {
  lua_pushinteger(L, eventer_loop_concurrency());
  return 1;
}

/*! \lua rv = mtev.watchdog_child_heartbeat()
\brief Heartbeat from a child process.
\return The return value of `mtev_watchdog_child_heartbeat()`
*/
static int
nl_watchdog_child_heartbeat(lua_State *L) {
  lua_pushinteger(L, mtev_watchdog_child_heartbeat());
  return 1;
}

/*! \lua timeout = mtev.watchdog_timeout()
\brief Return the watchdog timeout on the current thread.
\return A timeout in seconds, or nil if no watchdog configured.
*/
static int
nl_watchdog_timeout(lua_State *L) {
  double timeout = eventer_watchdog_timeout();
  if(timeout != 0.0)
    lua_pushnumber(L, timeout);
  else
    lua_pushnil(L);
  return 1;
}

/*! \lua pid = mtev.process:pid()
\brief Return the process id of a spawned process.
\return The process id.
*/
static int
mtev_lua_process_pid(lua_State *L) {
  struct spawn_info *spawn_info;
  /* the first arg is implicitly self (it's a method) */
  spawn_info = lua_touserdata(L, lua_upvalueindex(1));
  if(spawn_info != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  lua_pushinteger(L, spawn_info->pid);
  return 1;
}

static int
mtev_lua_process_kill_internal(lua_State *L, pid_t pid) {
  struct spawn_info *spawn_info;
  int signal_no = SIGTERM;
  /* the first arg is implicitly self (it's a method) */
  spawn_info = lua_touserdata(L, lua_upvalueindex(1));
  if(spawn_info != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");

  if(lua_gettop(L) > 1) {
    signal_no = lua_tointeger(L,2);
  }
  if(spawn_info->pid <= 0) {
    lua_pushboolean(L, 0);
    lua_pushinteger(L, ESRCH);
  }
  else {
    int rv = kill(pid, signal_no);
    lua_pushboolean(L, (rv == 0));
    if(rv < 0) lua_pushinteger(L, errno);
    else lua_pushnil(L);
  }
  return 2;
}

/*! \lua success, errno = mtev.process:kill(signal)
\brief Kill a spawned process.
\param signal the integer signal to deliver, if omitted `SIGTERM` is used.
\return true on success or false and an errno on failure.
*/
static int
mtev_lua_process_kill(lua_State *L) {
  struct spawn_info *spawn_info;
  spawn_info = lua_touserdata(L, lua_upvalueindex(1));
  if(spawn_info != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  return mtev_lua_process_kill_internal(L, spawn_info->pid);
}

/*! \lua success, errno = mtev.process:pgkill(signal)
\brief Kill a spawned process group.
\param signal the integer signal to deliver, if omitted `SIGTERM` is used.
\return true on success or false and an errno on failure.
*/
static int
mtev_lua_process_pgkill(lua_State *L) {
  struct spawn_info *spawn_info;
  spawn_info = lua_touserdata(L, lua_upvalueindex(1));
  if(spawn_info != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  return mtev_lua_process_kill_internal(L, 0 - spawn_info->pid);
}

#define MAKE_LUA_WFUNC_BOOL(name) static int \
mtev_lua_process_##name(lua_State *L) { \
  int status = luaL_checkinteger(L, 1); \
  lua_pushboolean(L, name(status)); \
  return 1; \
}

#define MAKE_LUA_WFUNC_INT(name) static int \
mtev_lua_process_##name(lua_State *L) { \
  int status = luaL_checkinteger(L, 1); \
  lua_pushinteger(L, name(status)); \
  return 1; \
}

/*! \lua mtev.WIFEXITED(status)
\param status a process status returned by `mtev.process:wait(timeout)`
\return true if the process terminated normally
*/
MAKE_LUA_WFUNC_BOOL(WIFEXITED)

/*! \lua mtev.WIFSIGNALED(status)
\param status a process status returned by `mtev.process:wait(timeout)`
\return true if the process terminated due to receipt of a signal
*/
MAKE_LUA_WFUNC_BOOL(WIFSIGNALED)

/*! \lua mtev.WCOREDUMP(status)
\param status a process status returned by `mtev.process:wait(timeout)`
\return true if the process produced a core dump

Only valid if `mtev.WIFSIGNALED(status)` is also true.
*/
MAKE_LUA_WFUNC_BOOL(WCOREDUMP)

/*! \lua mtev.WIFSTOPPED(status)
\param status a process status returned by `mtev.process:wait(timeout)`
\return true if the process was stopped, but not terminated
*/
MAKE_LUA_WFUNC_BOOL(WIFSTOPPED)

/*! \lua mtev.WIFCONTINUED(status)
\param status a process status returned by `mtev.process:wait(timeout)`
\return true if the process has continued after a job control stop, but not terminated
*/
MAKE_LUA_WFUNC_BOOL(WIFCONTINUED)

/*! \lua mtev.WEXITSTATUS(status)
\param status a process status returned by `mtev.process:wait(timeout)`
\return the exit status of the process

Only valid if `mtev.WIFEXITED(status)` is true.
*/
MAKE_LUA_WFUNC_INT(WEXITSTATUS)

/*! \lua mtev.WTERMSIG(status)
\param status a process status returned by `mtev.process:wait(timeout)`
\return the number of the signal that caused the termination of the process

Only valid if `mtev.WIFSIGNALED(status)` is true.
*/
MAKE_LUA_WFUNC_INT(WTERMSIG)

/*! \lua mtev.WSTOPSIG(status)
\param status a process status returned by `mtev.process:wait(timeout)`
\return the number of the signal that caused the process to stop

Only valid if `mtev.WIFSTOPPED(status)` is true.
*/
MAKE_LUA_WFUNC_INT(WSTOPSIG)

static int mtev_lua_process_wait_ex(struct nl_slcl *, mtev_boolean);

static int
mtev_lua_process_wait_wakeup(eventer_t e, int mask, void *vcl, struct timeval *now) {
  (void)mask;
  mtev_lua_resume_info_t *ci;
  struct nl_slcl *cl = vcl;
  int rv;

  ci = mtev_lua_find_resume_info(cl->L, mtev_false);
  if(!ci) { mtev_lua_eventer_cl_cleanup(e); return 0; }
  mtev_lua_deregister_event(ci, e, 0);

  if(compare_timeval(cl->deadline, *now) < 0) cl->deadline.tv_sec = 0;
  rv = mtev_lua_process_wait_ex(cl, mtev_false);
  free(cl);
  if(rv >= 0) mtev_lua_lmc_resume(ci->lmc, ci, rv);
  return 0;
}
static int
mtev_lua_process_wait_ex(struct nl_slcl *cl, mtev_boolean needs_yield) {
  int rv, status;
  mtev_lua_resume_info_t *ci;
  lua_State *L = cl->L;
  /* the first arg is implicitly self (it's a method) */
  mtevL(nldeb, "lua_process_wait_ex(pid: %d)\n", cl->spawn_info->pid);
  if(cl->spawn_info->pid == -1) {
    if(cl->spawn_info->last_errno != 0) {
      /* emulate a delayed exit with exit code 127 */
      lua_pushinteger(L, 127 << 8);
      lua_pushnil(L);
      return 2;
    }
    lua_pushnil(L);
    lua_pushinteger(L, EINVAL);
    return 2;
  }
  while((rv = waitpid(cl->spawn_info->pid, &status, WNOHANG)) == -1 && errno == EINTR);
  if(rv == cl->spawn_info->pid) {
    lua_pushinteger(L, status);
    return 1;
  }
  if(rv == 0 && cl->deadline.tv_sec != 0) {
    struct nl_slcl *newcl;
    newcl = calloc(1, sizeof(*newcl));
    newcl->L = L;
    newcl->free = nl_extended_free;
    newcl->spawn_info = cl->spawn_info;
    newcl->deadline = cl->deadline;
    eventer_t e = eventer_in_s_us(mtev_lua_process_wait_wakeup, newcl, 0, 20000);

    ci = mtev_lua_find_resume_info(L, mtev_true);
    mtevAssert(ci);
    mtev_lua_register_event(ci, e);
    eventer_add(e);
    if(needs_yield) {
     return mtev_lua_yield(ci, 0);
    }
    return -1;
  }
  if(rv == 0) errno = ETIMEDOUT;
  lua_pushnil(L);
  lua_pushinteger(L, errno);
  return 2;
}

/*! \lua status, errno = mtev.process:wait(timeout)
\brief Attempt to wait for a spawned process to terminate.
\param timeout an option time in second to wait for exit (0 in unspecified).
\return The process status and an errno if applicable.

Wait for a process (using `waitpid` with the `WNOHANG` option) to terminate
and return its exit status.  If the process has not exited and the timeout
has elapsed, the call will return with a nil value for status.  The lua
subsystem exists within a complex system that might handle process in different
ways, so it does not rely on `SIGCHLD` signal delivery and instead polls the
system using `waitpid` every 20ms.
*/
static int
mtev_lua_process_wait(lua_State *L) {
  struct spawn_info *spawn_info;
  struct nl_slcl dummy;
  memset(&dummy, 0, sizeof(dummy));
  /* the first arg is implicitly self (it's a method) */
  spawn_info = lua_touserdata(L, lua_upvalueindex(1));
  if(spawn_info != lua_touserdata(L, 1))
    luaL_error(L, "must be called as method");
  double timeout = lua_tonumber(L, 2);
  if(timeout <= 0) timeout = 0;
  else {
    mtev_gettimeofday(&dummy.deadline, NULL);
    dummy.deadline.tv_sec += (int)timeout;
    dummy.deadline.tv_usec += (int)((timeout - (double)(int)timeout) * 1000000.0);
    if(dummy.deadline.tv_usec > 1000000) {
      dummy.deadline.tv_usec -= 1000000;
      dummy.deadline.tv_sec += 1;
    }
  }

  dummy.L = L;
  dummy.spawn_info = spawn_info;

  return mtev_lua_process_wait_ex(&dummy, mtev_true);
}


static int
mtev_lua_process_index_func(lua_State *L) {
  int n;
  const char *k;
  struct spawn_info *udata;
  n = lua_gettop(L); /* number of arguments */
  mtevAssert(n == 2);
  if(!luaL_checkudata(L, 1, "mtev.process")) {
    luaL_error(L, "metatable error, arg1 not a mtev.process!");
  }
  udata = lua_touserdata(L, 1);
  if(!lua_isstring(L, 2)) {
    luaL_error(L, "metatable error, arg2 not a string!");
  }
  k = lua_tostring(L, 2);
  switch(*k) {
    case 'k':
      LUA_DISPATCH(kill, mtev_lua_process_kill);
      break;
    case 'p':
      LUA_DISPATCH(pid, mtev_lua_process_pid);
      LUA_DISPATCH(pgkill, mtev_lua_process_pgkill);
      break;
    case 'w':
      LUA_DISPATCH(wait, mtev_lua_process_wait);
      break;
    default:
      break;
  }
  luaL_error(L, "mtev.process no such element: %s", k);
  return 0;
}

static int
mtev_lua_eventer_gc(lua_State *L) {
  eventer_t *eptr, e;

  eptr = (eventer_t *)lua_touserdata(L,1);
  /* Simply null it out so if we try to use it, we'll notice */
  e = *eptr;
  *eptr = NULL;
  if(e) {
    mtev_lua_eventer_cl_cleanup(e);
    eventer_free(e);
  }
  return 0;
}

static int
mtev_lua_process_gc(lua_State *L) {
  struct spawn_info *spawn_info;

  spawn_info = (struct spawn_info *)lua_touserdata(L,1);
  if(spawn_info->pid != -1) {
    int status;
    if(spawn_info->pid != waitpid(spawn_info->pid, &status, WNOHANG)) {
      kill(spawn_info->pid, SIGKILL);
      while(waitpid(spawn_info->pid, &status, 0) == -1 && errno == EINTR);
    }
    spawn_info->pid = -1;
  }
  return 0;
}

static void
mtev_lua_free_table(void *vtable) {
  mtev_lua_table_t* table = vtable;
  mtev_hash_destroy(&table->string_keys, free, mtev_lua_free_data);
  mtev_hash_destroy(&table->int_keys, free, mtev_lua_free_data);
  free(table);
}

static void
mtev_lua_free_data(void *vdata) {
  lua_data_t* data = vdata;
  if(data == NULL) return;
  switch(data->lua_type){
    case(LUA_TSTRING):
      free(data->value.string);
      break;
    case(LUA_TTABLE):
      mtev_lua_free_table(data->value.table);
    break;
  }

  free(data);
}

static mtev_lua_table_t*
mtev_lua_serialize_table(lua_State *L, int index) {
  mtev_lua_table_t *table;
  lua_data_t *value;
  lua_Number number_key;
  char* number_key_str;
  const char* string_key;
  size_t string_key_len;

  if(index < 0) {
    index = lua_gettop(L) + index + 1;
  }

  lua_getfield (L, index, "serialize"); // -1: serialize
  if(lua_isfunction(L, -1)) {

    lua_pushvalue(L, index); // -2: serialize -1: self
    lua_call(L, 1, 1); // -1: result
    if(!lua_istable(L, -1)) {
      luaL_error(L, "serialize() must return a table\n");
      return NULL;
    }
    return mtev_lua_serialize_table(L, -1);
  } else {
    lua_pop(L, 1);
  }

  table = calloc(1, sizeof(mtev_lua_table_t));
  mtev_hash_init(&table->int_keys);
  mtev_hash_init(&table->string_keys);
  lua_pushnil(L); // first key
  while (lua_next(L, index) != 0) {
    int key_index = lua_gettop(L) - 1;
    value = mtev_lua_serialize(L, -1);

    if(lua_isnumber(L, key_index)) {
      number_key = lua_tonumber(L, key_index);
      number_key_str = malloc(sizeof(lua_Number));
      memcpy(number_key_str, &number_key, sizeof(lua_Number));
      mtev_hash_store(&table->int_keys, number_key_str, sizeof(lua_Number), value);

    } else if(lua_isstring(L, key_index)) {
      string_key = lua_tolstring(L, key_index, &string_key_len);
      mtev_hash_store(&table->string_keys, strdup(string_key), string_key_len, value);
    } else {
      mtev_lua_free_table(table);
      luaL_error(L, "Cannot serialize tables with anything but strings and numbers as keys, got %s instead\n", lua_typename(L, lua_type(L, key_index)));
      return NULL;
    }

    lua_pop(L, 1); //remove value, keep key for next iteration
    lua_settop(L, index+1); // remove everything above the last key
  }

  return table;
}

static lua_data_t*
mtev_lua_serialize(lua_State *L, int index){
  lua_data_t *data;
  int type;
  type = lua_type(L, index);

  if(type == LUA_TNIL) {
    return NULL;
  }

  data = calloc(1, sizeof(lua_data_t));
  data->lua_type = type;

  switch(type){
    case(LUA_TNUMBER):
      data->value.number = lua_tonumber(L, index);
      break;
    case(LUA_TSTRING):
      data->value.string = strdup(lua_tostring(L, index));
      break;
    case(LUA_TBOOLEAN):
      data->value.boolean = lua_toboolean(L, index);
      break;
    case(LUA_TTABLE):
      data->value.table = mtev_lua_serialize_table(L, index);
      break;
    default:
      free(data);
      data = NULL;
      mtevL(nldeb, "Cannot serialize unsupported lua type %d\n", type);
  }

  return data;
}

void
mtev_lua_deserialize_table(lua_State *L, mtev_lua_table_t *table){
  lua_Number number_key;
  mtev_hash_iter int_iter = MTEV_HASH_ITER_ZERO;
  mtev_hash_iter str_iter = MTEV_HASH_ITER_ZERO;

  lua_createtable(L, 0, mtev_hash_size(&table->string_keys) + mtev_hash_size(&table->int_keys));

  while(mtev_hash_adv(&table->int_keys, &int_iter)) {
    number_key = *(lua_Number*)int_iter.key.ptr;
    lua_pushnumber(L, number_key);
    mtev_lua_deserialize(L, (lua_data_t *)int_iter.value.ptr);
    lua_settable(L, -3);
  }

  while(mtev_hash_adv(&table->string_keys, &str_iter)) {
    lua_pushstring(L, str_iter.key.str);
    mtev_lua_deserialize(L, (lua_data_t *)str_iter.value.ptr);
    lua_settable(L, -3);
  }
}

void
mtev_lua_deserialize(lua_State *L, const lua_data_t *data){
  if(!data) {
    lua_pushnil(L);
    return;
  }
  switch(data->lua_type){
    case(LUA_TNUMBER):
      lua_pushnumber(L, data->value.number);
      break;
    case(LUA_TSTRING):
      lua_pushstring(L, data->value.string);
      break;
    case(LUA_TBOOLEAN):
      lua_pushboolean(L, data->value.boolean);
      break;
    case(LUA_TTABLE):
      mtev_lua_deserialize_table(L, data->value.table);
      break;
    case(LUA_TNIL): // we already returned NULL
    default:
      lua_pushnil(L);
      mtevL(nlerr, "Cannot deserialize unsupported lua type %d (offering nil)\n", data->lua_type);
  }
}
/*! \lua mtev.shared_notify(key,value)
    \brief Enqueue (via serialization) a value in some globally named queue.
    \param key must be string naming the queue
    \param value is a lua value simple enough to serialize (no functions, udata, etc.)
    \return none

    This function allows communication across lua states.
*/
struct shared_queue_node {
  void *data;
  struct shared_queue_node *next;
};
struct shared_queue {
  struct shared_queue_node *head;
  struct shared_queue_node *tail;
};
static int
nl_shared_waitfor_notify(lua_State *L) {
  void* vsq;
  struct shared_queue *sq = NULL;
  struct shared_queue_node *node = NULL;
  lua_data_t *data;
  size_t key_len;
  const char *key;
  if(lua_gettop(L) != 2 || !lua_isstring(L,1))
    return luaL_error(L, "bad parameters to mtev.shared_notify(str, value)");
  key = lua_tolstring(L, 1, &key_len);

  data = mtev_lua_serialize(L, 2);
  node = calloc(1, sizeof(*node));
  node->data = data;

  pthread_mutex_lock(&shared_queues_mutex);
  if(!mtev_hash_retrieve(&shared_queues, key, key_len, &vsq)) {
    sq = calloc(1, sizeof(*sq));
    mtev_hash_store(&shared_queues, strdup(key), key_len, sq);
  }
  else {
    sq = vsq;
  }
  if(lua_isnil(L,2)) {
    node->data = NULL;
    mtev_lua_free_data(data);
  }
  if(sq->tail) {
    sq->tail->next = node;
  }
  else {
    sq->head = node;
  }
  sq->tail = node;
  pthread_mutex_unlock(&shared_queues_mutex);

  return 0;
}
/*! \lua mtev.shared_waitfor(key, timeout)
    \brief Retrieve (via deserialization) a value in some globally named queue.
    \param key must be string naming the queue
    \param timeout number of sections to wait before returning nil
    \return a lua value or nil

    This function allows communication across lua states.
*/
struct shared_queues_wait {
  eventer_t e;
  mtev_lua_resume_info_t *ci;
  lua_State *L;
  char *key;
  bool forever;
  struct timeval deadline;
};
static void shared_queues_wait_free(struct shared_queues_wait *sqw) {
  free(sqw->key);
  free(sqw);
}

static struct shared_queue_node *
nl_shared_queues_dequeue(const char *key, size_t key_len) {
  void *vsq;
  struct shared_queue *sq = NULL;
  struct shared_queue_node *node = NULL;
  pthread_mutex_lock(&shared_queues_mutex);
  if(!mtev_hash_retrieve(&shared_queues, key, key_len, &vsq)) {
    sq = calloc(1, sizeof(*sq));
    mtev_hash_store(&shared_queues, strdup(key), key_len, sq);
  }
  else {
    sq = vsq;
  }

  if(sq->head) {
    node = sq->head;
    sq->head = sq->head->next;
    if(sq->head == NULL) sq->tail = NULL;
  }
  pthread_mutex_unlock(&shared_queues_mutex);

  return node;
}

static int
nl_shared_waitfor_wakeup(eventer_t e, int mask, void *closure, struct timeval *now) {
  (void)mask;
  struct shared_queues_wait *crutch = (struct shared_queues_wait *)closure;
  struct shared_queue_node *node = nl_shared_queues_dequeue(crutch->key, strlen(crutch->key));

  if(!crutch->ci) {
    mtev_lua_eventer_cl_cleanup(e);
  }
  else {
    mtev_lua_deregister_event(crutch->ci, e, 0);
    if(node) {
      if(node->data) {
        mtev_lua_deserialize(crutch->L, node->data);
        mtev_lua_free_data((lua_data_t *)node->data);
      }
      else {
        lua_pushnil(crutch->L);
      }
      mtev_lua_lmc_resume(crutch->ci->lmc, crutch->ci, 1);
    }
    else {
      if(crutch->forever) {
        crutch->deadline = *now;
        crutch->deadline.tv_sec += 10000;
      }
      if(compare_timeval(*now, crutch->deadline) < 0) {
        crutch->e = eventer_alloc_timer(nl_shared_waitfor_wakeup, crutch, &crutch->deadline);
        mtev_lua_register_event(crutch->ci, crutch->e);
        eventer_add(crutch->e);
      }
      else {
        lua_pushnil(crutch->L);
        mtev_lua_lmc_resume(crutch->ci->lmc, crutch->ci, 1);
      }
    }
    free(node);
  }
  shared_queues_wait_free(crutch);
  return 0;
}

static int
nl_shared_waitfor(lua_State *L) {
  struct shared_queue_node *node = NULL;
  size_t key_len;
  const char *key;
  double timeout = -1;
  if(lua_gettop(L) > 2 || !lua_isstring(L,1) || (lua_gettop(L) == 2 && !lua_isnumber(L,2)))
    return luaL_error(L, "bad parameters to mtev.shared_waitfor(str [, timeout])");
  key = lua_tolstring(L, 1, &key_len);
  if(lua_gettop(L) == 2) {
    timeout = lua_tonumber(L, 2);
  }

  node = nl_shared_queues_dequeue(key, key_len);

  if(node) {
    if(node->data) {
      mtev_lua_deserialize(L, (lua_data_t *)node->data);
      mtev_lua_free_data((lua_data_t *)node->data);
    }
    else {
      lua_pushnil(L);
    }
    free(node);
    return 1;
  }

  /* There is no item yet, so we need to wait for one assuming timeout is non-zero */
  if(timeout == 0) return 0;

  mtev_lua_resume_info_t *ci;
  ci = mtev_lua_find_resume_info(L, mtev_true); /* we cannot suspend without context */

  struct shared_queues_wait *crutch = calloc(1, sizeof(*crutch));
  double ntimeout = timeout < 0 ? 10000 : timeout;
  /* If the timeout is < 0, then there is an infinite wait...
   * However we use the scheduled timeout to trigger a wakeup by externally
   * adjusting the timeout to "now" when something shows up to notify upon.
   * So, we need a timeout and thus choose an arbitrarily long timeout of
   * 10000 seconds.
   */
  struct timeval diff =
    { .tv_sec = floor(ntimeout), .tv_usec = fmod(ntimeout, 1) * 1000000 };
  mtev_gettimeofday(&crutch->deadline, NULL);
  add_timeval(crutch->deadline, diff, &crutch->deadline);
  crutch->ci = ci;
  crutch->L = L;
  crutch->forever = (timeout < 0);
  crutch->key = mtev_strndup(key, key_len);
  crutch->e = eventer_alloc_timer(nl_shared_waitfor_wakeup, crutch, &crutch->deadline);
  mtev_lua_register_event(ci, crutch->e);
  eventer_add(crutch->e);
  return mtev_lua_yield(ci, 0);
}
/*! \lua mtev.shared_set(key,value)
    \brief Store (via serialization) a value at some globally named key.
    \param key must be string
    \param value is a lua value simple enough to serialize (no functions, udata, etc.)
    \return none

    This function allows communication across lua states via mutex-protected storage
    of values.
*/
static int
nl_shared_set(lua_State *L) {
  void* vdata;
  lua_data_t *data;
  size_t key_len;
  const char *key;
  if(lua_gettop(L) != 2 || !lua_isstring(L,1))
    return luaL_error(L, "bad parameters to mtev.shared_set(str, str)");
  key = lua_tolstring(L, 1, &key_len);

  data = mtev_lua_serialize(L, 2);
  pthread_mutex_lock(&shared_table_mutex);
  if(!mtev_hash_retrieve(&shared_table, key, key_len, &vdata)) {
    if(data != NULL) {
      mtev_hash_store(&shared_table, strdup(key), key_len, data);
    }
  } else {
    if(lua_isnil(L,2)) {
      mtev_hash_delete(&shared_table, key, key_len, free, mtev_lua_free_data);
      free(data);
    } else {
      mtev_hash_replace(&shared_table, strdup(key), key_len, data, free, mtev_lua_free_data);
    }
  }
  pthread_mutex_unlock(&shared_table_mutex);

  return 0;
}
/*! \lua mtev.shared_get(key)
    \brief Retrieve (via deserialization) a value at some globally named key.
    \param key must be string
    \return a lua value or nil

    This function allows communication across lua states via mutex-protected storage
    of values.
*/
static int
nl_shared_get(lua_State *L) {
  lua_data_t *data;
  size_t len;
  const char *key;
  if(lua_gettop(L) != 1 || !lua_isstring(L,1))
    return luaL_error(L, "bad parameters to mtev.shared_get(str)");
  key = lua_tolstring(L, 1, &len);
  pthread_mutex_lock(&shared_table_mutex);
  if(!mtev_hash_retrieve(&shared_table, key, len, (void**)&data)) {
    lua_pushnil(L);
  } else {
    mtev_lua_deserialize(L, data);
  }
  pthread_mutex_unlock(&shared_table_mutex);

  return 1;
}

/*! \lua seq = mtev.shared_seq(keyname)
\brief returns a sequence number that is increasing across all mtev-lua states and coroutines
\param keyname the globally unique name of the sequence to return and post-increment.
\return seq the sequence number
*/
static int
nl_shared_seq(lua_State *L) {
  void *vseq;
  const char *key = luaL_checkstring(L,1);
  while(1) {
    if(mtev_hash_retrieve(shared_seq_table, key, strlen(key), &vseq)) {
      lua_pushnumber(L, ck_pr_faa_64((uint64_t *)vseq, 1));
      return 1;
    }
    uint64_t *seq = calloc(1, sizeof(*seq));
    char *key_copy = strdup(key);
    if(!mtev_hash_store(shared_seq_table, key_copy, strlen(key_copy), seq)) {
      free(key_copy);
      free(seq);
    }
  }
}



/*! \lua sem = mtev.semaphore(name, value)
\brief initializes semaphore with a given initial value, and returns a pointer to the semaphore object.
\param name of the semaphore
\param value initial semaphore value used if not already initialized

If a semaphore with the same name already exists, no initialization takes place, and the second argument is ignored.

Semaphores are a way to synchronize actions between different lua states.

Example:
```lua
sem = mtev.semaphore("my-first-semaphore", 10)
sem:acquire()
-- ... do something while holding the lock
sem:release()
```
*/

static int
nl_semaphore(lua_State *L) {
  int64_t val, *valp = NULL, **valpp;
  const char *key = luaL_checkstring(L,1);
  val = luaL_checkinteger(L,2);
retry:
  if (!mtev_hash_retrieve(semaphore_table, key, strlen(key), (void**) &valp)) {
    /* initialize new semaphore counter */
    valp = calloc(1, sizeof(val));
    *valp = val;
    if(!mtev_hash_store(semaphore_table, key, strlen(key), valp)) {
      /* lost race */
      free(valp);
      valp = NULL;
      goto retry;
    }
  }
  /* return new semaphore object */
  valpp = lua_newuserdata(L, sizeof(valp));
  *valpp = valp;
  luaL_getmetatable(L, "mtev.semaphore");
  lua_setmetatable(L, -2);
  return 1;
}

/*! \lua semaphore:try_acquire()
\brief returns true of the semaphore lock could be acquired, false if not.
*/
static int
mtev_lua_semaphore_try_acquire(lua_State *L) {
  int64_t **valpp, old;
  int lua_rc = 0;
  valpp = luaL_checkudata(L, 1, "mtev.semaphore");
  while((old = ck_pr_load_64((uint64_t*) *valpp)) > 0) {
    if(ck_pr_cas_64((uint64_t*) *valpp, (uint64_t) old, (uint64_t) old-1)) {
      lua_rc = 1;
      break;
    }
  }
  lua_pushboolean(L, lua_rc);
  return 1;
}

/*! \lua semaphore:release()
\brief release the semaphore lock.
*/
static int
mtev_lua_semaphore_release(lua_State *L) {
  int64_t **valpp = luaL_checkudata(L, 1, "mtev.semaphore");
  ck_pr_inc_64((uint64_t*) *valpp);
  return 0;
}

static int
mtev_lua_semaphore_index_func(lua_State *L) {
  if(lua_gettop(L) != 2) {
    return luaL_error(L, "Illegal use of lua semaphore");
  }
  void* udata = luaL_checkudata(L, 1, "mtev.semaphore");
  const char* k = luaL_checkstring(L, 2);
  const char* key = udata;
  switch(*k) {
  case 'a':
    if(!strcmp(k, "acquire")) {
      lua_getglobal(L, "mtev");
      lua_getfield(L, -1, "__semaphore_acquire");
      return 1;
    }
    break;
  case 't':
    LUA_DISPATCH(try_acquire, mtev_lua_semaphore_try_acquire);
    break;
  case 'r':
    LUA_DISPATCH(release, mtev_lua_semaphore_release);
    break;
  }
  (void) key;
  return luaL_error(L, "mtev.semaphore no such element: %s", k);
}

/*! \lua mtev.runtime_defunct(f)
 *  \brief Determines in the currrent runtime is defunct (you should end your work)
 *  \param f an optional callback function to call when a state goes defunct.
 *  \returns true or false
 */
static int
nl_runtime_defunct(lua_State *L) {
  mtev_lua_resume_info_t *ci = mtev_lua_find_resume_info(L, mtev_false);
  if(!ci) luaL_error(L, "unmanaged runtime!");
  mtev_lua_validate_lmc(ci->lmc);
  bool has_func = (lua_gettop(L) > 0);
  if(has_func && !lua_isfunction(L,1)) {
    luaL_error(L, "mtev.runtime_defunct first art not a function");
  }

  if(ci->lmc->wants_restart) {
    lua_pushvalue(L, 1);
    lua_pcall(L, 0, 0, 0);
    lua_pushboolean(L, 1);
    return 1;
  }
  lua_getglobal(L, "mtev");
  lua_pushvalue(L, 1);
  lua_setfield(L, -2, "__defunct_cb");
  lua_pushboolean(L, 0);
  return 1;
}

/*! \lua mtev.runtime_reload()
 *  \brief Forces a generational counter bump so that a new runtime will be created.
 */
static int
nl_runtime_reload(lua_State *L) {
  (void)L;
  mtev_lua_trigger_reload_dyn();
  return 0;
}

static int
cancel_coro_cb(eventer_t e, int mask, void *closure, struct timeval *now) {
  (void)e;
  (void)mask;
  (void)now;
  mtev_lua_resume_info_t *ci = closure;
  mtev_lua_cancel_coro(ci);
  return 0;
}
/*! \lua mtev.cancel_coro()
*/
static int
nl_cancel_coro(lua_State *L) {
  mtev_lua_resume_info_t *ci;
  lua_State *co = NULL;
  if(lua_isstring(L,1)) {
    const char *ptraddrstr = lua_tostring(L,1);
    if(ptraddrstr) {
      char *endptr;
      if(!strncasecmp(ptraddrstr, "0x", 2)) {
        uintptr_t ptr = strtoull(ptraddrstr+2, &endptr, 16);
        if(*endptr == '\0') co = (lua_State *)ptr;
      } else {
        uintptr_t ptr = strtoull(ptraddrstr, &endptr, 10);
        if(*endptr == '\0') co = (lua_State *)ptr;
      }
    }
    if(co == NULL) luaL_error(L, "bad argument to mtev.cancel_coro");
  } else {
    co = lua_tothread(L,1);
  }
  ci = mtev_lua_find_resume_info_any_thread(co);
  if(!ci) return 0;
  if(pthread_equal(ci->lmc->owner, pthread_self())) {
    mtev_lua_cancel_coro(ci);
    return 0;
  }
  if(eventer_is_loop(ci->lmc->owner) < 0)
    luaL_error(L, "coroutine not in event loop");
  eventer_t e = eventer_in_s_us(cancel_coro_cb, ci, 0, 0);
  eventer_set_owner(e, ci->lmc->owner);
  eventer_add(e);
  return 0;
}

/*! \lua details = mtev.uname()
\brief Returns info from the uname libc call
\return A table resembling the `struct utsname`
*/
static int
nl_uname(lua_State *L) {
  struct utsname uts;
  if(uname(&uts) < 0) {
    return 0;
  }
  lua_createtable(L, 0, 5);
  lua_pushstring(L, uts.sysname);
  lua_setfield(L, -2, "sysname");
  lua_pushstring(L, uts.nodename);
  lua_setfield(L, -2, "nodename");
  lua_pushstring(L, uts.release);
  lua_setfield(L, -2, "release");
  lua_pushstring(L, uts.version);
  lua_setfield(L, -2, "version");
  lua_pushstring(L, uts.machine);
  lua_setfield(L, -2, "machine");
  return 1;
}

/*! \lua address = mtev.getip_ipv4([tgt])
\brief Returns the local address of a connection to tgt.
\param tgt an IP address to target (default 8.8.8.8)
\return address  - IP address as a string
*/
static int
nl_getip_ipv4(lua_State *L) {
  struct in_addr tgt, me;
  memset(&tgt, 8, sizeof(tgt));
  if(lua_gettop(L) > 0) {
    const char *tgtstr = luaL_checkstring(L,1);
    if(inet_pton(AF_INET, tgtstr, &tgt) != 1) {
      luaL_error(L, "invalid IP address passed");
    }
  }
  if(mtev_getip_ipv4(tgt, &me) != 0) {
    luaL_error(L, "failed to determine local IP");
  }
  char ipstr[INET_ADDRSTRLEN];
  if(inet_ntop(AF_INET, &me, ipstr, INET_ADDRSTRLEN) == NULL) {\
    luaL_error(L, "error formatting local IP");
  }
  lua_pushstring(L, ipstr);
  return 1;
}

/*! \lua ipstr, family = mtev.getaddrinfo(hostname)
\brief Resolves host name using the OS provided getaddrinfo function
\return ipstr  - IP address represented as a string
\return family - "inet" or "inet6" depending on whether the returned address is IPv4, IPv6

In particular this will respect the /etc/host entries.

In the case of error, we return `false, errormsg`
*/
static int
nl_getaddrinfo(lua_State *L) {
  // TODO: Support ipv6 results
  if(lua_gettop(L) != 1) {
    return luaL_error(L, "mtev.getaddrinfo expects exactly one argument");
  }
  const char *host = luaL_checkstring(L, 1);
  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0;
  hints.ai_protocol = 0;
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;
  struct addrinfo *result = NULL;
  int rc = getaddrinfo(host, NULL, &hints, &result);
  if(rc != 0) {
    lua_pushboolean(L, 0);
    lua_pushfstring(L, "getaddrinfo(%s) failed: %s", host, gai_strerror(rc));
    if(result) freeaddrinfo(result);
    return 2;
  }
  if(result == NULL) {
    lua_pushboolean(L, 0);
    lua_pushfstring(L, "not found");
    freeaddrinfo(result);
    return 2;
  }
  // getaddrinfo returns a list of results. We just pick the first here.
  char ipstr[INET_ADDRSTRLEN] = "";
  struct sockaddr_in *in4 = (struct sockaddr_in *) result->ai_addr;
  if(inet_ntop(AF_INET, &(in4->sin_addr), ipstr, INET_ADDRSTRLEN) == NULL) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "error decoding address");
    freeaddrinfo(result);
    return 2;
  }
  freeaddrinfo(result);
  lua_pushstring(L, ipstr);
  // we only support family=inet at the moment (IPv4)
  lua_pushstring(L, "inet");
  return 2;
}

/*! \lua rc, family, addr = mtev.inet_pton(address)
\brief Wrapper around inet_pton(3). Can be used to validate IP addresses and detect the address family (IPv4,IPv6)
\param address to parse
\return rc true if address is a valid IP address, false otherwise
\return family address family of the address, either "inet" or "inet6".
\return addr struct in_addr as udata

*/

static int
nl_inet_pton(lua_State *L) {
  if(lua_gettop(L) != 1) {
    return luaL_error(L, "mtev.inet_pton expects exactly one argument");
  }
  const char *host = luaL_checkstring(L, 1);
  struct in_addr addr;
  int rc;
  rc = inet_pton(AF_INET, host, &addr);
  if (rc == 1) {
    lua_pushboolean(L, 1);
    lua_pushstring(L, "inet");
    struct in_addr *u = lua_newuserdata(L, sizeof(struct in_addr));
    *u = addr;
    return 3;
  }
  struct in6_addr addr6;
  rc = inet_pton(AF_INET6, host, &addr6);
  if (rc == 1) {
    lua_pushboolean(L, 1);
    lua_pushstring(L, "inet6");
    struct in6_addr *u = lua_newuserdata(L, sizeof(struct in6_addr));
    *u = addr6;
    return 3;
  }
  // error case
  lua_pushboolean(L, 0);
  return 1;
}

static void mtev_lua_init(void) {
  static int done = 0;
  if(done) return;
  done = 1;
  mtev_lua_init_globals();
  register_console_lua_commands();
  eventer_name_callback("lua/resume", nl_resume);
  eventer_name_callback("lua/sleep", nl_sleep_complete);
  eventer_name_callback("lua/socket_read",
                        mtev_lua_socket_read_complete);
  eventer_name_callback("lua/socket_write",
                        mtev_lua_socket_write_complete);
  eventer_name_callback("lua/socket_recv",
                        mtev_lua_socket_recv_complete);
  eventer_name_callback("lua/socket_send",
                        mtev_lua_socket_send_complete);
  eventer_name_callback("lua/socket_connect",
                        mtev_lua_socket_connect_complete);
  eventer_name_callback("lua/socket_accept",
                        mtev_lua_socket_accept_complete);
  eventer_name_callback("lua/ssl_upgrade", mtev_lua_ssl_upgrade);
  eventer_name_callback("lua/shared_waitfor_wakeup", nl_shared_waitfor_wakeup);
  eventer_name_callback("lua/waitfor_timeout", nl_waitfor_timeout);
  eventer_name_callback("lua/waitfor_ping", nl_waitfor_ping);
  eventer_name_callback("lua/process_wait", mtev_lua_process_wait_wakeup);
  nlerr = mtev_log_stream_find("error/lua");
  nldeb = mtev_log_stream_find("debug/lua");
  if(!nlerr) nlerr = mtev_stderr;
  if(!nldeb) nldeb = mtev_debug;
  mtev_lua_init_dns();

  /* init shared_seq */
  shared_seq_table = calloc(1, sizeof(*shared_seq_table));
  mtev_hash_init_locks(shared_seq_table, MTEV_HASH_DEFAULT_SIZE, MTEV_HASH_LOCK_MODE_MUTEX);

  /* init shared_set/get */
  mtev_hash_init_locks(&shared_table, 8, MTEV_HASH_LOCK_MODE_NONE);
  if(pthread_mutex_init(&shared_table_mutex, NULL) != 0) {
    mtevL(nlerr, "Unable to initialize shared_table_mutex\n");
  }

  /* init shared_waitfor/notify */
  mtev_hash_init_locks(&shared_queues, 8, MTEV_HASH_LOCK_MODE_NONE);
  if(pthread_mutex_init(&shared_queues_mutex, NULL) != 0) {
    mtevL(nlerr, "Unable to initialize shared_queues_mutex\n");
  }

  /* init semaphore */
  semaphore_table = calloc(1, sizeof(*semaphore_table));
  mtev_hash_init_locks(semaphore_table, 0, MTEV_HASH_LOCK_MODE_MUTEX);

  mtev_lua_register_dynamic_ctype("mtev_cluster_t *", nl_push_ctype_mtev_cluster);
}

static const luaL_Reg mtevlib[] = {
  { "runtime_reload", nl_runtime_reload },
  { "runtime_defunct", nl_runtime_defunct },
  { "cancel_coro", nl_cancel_coro },
  { "cluster", nl_mtev_cluster },
  { "waitfor", nl_waitfor },
  { "notify", nl_waitfor_notify },
  { "shared_waitfor", nl_shared_waitfor },
  { "shared_notify", nl_shared_waitfor_notify },
  { "sleep", nl_sleep },
  { "gettimeofday", nl_gettimeofday },
  { "uuid", nl_uuid },
  { "socket", nl_socket },
  { "dns", nl_dns_lookup },
  { "log", nl_log },
  { "log_enabled", nl_log_enabled },
  { "enable_log", nl_enable_log },
  { "print", nl_print },
  { "printto", nl_printto },
  { "unlink", nl_unlink },
  { "rmdir", nl_rmdir },
  { "mkdir", nl_mkdir },
  { "mkdir_for_file", nl_mkdir_for_file },
  { "getcwd", nl_getcwd },
  { "open", nl_open },
  { "write", nl_write },
  { "close", nl_close },
  { "chmod", nl_chmod },
  { "stat", nl_stat },
  { "readdir", nl_readdir },
  { "realpath", nl_realpath },
  { "getip_ipv4", nl_getip_ipv4 },
  { "getaddrinfo", nl_getaddrinfo },
  { "getuid", nl_getuid },
  { "getgid", nl_getgid },
  { "geteuid", nl_geteuid },
  { "getegid", nl_getegid },
  { "getpid", nl_getpid },
  { "getppid", nl_getppid },
  { "lockfile_acquire", nl_lockfile_acquire },
  { "lockfile_release", nl_lockfile_release },
  { "crc32", nl_crc32 },
  { "base32_decode", nl_base32_decode },
  { "base32_encode", nl_base32_encode },
  { "base64_decode", nl_base64_decode },
  { "base64_encode", nl_base64_encode },
  { "timezone", nl_timezone },
  { "utf8tohtml", nl_utf8tohtml },
  { "hmac_sha1_encode", nl_hmac_sha1_encode },
  { "hmac_sha256_encode", nl_hmac_sha256_encode },
/*! \lua digest_hex = mtev.sha256_hash(s)
\param s a string
\return the SHA256 digest of the input string, encoded in hexadecimal format

**DEPRECATED**

Use sha256_hex instead.
*/
  { "sha256_hash", nl_sha256_hex},
  { "md5_hex", nl_md5_hex },
  { "md5", nl_md5 },
  { "sha1_hex", nl_sha1_hex },
  { "sha1", nl_sha1 },
  { "sha256_hex", nl_sha256_hex },
  { "sha256", nl_sha256 },
  { "pcre", nl_pcre },
  { "gunzip", nl_gunzip },
  { "conf", nl_conf_get_string },
  { "conf_get", nl_conf_get_string },
  { "conf_get_string", nl_conf_get_string },
  { "conf_string", nl_conf_get_string },
  { "conf_get_string_list", nl_conf_get_string_list },
  { "conf_replace_string", nl_conf_replace_value },
  { "conf_get_integer", nl_conf_get_integer },
  { "conf_integer", nl_conf_get_integer },
  { "conf_replace_integer", nl_conf_replace_value },
  { "conf_get_boolean", nl_conf_get_boolean },
  { "conf_boolean", nl_conf_get_boolean },
  { "conf_replace_boolean", nl_conf_replace_boolean },
  { "conf_get_number", nl_conf_get_float },
  { "conf_number", nl_conf_get_float },
  { "conf_replace_number", nl_conf_replace_value },
  { "parsexml", nl_parsexml },
  { "parsejson", nl_parsejson },
  { "tojson", nl_tojson },
#define BIND_LUA_WFUNC(name)  { #name, mtev_lua_process_##name },
  BIND_LUA_WFUNC(WIFEXITED)
  BIND_LUA_WFUNC(WIFSIGNALED)
  BIND_LUA_WFUNC(WCOREDUMP)
  BIND_LUA_WFUNC(WIFSTOPPED)
  BIND_LUA_WFUNC(WIFCONTINUED)
  BIND_LUA_WFUNC(WEXITSTATUS)
  BIND_LUA_WFUNC(WTERMSIG)
  BIND_LUA_WFUNC(WSTOPSIG)
  { "spawn", nl_spawn },
  { "thread_self", nl_thread_self },
  { "eventer_loop_concurrency", nl_eventer_loop_concurrency },
  { "shared_set", nl_shared_set},
  { "shared_get", nl_shared_get},
  { "shared_seq", nl_shared_seq},
  { "watchdog_child_heartbeat", nl_watchdog_child_heartbeat },
  { "watchdog_timeout", nl_watchdog_timeout },
  { "websocket_client_connect", nl_websocket_client_connect },
  { "semaphore", nl_semaphore },
  { "inet_pton", nl_inet_pton },
  { "uname", nl_uname },
  { NULL, NULL }
};

int luaopen_mtev(lua_State *L) {
  mtev_lua_init();

  luaL_newmetatable(L, "mtev.timezone");
  lua_pushcclosure(L, mtev_timezone_index_func, 0);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, mtev_lua_timezone_gc);
  lua_setfield(L, -2, "__gc");

  luaL_newmetatable(L, "mtev.eventer");
  lua_pushcclosure(L, mtev_eventer_index_func, 0);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, mtev_lua_eventer_gc);
  lua_setfield(L, -2, "__gc");

  luaL_newmetatable(L, "mtev.eventer.ssl_ctx");
  lua_pushcclosure(L, mtev_ssl_ctx_index_func, 0);
  lua_setfield(L, -2, "__index");

  luaL_newmetatable(L, "mtev.websocket_client");
  lua_pushcclosure(L, mtev_websocket_client_index_func, 0);
  lua_setfield(L, -2, "__index");
  luaL_newmetatable(L, "mtev.websocket_client");
  lua_pushcclosure(L, mtev_lua_ws_gc, 0);
  lua_setfield(L, -2, "__gc");

  luaL_newmetatable(L, "mtev.process");
  lua_pushcfunction(L, mtev_lua_process_gc);
  lua_setfield(L, -2, "__gc");
  luaL_newmetatable(L, "mtev.process");
  lua_pushcfunction(L, mtev_lua_process_index_func);
  lua_setfield(L, -2, "__index");

  luaL_newmetatable(L, "mtev.dns");
  lua_pushcfunction(L, mtev_lua_dns_gc);
  lua_setfield(L, -2, "__gc");
  lua_pushcfunction(L, mtev_lua_dns_index_func);
  lua_setfield(L, -2, "__index");

  luaL_newmetatable(L, "mtev.gunzip");
  lua_pushcfunction(L, mtev_lua_gunzip_gc);
  lua_setfield(L, -2, "__gc");

  luaL_newmetatable(L, "mtev.pcre");
  lua_pushcfunction(L, mtev_lua_pcre_gc);
  lua_setfield(L, -2, "__gc");

  luaL_newmetatable(L, "mtev.json_object");
  lua_pushcfunction(L, mtev_lua_json_object_gc);
  lua_setfield(L, -2, "__gc");

  luaL_newmetatable(L, "mtev.json");
  lua_pushcfunction(L, mtev_lua_json_gc);
  lua_setfield(L, -2, "__gc");
  luaL_newmetatable(L, "mtev.json");
  lua_pushcclosure(L, mtev_json_index_func, 0);
  lua_setfield(L, -2, "__index");

  luaL_newmetatable(L, "mtev.xmldoc");
  lua_pushcfunction(L, mtev_lua_xmldoc_gc);
  lua_setfield(L, -2, "__gc");
  luaL_newmetatable(L, "mtev.xmldoc");
  lua_pushcclosure(L, mtev_xmldoc_index_func, 0);
  lua_setfield(L, -2, "__index");

  luaL_newmetatable(L, "mtev.xmlnode");
  lua_pushcclosure(L, mtev_xmlnode_index_func, 0);
  lua_setfield(L, -2, "__index");

  luaL_newmetatable(L, "mtev.cluster");
  lua_pushcclosure(L, mtev_cluster_index_func, 0);
  lua_setfield(L, -2, "__index");

  luaL_newmetatable(L, "mtev.xpathiter");
  lua_pushcfunction(L, mtev_lua_xpathiter_gc);
  lua_setfield(L, -2, "__gc");

  luaL_newmetatable(L, "mtev.semaphore");
  lua_pushcclosure(L, mtev_lua_semaphore_index_func, 0);
  lua_setfield(L, -2, "__index");

  luaL_openlib(L, "mtev", mtevlib, 0);

  lua_getglobal(L, "mtev");
  lua_newtable(L);
  luaL_setfuncs(L, mtevtimevallib, 0);
  lua_setfield(L, -2, "timeval");
  lua_pop(L, 1);

  luaL_newmetatable(L, "mtev.timeval");
  lua_pushcclosure(L, mtev_lua_timeval_index_func, 0);
  lua_setfield(L, -2, "__index");
  lua_pushcclosure(L, mtev_lua_timeval_tostring, 0);
  lua_setfield(L, -2, "__tostring");
  lua_pushcclosure(L, mtev_lua_timeval_sub, 0);
  lua_setfield(L, -2, "__sub");
  lua_pushcclosure(L, mtev_lua_timeval_add, 0);
  lua_setfield(L, -2, "__add");
  lua_pushcclosure(L, mtev_lua_timeval_eq, 0);
  lua_setfield(L, -2, "__eq");
  lua_pushcclosure(L, mtev_lua_timeval_le, 0);
  lua_setfield(L, -2, "__le");
  lua_pushcclosure(L, mtev_lua_timeval_lt, 0);
  lua_setfield(L, -2, "__lt");

  lua_getglobal(L, "_G");
  lua_getglobal(L, "mtev");
  lua_getfield(L, -1, "print");
  lua_remove(L, -2);
  lua_setfield(L, -2, "print");
  lua_pop(L, 1);

#define LUA_DEFINE_INT(L, name) do { \
  lua_pushinteger(L, name); \
  lua_setglobal(L, #name); \
} while(0)
  LUA_DEFINE_INT(L, S_IFIFO);
  LUA_DEFINE_INT(L, S_IFCHR);
  LUA_DEFINE_INT(L, S_IFDIR);
  LUA_DEFINE_INT(L, S_IFBLK);
  LUA_DEFINE_INT(L, S_IFREG);
  LUA_DEFINE_INT(L, S_IFLNK);
  LUA_DEFINE_INT(L, S_IFSOCK);
  LUA_DEFINE_INT(L, O_RDONLY);
  LUA_DEFINE_INT(L, O_WRONLY);
  LUA_DEFINE_INT(L, O_RDWR);
  LUA_DEFINE_INT(L, O_APPEND);
  LUA_DEFINE_INT(L, O_SYNC);
#ifdef O_NOFOLLOW
  LUA_DEFINE_INT(L, O_NOFOLLOW);
#else
#define O_NOFOLLOW 0
  LUA_DEFINE_INT(L, O_NOFOLLOW);
#undef O_NOFOLLOW
#endif
  LUA_DEFINE_INT(L, O_CREAT);
  LUA_DEFINE_INT(L, O_TRUNC);
  LUA_DEFINE_INT(L, O_EXCL);

  LUA_DEFINE_INT(L, MTEV_HOOK_CONTINUE);
  LUA_DEFINE_INT(L, MTEV_HOOK_DONE);
  LUA_DEFINE_INT(L, MTEV_HOOK_ABORT);

  luaopen_mtev_stats(L);
  luaopen_mtev_crypto(L);
  luaopen_mtev_http(L);
  luaopen_mtev_zipkin(L);
  luaopen_bit(L);
  luaopen_pack(L);
  return 0;
}
