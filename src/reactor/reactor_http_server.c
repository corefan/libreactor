#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <time.h>
#include <string.h>
#include <netdb.h>
#include <err.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>

#include <dynamic.h>

#include "reactor_user.h"
#include "reactor_pool.h"
#include "reactor_core.h"
#include "reactor_resolver.h"
#include "reactor_stream.h"
#include "reactor_tcp.h"
#include "reactor_http_parser.h"
#include "reactor_http.h"
#include "reactor_http_server.h"

static void reactor_http_server_map_release(void *data)
{
  reactor_http_server_map *map;

  map = data;
  free(map->method);
  free(map->path);
}

static void reactor_http_server_hold(reactor_http_server *server)
{
  server->ref ++;
}

static void reactor_http_server_release(reactor_http_server *server)
{
  server->ref --;
  if (!server->ref)
    {
      vector_destruct(&server->map);
      if (server->user.callback)
        reactor_user_dispatch(&server->user, REACTOR_HTTP_SERVER_EVENT_CLOSE, server);
    }
}

static void reactor_http_server_error(reactor_http_server *server)
{
  if (server->user.callback)
    reactor_user_dispatch(&server->user, REACTOR_HTTP_SERVER_EVENT_ERROR, server);
}

/*
static void reactor_http_server_date(reactor_http_server *server)
{
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  time_t t;
  struct tm tm;

  (void) time(&t);
  (void) gmtime_r(&t, &tm);
  (void) strftime(server->date, sizeof server->date, "---, %d --- %Y %H:%M:%S GMT", &tm);
  memcpy(server->date, days[tm.tm_wday], 3);
  memcpy(server->date + 8, months[tm.tm_mon], 3);
}
*/

static void reactor_http_server_request(reactor_http_server_session *session, reactor_http_request *request)
{
  reactor_http_server_map *map;
  size_t i;

  map = vector_data(&session->server->map);
  for (i = 0; i < vector_size(&session->server->map); i ++)
    if ((!map[i].method || strcmp(map[i].method, request->method) == 0) &&
        (!map[i].path || strcmp(map[i].path, request->path) == 0))
      {
       reactor_user_dispatch(&map->user, REACTOR_HTTP_SERVER_EVENT_REQUEST,
                              (reactor_http_server_context[]){{.session = session, .request = request}});
        return;
      }
}

static void reactor_http_server_event_http(void *state, int type, void *data)
{
  reactor_http_server_session *session = state;
  reactor_http_request *request;

  switch (type)
    {
    case REACTOR_HTTP_EVENT_HANGUP:
    case REACTOR_HTTP_EVENT_ERROR:
      reactor_http_close(&session->http);
      break;
    case REACTOR_HTTP_EVENT_CLOSE:
      reactor_http_server_release(session->server);
      free(session);
      break;
    case REACTOR_HTTP_EVENT_REQUEST:
      request = data;
      reactor_http_server_request(session, request);
      break;
    }
}

static void reactor_http_server_event_tcp(void *state, int type, void *data)
{
  reactor_http_server *server = state;
  reactor_http_server_session *session;
  int s;

  switch (type)
    {
    case REACTOR_TCP_EVENT_ERROR:
      reactor_http_server_error(server);
      break;
    case REACTOR_TCP_EVENT_ACCEPT:
      s = *(int *) data;
      session = malloc(sizeof *session);
      session->server = server;
      reactor_http_server_hold(server);
      reactor_http_open(&session->http, reactor_http_server_event_http, session, s, REACTOR_HTTP_FLAG_SERVER);
      break;
  }
}

void reactor_http_server_open(reactor_http_server *server, reactor_user_callback *callback, void *state,
                              char *host, char *port)
{
  server->ref = 0;
  server->state = REACTOR_HTTP_SERVER_STATE_OPEN;
  vector_construct(&server->map, sizeof(reactor_http_server_map));
  vector_object_release(&server->map, reactor_http_server_map_release);
  reactor_user_construct(&server->user, callback, state);
  reactor_http_server_hold(server);
  reactor_tcp_open(&server->tcp, reactor_http_server_event_tcp, server, host, port, REACTOR_HTTP_FLAG_SERVER);
}

void reactor_http_server_close(reactor_http_server *server)
{
  (void) server;
  /*
  if (reactor_http_server_active(server))
    {
      server->flags &= ~REACTOR_HTTP_SERVER_FLAG_ACTIVE;
      if (reactor_tcp_active(&server->tcp))
        reactor_tcp_close(&server->tcp);
      if (reactor_timer_active(&server->timer))
        reactor_timer_close(&server->timer);
    }
  */
}

void reactor_http_server_route(reactor_http_server *server, reactor_user_callback *callback, void *state,
                               char *method, char *path)
{
  vector_push_back(&server->map, (reactor_http_server_map[]) {{
        .user = {.callback = callback, .state = state},
        .method = method,
        .path = path}});
}

void reactor_http_server_respond_mime(reactor_http_server_session *session, char *type, char *data, size_t size)
{
  reactor_http_write_response(&session->http, (reactor_http_response[]){{1, 200, "OK",
          1, (reactor_http_header[]){{"Content-Type", type}}, data, size}});
}

void reactor_http_server_respond_text(reactor_http_server_session *session, char *text)
{
  reactor_http_server_respond_mime(session, "text/plain", text, strlen(text));
}