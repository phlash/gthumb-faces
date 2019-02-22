/* -*- Mode: C; tab-width: 4; expand-tabs; indent-tabs-mode: t; c-basic-offset: 4 -*- */

/*
 *  GThumb
 *
 *  Copyright (C) 2010 Free Software Foundation, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 *  Faces extension - marks up loaded image files with face rectangles
 *  stored in the configured 'faces_in_photos' SQLite database.
 *
 *  Currently this intercepts the loaders for MIME types image/jpeg and
 *  image/png, as there are no suitable hooks called from gThumb during
 *  image loading or rendering - oops.
 */

#include <config.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <gthumb.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

// where we store our prefs (in dconf-editor)
#define GTHUMB_FACES_SCHEMA GTHUMB_SCHEMA ".faces"
#define PREF_FACES_DBPATH "dbpath"

// Database handle and default location
static sqlite3 *db = NULL;
static char *dbfile = "/home/shared/photos/faces.db";

// local debug messages
static void _dbg(const char *fmt, ...)
{
    static GMutex _m;
    time_t now = time(NULL);
    if (getenv("FACES_DEBUG")!=NULL) {
        struct tm ts;
        char bf[30];
        va_list va;
        localtime_r(&now, &ts);
        strftime(bf, sizeof(bf), "[%F %T] ", &ts);
        g_mutex_lock(&_m);
        fprintf(stderr, bf);
        va_start(va, fmt);
        vfprintf(stderr, fmt, va);
        g_mutex_unlock(&_m);
        va_end(va);
    }
}

