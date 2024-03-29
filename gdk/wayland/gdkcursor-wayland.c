/* GDK - The GIMP Drawing Kit
 * Copyright (C) 1995-1997 Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Modified by the GTK+ Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GTK+ Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GTK+ at ftp://ftp.gtk.org/pub/gtk/. 
 */

#include "config.h"

#define GDK_PIXBUF_ENABLE_BACKEND

#include <string.h>

#include "gdkprivate-wayland.h"
#include "gdkcursorprivate.h"
#include "gdkdisplay-wayland.h"
#include "gdkwayland.h"
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <wayland-cursor.h>

#define GDK_TYPE_WAYLAND_CURSOR              (_gdk_wayland_cursor_get_type ())
#define GDK_WAYLAND_CURSOR(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), GDK_TYPE_WAYLAND_CURSOR, GdkWaylandCursor))
#define GDK_WAYLAND_CURSOR_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), GDK_TYPE_WAYLAND_CURSOR, GdkWaylandCursorClass))
#define GDK_IS_WAYLAND_CURSOR(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDK_TYPE_WAYLAND_CURSOR))
#define GDK_IS_WAYLAND_CURSOR_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), GDK_TYPE_WAYLAND_CURSOR))
#define GDK_WAYLAND_CURSOR_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), GDK_TYPE_WAYLAND_CURSOR, GdkWaylandCursorClass))

typedef struct _GdkWaylandCursor GdkWaylandCursor;
typedef struct _GdkWaylandCursorClass GdkWaylandCursorClass;

struct _GdkWaylandCursor
{
  GdkCursor cursor;
  gchar *name;
  guint serial;

  struct
  {
    int hotspot_x, hotspot_y;
    int width, height;
    struct wl_buffer *buffer;
  } pixbuf;

  struct wl_cursor *wl_cursor;
};

struct _GdkWaylandCursorClass
{
  GdkCursorClass cursor_class;
};

G_DEFINE_TYPE (GdkWaylandCursor, _gdk_wayland_cursor, GDK_TYPE_CURSOR)

static guint theme_serial = 0;

struct cursor_cache_key
{
  GdkCursorType type;
  const char *name;
};

static void
add_to_cache (GdkWaylandDisplay *display, GdkWaylandCursor *cursor)
{
  display->cursor_cache = g_slist_prepend (display->cursor_cache, cursor);

  g_object_ref (cursor);
}

static gint
cache_compare_func (gconstpointer listelem,
                    gconstpointer target)
{
  GdkWaylandCursor *cursor = (GdkWaylandCursor *) listelem;
  struct cursor_cache_key* key = (struct cursor_cache_key *) target;

  if (cursor->cursor.type != key->type)
    return 1; /* No match */

  /* Elements marked as pixmap must be named cursors
   * (since we don't store normal pixmap cursors
   */
  if (key->type == GDK_CURSOR_IS_PIXMAP)
    return strcmp (key->name, cursor->name);

  return 0; /* Match */
}

static GdkWaylandCursor*
find_in_cache (GdkWaylandDisplay *display,
               GdkCursorType      type,
               const char        *name)
{
  GSList* res;
  struct cursor_cache_key key;

  key.type = type;
  key.name = name;

  res = g_slist_find_custom (display->cursor_cache, &key, cache_compare_func);

  if (res)
    return (GdkWaylandCursor *) res->data;

  return NULL;
}

/* Called by gdk_wayland_display_finalize to flush any cached cursors
 * for a dead display.
 */
void
_gdk_wayland_display_finalize_cursors (GdkWaylandDisplay *display)
{
  g_slist_foreach (display->cursor_cache, (GFunc) g_object_unref, NULL);
  g_slist_free (display->cursor_cache);
}

static gboolean
set_cursor_from_theme (GdkWaylandCursor *cursor, struct wl_cursor_theme *theme)
{
  struct wl_cursor *c;

  c = wl_cursor_theme_get_cursor (theme, cursor->name);
  if (!c)
    {
      g_warning (G_STRLOC ": Unable to load %s from the cursor theme", cursor->name);

      /* return the left_ptr cursor as a fallback */
      c = wl_cursor_theme_get_cursor (theme, "left_ptr");

      if (!c)
        return FALSE;
    }

  cursor->wl_cursor = c;

  return TRUE;
}

void
_gdk_wayland_display_update_cursors (GdkWaylandDisplay      *display,
                                     struct wl_cursor_theme *theme)
{
  g_slist_foreach (display->cursor_cache, (GFunc) set_cursor_from_theme, theme);
}

