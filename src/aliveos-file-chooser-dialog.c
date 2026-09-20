/*
 * dory-file-chooser-dialog: Custom layout file picker dialog implementation
 */

#include <config.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>

#include "aliveos-file-chooser-dialog.h"


AliveOSFileFilter *
aliveos_file_filter_new (const gchar *name)
{
    AliveOSFileFilter *filter = g_new0 (AliveOSFileFilter, 1);
    filter->name = g_strdup (name);
    return filter;
}

void
aliveos_file_filter_add_pattern (AliveOSFileFilter *filter, const gchar *pattern)
{
    if (filter && pattern && *pattern) {
        filter->patterns = g_list_append (filter->patterns, g_strdup (pattern));
    }
}

void
aliveos_file_filter_add_mime_type (AliveOSFileFilter *filter, const gchar *mime_type)
{
    if (filter && mime_type && *mime_type) {
        filter->mime_types = g_list_append (filter->mime_types, g_strdup (mime_type));
    }
}

void
aliveos_file_filter_free (AliveOSFileFilter *filter)
{
    if (!filter)
        return;
    g_free (filter->name);
    g_list_free_full (filter->patterns, g_free);
    g_list_free_full (filter->mime_types, g_free);
    g_free (filter);
}

typedef struct {
    GtkBuilder *builder;
    GtkWidget *dialog;
    
    GtkFileChooserAction action;
    gboolean select_multiple;
    
    GFile *current_folder;
    GList *history_back;
    GList *history_forward;
    
    GtkListStore *list_store;
    GtkWidget *tree_view;
    GtkWidget *icon_view;
    GtkWidget *compact_view;
    
    GtkWidget *places_sidebar;
    GtkWidget *path_bar_label;
    GtkWidget *filename_entry;
    GtkWidget *filter_combo;
    
    GtkWidget *preview_image;
    GtkWidget *preview_name_label;
    GtkWidget *preview_size_val;
    GtkWidget *preview_type_val;
    GtkWidget *preview_modified_val;
    GdkPixbuf *preview_pixbuf;
    gint preview_target_size;
    
    GSList *selected_uris;
    gchar *selected_uri;

    gint small_icon_size;
    gint large_icon_size;
    gboolean show_hidden;
    
    GtkWidget *path_stack;
    GtkWidget *path_entry;
    GtkWidget *path_breadcrumb_box;
    GSettings *settings;
    guint zoom_timeout_id;
    guint preview_timeout_id;
    gint last_preview_target;
    GHashTable *thumb_cache;
    GList *filters;

} DialogData;

enum {
    COL_NAME,
    COL_ICON,
    COL_GRID_ICON,
    COL_SIZE,
    COL_SIZE_STR,
    COL_TYPE,
    COL_MODIFIED_TIME,
    COL_MODIFIED_STR,
    COL_IS_DIR,
    COL_URI,
    COL_GICON,
    COL_THUMBNAIL_PATH,
    COL_NUM_COLUMNS
};

static void load_directory (DialogData *data, GFile *folder);
static void navigate_to (DialogData *data, GFile *folder, gboolean save_history);
static void schedule_zoom_update (DialogData *data);
static void update_store_icons (DialogData *data, gint small_icon_size, gint large_icon_size);
static void update_preview_thumbnail (DialogData *data);

static void
add_to_recent_locations (DialogData *data, const gchar *uri)
{
    if (!uri)
        return;

    /* Skip special locations */
    if (g_str_has_prefix (uri, "trash://") ||
        g_str_has_prefix (uri, "search://") ||
        g_str_has_prefix (uri, "recent://") ||
        g_str_has_prefix (uri, "favorite://") ||
        g_str_has_prefix (uri, "x-nemo-desktop://")) {
        return;
    }

    GtkRecentManager *recent_manager = gtk_recent_manager_get_default ();
    GtkRecentData recent_data = { 0 };
    recent_data.mime_type = "inode/directory";
    recent_data.app_name = g_strdup (g_get_application_name ());
    recent_data.app_exec = g_strdup ("nemo %u");
    recent_data.is_private = FALSE;

    gtk_recent_manager_add_full (recent_manager, uri, &recent_data);

    g_free (recent_data.app_name);
    g_free (recent_data.app_exec);
}