// image loader interceptor - overlays face rectangles on GthImage..
static GthImageLoaderFunc prev_jpeg = NULL;
static GthImageLoaderFunc prev_png = NULL;
static GthImage * loader_intercept (
                GInputStream  *istream,
                GthFileData   *file_data,
                int            requested_size,
                int           *original_width_p,
                int           *original_height_p,
                gboolean      *loaded_original_p,
                gpointer       user_data,
                GCancellable  *cancellable,
                GError       **error,
                GthImageLoaderFunc prev)
{
    // Chain through to the original loader..
    GthImage *image = prev(istream, file_data, requested_size, original_width_p, original_height_p, loaded_original_p, user_data, cancellable, error);
    // Query DB, find faces in this image (if any)
    if (!db)
        return image;
    if (!file_data || !file_data->file) {
        fputs("faces: missing file data\n", stderr);
        return image;
    }
    char *path = g_file_get_path(file_data->file);
    if (!path) {
        fputs("faces: non-local image URI\n", stderr);
        return image;
    }

    sqlite3_stmt *stmt = NULL;
    _dbg("faces: querying for faces in file: %s\n", path);
    int rv = sqlite3_prepare_v2(db,
        "SELECT DISTINCT d.left, d.top, d.right, d.bottom, g.label, g.grp, d.inpic from file_paths f inner join face_data as d on d.hash = f.hash inner join face_groups as g on g.grp = d.grp where f.path = ?1",
        -1 , &stmt, NULL);
    if (SQLITE_OK != rv)
        fprintf(stderr, "faces: sqlite_prepare error: %d\n", rv);
    rv = sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    if (SQLITE_OK != rv)
        fprintf(stderr, "faces: sqlite_bind error: %d\n", rv);
    while ((rv = sqlite3_step(stmt)) != SQLITE_DONE)
    {
        int l, t, r, b, p;
        const char *n, *g;
        if (SQLITE_ROW != rv) {
            fprintf(stderr, "faces: sqlite_step error: %d\n", rv);
            break;
        }
        l = sqlite3_column_int(stmt, 0);
        t = sqlite3_column_int(stmt, 1);
        r = sqlite3_column_int(stmt, 2);
        b = sqlite3_column_int(stmt, 3);
        n = sqlite3_column_text(stmt, 4);
        g = sqlite3_column_text(stmt, 5);
        p = sqlite3_column_int(stmt, 6);
        // Tag faces with a named rectangle =)
        cairo_surface_t *cs = gth_image_get_cairo_surface(image);
        if (!cs) {
            fprintf(stderr, "faces: unable to get cairo surface: %s\n", path);
            continue;
        }
        int w, h;
        w = cairo_image_surface_get_width(cs);
        h = cairo_image_surface_get_height(cs);
        _dbg("\t%s(%s)@%d,%d,%d,%d original %dx%d, this %dx%d\n", n, g, l, t, r, b, *original_width_p, *original_height_p, w, h);
        cairo_t *cr = cairo_create(cs);
        if (cr) {
            double sw = ((double)w)/((double)(*original_width_p));
            double sh = ((double)h)/((double)(*original_height_p));
            cairo_scale(cr, sw, sh);
            if (p>0)
                cairo_set_source_rgb(cr, 0, 1.0, 0);
            else
                cairo_set_source_rgb(cr, 1.0, 0, 0);
            cairo_rectangle(cr, l, t, r-l, b-t);
            cairo_move_to(cr, l+5, b-5);
            cairo_set_font_size(cr, 20/sw);
            cairo_text_path(cr, n);
            cairo_text_path(cr, " (");
            cairo_text_path(cr, g);
            cairo_text_path(cr, ")");
            cairo_stroke(cr);
            cairo_destroy(cr);
        } else {
            fprintf(stderr, "faces: unable to create cairo context: %s\n", path);
        }
        cairo_surface_destroy(cs);
    }
    sqlite3_finalize(stmt);
    _dbg("faces: done\n");
    return image;
}
static GthImage * jpeg_intercept (
                GInputStream  *istream,
                GthFileData   *file_data,
                int            requested_size,
                int           *original_width_p,
                int           *original_height_p,
                gboolean      *loaded_original_p,
                gpointer       user_data,
                GCancellable  *cancellable,
                GError       **error)
{
    return loader_intercept(
                istream, file_data, requested_size, original_width_p, original_height_p, loaded_original_p, user_data, cancellable, error, prev_jpeg);
}
static GthImage * png_intercept (
                GInputStream  *istream,
                GthFileData   *file_data,
                int            requested_size,
                int           *original_width_p,
                int           *original_height_p,
                gboolean      *loaded_original_p,
                gpointer       user_data,
                GCancellable  *cancellable,
                GError       **error)
{
    return loader_intercept(
                istream, file_data, requested_size, original_width_p, original_height_p, loaded_original_p, user_data, cancellable, error, prev_png);
}


// ** Extend the file tree with face names **

// Define a GClass, derived from GthFileSource that can be included in the browser file tree (yes Gthumb is this ugly)
// mostly stolen from selections extension
typedef struct {
    GthFileSource __parent;
    // additional instance data for this class
    int id;
} FacesFileSource;

typedef struct {
    GthFileSourceClass __parent_class;
} FacesFileSourceClass;