static void
gdk_wayland_cursor_finalize (GObject *object)
{
  GdkWaylandCursor *cursor = GDK_WAYLAND_CURSOR (object);

  g_free (cursor->name);
  if (cursor->pixbuf.buffer)
    wl_buffer_destroy (cursor->pixbuf.buffer);

  G_OBJECT_CLASS (_gdk_wayland_cursor_parent_class)->finalize (object);
}

static GdkPixbuf*
gdk_wayland_cursor_get_image (GdkCursor *cursor)
{
  return NULL;
}

struct wl_buffer *
_gdk_wayland_cursor_get_buffer (GdkCursor *cursor,
                                guint      image_index,
                                int       *x,
                                int       *y,
                                int       *w,
                                int       *h)
{
  GdkWaylandCursor *wayland_cursor = GDK_WAYLAND_CURSOR (cursor);

  if (wayland_cursor->wl_cursor)
    {
      struct wl_cursor_image *image;

      if (image_index >= wayland_cursor->wl_cursor->image_count)
        {
          g_warning (G_STRLOC " out of bounds cursor image [%d / %d]",
                     image_index,
                     wayland_cursor->wl_cursor->image_count - 1);
          image_index = 0;
        }

      image = wayland_cursor->wl_cursor->images[image_index];

      *x = image->hotspot_x;
      *y = image->hotspot_y;

      *w = image->width;
      *h = image->height;

      return wl_cursor_image_get_buffer (image);
    }
  else /* From pixbuf */
    {
      *x = wayland_cursor->pixbuf.hotspot_x;
      *y = wayland_cursor->pixbuf.hotspot_y;

      *w = wayland_cursor->pixbuf.width;
      *h = wayland_cursor->pixbuf.height;

      return wayland_cursor->pixbuf.buffer;
    }
}

guint
_gdk_wayland_cursor_get_next_image_index (GdkCursor *cursor,
                                          guint      current_image_index,
                                          guint     *next_image_delay)
{
  struct wl_cursor *wl_cursor = GDK_WAYLAND_CURSOR (cursor)->wl_cursor;

  if (wl_cursor && wl_cursor->image_count > 1)
    {
      if (current_image_index >= wl_cursor->image_count)
        {
          g_warning (G_STRLOC " out of bounds cursor image [%d / %d]",
                     current_image_index, wl_cursor->image_count - 1);
          current_image_index = 0;
        }

      /* Return the time to next image */
      if (next_image_delay)
        *next_image_delay = wl_cursor->images[current_image_index]->delay;

      return (current_image_index + 1) % wl_cursor->image_count;
    }
  else
    return current_image_index;
}

static void
_gdk_wayland_cursor_class_init (GdkWaylandCursorClass *wayland_cursor_class)
{
  GdkCursorClass *cursor_class = GDK_CURSOR_CLASS (wayland_cursor_class);
  GObjectClass *object_class = G_OBJECT_CLASS (wayland_cursor_class);

  object_class->finalize = gdk_wayland_cursor_finalize;

  cursor_class->get_image = gdk_wayland_cursor_get_image;
}

static void
_gdk_wayland_cursor_init (GdkWaylandCursor *cursor)
{
}

/* Used to implement from_pixbuf below */
static void
set_pixbuf (gpointer argb_pixels, int width, int height, GdkPixbuf *pixbuf)
{
  int stride, i, n_channels;
  unsigned char *pixels, *end, *s, *d;

  stride = gdk_pixbuf_get_rowstride(pixbuf);
  pixels = gdk_pixbuf_get_pixels(pixbuf);
  n_channels = gdk_pixbuf_get_n_channels(pixbuf);

#define MULT(_d,c,a,t) \
	do { t = c * a + 0x7f; _d = ((t >> 8) + t) >> 8; } while (0)

  if (n_channels == 4)
    {
      for (i = 0; i < height; i++)
	{
	  s = pixels + i * stride;
          end = s + width * 4;
          d = argb_pixels + i * width * 4;
	  while (s < end)
	    {
	      unsigned int t;

	      MULT(d[0], s[2], s[3], t);
	      MULT(d[1], s[1], s[3], t);
	      MULT(d[2], s[0], s[3], t);
	      d[3] = s[3];
	      s += 4;
	      d += 4;
	    }
	}
    }
  else if (n_channels == 3)
    {
      for (i = 0; i < height; i++)
	{
	  s = pixels + i * stride;
          end = s + width * 3;
          d = argb_pixels + i * width * 4;
	  while (s < end)
	    {
	      d[0] = s[2];
	      d[1] = s[1];
	      d[2] = s[0];
	      d[3] = 0xff;
	      s += 3;
	      d += 4;
	    }
	}
    }
}

