/*
 * Copyright © 2016 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
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
 *
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
 */

#define _GNU_SOURCE 1

#include "config.h"

#include <string.h>

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <gio/gdesktopappinfo.h>

#include "appchooserdialog.h"
#include "appchooserrow.h"

#define LOCATION_MAX_LENGTH 40
#define INITIAL_LIST_SIZE 3

struct _AppChooserDialog {
  GtkWindow parent;

  GtkWidget *scrolled_window;
  GtkWidget *cancel_button;
  GtkWidget *open_button;
  GtkWidget *list;
  GtkWidget *heading;
  GtkWidget *stack;
  GtkWidget *empty_box;
  GtkWidget *empty_label;

  char *content_type;

  char **choices;

  GtkWidget *selected_row;
  GtkWidget *more_row;

  GAppInfo *info;
};

struct _AppChooserDialogClass {
  GtkWindowClass parent_class;

  void (* close) (AppChooserDialog *dialog);
};

enum {
  CLOSE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (AppChooserDialog, app_chooser_dialog, GTK_TYPE_WINDOW)

static void
update_header (GtkListBoxRow *row,
               GtkListBoxRow *before,
               gpointer       data)
{
  if (before != NULL &&
      gtk_list_box_row_get_header (row) == NULL)
    {
      GtkWidget *separator;

      separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
      gtk_widget_show (separator);
      gtk_list_box_row_set_header (row, separator);
    }
}

static void
app_chooser_dialog_init (AppChooserDialog *dialog)
{
  gtk_widget_init_template (GTK_WIDGET (dialog));

  gtk_list_box_set_header_func (GTK_LIST_BOX (dialog->list), update_header, NULL, NULL);
}

static void
app_chooser_dialog_finalize (GObject *object)
{
  AppChooserDialog *dialog = APP_CHOOSER_DIALOG (object);

  g_free (dialog->content_type);
  g_strfreev (dialog->choices);

  G_OBJECT_CLASS (app_chooser_dialog_parent_class)->finalize (object);
}

GAppInfo *
app_chooser_dialog_get_info (AppChooserDialog *dialog)
{
  return dialog->info;
}

static void
close_dialog (AppChooserDialog *dialog,
              GAppInfo *info)
{
  dialog->info = info;
  g_signal_emit (dialog, signals[CLOSE], 0);
}

static void
show_more (AppChooserDialog *dialog)
{
  int i;

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (dialog->scrolled_window),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_window_set_resizable (GTK_WINDOW (dialog), TRUE);
  gtk_widget_hide (dialog->more_row);

  g_return_if_fail (g_strv_length ((char **)dialog->choices) > INITIAL_LIST_SIZE);

  for (i = INITIAL_LIST_SIZE; dialog->choices[i]; i++)
    {
      g_autofree char *desktop_id = g_strconcat (dialog->choices[i], ".desktop", NULL);
      g_autoptr(GAppInfo) info = G_APP_INFO (g_desktop_app_info_new (desktop_id));
      GtkWidget *row;

      if (info == NULL)
        continue;

      row = GTK_WIDGET (app_chooser_row_new (info));
      gtk_widget_set_visible (row, TRUE);
      gtk_list_box_insert (GTK_LIST_BOX (dialog->list), row, -1);
   }
}

static void
row_activated (GtkListBox *list,
               GtkWidget *row,
               AppChooserDialog *dialog)
{
  GAppInfo *info = NULL;

  if (row == dialog->more_row)
    {
      show_more (dialog);
      return;
    }

  if (dialog->selected_row)
    info = app_chooser_row_get_info (APP_CHOOSER_ROW (dialog->selected_row));
  close_dialog (dialog, info);
}

static void
row_selected (GtkListBox *list,
              GtkWidget *row,
              AppChooserDialog *dialog)
{
  gtk_widget_set_sensitive (dialog->open_button, TRUE);
  dialog->selected_row = row;
}

static void
cancel_clicked (GtkWidget *button,
                AppChooserDialog *dialog)
{
  close_dialog (dialog, NULL);
}

static void
open_clicked (GtkWidget *button,
              AppChooserDialog *dialog)
{
  GAppInfo *info = NULL;

  if (dialog->selected_row)
    info = app_chooser_row_get_info (APP_CHOOSER_ROW (dialog->selected_row));
  close_dialog (dialog, info);
}

static gboolean
app_chooser_delete_event (GtkWidget *dialog, GdkEventAny *event)
{
  close_dialog (APP_CHOOSER_DIALOG (dialog), NULL);

  return TRUE;
}

static void
app_chooser_dialog_close (AppChooserDialog *dialog)
{
  gtk_window_close (GTK_WINDOW (dialog));
}

static void
app_chooser_dialog_class_init (AppChooserDialogClass *class)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GtkBindingSet *binding_set;