static int is_face_uri(const char *uri) {
    int rv = 1;
    if (! g_str_has_prefix(uri, "face:///"))
        rv = -1;
    else if (! strcmp(uri, "face:///"))
        rv = 0;
    return rv;
}
static GList *faces_file_source_get_entry_points(GthFileSource *fs) {
    _dbg("faces: file_source(%d): get_entry_points\n", ((FacesFileSource*)fs)->id);
    GList     *list = NULL;
    GFile     *file;
    GFileInfo *info;
    file = g_file_new_for_uri("face:///");
    info = gth_file_source_get_file_info(fs, file, GFILE_BASIC_ATTRIBUTES);
    list = g_list_append(list, gth_file_data_new(file, info));
    g_object_unref(info);
    g_object_unref(file);
    return list;
}
static GFile *faces_file_source_to_gio_file(GthFileSource *fs, GFile *file) {
    _dbg("faces: file_source(%d): to_gio_file\n", ((FacesFileSource*)fs)->id);
    return g_file_dup(file);
}
static void faces_file_source_update_file_info(GthFileSource *fs, GFile *file, GFileInfo *info) {
    char *uri = g_file_get_uri(file);
    int n_face = is_face_uri(uri);
    _dbg("faces: file_source(%d): update_file_info (%s=%d)\n", ((FacesFileSource*)fs)->id, uri, n_face);
    g_file_info_set_file_type(info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_content_type(info, "gthumb/face");
    //g_file_info_set_sort_order(info, n_face);
    g_file_info_set_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
    g_file_info_set_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
    g_file_info_set_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
    g_file_info_set_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);
    // Do not display fold arrow on leaf items (magic attribute name...)
    if (n_face > 0) {
        g_file_info_set_attribute_boolean(info, "gthumb::no-child", TRUE);
    }
    // The displayed  & internal name - whoo!
    char *name;
    if (0 == n_face) {
        name = g_strdup("Faces");
    } else if (n_face > 0) {
        name = g_uri_unescape_string(uri+8, "");
    } else {
        name = g_strdup("Unknown");
    }
    g_file_info_set_display_name(info, name);
    g_free(name);
    if (n_face > 0) {
        name = g_uri_unescape_string(uri+8, "");
    } else {
        name = g_strdup("");
    }
    g_file_info_set_name(info, name);
    g_free(name);
    // The tree icon - double whoo!
    // We are using the generic tagging icon for now..
    GIcon *icon = g_themed_icon_new("tag-symbolic");
    g_file_info_set_symbolic_icon(info, icon);
    g_object_unref(icon);
    g_free(uri);
}
static GFileInfo *faces_file_source_get_file_info(GthFileSource *fs, GFile *file, const char *attrs) {
    char *uri = g_file_get_uri(file);
    _dbg("faces: file_source(%d): get_file_info (%s)\n", ((FacesFileSource*)fs)->id, uri);
    g_free(uri);
    GFileInfo *info = g_file_info_new();
    faces_file_source_update_file_info(fs, file, info);
    return info;
}
static GthFileData *faces_file_source_get_file_data(GthFileSource *fs, GFile *file, GFileInfo *info) {
    char *uri = g_file_get_uri(file);
    _dbg("faces: file_source(%d): get_file_data (%s)\n", ((FacesFileSource*)fs)->id, uri);
    g_free(uri);
    if (G_FILE_TYPE_DIRECTORY == g_file_info_get_file_type(info))
        faces_file_source_update_file_info(fs, file, info);
    GthFileData *data = gth_file_data_new(file, info);
    return data;
}
static void faces_file_source_write_metadata(GthFileSource *fs, GthFileData *fd, const char *attrs, ReadyCallback ready, gpointer user) {
    _dbg("faces: file_source(%d): write_metadata\n", ((FacesFileSource*)fs)->id);
    object_ready_with_error(fs, ready, user, NULL);
}
static void faces_file_source_read_metadata(GthFileSource *fs, GthFileData *fd, const char *attrs, ReadyCallback ready, gpointer user) {
    char *uri = g_file_get_uri(fd->file);
    _dbg("faces: file_source(%d): read_metadata (%s)\n", ((FacesFileSource*)fs)->id, uri);
    g_free(uri);
    faces_file_source_update_file_info(fs, fd->file, fd->info);
    object_ready_with_error(fs, ready, user, NULL);
}
static void faces_file_source_rename(GthFileSource *fs, GFile *file, const char *name, ReadyCallback ready, gpointer user) {
    _dbg("faces: file_source(%d): rename\n", ((FacesFileSource*)fs)->id);
    object_ready_with_error(fs, ready, user, NULL);
}
typedef struct {
    FacesFileSource *ffs;
    GFile *parent;
    char *attrs;
    ForEachChildCallback fec;
    ReadyCallback ready;
    gpointer user;
} FacesIterateState;
static void faces_file_source_iterate_faces(gpointer user) {
    FacesIterateState *state = (FacesIterateState *)user;
    char *uri = g_file_get_uri(state->parent);
    _dbg("faces: file_source(%d): iterate_faces (%s): enter\n", state->ffs->id, uri);
    // Iterate database and extract labels..
    if (db) {
        sqlite3_stmt *stmt;
        int rv = sqlite3_prepare_v2(db, "SELECT label FROM face_labels", -1, &stmt, NULL);
        if (SQLITE_OK != rv) {
            fprintf(stderr, "sqlite3 failed to select labels: %d\n", rv);
            goto done;
        }
        while ((rv = sqlite3_step(stmt)) != SQLITE_DONE) {
            if (SQLITE_ROW != rv) {
                fprintf(stderr, "sqlite3 failed to read label row: %s\n", rv);
                goto done;
            }
            const char *label = sqlite3_column_text(stmt, 0);
            char *face = g_strdup_printf("face:///%s", label);
            GFile *file = g_file_new_for_uri(face);
            // Get escaped URI from file object
            g_free(face);
            face = g_file_get_uri(file);
            GFileInfo *info = g_file_info_new();
            faces_file_source_update_file_info((GthFileSource*)state->ffs, file, info);
            _dbg("faces: file_source(%d): fec callback for: %s\n", state->ffs->id, face);
            state->fec(file, info, state->user);
            g_object_unref(info);
            g_free(face);
            g_object_unref(file);
        }
        sqlite3_finalize(stmt);
    }
done:
    object_ready_with_error(state->ffs, state->ready, state->user, NULL);
    _dbg("faces: file_source(%d): iterate_faces (%s): exit\n", state->ffs->id, uri);
    g_free(uri);
    if (state->attrs)
        g_free(state->attrs);
    g_free(state);
}
static void faces_file_source_iterate_face(gpointer user) {
    FacesIterateState *state = (FacesIterateState *)user;
    char *uri = g_file_get_uri(state->parent);
    _dbg("faces: file_source(%d): iterate_face (%s): enter\n", state->ffs->id, uri);
    if (is_face_uri(uri) <= 0) {
        fprintf(stderr, "faces: iterate_face: not a face uri: %s\n", uri);
        goto done;
    }
    char *face = g_uri_unescape_string(uri+8, "");
    if (NULL == face) {
        fprintf(stderr, "faces: iterate_face: failed to unescape: %s\n", uri);
        goto done;
    }
    if (db) {
        sqlite3_stmt *stmt;
        int rv = sqlite3_prepare_v2(db,
                        "SELECT DISTINCT(p.path) " \
                        "FROM face_groups as g " \
                        "INNER JOIN face_data as d ON g.grp = d.grp " \
                        "INNER JOIN file_paths as p ON p.hash = d.hash " \
                        "WHERE g.label = ?1", -1, &stmt, NULL);
        if (rv != SQLITE_OK) {
            fprintf(stderr, "faces: iterate_face: prepare failed: %d\n", rv);
            goto done;
        }
        rv = sqlite3_bind_text(stmt, 1, face, -1, SQLITE_STATIC);
        if (SQLITE_OK != rv) {
            fprintf(stderr, "faces: sqlite_bind error: %d\n", rv);
        }
        while ((rv = sqlite3_step(stmt)) != SQLITE_DONE) {
            if (rv != SQLITE_ROW) {
                fprintf(stderr, "faces: iterate_face: failed to read face data: %d\n", rv);
                goto done;
            }
            const char *path = sqlite3_column_text(stmt, 0);
            char *furi = g_strdup_printf("file://%s", path);
            GFile *file = g_file_new_for_uri(furi);
            g_free(furi);
            furi = g_file_get_uri(file);
            GError *err = NULL;
            GFileInfo *info = g_file_query_info(file, state->attrs, 0, NULL, &err);
            if (NULL == info) {
                fprintf(stderr, "faces: warning: unable to read file info: %s\n", furi);
            } else {
                _dbg("faces: file_source(%d): fec callback for: %s\n", state->ffs->id, furi);
                state->fec(file, info, state->user);
                g_object_unref(info);
            }
            g_free(furi);
            g_object_unref(file);
        }
        sqlite3_finalize(stmt);
    }
    g_free(face);
done:
    object_ready_with_error(state->ffs, state->ready, state->user, NULL);
    _dbg("faces: file_source(%d): iterate_face (%s): exit\n", state->ffs->id, uri);
    g_free(uri);
    if (state->attrs)
        g_free(state->attrs);
    g_free(state);
}
static void faces_file_source_for_each_child(GthFileSource *fs, GFile *parent, gboolean rec, const char *attrs, StartDirCallback sdc, ForEachChildCallback fec, ReadyCallback ready, gpointer user) {
    FacesFileSource *ffs = (FacesFileSource*)fs;
    char *uri = g_file_get_uri(parent);
    int n_face = is_face_uri(uri);
    _dbg("faces: file_source(%d): for_each_child (%s=%d) rec=%d\n", ffs->id, uri, n_face, rec);
    GError *err = NULL;
    if (NULL != sdc) {
        GFileInfo *info = faces_file_source_get_file_info(fs, parent, "");
        _dbg("faces: file_source(%d): sdc callback for: %s\n", ffs->id, uri);
        DirOp op = sdc(parent, info, &err, user);
        g_object_unref(info);
        switch (op) {
        case DIR_OP_CONTINUE:
            break;
        case DIR_OP_SKIP:
            err = NULL;
            goto done;
        case DIR_OP_STOP:
            goto done;
        }
    }
    FacesIterateState *state = g_new0(FacesIterateState, 1);
    state->ffs = ffs;
    state->parent = parent;
    state->attrs = g_strdup(attrs);
    state->fec = fec;
    state->ready = ready;
    state->user = user;
    if (n_face > 0) {
        // Face selected, go get files
        call_when_idle(faces_file_source_iterate_face, state);
    } else {
        // Root selected, list faces
        call_when_idle(faces_file_source_iterate_faces, state);
    }
    g_free(uri);
    return;
done:
    g_free(uri);
    object_ready_with_error(fs, ready, user, err);
}
static void faces_file_source_copy(GthFileSource *fs, GthFileData *dest, GList *list, gboolean move, int destpos, ProgressCallback prg, DialogCallback dlg, ReadyCallback ready, gpointer user) {
    _dbg("faces: file_source_copy\n");
    object_ready_with_error(fs, ready, user, NULL);
}
static gboolean faces_file_source_can_cut(GthFileSource *fs, GFile *file) {
    _dbg("faces: file_source_can_cut\n");
    return FALSE;
}
static gboolean faces_file_source_is_reorderable(GthFileSource *fs) {
    _dbg("faces: file_source_is_reorderable\n");
    return FALSE;
}
static void faces_file_source_reorder(GthFileSource *fs, GthFileData *dest, GList *vis, GList *move, int destpos, ReadyCallback ready, gpointer user) {
    _dbg("faces: file_source_reorder\n");
    object_ready_with_error(fs, ready, user, NULL);
}
static void faces_file_source_remove(GthFileSource *fs, GthFileData *loc, GList *list, gboolean perm, GtkWindow *parent) {
    _dbg("faces: file_source_remove\n");
}
static gboolean faces_file_source_shows_extra_widget(GthFileSource *fs) {
    return FALSE;
}
static void faces_file_source_finalize(GObject *obj) {
    FacesFileSource *self = (FacesFileSource*)obj;
    _dbg("faces: file_source(%d): finalized\n", self->id);
}