GdkCursor *
_gdk_wayland_display_get_cursor_for_type (GdkDisplay    *display,
					  GdkCursorType  cursor_type)
{
  GEnumClass *enum_class;
  GEnumValue *enum_value;
  gchar *cursor_name;
  GdkCursor *result;

  enum_class = g_type_class_ref (GDK_TYPE_CURSOR_TYPE);
  enum_value = g_enum_get_value (enum_class, cursor_type);
  cursor_name = g_strdup (enum_value->value_nick);
  g_strdelimit (cursor_name, "-", '_');
  g_type_class_unref (enum_class);

  result = _gdk_wayland_display_get_cursor_for_name (display, cursor_name);

  g_free (cursor_name);

  return result;
}

GdkCursor *
_gdk_wayland_display_get_cursor_for_name (GdkDisplay  *display,
					  const gchar *name)
{
  GdkWaylandCursor *private;
  GdkWaylandDisplay *wayland_display = GDK_WAYLAND_DISPLAY (display);

  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);

  private = find_in_cache (wayland_display, GDK_CURSOR_IS_PIXMAP, name);
  if (private)
    {
      /* Cache had it, add a ref for this user */
      g_object_ref (private);

      return (GdkCursor*) private;
    }

  private = g_object_new (GDK_TYPE_WAYLAND_CURSOR,
                          "cursor-type", GDK_CURSOR_IS_PIXMAP,
                          "display", display,
                          NULL);
  private->name = g_strdup (name);
  private->serial = theme_serial;

  /* Blank cursor case */
  if (!name || g_str_equal (name, "blank_cursor"))
    return GDK_CURSOR (private);

  if (!set_cursor_from_theme (private, wayland_display->cursor_theme))
    return GDK_CURSOR (private);

  add_to_cache (wayland_display, private);

  return GDK_CURSOR (private);
}

GdkCursor *
_gdk_wayland_display_get_cursor_for_pixbuf (GdkDisplay *display,
					    GdkPixbuf  *pixbuf,
					    gint        x,
					    gint        y)
{
  GdkWaylandCursor *cursor;
  GdkWaylandDisplay *wayland_display = GDK_WAYLAND_DISPLAY (display);
  int stride;
  size_t size;
  gpointer data;
  struct wl_shm_pool *pool;

  g_return_val_if_fail (GDK_IS_DISPLAY (display), NULL);
  g_return_val_if_fail (GDK_IS_PIXBUF (pixbuf), NULL);
  g_return_val_if_fail (0 <= x && x < gdk_pixbuf_get_width (pixbuf), NULL);
  g_return_val_if_fail (0 <= y && y < gdk_pixbuf_get_height (pixbuf), NULL);

  cursor = g_object_new (GDK_TYPE_WAYLAND_CURSOR,
			 "cursor-type", GDK_CURSOR_IS_PIXMAP,
			 "display", wayland_display,
			 NULL);
  cursor->name = NULL;
  cursor->serial = theme_serial;
  cursor->pixbuf.hotspot_x = x;
  cursor->pixbuf.hotspot_y = y;

  if (pixbuf)
    {
      cursor->pixbuf.width = gdk_pixbuf_get_width (pixbuf);
      cursor->pixbuf.height = gdk_pixbuf_get_height (pixbuf);
    }
  else
    {
      cursor->pixbuf.width = 1;
      cursor->pixbuf.height = 1;
    }

  pool = _create_shm_pool (wayland_display->shm,
                           cursor->pixbuf.width,
                           cursor->pixbuf.height,
                           &size,
                           &data);

  if (pixbuf)
    set_pixbuf (data, cursor->pixbuf.width, cursor->pixbuf.height, pixbuf);
  else
    memset (data, 0, 4);

  stride = cursor->pixbuf.width * 4;
  cursor->pixbuf.buffer = wl_shm_pool_create_buffer (pool, 0,
                                                     cursor->pixbuf.width,
                                                     cursor->pixbuf.height,
                                                     stride,
                                                     WL_SHM_FORMAT_ARGB8888);

  wl_shm_pool_destroy (pool);

  return GDK_CURSOR (cursor);
}

void
_gdk_wayland_display_get_default_cursor_size (GdkDisplay *display,
					      guint       *width,
					      guint       *height)
{
  *width = 32;
  *height = 32;
}

void
_gdk_wayland_display_get_maximal_cursor_size (GdkDisplay *display,
					      guint       *width,
					      guint       *height)
{
  *width = 256;
  *height = 256;
}

gboolean
_gdk_wayland_display_supports_cursor_alpha (GdkDisplay *display)
{
  return TRUE;
}

gboolean
_gdk_wayland_display_supports_cursor_color (GdkDisplay *display)
{
  return TRUE;
}
