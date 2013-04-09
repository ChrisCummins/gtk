/* testanimatedcursor.c - Animated cursor demo
 * Copyright (C) 2013 Intel Corporation.
 * Author: Chris Cummins
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

#include <gtk/gtk.h>

static gboolean animated = FALSE;
static GdkCursor *cursor;

static void
destroy_cb (GtkWidget *widget,
            gpointer data)
{
  gtk_main_quit ();
}

static void
toggle_cursor (GtkWidget *window)
{
  if (animated)
    gdk_window_set_cursor (gtk_widget_get_window (window), NULL);
  else
    gdk_window_set_cursor (gtk_widget_get_window (window), cursor);

  animated = !animated;
}

int
main (int argc, char *argv[])
{
  GtkWidget *window;
  GtkWidget *button;

  gtk_init (&argc, &argv);
  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);

  g_signal_connect (window, "destroy", G_CALLBACK (destroy_cb), window);
  gtk_window_set_default_size (GTK_WINDOW (window), 280, 90);
  gtk_window_set_title (GTK_WINDOW (window), "Animated Cursor");

  gtk_container_set_border_width (GTK_CONTAINER(window), 20);
  button = gtk_button_new_with_label ("Toggle animated cursor");
  g_signal_connect (button, "clicked", G_CALLBACK (toggle_cursor), NULL);
  gtk_container_add (GTK_CONTAINER (window), button);

  cursor = gdk_cursor_new (GDK_WATCH);

  gtk_widget_show (button);
  gtk_widget_show (window);

  gtk_main ();

  return 0;
}