static GFile *
resolve_valid_directory (const gchar *uri_or_path)
{
    if (!uri_or_path || !*uri_or_path)
        return NULL;

    GFile *file = g_file_new_for_commandline_arg (uri_or_path);
    if (!file)
        return NULL;

    if (!g_file_query_exists (file, NULL)) {
        g_object_unref (file);
        return NULL;
    }

    GFileType type = g_file_query_file_type (file, G_FILE_QUERY_INFO_NONE, NULL);
    if (type == G_FILE_TYPE_DIRECTORY) {
        return file;
    } else if (type == G_FILE_TYPE_REGULAR || type == G_FILE_TYPE_SYMBOLIC_LINK) {
        GFile *parent = g_file_get_parent (file);
        g_object_unref (file);
        if (parent && g_file_query_exists (parent, NULL) &&
            g_file_query_file_type (parent, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY) {
            return parent;
        }
        g_clear_object (&parent);
    } else {
        g_object_unref (file);
    }

    return NULL;
}

static void
save_chooser_settings (DialogData *data)
{
    if (!data->settings)
        return;

    /* Save last folder */
    if (data->current_folder && g_file_query_exists (data->current_folder, NULL)) {
        GFileType type = g_file_query_file_type (data->current_folder, G_FILE_QUERY_INFO_NONE, NULL);
        if (type == G_FILE_TYPE_DIRECTORY) {
            g_autofree gchar *uri = g_file_get_uri (data->current_folder);
            g_settings_set_string (data->settings, "last-chooser-folder", uri);
        }
    }

    /* Save view mode */
    GtkWidget *stack = GTK_WIDGET (gtk_builder_get_object (data->builder, "browser_stack"));
    const gchar *view = gtk_stack_get_visible_child_name (GTK_STACK (stack));
    g_settings_set_string (data->settings, "chooser-view-mode", view ? view : "grid");

    /* Save sort column and direction */
    GtkTreeSortable *sortable = GTK_TREE_SORTABLE (data->list_store);
    gint sort_id;
    GtkSortType sort_type;
    if (gtk_tree_sortable_get_sort_column_id (sortable, &sort_id, &sort_type)) {
        const gchar *col_name = "name";
        switch (sort_id) {
            case COL_NAME: col_name = "name"; break;
            case COL_SIZE: col_name = "size"; break;
            case COL_TYPE: col_name = "type"; break;
            case COL_MODIFIED_TIME: col_name = "modified"; break;
        }
        g_settings_set_string (data->settings, "chooser-sort-column", col_name);
        g_settings_set_boolean (data->settings, "chooser-sort-ascending", sort_type == GTK_SORT_ASCENDING);
    }

    /* Save hidden files state */
    g_settings_set_boolean (data->settings, "chooser-show-hidden", data->show_hidden);
}

static void
restore_chooser_settings (DialogData *data)
{
    if (!data->settings)
        return;

    /* Restore hidden files state */
    data->show_hidden = g_settings_get_boolean (data->settings, "chooser-show-hidden");

    /* Restore sort column */
    g_autofree gchar *sort_col = g_settings_get_string (data->settings, "chooser-sort-column");
    gint sort_id = COL_NAME;
    if (g_strcmp0 (sort_col, "size") == 0) sort_id = COL_SIZE;
    else if (g_strcmp0 (sort_col, "type") == 0) sort_id = COL_TYPE;
    else if (g_strcmp0 (sort_col, "modified") == 0) sort_id = COL_MODIFIED_TIME;

    gboolean ascending = g_settings_get_boolean (data->settings, "chooser-sort-ascending");
    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (data->list_store), sort_id,
                                         ascending ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
}

static gchar *
get_last_folder (DialogData *data)
{
    if (!data->settings)
        return NULL;
    return g_settings_get_string (data->settings, "last-chooser-folder");
}

static void
on_breadcrumb_clicked (GtkButton *btn, gpointer user_data)
{
    DialogData *data = user_data;
    gchar *path = g_object_get_data (G_OBJECT (btn), "breadcrumb-path");
    
    if (path && *path) {
        GFile *folder = g_file_new_for_path (path);
        navigate_to (data, folder, TRUE);
        g_object_unref (folder);
    }
}

static void
update_breadcrumb_bar (DialogData *data)
{
    if (!data->path_breadcrumb_box || !data->current_folder)
        return;

    /* Clear existing breadcrumbs */
    gtk_container_foreach (GTK_CONTAINER (data->path_breadcrumb_box),
                           (GtkCallback) gtk_widget_destroy, NULL);

    g_autofree gchar *path = g_file_get_path (data->current_folder);
    if (!path)
        return;

    gchar **parts = g_strsplit (path, "/", -1);
    GString *accumulated = g_string_new (NULL);

    for (gint i = 0; parts[i] != NULL; i++) {
        const gchar *part = parts[i];
        if (*part == '\0') {
            accumulated->len = 0;
            g_string_append_c (accumulated, '/');
            continue;
        }

        /* Add separator */
        if (accumulated->len > 0 && accumulated->str[accumulated->len - 1] != '/') {
            GtkWidget *sep = gtk_label_new (NULL);
            gtk_label_set_markup (GTK_LABEL (sep), "<span foreground='#888888'> / </span>");
            gtk_widget_set_halign (sep, GTK_ALIGN_START);
            gtk_container_add (GTK_CONTAINER (data->path_breadcrumb_box), sep);
        }

        accumulated->len = strlen (accumulated->str);
        if (i > 0 && accumulated->str[accumulated->len - 1] != '/')
            g_string_append_c (accumulated, '/');
        g_string_append (accumulated, part);

        GtkWidget *btn = gtk_button_new_with_label (part);
        gtk_widget_set_name (btn, "breadcrumb-button");
        gtk_widget_set_halign (btn, GTK_ALIGN_START);

        /* Store the full path up to this segment */
        gchar *full_path = g_strdup (accumulated->str);
        g_object_set_data_full (G_OBJECT (btn), "breadcrumb-path", full_path, g_free);

        /* Connect click signal to navigate, NOT to toggle edit mode */
        g_signal_connect (btn, "clicked", G_CALLBACK (on_breadcrumb_clicked), data);

        gtk_container_add (GTK_CONTAINER (data->path_breadcrumb_box), btn);
    }

    gtk_widget_show_all (data->path_breadcrumb_box);
    g_strfreev (parts);
    g_string_free (accumulated, TRUE);
}

static gint
compare_rows_folders_first (GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer user_data)
{
    gint sort_col_id = GPOINTER_TO_INT (user_data);
    gboolean is_dir_a, is_dir_b;
    
    gtk_tree_model_get (model, a, COL_IS_DIR, &is_dir_a, -1);
    gtk_tree_model_get (model, b, COL_IS_DIR, &is_dir_b, -1);
    
    if (is_dir_a && !is_dir_b) {
        return -1;
    }
    if (!is_dir_a && is_dir_b) {
        return 1;
    }
    
    switch (sort_col_id) {
        case COL_NAME: {
            gchar *name_a, *name_b;
            gtk_tree_model_get (model, a, COL_NAME, &name_a, -1);
            gtk_tree_model_get (model, b, COL_NAME, &name_b, -1);
            gint res = g_utf8_collate (name_a, name_b);
            g_free (name_a);
            g_free (name_b);
            return res;
        }
        case COL_SIZE: {
            gint64 size_a, size_b;
            gtk_tree_model_get (model, a, COL_SIZE, &size_a, -1);
            gtk_tree_model_get (model, b, COL_SIZE, &size_b, -1);
            if (size_a < size_b) return -1;
            if (size_a > size_b) return 1;
            return 0;
        }
        case COL_TYPE: {
            gchar *type_a, *type_b;
            gtk_tree_model_get (model, a, COL_TYPE, &type_a, -1);
            gtk_tree_model_get (model, b, COL_TYPE, &type_b, -1);
            gint res = g_utf8_collate (type_a, type_b);
            g_free (type_a);
            g_free (type_b);
            return res;
        }
        case COL_MODIFIED_TIME: {
            gint64 time_a, time_b;
            gtk_tree_model_get (model, a, COL_MODIFIED_TIME, &time_a, -1);
            gtk_tree_model_get (model, b, COL_MODIFIED_TIME, &time_b, -1);
            if (time_a < time_b) return -1;
            if (time_a > time_b) return 1;
            return 0;
        }
        default:
            return 0;
    }
}

static void
free_dialog_data (gpointer user_data)
{
    DialogData *data = user_data;
    
    /* Save settings before destroying */
    save_chooser_settings (data);
    
    g_clear_object (&data->builder);
    g_clear_object (&data->current_folder);
    
    g_list_free_full (data->history_back, g_object_unref);
    g_list_free_full (data->history_forward, g_object_unref);
    
    g_slist_free_full (data->selected_uris, g_free);
    g_free (data->selected_uri);
    
    if (data->zoom_timeout_id)
        g_source_remove (data->zoom_timeout_id);
    if (data->preview_timeout_id)
        g_source_remove (data->preview_timeout_id);
    g_clear_object (&data->preview_pixbuf);
    if (data->thumb_cache)
        g_hash_table_unref (data->thumb_cache);
    g_clear_object (&data->settings);
    if (data->filters) {
        g_list_free_full (data->filters, (GDestroyNotify) aliveos_file_filter_free);
        data->filters = NULL;
    }
    
    g_free (data);
}

static void
update_navigation_buttons (DialogData *data)
{
    GtkWidget *back_btn = GTK_WIDGET (gtk_builder_get_object (data->builder, "back_button"));
    GtkWidget *fwd_btn = GTK_WIDGET (gtk_builder_get_object (data->builder, "forward_button"));
    GtkWidget *up_btn = GTK_WIDGET (gtk_builder_get_object (data->builder, "up_button"));
    
    gtk_widget_set_sensitive (back_btn, data->history_back != NULL);
    gtk_widget_set_sensitive (fwd_btn, data->history_forward != NULL);
    
    if (up_btn) {
        gboolean has_parent = FALSE;
        if (data->current_folder) {
            GFile *parent = g_file_get_parent (data->current_folder);
            if (parent) {
                has_parent = TRUE;
                g_object_unref (parent);
            }
        }
        gtk_widget_set_sensitive (up_btn, has_parent);
    }
}

static void
navigate_to (DialogData *data, GFile *folder, gboolean save_history)
{
    if (!folder)
        return;
        
    if (save_history && data->current_folder) {
        data->history_back = g_list_prepend (data->history_back, g_object_ref (data->current_folder));
        g_list_free_full (data->history_forward, g_object_unref);
        data->history_forward = NULL;
    }
    
    g_clear_object (&data->current_folder);
    data->current_folder = g_object_ref (folder);
    
    /* Update path label */
    gchar *parse_name = g_file_get_parse_name (folder);
    gtk_label_set_text (GTK_LABEL (data->path_bar_label), parse_name);
    g_free (parse_name);
    
    /* Update path entry */
    if (data->path_entry) {
        gchar *path = g_file_get_path (folder);
        if (path) {
            gtk_entry_set_text (GTK_ENTRY (data->path_entry), path);
            g_free (path);
        } else {
            gchar *uri = g_file_get_uri (folder);
            gtk_entry_set_text (GTK_ENTRY (data->path_entry), uri);
            g_free (uri);
        }
    }
    
    /* Update breadcrumb bar */
    update_breadcrumb_bar (data);
    
    /* Save settings */
    save_chooser_settings (data);
    
    /* Set places sidebar focus */
    gtk_places_sidebar_set_location (GTK_PLACES_SIDEBAR (data->places_sidebar), folder);
    
    load_directory (data, folder);
    update_navigation_buttons (data);
}

static void
on_back_clicked (GtkButton *btn, gpointer user_data)
{
    DialogData *data = user_data;
    if (data->history_back) {
        GFile *folder = G_FILE (data->history_back->data);
        data->history_back = g_list_remove_link (data->history_back, data->history_back);
        
        if (data->current_folder)
            data->history_forward = g_list_prepend (data->history_forward, g_object_ref (data->current_folder));
            
        navigate_to (data, folder, FALSE);
        g_object_unref (folder);
    }
}

static void
on_forward_clicked (GtkButton *btn, gpointer user_data)
{
    DialogData *data = user_data;
    if (data->history_forward) {
        GFile *folder = G_FILE (data->history_forward->data);
        data->history_forward = g_list_remove_link (data->history_forward, data->history_forward);
        
        if (data->current_folder)
            data->history_back = g_list_prepend (data->history_back, g_object_ref (data->current_folder));
            
        navigate_to (data, folder, FALSE);
        g_object_unref (folder);
    }
}

static void
on_up_clicked (GtkButton *btn, gpointer user_data)
{
    DialogData *data = user_data;
    if (data->current_folder) {
        GFile *parent = g_file_get_parent (data->current_folder);
        if (parent) {
            navigate_to (data, parent, TRUE);
            g_object_unref (parent);
        }
    }
}

static void
on_places_sidebar_open_location (GtkPlacesSidebar *sidebar, GFile *location, GtkPlacesOpenFlags flags, gpointer user_data)
{
    navigate_to (user_data, location, TRUE);
    g_autofree gchar *uri = g_file_get_uri (location);
    add_to_recent_locations (user_data, uri);
}

static void
preview_info_ready_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    DialogData *data = user_data;
    g_autoptr(GFileInfo) info = g_file_query_info_finish (G_FILE (source_object), res, NULL);
    if (!info) {
        gtk_image_set_from_icon_name (GTK_IMAGE (data->preview_image), "text-x-generic", GTK_ICON_SIZE_DIALOG);
        return;
    }
    
    gchar *uri = g_file_get_uri (G_FILE (source_object));
    if (!data->selected_uri || g_strcmp0 (data->selected_uri, uri) != 0) {
        g_free (uri);
        return;
    }
    g_free (uri);
    
    const gchar *thumb_path = g_file_info_get_attribute_byte_string (info, "thumbnail::path");
    g_autoptr(GdkPixbuf) pixbuf = NULL;
    
    g_autoptr(GError) error = NULL;
    if (thumb_path) {
        pixbuf = gdk_pixbuf_new_from_file_at_size (thumb_path, 512, 512, &error);
        if (!pixbuf)
            g_debug ("Failed to load thumbnail %s: %s", thumb_path, error ? error->message : "unknown error");
    }
    
    if (!pixbuf) {
        gchar *path = g_file_get_path (G_FILE (source_object));
        if (path) {
            g_clear_error (&error);
            pixbuf = gdk_pixbuf_new_from_file_at_size (path, 512, 512, &error);
            if (!pixbuf)
                g_debug ("Failed to load preview for %s: %s", path, error ? error->message : "unknown error");
            g_free (path);
        }
    }
    
    if (pixbuf) {
        gint pw = gdk_pixbuf_get_width (pixbuf);
        gint ph = gdk_pixbuf_get_height (pixbuf);
        if (pw <= 0 || ph <= 0 || pw > 8192 || ph > 8192) {
            g_warning ("Rejecting preview image with invalid dimensions %dx%d", pw, ph);
            g_clear_object (&pixbuf);
        }
    }
    
    if (pixbuf) {
        g_clear_object (&data->preview_pixbuf);
        data->preview_pixbuf = g_object_ref (pixbuf);
        update_preview_thumbnail (data);
    } else {
        GIcon *icon = g_file_info_get_icon (info);
        g_autoptr(GtkIconInfo) icon_info = gtk_icon_theme_lookup_by_gicon (gtk_icon_theme_get_default (), icon, 384, GTK_ICON_LOOKUP_FORCE_SIZE);
        g_autoptr(GdkPixbuf) fallback_pixbuf = NULL;
        if (icon_info) {
            fallback_pixbuf = gtk_icon_info_load_icon (icon_info, NULL);
        }
        if (fallback_pixbuf) {
            g_clear_object (&data->preview_pixbuf);
            data->preview_pixbuf = g_object_ref (fallback_pixbuf);
            update_preview_thumbnail (data);
        } else {
            gtk_image_set_from_icon_name (GTK_IMAGE (data->preview_image), "text-x-generic", GTK_ICON_SIZE_DIALOG);
        }
    }
}

static void
update_preview_thumbnail (DialogData *data)
{
    if (!data->preview_pixbuf)
        return;

    gint w = gdk_pixbuf_get_width (data->preview_pixbuf);
    gint h = gdk_pixbuf_get_height (data->preview_pixbuf);
    if (w <= 0 || h <= 0)
        return;

    gint target = data->preview_target_size;
    if (target < 64)
        target = 192;
    if (target > 384)
        target = 384;

    gint tw, th;
    if (w >= h) {
        tw = target;
        th = MAX (1, h * target / w);
    } else {
        th = target;
        tw = MAX (1, w * target / h);
    }

    g_autoptr(GdkPixbuf) scaled = gdk_pixbuf_scale_simple (data->preview_pixbuf, tw, th, GDK_INTERP_BILINEAR);
    if (scaled)
        gtk_image_set_from_pixbuf (GTK_IMAGE (data->preview_image), scaled);
}

static gboolean
preview_resize_settle_cb (gpointer user_data)
{
    DialogData *data = user_data;
    data->preview_timeout_id = 0;
    update_preview_thumbnail (data);
    return G_SOURCE_REMOVE;
}

