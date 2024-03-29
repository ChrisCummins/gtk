#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "gdkbroadway-server.h"

#include "gdkprivate-broadway.h"

#include <glib.h>
#include <glib/gprintf.h>
#include <gio/gunixsocketaddress.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

typedef struct BroadwayInput BroadwayInput;

struct _GdkBroadwayServer {
  GObject parent_instance;

  guint32 next_serial;
  GSocketConnection *connection;

  guint32 recv_buffer_size;
  guint8 recv_buffer[1024];

  guint process_input_idle;
  GList *incomming;
  
};

struct _GdkBroadwayServerClass
{
  GObjectClass parent_class;
};

static gboolean input_available_cb (gpointer stream, gpointer user_data);

G_DEFINE_TYPE (GdkBroadwayServer, gdk_broadway_server, G_TYPE_OBJECT)

static void
gdk_broadway_server_init (GdkBroadwayServer *server)
{
  server->next_serial = 1;
}

static void
gdk_broadway_server_finalize (GObject *object)
{
  G_OBJECT_CLASS (gdk_broadway_server_parent_class)->finalize (object);
}

static void
gdk_broadway_server_class_init (GdkBroadwayServerClass * class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = gdk_broadway_server_finalize;
}

gboolean
_gdk_broadway_server_lookahead_event (GdkBroadwayServer  *server,
				      const char         *types)
{

  return FALSE;
}

gulong
_gdk_broadway_server_get_next_serial (GdkBroadwayServer *server)
{
  return (gulong)server->next_serial;
}

GdkBroadwayServer *
_gdk_broadway_server_new (int port, GError **error)
{
  GdkBroadwayServer *server;
  char *basename;
  GSocketClient *client;
  GSocketConnection *connection;
  GSocketAddress *address;
  GPollableInputStream *pollable;
  GInputStream *in;
  GSource *source;
  char *path;

  basename = g_strdup_printf ("broadway%d.socket", port);
  path = g_build_filename (g_get_user_runtime_dir (), basename, NULL);
  g_free (basename);

  address = g_unix_socket_address_new_with_type (path, -1,
						 G_UNIX_SOCKET_ADDRESS_ABSTRACT);
  g_free (path);

  client = g_socket_client_new ();

  error = NULL;
  connection = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (address), NULL, error);
  
  g_object_unref (address);
  g_object_unref (client);

  if (connection == NULL)
    return NULL;

  server = g_object_new (GDK_TYPE_BROADWAY_SERVER, NULL);
  server->connection = connection;

  in = g_io_stream_get_input_stream (G_IO_STREAM (server->connection));
  pollable = G_POLLABLE_INPUT_STREAM (in);

  source = g_pollable_input_stream_create_source (pollable, NULL);
  g_source_attach (source, NULL);
  g_source_set_callback (source, (GSourceFunc)input_available_cb, server, NULL);

  return server;
}

guint32
_gdk_broadway_server_get_last_seen_time (GdkBroadwayServer *server)
{
  return 0;
}

static guint32
gdk_broadway_server_send_message_with_size (GdkBroadwayServer *server, BroadwayRequestBase *base,
					    gsize size, guint32 type)
{
  GOutputStream *out;
  gsize written;

  base->size = size;
  base->type = type;
  base->serial = server->next_serial++;

  out = g_io_stream_get_output_stream (G_IO_STREAM (server->connection));

  if (!g_output_stream_write_all (out, base, size, &written, NULL, NULL))
    {
      g_printerr ("Unable to write to server\n");
      exit (1);
    }

  g_assert (written == size);

  return base->serial;
}

#define gdk_broadway_server_send_message(_server, _msg, _type) \
  gdk_broadway_server_send_message_with_size(_server, (BroadwayRequestBase *)&_msg, sizeof (_msg), _type)

static void
parse_all_input (GdkBroadwayServer *server)
{
  guint8 *p, *end;
  guint32 size;
  BroadwayReply *reply;

  p = server->recv_buffer;
  end = p + server->recv_buffer_size;

  while (p + sizeof (guint32) <= end)
    {
      memcpy (&size, p, sizeof (guint32));
      if (p + size > end)
	break;

      reply = g_memdup (p, size);
      p += size;

      server->incomming = g_list_append (server->incomming, reply);
    }

  if (p < end)
    memmove (server->recv_buffer, p, end - p);
  server->recv_buffer_size = end - p;
}

