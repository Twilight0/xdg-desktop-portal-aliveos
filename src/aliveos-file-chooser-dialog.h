/*
 * aliveos-file-chooser-dialog: Custom layout file picker dialog header
 */

#ifndef __ALIVEOS_FILE_CHOOSER_DIALOG_H__
#define __ALIVEOS_FILE_CHOOSER_DIALOG_H__

#include <gtk/gtk.h>

typedef struct {
    gchar *name;
    GList *patterns;
    GList *mime_types;
} AliveOSFileFilter;

AliveOSFileFilter * aliveos_file_filter_new (const gchar *name);
void                aliveos_file_filter_add_pattern (AliveOSFileFilter *filter, const gchar *pattern);
void                aliveos_file_filter_add_mime_type (AliveOSFileFilter *filter, const gchar *mime_type);
void                aliveos_file_filter_free (AliveOSFileFilter *filter);

GtkWidget * aliveos_file_chooser_dialog_new (const gchar *title,
                                             GtkFileChooserAction action,
                                             gboolean select_multiple,
                                             const gchar *initial_folder_uri,
                                             const gchar *suggested_name,
                                             GList *filters);

GSList * aliveos_file_chooser_dialog_get_selected_uris (GtkDialog *dialog);
gchar *  aliveos_file_chooser_dialog_get_selected_uri  (GtkDialog *dialog);

#endif /* __ALIVEOS_FILE_CHOOSER_DIALOG_H__ */