static void
schedule_preview_resize (DialogData *data)
{
    if (data->preview_timeout_id)
        g_source_remove (data->preview_timeout_id);
    data->preview_timeout_id = g_timeout_add (60, preview_resize_settle_cb, data);
}

static void
on_preview_box_size_allocate (GtkWidget *widget, GdkRectangle *allocation, gpointer user_data)
{
    DialogData *data = user_data;

    gint target = allocation->width - 16;
    if (target < 64)
        target = 64;
    if (target > 384)
        target = 384;
    if (target != data->preview_target_size) {
        data->preview_target_size = target;
        if (data->preview_pixbuf)
            schedule_preview_resize (data);
    }
}

static void
update_preview (DialogData *data, const gchar *uri, const gchar *name, gboolean is_dir, goffset size,
                 const gchar *type_desc, const gchar *mod_str)
{
    gtk_label_set_text (GTK_LABEL (data->preview_name_label), name ? name : _("No File Selected"));
    gtk_label_set_text (GTK_LABEL (data->preview_type_val), type_desc ? type_desc : "-");
    gtk_label_set_text (GTK_LABEL (data->preview_modified_val), mod_str ? mod_str : "-");
    
    g_clear_object (&data->preview_pixbuf);
    
    if (is_dir) {
        gtk_label_set_text (GTK_LABEL (data->preview_size_val), _("Folder"));
        gtk_image_set_from_icon_name (GTK_IMAGE (data->preview_image), "folder", GTK_ICON_SIZE_DIALOG);
    } else if (uri) {
        gchar *size_str = g_format_size (size);
        gtk_label_set_text (GTK_LABEL (data->preview_size_val), size_str);
        g_free (size_str);
        
        gtk_image_set_from_icon_name (GTK_IMAGE (data->preview_image), "image-missing", GTK_ICON_SIZE_DIALOG);
        
        GFile *file = g_file_new_for_uri (uri);
        g_file_query_info_async (file,
                                 "standard::*,thumbnail::path",
                                 G_FILE_QUERY_INFO_NONE,
                                 G_PRIORITY_DEFAULT,
                                 NULL,
                                 preview_info_ready_cb,
                                 data);
        g_object_unref (file);
    } else {
        gtk_label_set_text (GTK_LABEL (data->preview_size_val), "-");
        gtk_image_set_from_icon_name (GTK_IMAGE (data->preview_image), "image-missing", GTK_ICON_SIZE_DIALOG);
    }
}

static void
on_tree_selection_changed (GtkTreeSelection *selection, gpointer user_data)
{
    DialogData *data = user_data;
    GtkTreeModel *model;
    GList *paths = gtk_tree_selection_get_selected_rows (selection, &model);
    
    g_slist_free_full (data->selected_uris, g_free);
    data->selected_uris = NULL;

    gchar *last_name = NULL;
    gchar *last_uri = NULL;
    gboolean last_is_dir = FALSE;
    goffset last_size = 0;
    gchar *last_type = NULL;
    gchar *last_mod = NULL;

    for (GList *l = paths; l != NULL; l = l->next) {
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter (model, &iter, l->data)) {
            gchar *name = NULL;
            gchar *uri = NULL;
            gboolean is_dir = FALSE;
            goffset size = 0;
            gchar *type = NULL;
            gchar *mod = NULL;

            gtk_tree_model_get (model, &iter,
                                COL_NAME, &name,
                                COL_URI, &uri,
                                COL_IS_DIR, &is_dir,
                                COL_SIZE, &size,
                                COL_TYPE, &type,
                                COL_MODIFIED_STR, &mod,
                                -1);

            data->selected_uris = g_slist_append (data->selected_uris, g_strdup (uri));

            g_free (last_name);
            g_free (last_uri);
            g_free (last_type);
            g_free (last_mod);
            last_name = name;
            last_uri = uri;
            last_is_dir = is_dir;
            last_size = size;
            last_type = type;
            last_mod = mod;
        }
    }

    if (last_uri) {
        g_free (data->selected_uri);
        data->selected_uri = g_strdup (last_uri);

        if (data->action != GTK_FILE_CHOOSER_ACTION_SAVE || !last_is_dir) {
            gtk_entry_set_text (GTK_ENTRY (data->filename_entry), last_name);
        }

        update_preview (data, last_uri, last_name, last_is_dir, last_size, last_type, last_mod);
    }

    g_free (last_name);
    g_free (last_uri);
    g_free (last_type);
    g_free (last_mod);
    g_list_free_full (paths, (GDestroyNotify)gtk_tree_path_free);
}

static void
on_icon_view_item_activated (GtkIconView *icon_view, GtkTreePath *path, gpointer user_data)
{
    DialogData *data = user_data;
    GtkTreeModel *model = gtk_icon_view_get_model (icon_view);
    GtkTreeIter iter;
    
    if (gtk_tree_model_get_iter (model, &iter, path)) {
        gboolean is_dir;
        gchar *uri;
        
        gtk_tree_model_get (model, &iter, COL_IS_DIR, &is_dir, COL_URI, &uri, -1);
        
        if (is_dir) {
            GFile *folder = g_file_new_for_uri (uri);
            navigate_to (data, folder, TRUE);
            add_to_recent_locations (data, uri);
            g_object_unref (folder);
        } else {
            add_to_recent_locations (data, uri);
            gtk_dialog_response (GTK_DIALOG (data->dialog), GTK_RESPONSE_ACCEPT);
        }
        g_free (uri);
    }
}

static void
on_icon_view_selection_changed (GtkIconView *icon_view, gpointer user_data)
{
    DialogData *data = user_data;
    GList *paths = gtk_icon_view_get_selected_items (icon_view);
    GtkTreeModel *model = gtk_icon_view_get_model (icon_view);

    g_slist_free_full (data->selected_uris, g_free);
    data->selected_uris = NULL;

    gchar *last_name = NULL;
    gchar *last_uri = NULL;
    gboolean last_is_dir = FALSE;
    goffset last_size = 0;
    gchar *last_type = NULL;
    gchar *last_mod = NULL;

    for (GList *l = paths; l != NULL; l = l->next) {
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter (model, &iter, l->data)) {
            gchar *name = NULL;
            gchar *uri = NULL;
            gboolean is_dir = FALSE;
            goffset size = 0;
            gchar *type = NULL;
            gchar *mod = NULL;

            gtk_tree_model_get (model, &iter,
                                COL_NAME, &name,
                                COL_URI, &uri,
                                COL_IS_DIR, &is_dir,
                                COL_SIZE, &size,
                                COL_TYPE, &type,
                                COL_MODIFIED_STR, &mod,
                                -1);

            data->selected_uris = g_slist_append (data->selected_uris, g_strdup (uri));

            g_free (last_name);
            g_free (last_uri);
            g_free (last_type);
            g_free (last_mod);
            last_name = name;
            last_uri = uri;
            last_is_dir = is_dir;
            last_size = size;
            last_type = type;
            last_mod = mod;
        }
    }

    if (last_uri) {
        g_free (data->selected_uri);
        data->selected_uri = g_strdup (last_uri);

        if (data->action != GTK_FILE_CHOOSER_ACTION_SAVE || !last_is_dir) {
            gtk_entry_set_text (GTK_ENTRY (data->filename_entry), last_name);
        }

        update_preview (data, last_uri, last_name, last_is_dir, last_size, last_type, last_mod);
    }

    g_free (last_name);
    g_free (last_uri);
    g_free (last_type);
    g_free (last_mod);
    g_list_free_full (paths, (GDestroyNotify)gtk_tree_path_free);
}

static void
on_tree_view_row_activated (GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer user_data)
{
    DialogData *data = user_data;
    GtkTreeModel *model = gtk_tree_view_get_model (tree_view);
    GtkTreeIter iter;
    
    if (gtk_tree_model_get_iter (model, &iter, path)) {
        gboolean is_dir;
        gchar *uri;
        
        gtk_tree_model_get (model, &iter, COL_IS_DIR, &is_dir, COL_URI, &uri, -1);
        
        if (is_dir) {
            GFile *folder = g_file_new_for_uri (uri);
            navigate_to (data, folder, TRUE);
            add_to_recent_locations (data, uri);
            g_object_unref (folder);
        } else {
            add_to_recent_locations (data, uri);
            gtk_dialog_response (GTK_DIALOG (data->dialog), GTK_RESPONSE_ACCEPT);
        }
        g_free (uri);
    }
}

static void
on_view_toggle_changed (GtkToggleButton *btn, gpointer user_data)
{
    DialogData *data = user_data;
    gboolean is_grid = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (gtk_builder_get_object (data->builder, "grid_view_toggle")));
    gboolean is_compact = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (gtk_builder_get_object (data->builder, "compact_view_toggle")));
    GtkWidget *stack = GTK_WIDGET (gtk_builder_get_object (data->builder, "browser_stack"));
    
    if (is_grid) {
        gtk_stack_set_visible_child_name (GTK_STACK (stack), "grid");
    } else if (is_compact) {
        gtk_stack_set_visible_child_name (GTK_STACK (stack), "compact");
    } else {
        gtk_stack_set_visible_child_name (GTK_STACK (stack), "list");
    }
}

#define THUMB_SOURCE_SIZE 256

static GdkPixbuf *
get_thumb_source (DialogData *data, const gchar *thumbnail_path)
{
    GdkPixbuf *cached = g_hash_table_lookup (data->thumb_cache, thumbnail_path);
    if (cached)
        return cached;

    g_autoptr(GError) err = NULL;
    GdkPixbuf *decoded = gdk_pixbuf_new_from_file_at_size (thumbnail_path, THUMB_SOURCE_SIZE, THUMB_SOURCE_SIZE, &err);
    if (!decoded) {
        g_debug ("Failed to load thumbnail %s: %s", thumbnail_path, err ? err->message : "unknown error");
        return NULL;
    }

    gint bw = gdk_pixbuf_get_width (decoded);
    gint bh = gdk_pixbuf_get_height (decoded);
    if (bw <= 0 || bh <= 0 || bw > 8192 || bh > 8192) {
        g_warning ("Rejecting thumbnail with invalid dimensions %dx%d", bw, bh);
        g_object_unref (decoded);
        return NULL;
    }

    g_hash_table_insert (data->thumb_cache, g_strdup (thumbnail_path), decoded);
    return decoded;
}

static GdkPixbuf *
scale_pixbuf_fit (GdkPixbuf *src, gint size)
{
    gint w = gdk_pixbuf_get_width (src);
    gint h = gdk_pixbuf_get_height (src);
    if (w <= 0 || h <= 0 || size <= 0)
        return g_object_ref (src);

    gdouble scale = (gdouble) size / MAX (w, h);
    gint tw = MAX (1, (gint) (w * scale));
    gint th = MAX (1, (gint) (h * scale));
    if (tw == w && th == h)
        return g_object_ref (src);
    return gdk_pixbuf_scale_simple (src, tw, th, GDK_INTERP_BILINEAR);
}