  object_class->finalize = app_chooser_dialog_finalize;

  widget_class->delete_event = app_chooser_delete_event;

  class->close = app_chooser_dialog_close;

  signals[CLOSE] = g_signal_new ("close",
                                G_TYPE_FROM_CLASS (class),
                                G_SIGNAL_ACTION | G_SIGNAL_RUN_LAST,
                                0,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 0);

  binding_set = gtk_binding_set_by_class (class);
  gtk_binding_entry_add_signal (binding_set, GDK_KEY_Escape, 0, "close", 0);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/xapp/appchooserdialog.ui");
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, scrolled_window);
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, open_button);
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, list);
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, heading);
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, stack);
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, empty_box);
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, empty_label);

  gtk_widget_class_bind_template_callback (widget_class, row_activated);
  gtk_widget_class_bind_template_callback (widget_class, row_selected);
  gtk_widget_class_bind_template_callback (widget_class, cancel_clicked);
  gtk_widget_class_bind_template_callback (widget_class, open_clicked);
}

/* Ellipsize the location, keeping the suffix which is likely
 * to have more relevant information, such as filename and extension.
 */
static char *
shorten_location (const char *location)
{
  int len;

  if (location == NULL)
    return NULL;

  len = g_utf8_strlen (location, -1);

  if (len < LOCATION_MAX_LENGTH)
    return g_strdup (location);

  for (; len >= LOCATION_MAX_LENGTH; len--)
    location = g_utf8_next_char (location);

  return g_strconcat ("…", location, NULL);
}

static void
ensure_default_in_initial_list (char       **choices,
                                const char  *default_id)
{
  int i;
  guint n_choices;

  if (default_id == NULL)
    return;

  n_choices = g_strv_length (choices);
  if (n_choices <= INITIAL_LIST_SIZE)
    return;

  for (i = 0; i < INITIAL_LIST_SIZE; i++)
    {
      if (strcmp (choices[i], default_id) == 0)
        return;
    }

  for (i = INITIAL_LIST_SIZE; i < n_choices; i++)
    {
      if (strcmp (choices[i], default_id) == 0)
        {
          char *tmp = choices[0];
          choices[0] = choices[i];
          choices[i] = tmp;
          return;
        }
    }

  g_warning ("default_id not found\n");
}

static void
more_pressed (GtkGestureMultiPress *gesture,
              int n_press,
              double x,
              double y,
              AppChooserDialog *dialog)
{
  if (n_press != 1)
    return;

  if (gtk_list_box_get_row_at_y (GTK_LIST_BOX (dialog->list), y) == GTK_LIST_BOX_ROW (dialog->more_row))
    show_more (dialog);
}

