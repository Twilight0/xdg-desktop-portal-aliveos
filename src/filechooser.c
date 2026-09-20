#define _GNU_SOURCE 1

#include <config.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include "xdg-desktop-portal-dbus.h"
#include "filechooser.h"
#include "request.h"
#include "utils.h"
#include "aliveos-file-chooser-dialog.h"

typedef enum {
    FILE_CHOOSER_ACTION_OPEN,
    FILE_CHOOSER_ACTION_SAVE,
    FILE_CHOOSER_ACTION_SAVE_FILES
} FileChooserActionType;

typedef struct {
    XdpImplFileChooser *impl;
    GDBusMethodInvocation *invocation;
    Request *request;
    FileChooserActionType action_type;
    GVariant *options;
    GtkWidget *dialog;
} FileChooserHandle;

static void on_dialog_response (GtkDialog *dialog, gint response_id, gpointer user_data);
static gboolean handle_close (XdpImplRequest *object, GDBusMethodInvocation *invocation, gpointer user_data);

static void
file_chooser_handle_free (FileChooserHandle *handle)
{
    g_clear_object (&handle->request);
    if (handle->options)
        g_variant_unref (handle->options);
    g_free (handle);
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              gpointer user_data)
{
    FileChooserHandle *handle = user_data;
    g_debug ("FileChooser request closed by client (handle-close)");

    if (handle->dialog) {
        g_signal_handlers_disconnect_by_func (handle->dialog, G_CALLBACK (on_dialog_response), handle);
        gtk_widget_destroy (handle->dialog);
        handle->dialog = NULL;
    }

    g_autoptr(GVariantBuilder) results_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

    if (handle->action_type == FILE_CHOOSER_ACTION_OPEN) {
        xdp_impl_file_chooser_complete_open_file (handle->impl, handle->invocation, 1, g_variant_builder_end (results_builder));
    } else if (handle->action_type == FILE_CHOOSER_ACTION_SAVE) {
        xdp_impl_file_chooser_complete_save_file (handle->impl, handle->invocation, 1, g_variant_builder_end (results_builder));
    } else if (handle->action_type == FILE_CHOOSER_ACTION_SAVE_FILES) {
        xdp_impl_file_chooser_complete_save_files (handle->impl, handle->invocation, 1, g_variant_builder_end (results_builder));
    }

    g_signal_handlers_disconnect_by_func (handle->request, G_CALLBACK (handle_close), handle);
    if (handle->request->exported) {
        request_unexport (handle->request);
    }
    file_chooser_handle_free (handle);
    return FALSE;
}