static void
update_store_icons (DialogData *data, gint small_icon_size, gint large_icon_size)
{
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (data->list_store), &iter);
    
    while (valid) {
        GIcon *icon = NULL;
        gchar *thumbnail_path = NULL;
        
        gtk_tree_model_get (GTK_TREE_MODEL (data->list_store), &iter,
                            COL_GICON, &icon,
                            COL_THUMBNAIL_PATH, &thumbnail_path,
                            -1);
                            
        g_autoptr(GdkPixbuf) pixbuf_small = NULL;
        g_autoptr(GdkPixbuf) pixbuf_large = NULL;
        
        if (thumbnail_path) {
            GdkPixbuf *base = get_thumb_source (data, thumbnail_path);
            if (base) {
                pixbuf_large = scale_pixbuf_fit (base, large_icon_size);
                if (small_icon_size != large_icon_size && small_icon_size > 0) {
                    pixbuf_small = scale_pixbuf_fit (base, small_icon_size);
                } else {
                    pixbuf_small = g_object_ref (base);
                }
            }
        }
        
        if (icon) {
            if (!pixbuf_small) {
                g_autoptr(GtkIconInfo) icon_info = gtk_icon_theme_lookup_by_gicon (
                    gtk_icon_theme_get_default (), icon, small_icon_size, GTK_ICON_LOOKUP_FORCE_SIZE);
                if (icon_info) {
                    pixbuf_small = gtk_icon_info_load_icon (icon_info, NULL);
                }
            }
            if (!pixbuf_large) {
                g_autoptr(GtkIconInfo) icon_info = gtk_icon_theme_lookup_by_gicon (
                    gtk_icon_theme_get_default (), icon, large_icon_size, GTK_ICON_LOOKUP_FORCE_SIZE);
                if (icon_info) {
                    pixbuf_large = gtk_icon_info_load_icon (icon_info, NULL);
                }
            }
            g_object_unref (icon);
        }
        
        gtk_list_store_set (data->list_store, &iter,
                            COL_ICON, pixbuf_small,
                            COL_GRID_ICON, pixbuf_large,
                            -1);
                            
        g_free (thumbnail_path);
        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (data->list_store), &iter);
    }
}

static gboolean
on_view_scroll_event (GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
    DialogData *data = user_data;
    
    if (event->state & GDK_CONTROL_MASK) {
        gdouble delta_y = 0.0;
        gdouble delta_x = 0.0;
        gboolean is_scroll_up = FALSE;
        gboolean has_delta = FALSE;
        
        if (event->direction == GDK_SCROLL_SMOOTH) {
            if (gdk_event_get_scroll_deltas ((GdkEvent *)event, &delta_x, &delta_y)) {
                if (delta_y != 0.0) {
                    is_scroll_up = (delta_y < 0.0);
                    has_delta = TRUE;
                }
            }
        } else {
            is_scroll_up = (event->direction == GDK_SCROLL_UP || event->direction == GDK_SCROLL_LEFT);
            has_delta = TRUE;
        }
        
        if (has_delta) {
            GtkWidget *stack = GTK_WIDGET (gtk_builder_get_object (data->builder, "browser_stack"));
            const gchar *current_view = gtk_stack_get_visible_child_name (GTK_STACK (stack));
            
            if (g_strcmp0 (current_view, "grid") == 0) {
                gint item_width = gtk_icon_view_get_item_width (GTK_ICON_VIEW (data->icon_view));
                if (is_scroll_up) {
                    item_width = CLAMP (item_width + 16, 48, 256);
                } else {
                    item_width = CLAMP (item_width - 16, 48, 256);
                }
                gtk_icon_view_set_item_width (GTK_ICON_VIEW (data->icon_view), item_width);
                
                data->large_icon_size = CLAMP (item_width - 16, 32, 220);
                schedule_zoom_update (data);
            } else if (g_strcmp0 (current_view, "compact") == 0) {
                gint item_width = gtk_icon_view_get_item_width (GTK_ICON_VIEW (data->compact_view));
                if (item_width < 0) {
                    item_width = 120;
                }
                if (is_scroll_up) {
                    item_width = CLAMP (item_width + 16, 80, 320);
                } else {
                    item_width = CLAMP (item_width - 16, 80, 320);
                }
                gtk_icon_view_set_item_width (GTK_ICON_VIEW (data->compact_view), item_width);
                
                data->small_icon_size = CLAMP (item_width / 5, 16, 48);
                schedule_zoom_update (data);
            } else if (g_strcmp0 (current_view, "list") == 0) {
                if (is_scroll_up) {
                    data->small_icon_size = CLAMP (data->small_icon_size + 4, 16, 48);
                } else {
                    data->small_icon_size = CLAMP (data->small_icon_size - 4, 16, 48);
                }
                schedule_zoom_update (data);
            }
            return TRUE;
        }
    }
    
    return FALSE;
}

static gboolean
zoom_settle_cb (gpointer user_data)
{
    DialogData *data = user_data;
    data->zoom_timeout_id = 0;
    update_store_icons (data, data->small_icon_size, data->large_icon_size);
    return G_SOURCE_REMOVE;
}

static void
schedule_zoom_update (DialogData *data)
{
    if (data->zoom_timeout_id)
        g_source_remove (data->zoom_timeout_id);
    data->zoom_timeout_id = g_timeout_add (100, zoom_settle_cb, data);
}


static AliveOSFileFilter *
get_current_active_filter (DialogData *data)
{
    if (!data->filter_combo || !data->filters)
        return NULL;

    gint active = gtk_combo_box_get_active (GTK_COMBO_BOX (data->filter_combo));
    if (active < 0)
        return NULL;

    return g_list_nth_data (data->filters, active);
}

static gboolean
file_filter_matches (AliveOSFileFilter *filter, const gchar *name, const gchar *content_type)
{
    if (!filter)
        return TRUE;

    if (!filter->patterns && !filter->mime_types)
        return TRUE;

    for (GList *l = filter->patterns; l != NULL; l = l->next) {
        const gchar *pat = l->data;
        if (g_pattern_match_simple (pat, name))
            return TRUE;
    }

    if (content_type) {
        for (GList *l = filter->mime_types; l != NULL; l = l->next) {
            const gchar *mime = l->data;
            if (g_content_type_is_a (content_type, mime) || g_ascii_strcasecmp (content_type, mime) == 0)
                return TRUE;
        }
    }

    return FALSE;
}

static void
on_filter_combo_changed (GtkComboBox *combo, gpointer user_data)
{
    DialogData *data = user_data;
    if (data->current_folder) {
        load_directory (data, data->current_folder);
    }
}

static void
directory_enumerated_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    DialogData *data = user_data;
    g_autoptr(GFileEnumerator) enumerator = NULL;
    g_autoptr(GError) error = NULL;
    
    enumerator = g_file_enumerate_children_finish (G_FILE (source_object), res, &error);
    if (error) {
        g_warning ("Failed to list directory contents: %s", error->message);
        return;
    }
    
    gint sort_id;
    GtkSortType sort_type;
    gboolean has_sort = gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (data->list_store), &sort_id, &sort_type);
    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (data->list_store), GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, 0);
    
    gtk_list_store_clear (data->list_store);
    
    GList *files = NULL;
    GFileInfo *info;
    while ((info = g_file_enumerator_next_file (enumerator, NULL, &error)) != NULL) {
        files = g_list_prepend (files, info);
    }
    
    files = g_list_reverse (files);
    
    GList *l;
    for (l = files; l != NULL; l = l->next) {
        GFileInfo *file_info = l->data;
        const gchar *name = g_file_info_get_display_name (file_info);
        gboolean is_dir = g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY;
        goffset size = g_file_info_get_size (file_info);
        
        if (data->action == GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER && !is_dir) {
            g_object_unref (file_info);
            continue;
        }

        if (!is_dir) {
            AliveOSFileFilter *active_filter = get_current_active_filter (data);
            const gchar *c_type = g_file_info_get_content_type (file_info);
            if (!file_filter_matches (active_filter, name, c_type)) {
                g_object_unref (file_info);
                continue;
            }
        }
        
        gboolean is_hidden = g_file_info_get_is_hidden (file_info) || g_file_info_get_is_backup (file_info);
        if (is_hidden && !data->show_hidden) {
            g_object_unref (file_info);
            continue;
        }
        
        const gchar *content_type = g_file_info_get_content_type (file_info);
        gchar *type_desc = NULL;
        if (is_dir) {
            type_desc = g_strdup (_("Folder"));
        } else if (content_type) {
            type_desc = g_content_type_get_description (content_type);
        } else {
            type_desc = g_strdup (_("Unknown"));
        }
        
        GDateTime *mod_time = g_file_info_get_modification_date_time (file_info);
        gint64 mod_timestamp = 0;
        gchar *mod_str = NULL;
        if (mod_time) {
            mod_timestamp = g_date_time_to_unix (mod_time);
            mod_str = g_date_time_format (mod_time, "%Y-%m-%d %H:%M:%S");
        } else {
            mod_str = g_strdup ("-");
        }
        
        GIcon *icon = g_file_info_get_icon (file_info);
        g_autoptr(GtkIconInfo) icon_info_small = gtk_icon_theme_lookup_by_gicon (gtk_icon_theme_get_default (), icon, data->small_icon_size, GTK_ICON_LOOKUP_FORCE_SIZE);
        g_autoptr(GtkIconInfo) icon_info_large = gtk_icon_theme_lookup_by_gicon (gtk_icon_theme_get_default (), icon, data->large_icon_size, GTK_ICON_LOOKUP_FORCE_SIZE);
        
        g_autoptr(GdkPixbuf) pixbuf_small = NULL;
        g_autoptr(GdkPixbuf) pixbuf_large = NULL;
        
        const gchar *thumbnail_path = g_file_info_get_attribute_byte_string (file_info, "thumbnail::path");
        if (thumbnail_path) {
            GdkPixbuf *base = get_thumb_source (data, thumbnail_path);
            if (base) {
                pixbuf_small = scale_pixbuf_fit (base, data->small_icon_size);
                pixbuf_large = scale_pixbuf_fit (base, data->large_icon_size);
            }
        }
        
        if (!pixbuf_small && icon_info_small) {
            pixbuf_small = gtk_icon_info_load_icon (icon_info_small, NULL);
        }
        if (!pixbuf_large && icon_info_large) {
            pixbuf_large = gtk_icon_info_load_icon (icon_info_large, NULL);
        }
        
        g_autoptr(GFile) child_file = g_file_get_child (G_FILE (source_object), g_file_info_get_name (file_info));
        gchar *uri = g_file_get_uri (child_file);
        
        gchar *size_str = is_dir ? g_strdup (_("Folder")) : g_format_size (size);
        
        GtkTreeIter iter;
        gtk_list_store_append (data->list_store, &iter);
        gtk_list_store_set (data->list_store, &iter,
                             COL_NAME, name,
                             COL_ICON, pixbuf_small,
                             COL_GRID_ICON, pixbuf_large,
                             COL_SIZE, size,
                             COL_SIZE_STR, size_str,
                             COL_TYPE, type_desc,
                             COL_MODIFIED_TIME, mod_timestamp,
                             COL_MODIFIED_STR, mod_str,
                             COL_IS_DIR, is_dir,
                             COL_URI, uri,
                             COL_GICON, icon,
                             COL_THUMBNAIL_PATH, thumbnail_path,
                             -1);
                             
        g_free (uri);
        g_free (size_str);
        g_free (type_desc);
        g_free (mod_str);
        g_object_unref (file_info);
    }
    
    g_list_free (files);
    
    if (has_sort) {
        gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (data->list_store), sort_id, sort_type);
    } else {
        gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (data->list_store), COL_NAME, GTK_SORT_ASCENDING);
    }
}

