/* testcssboxshadows.c - CSS box shadows demo
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

static void
destroy_cb (GtkWidget *widget,
            gpointer data)
{
  gtk_main_quit ();
}

static void
apply_css (GtkWidget *widget, GtkCssProvider *provider)
{
  gtk_style_context_add_provider (gtk_widget_get_style_context (widget),
                                  GTK_STYLE_PROVIDER (provider), G_MAXUINT);
  if (GTK_IS_CONTAINER (widget))
    gtk_container_forall (GTK_CONTAINER (widget),
                          (GtkCallback) apply_css,
                          provider);
}

int
main (int argc, char *argv[])
{
  GtkWidget *window;
  GtkWidget *button;
  GtkCssProvider *provider;
  GError *error = NULL;

  gtk_init (&argc, &argv);
  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);

  g_signal_connect (window, "destroy", G_CALLBACK (destroy_cb), NULL);
  gtk_window_set_default_size (GTK_WINDOW (window), 280, 90);
  gtk_window_set_title (GTK_WINDOW (window), "CSS Box Shadows");

  gtk_container_set_border_width (GTK_CONTAINER (window), 20);
  button = gtk_button_new_with_label ("Press for outset shadow");
  gtk_container_add (GTK_CONTAINER (window), button);

  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_path (provider, "testcssboxshadows.css", &error);
  if (error)
    g_error ("%s\n", error->message);

  apply_css (window, provider);

  gtk_widget_show (button);
  gtk_widget_show (window);
  gtk_main ();

  return 0;
}