AppChooserDialog *
app_chooser_dialog_new (const char **choices,
                        const char *default_id,
                        const char *content_type,
                        const char *location)
{
  AppChooserDialog *dialog;
  int n_choices;
  int i;
  g_autofree char *short_location = shorten_location (location);

  dialog = g_object_new (app_chooser_dialog_get_type (), NULL);

  dialog->content_type = g_strdup (content_type);

  if (location)
    {
      g_autofree char *heading = NULL;

      heading = g_strdup_printf (_("Choose an application to open the file “%s”."), short_location);
      gtk_label_set_label (GTK_LABEL (dialog->heading), heading);
    }
  else
    {
      gtk_label_set_label (GTK_LABEL (dialog->heading), _("Choose an application."));
    }

  /* The caller's array is not ours to reorder, so work on a copy. */
  dialog->choices = g_strdupv ((char **)choices);
  ensure_default_in_initial_list (dialog->choices, default_id);
  choices = (const char **) dialog->choices;

  n_choices = g_strv_length ((char **)choices);
  if (n_choices == 0)
    {
      gtk_widget_show (dialog->empty_box);
      gtk_stack_set_visible_child_name (GTK_STACK (dialog->stack), "empty");
      gtk_widget_hide (dialog->open_button);
      gtk_button_set_label (GTK_BUTTON (dialog->cancel_button), _("_OK"));
      if (location)
        {
          g_autofree char *label = NULL;

          label = g_strdup_printf (_("No apps installed that can open “%s”."), short_location);
          gtk_label_set_label (GTK_LABEL (dialog->empty_label), label);
        }
      else
        {
          gtk_label_set_label (GTK_LABEL (dialog->empty_label), _("No suitable app installed."));
        }
    }
  else
    {
      gtk_stack_set_visible_child_name (GTK_STACK (dialog->stack), "list");
      gtk_widget_grab_default (dialog->open_button);
      for (i = 0; i < MIN (n_choices, INITIAL_LIST_SIZE); i++)
        {
          g_autofree char *desktop_id = g_strconcat (choices[i], ".desktop", NULL);
          g_autoptr(GAppInfo) info = G_APP_INFO (g_desktop_app_info_new (desktop_id));
          GtkWidget *row;

          if (info == NULL)
            continue;

          row = GTK_WIDGET (app_chooser_row_new (info));
          gtk_widget_set_visible (row, TRUE);
          gtk_list_box_insert (GTK_LIST_BOX (dialog->list), row, -1);

          if (default_id && strcmp (choices[i], default_id) == 0)
            {
              gtk_list_box_select_row (GTK_LIST_BOX (dialog->list), GTK_LIST_BOX_ROW (row));
              gtk_widget_set_sensitive (dialog->open_button, TRUE);
              dialog->selected_row = row;
            }
        }
      if (n_choices > INITIAL_LIST_SIZE)
        {
          GtkWidget *row;
          GtkWidget *image;
          GtkGesture *gesture;

          row = GTK_WIDGET (gtk_list_box_row_new ());

          gesture = gtk_gesture_multi_press_new (dialog->list);
          gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (gesture), FALSE);
          gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (gesture), GTK_PHASE_BUBBLE);
          gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (gesture), GDK_BUTTON_PRIMARY);

          g_signal_connect (gesture, "pressed", G_CALLBACK (more_pressed), dialog);

          gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
          image = gtk_image_new_from_icon_name ("view-more-symbolic", GTK_ICON_SIZE_BUTTON);
          g_object_set (image, "margin", 14, NULL);
          gtk_container_add (GTK_CONTAINER (row), image);
          gtk_widget_set_visible (row, TRUE);
          gtk_widget_set_visible (image, TRUE);
          gtk_list_box_insert (GTK_LIST_BOX (dialog->list), row, -1);
          dialog->more_row = row;
        }
      
    }

  return dialog;
}

void
app_chooser_dialog_update_choices (AppChooserDialog  *dialog,
                                   const char       **choices)
{
  int i;
  GPtrArray *new_choices;

  new_choices = g_ptr_array_new ();
  g_ptr_array_set_size (new_choices, g_strv_length (dialog->choices));
  for (i = 0; dialog->choices[i]; i++)
    new_choices->pdata[i] = dialog->choices[i];

  for (i = 0; choices[i]; i++)
    {
      if (g_strv_contains ((const char * const *)dialog->choices, choices[i]))
        continue;

      g_ptr_array_add (new_choices, g_strdup (choices[i]));

      /* While the "more" row is visible, extra choices stay hidden until it is expanded. */
      if (dialog->more_row == NULL || !gtk_widget_get_visible (dialog->more_row))
        {
          g_autofree char *desktop_id = g_strconcat (choices[i], ".desktop", NULL);
          g_autoptr(GAppInfo) info = G_APP_INFO (g_desktop_app_info_new (desktop_id));
          GtkWidget *row;

          if (info == NULL)
            continue;

          row = GTK_WIDGET (app_chooser_row_new (info));
          gtk_widget_set_visible (row, TRUE);
          gtk_list_box_insert (GTK_LIST_BOX (dialog->list), row, -1);
          gtk_stack_set_visible_child_name (GTK_STACK (dialog->stack), "list");
        }
    }

  g_ptr_array_add (new_choices, NULL);

  g_free (dialog->choices);
  dialog->choices = (char **) g_ptr_array_free (new_choices, FALSE);
}