static void
load_directory (DialogData *data, GFile *folder)
{
    if (data->thumb_cache)
        g_hash_table_remove_all (data->thumb_cache);

    g_file_enumerate_children_async (folder,
                                     "standard::*,thumbnail::path,time::modified",
                                     G_FILE_QUERY_INFO_NONE,
                                     G_PRIORITY_DEFAULT,
                                     NULL,
                                     directory_enumerated_cb,
                                     data);
}

static void
on_menu_item_toggled (GtkCheckMenuItem *menu_item, gpointer user_data)
{
    GtkTreeViewColumn *column = GTK_TREE_VIEW_COLUMN (user_data);
    gboolean visible = gtk_check_menu_item_get_active (menu_item);
    gtk_tree_view_column_set_visible (column, visible);
}

static void
show_header_context_menu (GtkWidget *tree_view, GdkEventButton *event, DialogData *data)
{
    GtkWidget *menu = gtk_menu_new ();
    GList *columns = gtk_tree_view_get_columns (GTK_TREE_VIEW (tree_view));
    GList *l;
    
    for (l = columns; l != NULL; l = l->next) {
        GtkTreeViewColumn *col = GTK_TREE_VIEW_COLUMN (l->data);
        const gchar *title = gtk_tree_view_column_get_title (col);
        
        if (!title || !*title)
            continue;
            
        GtkWidget *item = gtk_check_menu_item_new_with_label (title);
        gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), gtk_tree_view_column_get_visible (col));
        
        if (g_strcmp0 (title, _("Name")) == 0) {
            gtk_widget_set_sensitive (item, FALSE);
        }
        
        g_signal_connect (item, "toggled", G_CALLBACK (on_menu_item_toggled), col);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
    }
    
    g_list_free (columns);
    gtk_widget_show_all (menu);
    
    gtk_menu_popup_at_pointer (GTK_MENU (menu), (GdkEvent *)event);
}

static void
on_show_hidden_toggled (GtkCheckMenuItem *menu_item, gpointer user_data)
{
    DialogData *data = user_data;
    data->show_hidden = gtk_check_menu_item_get_active (menu_item);
    load_directory (data, data->current_folder);
}

static void
on_sort_method_selected (GtkRadioMenuItem *menu_item, gpointer user_data)
{
    if (gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (menu_item))) {
        DialogData *data = g_object_get_data (G_OBJECT (menu_item), "dialog-data");
        gint sort_col_id = GPOINTER_TO_INT (user_data);
        
        GtkTreeSortable *sortable = GTK_TREE_SORTABLE (data->list_store);
        gint current_id;
        GtkSortType current_type;
        
        if (gtk_tree_sortable_get_sort_column_id (sortable, &current_id, &current_type)) {
            gtk_tree_sortable_set_sort_column_id (sortable, sort_col_id, current_type);
        } else {
            gtk_tree_sortable_set_sort_column_id (sortable, sort_col_id, GTK_SORT_ASCENDING);
        }
    }
}


static void
on_show_in_file_manager_clicked (GtkWidget *menu_item, gpointer user_data)
{
    DialogData *data = user_data;
    if (data->current_folder) {
        gchar *uri = g_file_get_uri (data->current_folder);
        g_app_info_launch_default_for_uri (uri, NULL, NULL);
        g_free (uri);
    }
}

static void
on_copy_path_clicked (GtkWidget *menu_item, gpointer user_data)
{
    DialogData *data = user_data;
    gchar *text = NULL;
    
    if (data->selected_uri) {
        GFile *file = g_file_new_for_uri (data->selected_uri);
        text = g_file_get_path (file);
        if (!text) {
            text = g_strdup (data->selected_uri);
        }
        g_object_unref (file);
    } else if (data->current_folder) {
        text = g_file_get_path (data->current_folder);
    }
    
    if (text) {
        GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text (clipboard, text, -1);
        g_free (text);
    }
}

static void
on_refresh_clicked (GtkWidget *menu_item, gpointer user_data)
{
    DialogData *data = user_data;
    if (data->current_folder) {
        load_directory (data, data->current_folder);
    }
}

static void
on_properties_clicked (GtkWidget *menu_item, gpointer user_data)
{
    DialogData *data = user_data;
    GFile *file = NULL;
    if (data->selected_uri) {
        file = g_file_new_for_uri (data->selected_uri);
    } else if (data->current_folder) {
        file = g_object_ref (data->current_folder);
    }
    
    if (file) {
        g_autoptr(GFileInfo) info = g_file_query_info (file, "standard::*,time::modified", G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (info) {
            const gchar *display_name = g_file_info_get_display_name (info);
            gchar *path = g_file_get_path (file);
            goffset size = g_file_info_get_size (info);
            const gchar *content_type = g_file_info_get_content_type (info);
            g_autofree gchar *type_desc = g_content_type_get_description (content_type);
            
            GDateTime *mod_time = g_file_info_get_modification_date_time (info);
            g_autofree gchar *mod_str = mod_time ? g_date_time_format (mod_time, "%Y-%m-%d %H:%M:%S") : g_strdup ("-");
            
            g_autofree gchar *size_str = g_format_size (size);
            
            gchar *message = g_strdup_printf (
                _("Name: %s\nPath: %s\nType: %s\nSize: %s\nModified: %s"),
                display_name ? display_name : "-",
                path ? path : "-",
                type_desc ? type_desc : "-",
                size_str ? size_str : "-",
                mod_str ? mod_str : "-"
            );
            
            GtkWidget *dialog = gtk_message_dialog_new (GTK_WINDOW (data->dialog),
                                                         GTK_DIALOG_DESTROY_WITH_PARENT,
                                                         GTK_MESSAGE_INFO,
                                                         GTK_BUTTONS_OK,
                                                         "%s", _("Properties"));
            gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), "%s", message);
            gtk_dialog_run (GTK_DIALOG (dialog));
            gtk_widget_destroy (dialog);
            
            g_free (path);
            g_free (message);
        }
        g_object_unref (file);
    }
}

static void
do_create_folder (DialogData *data)
{
    if (!data->current_folder)
        return;
        
    GtkWidget *dialog = gtk_dialog_new_with_buttons (_("Create Folder"),
                                                     GTK_WINDOW (data->dialog),
                                                     GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                     _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                     _("_Create"), GTK_RESPONSE_ACCEPT,
                                                     NULL);
                                                 
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
    
    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width (GTK_CONTAINER (box), 12);
    gtk_container_add (GTK_CONTAINER (content_area), box);
    
    GtkWidget *label = gtk_label_new (_("Enter name for new folder:"));
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
    
    GtkWidget *entry = gtk_entry_new ();
    gtk_entry_set_text (GTK_ENTRY (entry), _("New Folder"));
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    gtk_box_pack_start (GTK_BOX (box), entry, FALSE, FALSE, 0);
    
    gtk_widget_show_all (box);
    
    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
        const gchar *name = gtk_entry_get_text (GTK_ENTRY (entry));
        if (name && *name) {
            GFile *new_dir = g_file_get_child (data->current_folder, name);
            g_autoptr(GError) error = NULL;
            if (g_file_make_directory (new_dir, NULL, &error)) {
                load_directory (data, data->current_folder);
            } else {
                GtkWidget *err_dialog = gtk_message_dialog_new (GTK_WINDOW (data->dialog),
                                                                 GTK_DIALOG_DESTROY_WITH_PARENT,
                                                                 GTK_MESSAGE_ERROR,
                                                                 GTK_BUTTONS_OK,
                                                                 _("Failed to create folder: %s"),
                                                                 error->message);
                gtk_dialog_run (GTK_DIALOG (err_dialog));
                gtk_widget_destroy (err_dialog);
            }
            g_object_unref (new_dir);
        }
    }
    gtk_widget_destroy (dialog);
}

static void
on_create_folder_clicked (GtkWidget *widget, gpointer user_data)
{
    do_create_folder (user_data);
}

static void
on_location_toggle_button_toggled (GtkToggleButton *toggle_button, gpointer user_data)
{
    DialogData *data = user_data;
    gboolean active = gtk_toggle_button_get_active (toggle_button);
    
    if (active) {
        gtk_stack_set_visible_child_name (GTK_STACK (data->path_stack), "entry");
        if (data->current_folder) {
            gchar *path = g_file_get_path (data->current_folder);
            if (path) {
                gtk_entry_set_text (GTK_ENTRY (data->path_entry), path);
                g_free (path);
            } else {
                gchar *uri = g_file_get_uri (data->current_folder);
                gtk_entry_set_text (GTK_ENTRY (data->path_entry), uri);
                g_free (uri);
            }
        }
        gtk_widget_grab_focus (data->path_entry);
        gtk_editable_select_region (GTK_EDITABLE (data->path_entry), 0, -1);
    } else {
        gtk_stack_set_visible_child (GTK_STACK (data->path_stack), data->path_breadcrumb_box);
    }
}

static void
on_path_entry_activate (GtkEntry *entry, gpointer user_data)
{
    DialogData *data = user_data;
    const gchar *text = gtk_entry_get_text (entry);
    
    if (text && *text) {
        GFile *new_folder = g_file_new_for_commandline_arg (text);
        if (g_file_query_exists (new_folder, NULL)) {
            navigate_to (data, new_folder, TRUE);
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (gtk_builder_get_object (data->builder, "location_toggle_button")), FALSE);
        } else {
            gtk_widget_grab_focus (GTK_WIDGET (entry));
        }
        g_object_unref (new_folder);
    }
}