static void faces_file_source_class_init(FacesFileSourceClass *class, void *data) {
    _dbg("faces: file_source_class_init\n");
    // Override any parent class methods we need to
    GthFileSourceClass *fsc = (GthFileSourceClass *)class;
    ((GObjectClass*)fsc)->finalize = faces_file_source_finalize;
    fsc->get_entry_points = faces_file_source_get_entry_points;
    fsc->to_gio_file = faces_file_source_to_gio_file;
    fsc->get_file_info = faces_file_source_get_file_info;
    fsc->get_file_data = faces_file_source_get_file_data;
    fsc->write_metadata = faces_file_source_write_metadata;
    fsc->read_metadata = faces_file_source_read_metadata;
    fsc->rename = faces_file_source_rename;
    fsc->for_each_child = faces_file_source_for_each_child;
    fsc->copy = faces_file_source_copy;
    fsc->can_cut = faces_file_source_can_cut;
    fsc->is_reorderable = faces_file_source_is_reorderable;
    fsc->reorder = faces_file_source_reorder;
    fsc->remove = faces_file_source_remove;
    fsc->shows_extra_widget = faces_file_source_shows_extra_widget;
}

static void faces_file_source_init(FacesFileSource *self, void *data) {
    static int _id = 0;
    self->id = ++_id;
    _dbg("faces: file_source(%d): init\n", self->id);
    gth_file_source_add_scheme(GTH_FILE_SOURCE(self), "face");
}