static void
read_some_input_blocking (GdkBroadwayServer *server)
{
  GInputStream *in;
  gssize res;

  in = g_io_stream_get_input_stream (G_IO_STREAM (server->connection));

  g_assert (server->recv_buffer_size < sizeof (server->recv_buffer));
  res = g_input_stream_read (in, &server->recv_buffer[server->recv_buffer_size],
			     sizeof (server->recv_buffer) - server->recv_buffer_size,
			     NULL, NULL);

  if (res <= 0)
    {
      g_printerr ("Unable to read from broadway server\n");
      exit (1);
    }

  server->recv_buffer_size += res;
}

static void
read_some_input_nonblocking (GdkBroadwayServer *server)
{
  GInputStream *in;
  GPollableInputStream *pollable;
  gssize res;
  GError *error;

  in = g_io_stream_get_input_stream (G_IO_STREAM (server->connection));
  pollable = G_POLLABLE_INPUT_STREAM (in);

  g_assert (server->recv_buffer_size < sizeof (server->recv_buffer));
  error = NULL;
  res = g_pollable_input_stream_read_nonblocking (pollable, &server->recv_buffer[server->recv_buffer_size],
						  sizeof (server->recv_buffer) - server->recv_buffer_size,
						  NULL, &error);

  if (res < 0 && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
    {
      g_error_free (error);
      res = 0;
    }
  else if (res <= 0)
    {
      g_printerr ("Unable to read from broadway server: %s\n", error ? error->message : "eof");
      exit (1);
    }

  server->recv_buffer_size += res;
}

static BroadwayReply *
find_response_by_serial (GdkBroadwayServer *server, guint32 serial)
{
  GList *l;

  for (l = server->incomming; l != NULL; l = l->next)
    {
      BroadwayReply *reply = l->data;

      if (reply->base.in_reply_to == serial)
	return reply;
    }

  return NULL;
}

static void
process_input_messages (GdkBroadwayServer *server)
{
  BroadwayReply *reply;

  if (server->process_input_idle != 0)
    {
      g_source_remove (server->process_input_idle);
      server->process_input_idle = 0;
    }

  while (server->incomming)
    {
      reply = server->incomming->data;
      server->incomming =
	g_list_delete_link (server->incomming,
			    server->incomming);

      if (reply->base.type == BROADWAY_REPLY_EVENT)
	_gdk_broadway_events_got_input (&reply->event.msg);
      else
	g_warning ("Unhandled reply type %d\n", reply->base.type);
      g_free (reply);
    }
}

static gboolean
process_input_idle_cb (GdkBroadwayServer *server)
{
  server->process_input_idle = 0;
  process_input_messages (server);
  return G_SOURCE_REMOVE;
}

static void
queue_process_input_at_idle (GdkBroadwayServer *server)
{
  if (server->process_input_idle == 0)
    server->process_input_idle =
      g_idle_add_full (G_PRIORITY_DEFAULT, (GSourceFunc)process_input_idle_cb, server, NULL);
}

static gboolean
input_available_cb (gpointer stream, gpointer user_data)
{
  GdkBroadwayServer *server = user_data;

  read_some_input_nonblocking (server);
  parse_all_input (server);

  process_input_messages (server);

  return G_SOURCE_CONTINUE;
}

static BroadwayReply *
gdk_broadway_server_wait_for_reply (GdkBroadwayServer *server,
				    guint32 serial)
{
  BroadwayReply *reply;

  while (TRUE)
    {
      reply = find_response_by_serial (server, serial);
      if (reply)
	{
	  server->incomming = g_list_remove (server->incomming, reply);
	  break;
	}

      read_some_input_blocking (server);
      parse_all_input (server);
    }

  queue_process_input_at_idle (server);
  return reply;
}

void
_gdk_broadway_server_flush (GdkBroadwayServer *server)
{
  BroadwayRequestFlush msg;

  gdk_broadway_server_send_message(server, msg, BROADWAY_REQUEST_FLUSH);
}

void
_gdk_broadway_server_sync (GdkBroadwayServer *server)
{
  BroadwayRequestSync msg;
  guint32 serial;
  BroadwayReply *reply;

  serial = gdk_broadway_server_send_message (server, msg,
					     BROADWAY_REQUEST_SYNC);
  reply = gdk_broadway_server_wait_for_reply (server, serial);

  g_assert (reply->base.type == BROADWAY_REPLY_SYNC);

  g_free (reply);

  return;
}

void
_gdk_broadway_server_query_mouse (GdkBroadwayServer *server,
				  guint32            *toplevel,
				  gint32             *root_x,
				  gint32             *root_y,
				  guint32            *mask)
{
  BroadwayRequestQueryMouse msg;
  guint32 serial;
  BroadwayReply *reply;

  serial = gdk_broadway_server_send_message (server, msg,
					     BROADWAY_REQUEST_QUERY_MOUSE);
  reply = gdk_broadway_server_wait_for_reply (server, serial);

  g_assert (reply->base.type == BROADWAY_REPLY_QUERY_MOUSE);

  if (toplevel)
    *toplevel = reply->query_mouse.toplevel;
  if (root_x)
    *root_x = reply->query_mouse.root_x;
  if (root_y)
    *root_y = reply->query_mouse.root_y;
  if (mask)
    *mask = reply->query_mouse.mask;
  
  g_free (reply);
}

guint32
_gdk_broadway_server_new_window (GdkBroadwayServer *server,
				 int x,
				 int y,
				 int width,
				 int height,
				 gboolean is_temp)
{
  BroadwayRequestNewWindow msg;
  guint32 serial, id;
  BroadwayReply *reply;

  msg.x = x;
  msg.y = y;
  msg.width = width;
  msg.height = height;
  msg.is_temp = is_temp;

  serial = gdk_broadway_server_send_message (server, msg,
					     BROADWAY_REQUEST_NEW_WINDOW);
  reply = gdk_broadway_server_wait_for_reply (server, serial);

  g_assert (reply->base.type == BROADWAY_REPLY_NEW_WINDOW);

  id = reply->new_window.id;
  
  g_free (reply);

  return id;
}

void
_gdk_broadway_server_destroy_window (GdkBroadwayServer *server,
				     gint id)
{
  BroadwayRequestDestroyWindow msg;

  msg.id = id;
  gdk_broadway_server_send_message (server, msg,
				    BROADWAY_REQUEST_DESTROY_WINDOW);
}

gboolean
_gdk_broadway_server_window_show (GdkBroadwayServer *server,
				  gint id)
{
  BroadwayRequestShowWindow msg;

  msg.id = id;
  gdk_broadway_server_send_message (server, msg,
				    BROADWAY_REQUEST_SHOW_WINDOW);
  
  return TRUE;
}

gboolean
_gdk_broadway_server_window_hide (GdkBroadwayServer *server,
				  gint id)
{
  BroadwayRequestHideWindow msg;

  msg.id = id;
  gdk_broadway_server_send_message (server, msg,
				    BROADWAY_REQUEST_HIDE_WINDOW);
  
  return TRUE;
}

void
_gdk_broadway_server_window_set_transient_for (GdkBroadwayServer *server,
					       gint id, gint parent)
{
  BroadwayRequestSetTransientFor msg;

  msg.id = id;
  msg.parent = parent;
  gdk_broadway_server_send_message (server, msg,
				    BROADWAY_REQUEST_SET_TRANSIENT_FOR);
}

gboolean
_gdk_broadway_server_window_translate (GdkBroadwayServer *server,
				       gint id,
				       cairo_region_t *area,
				       gint            dx,
				       gint            dy)
{
  BroadwayRequestTranslate *msg;
  cairo_rectangle_int_t rect;
  int i, n_rects;
  gsize msg_size;

  n_rects = cairo_region_num_rectangles (area);

  msg_size = sizeof (BroadwayRequestTranslate) + (n_rects-1) * sizeof (BroadwayRect);
  msg = g_malloc (msg_size);

  msg->id = id;
  msg->dx = dx;
  msg->dy = dy;
  msg->n_rects = n_rects;
  
  for (i = 0; i < n_rects; i++)
    {
      cairo_region_get_rectangle (area, i, &rect);
      msg->rects[i].x = rect.x;
      msg->rects[i].y = rect.y;
      msg->rects[i].width = rect.width;
      msg->rects[i].height = rect.height;
    }

  gdk_broadway_server_send_message_with_size (server, (BroadwayRequestBase *)msg, msg_size,
					      BROADWAY_REQUEST_TRANSLATE);
  g_free (msg);
  return TRUE;
}

static char
make_valid_fs_char (char c)
{
  char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890";

  return chars[c % sizeof (chars)];
}

/* name must have at least space for 34 bytes */
static int
create_random_shm (char *name)
{
  guint32 r;
  int i, o, fd;

  while (TRUE)
    {
      o = 0;
      name[o++] = '/';
      name[o++] = 'b';
      name[o++] = 'd';
      name[o++] = 'w';
      name[o++] = '-';
      for (i = 0; i < 32/4 - 1; i++)
	{
	  r = g_random_int ();
	  name[o++] = make_valid_fs_char ((r >> 0) & 0xff);
	  name[o++] = make_valid_fs_char ((r >> 8) & 0xff);
	  name[o++] = make_valid_fs_char ((r >> 16) & 0xff);
	  name[o++] = make_valid_fs_char ((r >> 24) & 0xff);
	}
      name[o++] = 0;
  
      fd = shm_open(name, O_RDWR|O_CREAT|O_EXCL, 0600);
      if (fd >= 0)
	return fd;

      if (errno != EEXIST)
	{
	  g_printerr ("Unable to allocate shared mem for window");
	  exit (1);
	}
    }

}

static const cairo_user_data_key_t gdk_broadway_shm_cairo_key;

typedef struct {
  char name[36];
  void *data;
  gsize data_size;
} BroadwayShmSurfaceData;

static void
shm_data_destroy (void *_data)
{
  BroadwayShmSurfaceData *data = _data;

  munmap (data->data, data->data_size);
  shm_unlink (data->name);
  g_free (data);
}

cairo_surface_t *
_gdk_broadway_server_create_surface (int                 width,
				     int                 height)
{
  BroadwayShmSurfaceData *data;
  cairo_surface_t *surface;
  int res;
  int fd;

  data = g_new (BroadwayShmSurfaceData, 1);
  data->data_size = width * height * sizeof (guint32);
  
  fd = create_random_shm (data->name);

  res = ftruncate (fd, data->data_size);
  g_assert (res != -1);

  data->data = mmap(0, data->data_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0); 
  (void) close(fd);

  surface = cairo_image_surface_create_for_data ((guchar *)data->data,
						 CAIRO_FORMAT_RGB24, width, height, width * sizeof (guint32));
  g_assert (surface != NULL);
  
  cairo_surface_set_user_data (surface, &gdk_broadway_shm_cairo_key,
			       data, shm_data_destroy);

  return surface;
}

void
_gdk_broadway_server_window_update (GdkBroadwayServer *server,
				    gint id,
				    cairo_surface_t *surface)
{
  BroadwayRequestUpdate msg;
  BroadwayShmSurfaceData *data;

  if (surface == NULL)
    return;

  data = cairo_surface_get_user_data (surface, &gdk_broadway_shm_cairo_key);
  g_assert (data != NULL);

  msg.id = id;
  memcpy (msg.name, data->name, 36);
  msg.width = cairo_image_surface_get_width (surface);
  msg.height = cairo_image_surface_get_height (surface);

  gdk_broadway_server_send_message (server, msg,
				    BROADWAY_REQUEST_UPDATE);
}

gboolean
_gdk_broadway_server_window_move_resize (GdkBroadwayServer *server,
					 gint id,
					 gboolean with_move,
					 int x,
					 int y,
					 int width,
					 int height)
{
  BroadwayRequestMoveResize msg;

  msg.id = id;
  msg.with_move = with_move;
  msg.x = x;
  msg.y = y;
  msg.width = width;
  msg.height = height;

  gdk_broadway_server_send_message (server, msg,
				    BROADWAY_REQUEST_MOVE_RESIZE);

  return TRUE;
}

GdkGrabStatus
_gdk_broadway_server_grab_pointer (GdkBroadwayServer *server,
				   gint id,
				   gboolean owner_events,
				   guint32 event_mask,
				   guint32 time_)
{
  BroadwayRequestGrabPointer msg;
  guint32 serial, status;
  BroadwayReply *reply;

  msg.id = id;
  msg.owner_events = owner_events;
  msg.event_mask = event_mask;
  msg.time_ = time_;

  serial = gdk_broadway_server_send_message (server, msg,
					     BROADWAY_REQUEST_GRAB_POINTER);
  reply = gdk_broadway_server_wait_for_reply (server, serial);

  g_assert (reply->base.type == BROADWAY_REPLY_GRAB_POINTER);

  status = reply->grab_pointer.status;

  g_free (reply);

  return status;
}

guint32
_gdk_broadway_server_ungrab_pointer (GdkBroadwayServer *server,
				     guint32    time_)
{
  BroadwayRequestUngrabPointer msg;
  guint32 serial, status;
  BroadwayReply *reply;

  msg.time_ = time_;

  serial = gdk_broadway_server_send_message (server, msg,
					     BROADWAY_REQUEST_UNGRAB_POINTER);
  reply = gdk_broadway_server_wait_for_reply (server, serial);

  g_assert (reply->base.type == BROADWAY_REPLY_UNGRAB_POINTER);

  status = reply->ungrab_pointer.status;

  g_free (reply);

  return status;
}