static gboolean
on_path_entry_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    DialogData *data = user_data;
    if (event->keyval == GDK_KEY_Escape) {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (gtk_builder_get_object (data->builder, "location_toggle_button")), FALSE);
        return TRUE;
    }
    return FALSE;
}

static gboolean
selection_has_special_uri (DialogData *data)
{
    GSList *l;
    for (l = data->selected_uris; l != NULL; l = l->next) {
        const gchar *uri = l->data;
        if (g_str_has_prefix (uri, "trash://") ||
            g_str_has_prefix (uri, "search://") ||
            g_str_has_prefix (uri, "recent://") ||
            g_str_has_prefix (uri, "favorite://") ||
            g_str_has_prefix (uri, "x-nemo-desktop://")) {
            return TRUE;
        }
    }
    return FALSE;
}

static void
do_rename_file (DialogData *data, const gchar *uri)
{
    if (!uri)
        return;

    GFile *file = g_file_new_for_uri (uri);
    g_autofree gchar *basename = g_file_get_basename (file);

    GtkWidget *dialog = gtk_dialog_new_with_buttons (_("Rename"),
                                                     GTK_WINDOW (data->dialog),
                                                     GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                     _("_Cancel"), GTK_RESPONSE_CANCEL,
                                                     _("_Rename"), GTK_RESPONSE_ACCEPT,
                                                     NULL);
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width (GTK_CONTAINER (box), 12);
    gtk_container_add (GTK_CONTAINER (content_area), box);

    GtkWidget *label = gtk_label_new (_("Enter new name:"));
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

    GtkWidget *entry = gtk_entry_new ();
    if (basename)
        gtk_entry_set_text (GTK_ENTRY (entry), basename);
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    gtk_box_pack_start (GTK_BOX (box), entry, FALSE, FALSE, 0);

    gtk_widget_show_all (box);

    gint response = gtk_dialog_run (GTK_DIALOG (dialog));
    if (response == GTK_RESPONSE_ACCEPT) {
        const gchar *new_name = gtk_entry_get_text (GTK_ENTRY (entry));
        if (new_name && *new_name) {
            g_autoptr(GError) error = NULL;
            GFile *new_file = g_file_set_display_name (file, new_name, NULL, &error);
            if (new_file) {
                g_object_unref (new_file);
                load_directory (data, data->current_folder);
            } else {
                GtkWidget *err_dialog = gtk_message_dialog_new (GTK_WINDOW (data->dialog),
                                                                 GTK_DIALOG_DESTROY_WITH_PARENT,
                                                                 GTK_MESSAGE_ERROR,
                                                                 GTK_BUTTONS_OK,
                                                                 _("Failed to rename file: %s"),
                                                                 error && error->message ? error->message : _("Unknown error"));
                gtk_dialog_run (GTK_DIALOG (err_dialog));
                gtk_widget_destroy (err_dialog);
            }
        }
    }

    gtk_widget_destroy (dialog);
    g_object_unref (file);
}

static void
on_open_clicked (GtkWidget *menu_item, gpointer user_data)
{
    DialogData *data = user_data;
    if (data->selected_uris && data->selected_uris->data) {
        const gchar *uri = data->selected_uris->data;
        g_autoptr(GError) error = NULL;
        if (!g_app_info_launch_default_for_uri (uri, NULL, &error)) {
            g_warning ("Failed to open %s: %s", uri, error ? error->message : "unknown error");
        }
    }
}

static void
on_rename_clicked (GtkWidget *menu_item, gpointer user_data)
{
    DialogData *data = user_data;
    if (data->selected_uris && data->selected_uris->data) {
        do_rename_file (data, data->selected_uris->data);
    }
}

static void
do_trash_selection (DialogData *data)
{
    if (!data->selected_uris)
        return;

    GSList *l;
    for (l = data->selected_uris; l != NULL; l = l->next) {
        const gchar *uri = l->data;
        if (g_str_has_prefix (uri, "trash://") ||
            g_str_has_prefix (uri, "search://") ||
            g_str_has_prefix (uri, "recent://") ||
            g_str_has_prefix (uri, "favorite://") ||
            g_str_has_prefix (uri, "x-nemo-desktop://")) {
            continue;
        }

        GFile *file = g_file_new_for_uri (uri);
        g_autoptr(GError) error = NULL;
        if (!g_file_trash (file, NULL, &error)) {
            g_warning ("Failed to move %s to trash: %s", uri, error ? error->message : "unknown error");
        }
        g_object_unref (file);
    }

    load_directory (data, data->current_folder);
}

static void
on_move_to_trash_clicked (GtkWidget *menu_item, gpointer user_data)
{
    do_trash_selection (user_data);
}

static void
do_delete_selection (DialogData *data)
{
    if (!data->selected_uris)
        return;

    guint count = g_slist_length (data->selected_uris);
    g_autofree gchar *message = NULL;

    if (count == 1) {
        const gchar *uri = data->selected_uris->data;
        GFile *file = g_file_new_for_uri (uri);
        g_autofree gchar *name = g_file_get_parse_name (file);
        message = g_strdup_printf (_("Are you sure you want to permanently delete \"%s\"?"), name ? name : "");
        g_object_unref (file);
    } else {
        message = g_strdup_printf (_("Are you sure you want to permanently delete %u items?"), count);
    }

    GtkWidget *dialog = gtk_message_dialog_new (GTK_WINDOW (data->dialog),
                                                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                GTK_MESSAGE_WARNING,
                                                GTK_BUTTONS_NONE,
                                                "%s", message);
    gtk_dialog_add_buttons (GTK_DIALOG (dialog),
                            _("_Cancel"), GTK_RESPONSE_CANCEL,
                            _("_Delete"), GTK_RESPONSE_ACCEPT,
                            NULL);
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_CANCEL);

    gint response = gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);

    if (response != GTK_RESPONSE_ACCEPT)
        return;

    GSList *l;
    for (l = data->selected_uris; l != NULL; l = l->next) {
        const gchar *uri = l->data;
        if (g_str_has_prefix (uri, "trash://") ||
            g_str_has_prefix (uri, "search://") ||
            g_str_has_prefix (uri, "recent://") ||
            g_str_has_prefix (uri, "favorite://") ||
            g_str_has_prefix (uri, "x-nemo-desktop://")) {
            continue;
        }

        GFile *file = g_file_new_for_uri (uri);
        g_autoptr(GError) error = NULL;
        if (!g_file_delete (file, NULL, &error)) {
            g_warning ("Failed to delete %s: %s", uri, error ? error->message : "unknown error");
        }
        g_object_unref (file);
    }

    load_directory (data, data->current_folder);
}

static void
on_delete_clicked (GtkWidget *menu_item, gpointer user_data)
{
    do_delete_selection (user_data);
}

static void
show_files_context_menu (GtkWidget *widget, GdkEventButton *event, DialogData *data)
{
    GtkWidget *menu = gtk_menu_new ();
    
    gboolean has_selection = (data->selected_uris != NULL) && !selection_has_special_uri (data);
    
    if (has_selection) {
        GtkWidget *open_item = gtk_menu_item_new_with_label (_("Open"));
        gtk_widget_set_sensitive (open_item, g_slist_length (data->selected_uris) == 1);
        g_signal_connect (open_item, "activate", G_CALLBACK (on_open_clicked), data);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), open_item);
        
        GtkWidget *rename_item = gtk_menu_item_new_with_label (_("Rename..."));
        gtk_widget_set_sensitive (rename_item, g_slist_length (data->selected_uris) == 1);
        g_signal_connect (rename_item, "activate", G_CALLBACK (on_rename_clicked), data);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), rename_item);
        
        GtkWidget *trash_item = gtk_menu_item_new_with_label (_("Move to Trash"));
        g_signal_connect (trash_item, "activate", G_CALLBACK (on_move_to_trash_clicked), data);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), trash_item);
        
        GtkWidget *delete_item = gtk_menu_item_new_with_label (_("Delete Permanently..."));
        g_signal_connect (delete_item, "activate", G_CALLBACK (on_delete_clicked), data);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), delete_item);
        
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
    }
    
    GtkWidget *create_item = gtk_menu_item_new_with_label (_("Create Folder..."));
    g_signal_connect (create_item, "activate", G_CALLBACK (on_create_folder_clicked), data);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), create_item);
    
    GtkWidget *fm_item = gtk_menu_item_new_with_label (_("Show in File Manager"));
    g_signal_connect (fm_item, "activate", G_CALLBACK (on_show_in_file_manager_clicked), data);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), fm_item);
    
    GtkWidget *copy_item = gtk_menu_item_new_with_label (_("Copy Location"));
    g_signal_connect (copy_item, "activate", G_CALLBACK (on_copy_path_clicked), data);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), copy_item);
    
    GtkWidget *refresh_item = gtk_menu_item_new_with_label (_("Refresh"));
    g_signal_connect (refresh_item, "activate", G_CALLBACK (on_refresh_clicked), data);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), refresh_item);
    
    GtkWidget *prop_item = gtk_menu_item_new_with_label (_("Properties"));
    g_signal_connect (prop_item, "activate", G_CALLBACK (on_properties_clicked), data);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), prop_item);
    
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
    
    GtkWidget *hidden_item = gtk_check_menu_item_new_with_label (_("Show Hidden Files"));
    gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (hidden_item), data->show_hidden);
    g_signal_connect (hidden_item, "toggled", G_CALLBACK (on_show_hidden_toggled), data);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), hidden_item);
    
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
    
    GtkWidget *sort_submenu_item = gtk_menu_item_new_with_label (_("Sort By"));
    GtkWidget *sort_submenu = gtk_menu_new ();
    
    GSList *group = NULL;
    GtkTreeSortable *sortable = GTK_TREE_SORTABLE (data->list_store);
    gint current_sort_id = COL_NAME;
    GtkSortType current_sort_type;
    gtk_tree_sortable_get_sort_column_id (sortable, &current_sort_id, &current_sort_type);
    
    struct {
        const gchar *label;
        gint col_id;
    } sort_options[] = {
        { N_("Name"), COL_NAME },
        { N_("Size"), COL_SIZE },
        { N_("Type"), COL_TYPE },
        { N_("Modified"), COL_MODIFIED_TIME }
    };
    
    for (gint i = 0; i < G_N_ELEMENTS (sort_options); i++) {
        GtkWidget *item = gtk_radio_menu_item_new_with_label (group, _(sort_options[i].label));
        group = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
        
        g_object_set_data (G_OBJECT (item), "dialog-data", data);
        gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (item), (current_sort_id == sort_options[i].col_id));
        g_signal_connect (item, "toggled", G_CALLBACK (on_sort_method_selected), GINT_TO_POINTER (sort_options[i].col_id));
        
        gtk_menu_shell_append (GTK_MENU_SHELL (sort_submenu), item);
    }
    
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (sort_submenu_item), sort_submenu);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), sort_submenu_item);
    
    gtk_widget_show_all (menu);
    gtk_menu_popup_at_pointer (GTK_MENU (menu), (GdkEvent *)event);
}