static void
on_dialog_response (GtkDialog *dialog,
                    gint response_id,
                    gpointer user_data)
{
    FileChooserHandle *handle = user_data;
    guint response = 2; // Default to error
    g_autoptr(GVariantBuilder) results_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));

    g_debug ("on_dialog_response: response_id=%d", response_id);

    if (response_id == GTK_RESPONSE_ACCEPT || response_id == GTK_RESPONSE_OK) {
        if (handle->action_type == FILE_CHOOSER_ACTION_OPEN) {
            GSList *uris = aliveos_file_chooser_dialog_get_selected_uris (dialog);
            if (uris) {
                response = 0; // Success
                g_autoptr(GVariantBuilder) uris_builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
                for (GSList *l = uris; l != NULL; l = l->next) {
                    g_variant_builder_add (uris_builder, "s", (const gchar *)l->data);
                }
                g_variant_builder_add (results_builder, "{sv}", "uris", g_variant_builder_end (uris_builder));
                g_slist_free_full (uris, g_free);
            } else {
                response = 1; // Cancelled
            }
        } else if (handle->action_type == FILE_CHOOSER_ACTION_SAVE) {
            gchar *uri = aliveos_file_chooser_dialog_get_selected_uri (dialog);
            if (uri && *uri) {
                /* Check for overwrite confirmation */
                g_autoptr(GFile) file = g_file_new_for_uri (uri);
                if (g_file_query_exists (file, NULL)) {
                    g_autofree gchar *basename = g_file_get_basename (file);
                    GtkWidget *msg_dialog = gtk_message_dialog_new (GTK_WINDOW (dialog),
                                                                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                                    GTK_MESSAGE_QUESTION,
                                                                    GTK_BUTTONS_YES_NO,
                                                                    _("A file named \"%s\" already exists. Do you want to replace it?"),
                                                                    basename);
                    gtk_window_set_title (GTK_WINDOW (msg_dialog), _("Replace File"));
                    gint res = gtk_dialog_run (GTK_DIALOG (msg_dialog));
                    gtk_widget_destroy (msg_dialog);

                    if (res != GTK_RESPONSE_YES) {
                        g_free (uri);
                        return; // Keep dialog open so user can adjust filename
                    }
                }

                response = 0; // Success
                const gchar *uris[2] = { uri, NULL };
                g_variant_builder_add (results_builder, "{sv}", "uris", g_variant_new_strv (uris, -1));
                g_free (uri);
            } else {
                response = 1; // Cancelled
            }
        } else if (handle->action_type == FILE_CHOOSER_ACTION_SAVE_FILES) {
            gchar *folder_uri = aliveos_file_chooser_dialog_get_selected_uri (dialog);
            if (folder_uri && *folder_uri) {
                response = 0; // Success
                g_autoptr(GVariantBuilder) uris_builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
                g_autoptr(GFile) folder = g_file_new_for_uri (folder_uri);

                g_auto(GStrv) suggested_filenames = NULL;
                if (g_variant_lookup (handle->options, "filenames", "^as", &suggested_filenames)) {
                    for (gint i = 0; suggested_filenames[i] != NULL; i++) {
                        g_autoptr(GFile) child = g_file_get_child (folder, suggested_filenames[i]);
                        g_autofree gchar *child_uri = g_file_get_uri (child);
                        g_variant_builder_add (uris_builder, "s", child_uri);
                    }
                } else {
                    GVariant *files_variant = g_variant_lookup_value (handle->options, "files", G_VARIANT_TYPE ("aay"));
                    if (files_variant) {
                        GVariantIter iter;
                        const gchar *file_path;
                        g_variant_iter_init (&iter, files_variant);
                        while (g_variant_iter_next (&iter, "&ay", &file_path)) {
                            g_autoptr(GFile) orig_f = g_file_new_for_path (file_path);
                            g_autofree gchar *bname = g_file_get_basename (orig_f);
                            g_autoptr(GFile) child = g_file_get_child (folder, bname);
                            g_autofree gchar *child_uri = g_file_get_uri (child);
                            g_variant_builder_add (uris_builder, "s", child_uri);
                        }
                        g_variant_unref (files_variant);
                    } else {
                        g_variant_builder_add (uris_builder, "s", folder_uri);
                    }
                }

                g_variant_builder_add (results_builder, "{sv}", "uris", g_variant_builder_end (uris_builder));
                g_free (folder_uri);
            } else {
                response = 1; // Cancelled
            }
        }
    } else if (response_id == GTK_RESPONSE_CANCEL || response_id == GTK_RESPONSE_DELETE_EVENT || response_id == GTK_RESPONSE_CLOSE) {
        response = 1; // Cancelled
    } else {
        response = 2; // Error
    }

    g_signal_handlers_disconnect_by_func (handle->request, G_CALLBACK (handle_close), handle);

    if (handle->request->exported) {
        request_unexport (handle->request);
    }

    g_debug ("on_dialog_response: completing call with response %u", response);

    if (handle->action_type == FILE_CHOOSER_ACTION_OPEN) {
        xdp_impl_file_chooser_complete_open_file (handle->impl,
                                                 handle->invocation,
                                                 response,
                                                 g_variant_builder_end (results_builder));
    } else if (handle->action_type == FILE_CHOOSER_ACTION_SAVE) {
        xdp_impl_file_chooser_complete_save_file (handle->impl,
                                                 handle->invocation,
                                                 response,
                                                 g_variant_builder_end (results_builder));
    } else if (handle->action_type == FILE_CHOOSER_ACTION_SAVE_FILES) {
        xdp_impl_file_chooser_complete_save_files (handle->impl,
                                                  handle->invocation,
                                                  response,
                                                  g_variant_builder_end (results_builder));
    }

    gtk_widget_destroy (GTK_WIDGET (dialog));
    file_chooser_handle_free (handle);
}

static GList *
parse_filters (GVariant *options)
{
    GList *filters = NULL;
    GVariantIter *filters_iter = NULL;

    if (g_variant_lookup (options, "filters", "a(sa(us))", &filters_iter)) {
        GVariantIter *filter_iter;
        const gchar *filter_name;
        while (g_variant_iter_loop (filters_iter, "(&sa(us))", &filter_name, &filter_iter)) {
            AliveOSFileFilter *filter = aliveos_file_filter_new (filter_name);
            guint32 type;
            const gchar *pattern;
            while (g_variant_iter_loop (filter_iter, "(u&s)", &type, &pattern)) {
                if (type == 0) {
                    aliveos_file_filter_add_pattern (filter, pattern);
                } else if (type == 1) {
                    aliveos_file_filter_add_mime_type (filter, pattern);
                }
            }
            filters = g_list_append (filters, filter);
        }
        g_variant_iter_free (filters_iter);
    }

    return filters;
}