static GType faces_file_source_get_type(void) {
    static GType type = 0;
    if (0 == type) {
        type = g_type_register_static_simple(
            gth_file_source_get_type(),
            "FacesFileSource",
            sizeof(FacesFileSourceClass),
            (GClassInitFunc)faces_file_source_class_init,
            sizeof(FacesFileSource),
            (GInstanceInitFunc)faces_file_source_init,
            0);
    }
    return type;
}

G_MODULE_EXPORT void
gthumb_extension_activate (void) {
    // Intercept image loaders
    char *mime_jpeg = "image/jpeg";
    char *mime_png = "image/png";
    prev_jpeg = gth_main_get_image_loader_func(mime_jpeg, GTH_IMAGE_FORMAT_CAIRO_SURFACE);
    if (prev_jpeg)
        gth_main_register_image_loader_func(jpeg_intercept, GTH_IMAGE_FORMAT_CAIRO_SURFACE, mime_jpeg, NULL);
    else
        fputs("faces: unable to intercept image/jpeg loader\n", stderr);
    prev_png = gth_main_get_image_loader_func(mime_png, GTH_IMAGE_FORMAT_CAIRO_SURFACE);
    if (prev_png)
        gth_main_register_image_loader_func(png_intercept, GTH_IMAGE_FORMAT_CAIRO_SURFACE, mime_png, NULL);
    else
        fputs("faces: unable to intercept image/png loader\n", stderr);
    // Read our database path and open it
    GSettings *settings = g_settings_new(GTHUMB_FACES_SCHEMA);
    char *dbpath = g_settings_get_string(settings, PREF_FACES_DBPATH);
    g_object_unref(settings);
    _dbg("faces: org.gnome.gthumb.faces.dbpath=%s\n", dbpath);
    if (!dbpath || !dbpath[0])
        dbpath = dbfile;
    if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "faces: unable to open database: %s\n", dbpath);
        db = NULL;
    }
    // Add new branch to browser tree
    gth_main_register_file_source(faces_file_source_get_type());
}


G_MODULE_EXPORT void
gthumb_extension_deactivate (void) {
    if (db)
        sqlite3_close(db);
}


G_MODULE_EXPORT gboolean
gthumb_extension_is_configurable (void) {
    return TRUE;
}


G_MODULE_EXPORT void
gthumb_extension_configure (GtkWindow *parent) {
    GtkWidget *dialog = gtk_message_dialog_new(parent, 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Hello config!");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}