static gboolean
on_tree_view_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    DialogData *data = user_data;
    
    if (event->type == GDK_BUTTON_PRESS && event->button == GDK_BUTTON_SECONDARY) {
        GdkWindow *bin_window = gtk_tree_view_get_bin_window (GTK_TREE_VIEW (widget));
        if (event->window != bin_window) {
            show_header_context_menu (widget, event, data);
            return TRUE;
        } else {
            GtkTreePath *path = NULL;
            if (gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (widget),
                                               (gint) event->x, (gint) event->y,
                                               &path, NULL, NULL, NULL)) {
                GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
                if (!gtk_tree_selection_path_is_selected (selection, path)) {
                    gtk_tree_selection_unselect_all (selection);
                    gtk_tree_selection_select_path (selection, path);
                }
                gtk_tree_path_free (path);
            }
            show_files_context_menu (widget, event, data);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean
on_icon_view_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    DialogData *data = user_data;
    
    if (event->type == GDK_BUTTON_PRESS && event->button == GDK_BUTTON_SECONDARY) {
        GtkTreePath *path = gtk_icon_view_get_path_at_pos (GTK_ICON_VIEW (widget),
                                                           (gint) event->x, (gint) event->y);
        if (path) {
            if (!gtk_icon_view_path_is_selected (GTK_ICON_VIEW (widget), path)) {
                gtk_icon_view_unselect_all (GTK_ICON_VIEW (widget));
                gtk_icon_view_select_path (GTK_ICON_VIEW (widget), path);
            }
            gtk_tree_path_free (path);
        }
        show_files_context_menu (widget, event, data);
        return TRUE;
    }
    return FALSE;
}

static gboolean
on_dialog_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    DialogData *data = user_data;
    
    if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_h || event->keyval == GDK_KEY_H)) {
        data->show_hidden = !data->show_hidden;
        load_directory (data, data->current_folder);
        return TRUE;
    }
    
    if ((event->state & GDK_CONTROL_MASK) && (event->keyval == GDK_KEY_l || event->keyval == GDK_KEY_L)) {
        GtkToggleButton *toggle = GTK_TOGGLE_BUTTON (gtk_builder_get_object (data->builder, "location_toggle_button"));
        gtk_toggle_button_set_active (toggle, !gtk_toggle_button_get_active (toggle));
        return TRUE;
    }
    
    return FALSE;
}

static gboolean
focus_filename_entry_idle (gpointer user_data)
{
    GtkWidget *dialog = user_data;
    DialogData *data = g_object_get_data (G_OBJECT (dialog), "dialog-data");
    
    if (data && data->filename_entry) {
        gtk_window_set_focus (GTK_WINDOW (dialog), data->filename_entry);
    }
    
    return G_SOURCE_REMOVE;
}

static void
on_dialog_response (GtkDialog *dialog, gint response_id, gpointer user_data)
{
    DialogData *data = user_data;
    save_chooser_settings (data);
}