static gboolean
handle_open_file (XdpImplFileChooser *object,
                  GDBusMethodInvocation *invocation,
                  const char *arg_handle,
                  const char *arg_app_id,
                  const char *arg_parent_window,
                  const char *arg_title,
                  GVariant *arg_options)
{
    GDBusConnection *connection;
    FileChooserHandle *handle;
    g_autoptr(Request) request = NULL;
    const char *sender;

    g_debug ("handle_open_file: started");

    if (!gtk_init_check (NULL, NULL)) {
        g_warning ("GTK is not initialized or display is not available");
        g_autoptr(GVariantBuilder) results_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
        xdp_impl_file_chooser_complete_open_file (object, invocation, 2, g_variant_builder_end (results_builder));
        return TRUE;
    }

    sender = g_dbus_method_invocation_get_sender (invocation);
    connection = g_dbus_method_invocation_get_connection (invocation);
    request = request_new (sender, arg_app_id, arg_handle);

    gboolean multiple = FALSE;
    g_variant_lookup (arg_options, "multiple", "b", &multiple);
    gboolean directory = FALSE;
    g_variant_lookup (arg_options, "directory", "b", &directory);

    g_autofree gchar *current_folder_uri = NULL;
    const gchar *current_folder_path = NULL;
    if (g_variant_lookup (arg_options, "current_folder", "^ay", &current_folder_path)) {
        g_autoptr(GFile) f = g_file_new_for_path (current_folder_path);
        current_folder_uri = g_file_get_uri (f);
    }

    GList *filters = parse_filters (arg_options);

    GtkWidget *dialog = aliveos_file_chooser_dialog_new (arg_title ? arg_title : (directory ? _("Select Folder") : _("Open File")),
                                                         directory ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN,
                                                         multiple,
                                                         current_folder_uri,
                                                         NULL,
                                                         filters);

    if (!dialog) {
        g_warning ("Failed to create AliveOS file chooser dialog");
        g_autoptr(GVariantBuilder) results_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
        xdp_impl_file_chooser_complete_open_file (object, invocation, 2, g_variant_builder_end (results_builder));
        return TRUE;
    }

    handle = g_new0 (FileChooserHandle, 1);
    handle->impl = object;
    handle->invocation = invocation;
    handle->request = g_object_ref (request);
    handle->action_type = FILE_CHOOSER_ACTION_OPEN;
    handle->options = g_variant_ref (arg_options);
    handle->dialog = dialog;

    g_signal_connect (dialog, "response", G_CALLBACK (on_dialog_response), handle);
    g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

    request_export (request, connection);

    gtk_widget_show_all (dialog);
    gtk_window_present (GTK_WINDOW (dialog));

    return TRUE;
}

static gboolean
handle_save_file (XdpImplFileChooser *object,
                  GDBusMethodInvocation *invocation,
                  const char *arg_handle,
                  const char *arg_app_id,
                  const char *arg_parent_window,
                  const char *arg_title,
                  GVariant *arg_options)
{
    GDBusConnection *connection;
    FileChooserHandle *handle;
    g_autoptr(Request) request = NULL;
    const char *sender;

    g_debug ("handle_save_file: started");

    if (!gtk_init_check (NULL, NULL)) {
        g_warning ("GTK is not initialized or display is not available");
        g_autoptr(GVariantBuilder) results_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
        xdp_impl_file_chooser_complete_save_file (object, invocation, 2, g_variant_builder_end (results_builder));
        return TRUE;
    }

    sender = g_dbus_method_invocation_get_sender (invocation);
    connection = g_dbus_method_invocation_get_connection (invocation);
    request = request_new (sender, arg_app_id, arg_handle);

    g_autofree gchar *current_folder_uri = NULL;
    const gchar *current_folder_path = NULL;
    if (g_variant_lookup (arg_options, "current_folder", "^ay", &current_folder_path)) {
        g_autoptr(GFile) f = g_file_new_for_path (current_folder_path);
        current_folder_uri = g_file_get_uri (f);
    }

    const gchar *current_name = NULL;
    g_variant_lookup (arg_options, "current_name", "&s", &current_name);

    const gchar *current_file_path = NULL;
    if (!current_name && g_variant_lookup (arg_options, "current_file", "^ay", &current_file_path)) {
        g_autoptr(GFile) f = g_file_new_for_path (current_file_path);
        current_name = g_file_get_basename (f);
        if (!current_folder_uri) {
            g_autoptr(GFile) parent = g_file_get_parent (f);
            if (parent)
                current_folder_uri = g_file_get_uri (parent);
        }
    }

    GList *filters = parse_filters (arg_options);

    GtkWidget *dialog = aliveos_file_chooser_dialog_new (arg_title ? arg_title : _("Save File"),
                                                         GTK_FILE_CHOOSER_ACTION_SAVE,
                                                         FALSE,
                                                         current_folder_uri,
                                                         current_name,
                                                         filters);

    if (!dialog) {
        g_warning ("Failed to create AliveOS file chooser dialog");
        g_autoptr(GVariantBuilder) results_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
        xdp_impl_file_chooser_complete_save_file (object, invocation, 2, g_variant_builder_end (results_builder));
        return TRUE;
    }

    handle = g_new0 (FileChooserHandle, 1);
    handle->impl = object;
    handle->invocation = invocation;
    handle->request = g_object_ref (request);
    handle->action_type = FILE_CHOOSER_ACTION_SAVE;
    handle->options = g_variant_ref (arg_options);
    handle->dialog = dialog;

    g_signal_connect (dialog, "response", G_CALLBACK (on_dialog_response), handle);
    g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

    request_export (request, connection);

    gtk_widget_show_all (dialog);
    gtk_window_present (GTK_WINDOW (dialog));

    return TRUE;
}