GtkWidget *
aliveos_file_chooser_dialog_new (const gchar *title,
                              GtkFileChooserAction action,
                              gboolean select_multiple,
                              const gchar *initial_folder_uri,
                              const gchar *suggested_name,
                              GList *filters)
{
    GtkBuilder *builder = gtk_builder_new ();
    gtk_builder_set_translation_domain (builder, GETTEXT_PACKAGE);
    
    GError *error = NULL;
    if (!gtk_builder_add_from_resource (builder, "/org/aliveos/portal/aliveos-file-chooser-dialog.glade", &error)) {
        g_warning ("Failed to load file chooser UI resource: %s", error->message);
        g_object_unref (builder);
        return NULL;
    }
    
    GtkWidget *dialog = GTK_WIDGET (gtk_builder_get_object (builder, "aliveos_file_chooser_dialog"));
    if (title && *title) {
        gtk_window_set_title (GTK_WINDOW (dialog), title);
    }
    
    DialogData *data = g_new0 (DialogData, 1);
    data->builder = builder;
    data->dialog = dialog;
    data->action = action;
    data->thumb_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
    data->select_multiple = select_multiple;
    data->show_hidden = FALSE;
    GSettingsSchemaSource *schema_source = g_settings_schema_source_get_default ();
    if (schema_source && g_settings_schema_source_lookup (schema_source, "org.aliveos.portal.filechooser", TRUE)) {
        data->settings = g_settings_new ("org.aliveos.portal.filechooser");
    } else if (schema_source && g_settings_schema_source_lookup (schema_source, "org.dory.window-state", TRUE)) {
        data->settings = g_settings_new ("org.dory.window-state");
    } else {
        data->settings = NULL;
    }
    
    GtkWidget *ok_btn = GTK_WIDGET (gtk_builder_get_object (builder, "ok_button"));
    if (ok_btn) {
        if (action == GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER) {
            gtk_button_set_label (GTK_BUTTON (ok_btn), _("_Select"));
        } else if (action == GTK_FILE_CHOOSER_ACTION_SAVE) {
            gtk_button_set_label (GTK_BUTTON (ok_btn), _("_Save"));
        }
    }
    
    g_object_set_data_full (G_OBJECT (dialog), "dialog-data", data, free_dialog_data);
    
    g_signal_connect (dialog, "key-press-event", G_CALLBACK (on_dialog_key_press), data);
    g_signal_connect (dialog, "response", G_CALLBACK (on_dialog_response), data);
    
    /* Bind UI elements */
    data->path_bar_label = GTK_WIDGET (gtk_builder_get_object (builder, "path_label"));
    data->path_stack = GTK_WIDGET (gtk_builder_get_object (builder, "path_stack"));
    data->path_entry = GTK_WIDGET (gtk_builder_get_object (builder, "path_entry"));
    
    /* Create breadcrumb path bar container */
    data->path_breadcrumb_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign (data->path_breadcrumb_box, GTK_ALIGN_START);
    gtk_widget_set_hexpand (data->path_breadcrumb_box, TRUE);
    gtk_widget_show (data->path_breadcrumb_box);
    
    /* Replace the path_label with breadcrumb box in the path_stack */
    GtkWidget *path_stack = data->path_stack;
    GtkWidget *label_child = GTK_WIDGET (gtk_builder_get_object (builder, "path_label"));
    if (label_child && GTK_IS_LABEL (label_child)) {
        gtk_widget_hide (label_child);
        g_object_ref (label_child);
        gtk_container_remove (GTK_CONTAINER (path_stack), label_child);
        gtk_container_add (GTK_CONTAINER (path_stack), data->path_breadcrumb_box);
        gtk_stack_set_visible_child (GTK_STACK (path_stack), data->path_breadcrumb_box);
    }
    
    data->filename_entry = GTK_WIDGET (gtk_builder_get_object (builder, "filename_entry"));
    data->filter_combo = GTK_WIDGET (gtk_builder_get_object (builder, "filter_combo"));
    
    data->preview_image = GTK_WIDGET (gtk_builder_get_object (builder, "preview_image"));
    data->preview_name_label = GTK_WIDGET (gtk_builder_get_object (builder, "preview_name_label"));
    data->preview_size_val = GTK_WIDGET (gtk_builder_get_object (builder, "preview_size_val"));
    data->preview_type_val = GTK_WIDGET (gtk_builder_get_object (builder, "preview_type_val"));
    data->preview_modified_val = GTK_WIDGET (gtk_builder_get_object (builder, "preview_modified_val"));
    data->preview_target_size = 192;
    
    GtkWidget *preview_box = GTK_WIDGET (gtk_builder_get_object (builder, "preview_box"));
    if (preview_box)
        g_signal_connect (preview_box, "size-allocate", G_CALLBACK (on_preview_box_size_allocate), data);
    
    /* Places Sidebar Setup */
    data->places_sidebar = gtk_places_sidebar_new ();
    gtk_places_sidebar_set_show_desktop (GTK_PLACES_SIDEBAR (data->places_sidebar), TRUE);
    gtk_places_sidebar_set_show_recent (GTK_PLACES_SIDEBAR (data->places_sidebar), TRUE);
    gtk_widget_show (data->places_sidebar);
    gtk_container_add (GTK_CONTAINER (gtk_builder_get_object (builder, "sidebar_scrolled")), data->places_sidebar);
    
    g_signal_connect (data->places_sidebar, "open-location", G_CALLBACK (on_places_sidebar_open_location), data);
    
    g_signal_connect (data->path_entry, "activate", G_CALLBACK (on_path_entry_activate), data);
    g_signal_connect (data->path_entry, "key-press-event", G_CALLBACK (on_path_entry_key_press), data);
    g_signal_connect (gtk_builder_get_object (builder, "location_toggle_button"), "toggled", G_CALLBACK (on_location_toggle_button_toggled), data);
    g_signal_connect (gtk_builder_get_object (builder, "create_folder_button"), "clicked", G_CALLBACK (on_create_folder_clicked), data);
    
    /* Set default icon sizes */
    data->small_icon_size = 24;
    data->large_icon_size = 96;
    
    /* List Store & Views Setup */
    data->list_store = gtk_list_store_new (COL_NUM_COLUMNS,
                                           G_TYPE_STRING,
                                           GDK_TYPE_PIXBUF,
                                           GDK_TYPE_PIXBUF,
                                           G_TYPE_INT64,
                                           G_TYPE_STRING,
                                           G_TYPE_STRING,
                                           G_TYPE_INT64,
                                           G_TYPE_STRING,
                                           G_TYPE_BOOLEAN,
                                           G_TYPE_STRING,
                                           G_TYPE_ICON,
                                           G_TYPE_STRING);
                                           
    GtkTreeSortable *sortable = GTK_TREE_SORTABLE (data->list_store);
    gtk_tree_sortable_set_sort_func (sortable, COL_NAME, compare_rows_folders_first, GINT_TO_POINTER (COL_NAME), NULL);
    gtk_tree_sortable_set_sort_func (sortable, COL_SIZE, compare_rows_folders_first, GINT_TO_POINTER (COL_SIZE), NULL);
    gtk_tree_sortable_set_sort_func (sortable, COL_TYPE, compare_rows_folders_first, GINT_TO_POINTER (COL_TYPE), NULL);
    gtk_tree_sortable_set_sort_func (sortable, COL_MODIFIED_TIME, compare_rows_folders_first, GINT_TO_POINTER (COL_MODIFIED_TIME), NULL);
    gtk_tree_sortable_set_sort_column_id (sortable, COL_NAME, GTK_SORT_ASCENDING);
    
    /* 1. Setup Tree View (List Mode) */
    data->tree_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (data->list_store));
    GtkCellRenderer *renderer_icon = gtk_cell_renderer_pixbuf_new ();
    GtkCellRenderer *renderer_text = gtk_cell_renderer_text_new ();
    
    GtkTreeViewColumn *col_name = gtk_tree_view_column_new ();
    gtk_tree_view_column_set_title (col_name, _("Name"));
    gtk_tree_view_column_pack_start (col_name, renderer_icon, FALSE);
    gtk_tree_view_column_pack_start (col_name, renderer_text, TRUE);
    gtk_tree_view_column_add_attribute (col_name, renderer_icon, "pixbuf", COL_ICON);
    gtk_tree_view_column_add_attribute (col_name, renderer_text, "text", COL_NAME);
    gtk_tree_view_column_set_sort_column_id (col_name, COL_NAME);
    gtk_tree_view_column_set_resizable (col_name, TRUE);
    gtk_tree_view_append_column (GTK_TREE_VIEW (data->tree_view), col_name);
    
    GtkTreeViewColumn *col_size = gtk_tree_view_column_new_with_attributes (_("Size"), renderer_text, "text", COL_SIZE_STR, NULL);
    gtk_tree_view_column_set_sort_column_id (col_size, COL_SIZE);
    gtk_tree_view_column_set_resizable (col_size, TRUE);
    gtk_tree_view_append_column (GTK_TREE_VIEW (data->tree_view), col_size);
    
    GtkTreeViewColumn *col_type = gtk_tree_view_column_new_with_attributes (_("Type"), renderer_text, "text", COL_TYPE, NULL);
    gtk_tree_view_column_set_sort_column_id (col_type, COL_TYPE);
    gtk_tree_view_column_set_resizable (col_type, TRUE);
    gtk_tree_view_append_column (GTK_TREE_VIEW (data->tree_view), col_type);
    
    GtkTreeViewColumn *col_modified = gtk_tree_view_column_new_with_attributes (_("Modified"), renderer_text, "text", COL_MODIFIED_STR, NULL);
    gtk_tree_view_column_set_sort_column_id (col_modified, COL_MODIFIED_TIME);
    gtk_tree_view_column_set_resizable (col_modified, TRUE);
    gtk_tree_view_append_column (GTK_TREE_VIEW (data->tree_view), col_modified);
    
    gtk_widget_show (data->tree_view);
    gtk_container_add (GTK_CONTAINER (gtk_builder_get_object (builder, "tree_scrolled")), data->tree_view);
    
    GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (data->tree_view));
    gtk_tree_selection_set_mode (selection, select_multiple ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
    g_signal_connect (selection, "changed", G_CALLBACK (on_tree_selection_changed), data);
    g_signal_connect (data->tree_view, "row-activated", G_CALLBACK (on_tree_view_row_activated), data);
    
    gtk_widget_add_events (data->tree_view, GDK_SCROLL_MASK);
    g_signal_connect (data->tree_view, "scroll-event", G_CALLBACK (on_view_scroll_event), data);
    
    gtk_widget_add_events (data->tree_view, GDK_BUTTON_PRESS_MASK);
    g_signal_connect (data->tree_view, "button-press-event", G_CALLBACK (on_tree_view_button_press), data);
    
    /* 2. Setup Icon View (Grid Mode) */
    data->icon_view = gtk_icon_view_new_with_model (GTK_TREE_MODEL (data->list_store));
    gtk_icon_view_set_text_column (GTK_ICON_VIEW (data->icon_view), COL_NAME);
    gtk_icon_view_set_pixbuf_column (GTK_ICON_VIEW (data->icon_view), COL_GRID_ICON);
    gtk_icon_view_set_item_width (GTK_ICON_VIEW (data->icon_view), 80);
    gtk_icon_view_set_selection_mode (GTK_ICON_VIEW (data->icon_view), select_multiple ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
    
    gtk_widget_show (data->icon_view);
    gtk_container_add (GTK_CONTAINER (gtk_builder_get_object (builder, "icon_scrolled")), data->icon_view);
    
    g_signal_connect (data->icon_view, "selection-changed", G_CALLBACK (on_icon_view_selection_changed), data);
    g_signal_connect (data->icon_view, "item-activated", G_CALLBACK (on_icon_view_item_activated), data);
    
    gtk_widget_add_events (data->icon_view, GDK_SCROLL_MASK);
    g_signal_connect (data->icon_view, "scroll-event", G_CALLBACK (on_view_scroll_event), data);
    
    gtk_widget_add_events (data->icon_view, GDK_BUTTON_PRESS_MASK);
    g_signal_connect (data->icon_view, "button-press-event", G_CALLBACK (on_icon_view_button_press), data);
    
    /* 3. Setup Compact View (Compact Mode) */
    data->compact_view = gtk_icon_view_new_with_model (GTK_TREE_MODEL (data->list_store));
    gtk_icon_view_set_text_column (GTK_ICON_VIEW (data->compact_view), COL_NAME);
    gtk_icon_view_set_pixbuf_column (GTK_ICON_VIEW (data->compact_view), COL_ICON);
    gtk_icon_view_set_item_orientation (GTK_ICON_VIEW (data->compact_view), GTK_ORIENTATION_HORIZONTAL);
    gtk_icon_view_set_item_width (GTK_ICON_VIEW (data->compact_view), 120);
    gtk_icon_view_set_selection_mode (GTK_ICON_VIEW (data->compact_view), select_multiple ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
    
    gtk_widget_show (data->compact_view);
    gtk_container_add (GTK_CONTAINER (gtk_builder_get_object (builder, "compact_scrolled")), data->compact_view);
    
    g_signal_connect (data->compact_view, "selection-changed", G_CALLBACK (on_icon_view_selection_changed), data);
    g_signal_connect (data->compact_view, "item-activated", G_CALLBACK (on_icon_view_item_activated), data);
    
    gtk_widget_add_events (data->compact_view, GDK_SCROLL_MASK);
    g_signal_connect (data->compact_view, "scroll-event", G_CALLBACK (on_view_scroll_event), data);
    
    gtk_widget_add_events (data->compact_view, GDK_BUTTON_PRESS_MASK);
    g_signal_connect (data->compact_view, "button-press-event", G_CALLBACK (on_icon_view_button_press), data);
    
    /* Connect header buttons */
    g_signal_connect (gtk_builder_get_object (builder, "back_button"), "clicked", G_CALLBACK (on_back_clicked), data);
    g_signal_connect (gtk_builder_get_object (builder, "forward_button"), "clicked", G_CALLBACK (on_forward_clicked), data);
    g_signal_connect (gtk_builder_get_object (builder, "up_button"), "clicked", G_CALLBACK (on_up_clicked), data);
    
    g_signal_connect (gtk_builder_get_object (builder, "grid_view_toggle"), "toggled", G_CALLBACK (on_view_toggle_changed), data);
    g_signal_connect (gtk_builder_get_object (builder, "compact_view_toggle"), "toggled", G_CALLBACK (on_view_toggle_changed), data);
    
    /* Initialize view states (Grid View active by default) */
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (gtk_builder_get_object (builder, "grid_view_toggle")), TRUE);
    gtk_stack_set_visible_child_name (GTK_STACK (gtk_builder_get_object (builder, "browser_stack")), "grid");
    
    /* Restore saved settings */
    restore_chooser_settings (data);
    
    /* Set initial directory */
    GFile *initial_dir = NULL;
    if (initial_folder_uri && *initial_folder_uri) {
        initial_dir = resolve_valid_directory (initial_folder_uri);
    }

    if (!initial_dir) {
        /* Try to restore last used folder */
        g_autofree gchar *last_folder = get_last_folder (data);
        if (last_folder && *last_folder) {
            initial_dir = resolve_valid_directory (last_folder);
        }
    }

    if (!initial_dir) {
        /* Fall back to home directory */
        initial_dir = g_file_new_for_path (g_get_home_dir ());
    }

    navigate_to (data, initial_dir, TRUE);
    g_object_unref (initial_dir);
    
    if (suggested_name && *suggested_name) {
        gtk_entry_set_text (GTK_ENTRY (data->filename_entry), suggested_name);
    }
    
    /* Setup filter combo box */
    data->filters = filters;
    if (data->filters) {
        for (GList *l = data->filters; l != NULL; l = l->next) {
            AliveOSFileFilter *f = l->data;
            gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (data->filter_combo), f->name ? f->name : _("All Files"));
        }
        gtk_combo_box_set_active (GTK_COMBO_BOX (data->filter_combo), 0);
    } else {
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (data->filter_combo), _("All Files"));
        gtk_combo_box_set_active (GTK_COMBO_BOX (data->filter_combo), 0);
    }
    g_signal_connect (data->filter_combo, "changed", G_CALLBACK (on_filter_combo_changed), data);
    
    /* For save mode: use idle callback to set focus after dialog is fully mapped */
    if (action == GTK_FILE_CHOOSER_ACTION_SAVE) {
        g_idle_add (focus_filename_entry_idle, dialog);
    }
    
    return dialog;
}

GSList *
aliveos_file_chooser_dialog_get_selected_uris (GtkDialog *dialog)
{
    DialogData *data = g_object_get_data (G_OBJECT (dialog), "dialog-data");
    if (!data)
        return NULL;
        
    g_message ("aliveos_file_chooser_dialog_get_selected_uris: action=%d, selected_uris_count=%d, current_folder=%p",
               data->action, g_slist_length (data->selected_uris), data->current_folder);
        
    GSList *list = NULL;
    GSList *l;
    for (l = data->selected_uris; l != NULL; l = l->next) {
        list = g_slist_append (list, g_strdup (l->data));
    }
    
    if (data->action == GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER && list == NULL) {
        if (data->current_folder) {
            gchar *uri = g_file_get_uri (data->current_folder);
            list = g_slist_append (list, uri);
        }
    }
    
    return list;
}

gchar *
aliveos_file_chooser_dialog_get_selected_uri (GtkDialog *dialog)
{
    DialogData *data = g_object_get_data (G_OBJECT (dialog), "dialog-data");
    if (!data)
        return NULL;

    if (data->action == GTK_FILE_CHOOSER_ACTION_SAVE) {
        const gchar *filename = gtk_entry_get_text (GTK_ENTRY (data->filename_entry));
        if (filename && *filename && data->current_folder) {
            g_autoptr(GError) error = NULL;
            g_autoptr(GFile) child = g_file_get_child_for_display_name (data->current_folder, filename, &error);
            if (child) {
                return g_file_get_uri (child);
            }
        }
    }

    if (data->selected_uri) {
        return g_strdup (data->selected_uri);
    }

    if (data->action == GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER && data->current_folder) {
        return g_file_get_uri (data->current_folder);
    }

    return NULL;
}