static gboolean
handle_save_files (XdpImplFileChooser *object,
                   GDBusMethodInvocation *invocation,
                   const char *arg_handle,
                   const char *arg_app_id,
                   const char *arg_parent_window,
                   const char *arg_title,
                   GVariant *arg_options)
{
    GDBusConnection *connection;
    FileChooserHandle *handle;
    g_autoptr(Request) request = NULL;
    const char *sender;

    g_debug ("handle_save_files: started");

    if (!gtk_init_check (NULL, NULL)) {
        g_warning ("GTK is not initialized or display is not available");
        g_autoptr(GVariantBuilder) results_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
        xdp_impl_file_chooser_complete_save_files (object, invocation, 2, g_variant_builder_end (results_builder));
        return TRUE;
    }

    sender = g_dbus_method_invocation_get_sender (invocation);
    connection = g_dbus_method_invocation_get_connection (invocation);
    request = request_new (sender, arg_app_id, arg_handle);

    g_autofree gchar *current_folder_uri = NULL;
    const gchar *current_folder_path = NULL;
    if (g_variant_lookup (arg_options, "current_folder", "^ay", &current_folder_path)) {
        g_autoptr(GFile) f = g_file_new_for_path (current_folder_path);
        current_folder_uri = g_file_get_uri (f);
    }

    GtkWidget *dialog = aliveos_file_chooser_dialog_new (arg_title ? arg_title : _("Select Folder to Save Files"),
                                                         GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                         FALSE,
                                                         current_folder_uri,
                                                         NULL,
                                                         NULL);

    if (!dialog) {
        g_warning ("Failed to create AliveOS file chooser dialog");
        g_autoptr(GVariantBuilder) results_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{sv}"));
        xdp_impl_file_chooser_complete_save_files (object, invocation, 2, g_variant_builder_end (results_builder));
        return TRUE;
    }

    handle = g_new0 (FileChooserHandle, 1);
    handle->impl = object;
    handle->invocation = invocation;
    handle->request = g_object_ref (request);
    handle->action_type = FILE_CHOOSER_ACTION_SAVE_FILES;
    handle->options = g_variant_ref (arg_options);
    handle->dialog = dialog;

    g_signal_connect (dialog, "response", G_CALLBACK (on_dialog_response), handle);
    g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

    request_export (request, connection);

    gtk_widget_show_all (dialog);
    gtk_window_present (GTK_WINDOW (dialog));

    return TRUE;
}

gboolean
filechooser_init (GDBusConnection *bus,
                  GError **error)
{
    GDBusInterfaceSkeleton *helper;

    helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_file_chooser_skeleton_new ());

    g_signal_connect (helper, "handle-open-file", G_CALLBACK (handle_open_file), NULL);
    g_signal_connect (helper, "handle-save-file", G_CALLBACK (handle_save_file), NULL);
    g_signal_connect (helper, "handle-save-files", G_CALLBACK (handle_save_files), NULL);

    if (!g_dbus_interface_skeleton_export (helper,
                                           bus,
                                           DESKTOP_PORTAL_OBJECT_PATH,
                                           error)) {
        return FALSE;
    }

    g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

    return TRUE;
}
