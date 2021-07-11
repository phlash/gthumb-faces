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
 *  This provides a new Tree object (file source) that lists known faces
 *  and intercepts image rendering to draw markers on the displayed image.
 *
 *  Previously this intercepted the loaders for MIME types image/jpeg and
 *  image/png, as I did not understand rendering hooks in gThumb.
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

// face query function, used by both load intercept and render overlay methods
static void find_faces(char *path, void (*fcb)(int,int,int,int,const char*,const char*,int,gpointer), gpointer user) {
    sqlite3_stmt *stmt = NULL;
    _dbg("faces: find_faces: %s\n", path);
    int rv = sqlite3_prepare_v2(db,
        "SELECT DISTINCT d.left, d.top, d.right, d.bottom, g.label, g.grp, d.inpic from file_paths f inner join face_data as d on d.hash = f.hash inner join face_groups as g on g.grp = d.grp where f.path = ?1",
        -1 , &stmt, NULL);
    if (SQLITE_OK != rv)
        fprintf(stderr, "faces: sqlite_prepare error: %d\n", rv);
    rv = sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    if (SQLITE_OK != rv)
        fprintf(stderr, "faces: sqlite_bind error: %d\n", rv);
    while ((rv = sqlite3_step(stmt)) != SQLITE_DONE) {
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
        fcb(l,t,r,b,n,g,p,user);
    }
    sqlite3_finalize(stmt);
    _dbg("faces: find_faces: done\n");
}

// image loader interceptor - overlays face rectangles on GthImage..
static GthImageLoaderFunc prev_jpeg = NULL;
static GthImageLoaderFunc prev_png = NULL;
static void draw_to_context(cairo_t *cr, int l, int t, int r, int b, const char *n, const char *g, int p) {
    cairo_save(cr);
    if (p>0)
        cairo_set_source_rgb(cr, 0, 1.0, 0);
    else
        cairo_set_source_rgb(cr, 1.0, 0, 0);
    cairo_set_line_width(cr, 2.0);
    cairo_rectangle(cr, l, t, r-l, b-t);
    cairo_move_to(cr, l, b+15);
    cairo_set_font_size(cr, 12);
    cairo_set_line_width(cr, 1.0);
    cairo_text_path(cr, n);
    cairo_text_path(cr, " (");
    cairo_text_path(cr, g);
    cairo_text_path(cr, ")");
    cairo_stroke(cr);
    cairo_restore(cr);
    _dbg("\tfaces: draw: %s(%s)@%d,%d,%d,%d\n", n, g, l, t, r, b);
}
typedef struct {
    GthImage *image;
    int w, h;
} InterceptData;
static void draw_to_image(int l, int t, int r, int b, const char *n, const char *g, int p, gpointer user) {
    // Tag faces with a named rectangle =)
    InterceptData *data = (InterceptData *)user;
    cairo_surface_t *cs = gth_image_get_cairo_surface(data->image);
    if (!cs) {
        fprintf(stderr, "faces: unable to get cairo surface\n");
        return;
    }
    int w, h;
    w = cairo_image_surface_get_width(cs);
    h = cairo_image_surface_get_height(cs);
    cairo_t *cr = cairo_create(cs);
    if (cr) {
        double sw = ((double)w)/((double)data->w);
        double sh = ((double)h)/((double)data->h);
        // we calculate as follows:
        //     rect (l,r) = face(l,r) * sw
        //     rect (t,b) = face(t,b) * sh
        l = (int)(((double)l)*sw);
        r = (int)(((double)r)*sw);
        t = (int)(((double)t)*sh);
        b = (int)(((double)b)*sh);
        draw_to_context(cr, l, t, r, b, n, g, p);
        cairo_destroy(cr);
    } else {
        fprintf(stderr, "faces: unable to create cairo context\n");
    }
    cairo_surface_destroy(cs);
}
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
    InterceptData data = { image, *original_width_p, *original_height_p };
    find_faces(path, draw_to_image, &data);
    g_free(path);
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
static void faces_file_source_update_file_info(GthFileSource *fs, GFile *file, GFileInfo *info, const char *count) {
    char *uri = g_file_get_uri(file);
    int n_face = is_face_uri(uri);
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
        char *tmp = g_uri_unescape_string(uri+8, "");
        name = g_strdup_printf("%s (%s)", tmp, count);
        g_free(tmp);
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
    _dbg("faces: file_source(%d): update_file_info (%s=%d) name=%s display=%s\n",
        ((FacesFileSource*)fs)->id, uri, n_face,
        g_file_info_get_name(info),
        g_file_info_get_display_name(info));
    g_free(uri);
}
static GFileInfo *faces_file_source_get_file_info(GthFileSource *fs, GFile *file, const char *attrs) {
    char *uri = g_file_get_uri(file);
    _dbg("faces: file_source(%d): get_file_info (%s)\n", ((FacesFileSource*)fs)->id, uri);
    g_free(uri);
    GFileInfo *info = g_file_info_new();
    faces_file_source_update_file_info(fs, file, info, "0");
    return info;
}
static GthFileData *faces_file_source_get_file_data(GthFileSource *fs, GFile *file, GFileInfo *info) {
    char *uri = g_file_get_uri(file);
    _dbg("faces: file_source(%d): get_file_data (%s)\n", ((FacesFileSource*)fs)->id, uri);
    g_free(uri);
    if (G_FILE_TYPE_DIRECTORY == g_file_info_get_file_type(info))
        faces_file_source_update_file_info(fs, file, info, "0");
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
    faces_file_source_update_file_info(fs, fd->file, fd->info, "0");
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
        // We count the number of faces associated to each label (approx number of files)
        int rv = sqlite3_prepare_v2(db,
            "SELECT g.label, count(d.grp) " \
            "FROM face_groups g inner join face_data d on d.grp = g.grp " \
            "group by g.label",
            -1, &stmt, NULL);
        if (SQLITE_OK != rv) {
            fprintf(stderr, "sqlite3 failed to select labels: %d\n", rv);
            goto done;
        }
        while ((rv = sqlite3_step(stmt)) != SQLITE_DONE) {
            if (SQLITE_ROW != rv) {
                fprintf(stderr, "sqlite3 failed to read label row: %s\n", rv);
                goto done;
            }
            char *label = g_uri_escape_string(sqlite3_column_text(stmt, 0), "", FALSE);
            char *face = g_strdup_printf("face:///%s", label);
            g_free(label);
            GFile *file = g_file_new_for_uri(face);
            GFileInfo *info = g_file_info_new();
            faces_file_source_update_file_info((GthFileSource*)state->ffs, file, info, sqlite3_column_text(stmt, 1));
            _dbg("faces: file_source(%d): fec callback for: %s\n", state->ffs->id, face);
            state->fec(file, info, state->user);
            g_object_unref(info);
            g_object_unref(file);
            g_free(face);
        }
        sqlite3_finalize(stmt);
        // special hack.. iterate _unknown_ faces by group id, in descending order of quantity
        if (getenv("FACES_ITERATE_UNKNOWN") != NULL) {
            stmt = NULL;
            rv = sqlite3_prepare_v2(db,
                "select g.grp, count(g.grp) " \
                "from face_data as d inner join face_groups as g on d.grp=g.grp " \
                "where g.label='_unknown_' group by g.grp order by count desc", -1, &stmt, NULL);
            if (SQLITE_OK != rv) {
                fprintf(stderr, "sqlite3 failed to select unknown counts: %d\n", rv);
                goto done;
            }
            while ((rv = sqlite3_step(stmt)) != SQLITE_DONE) {
                if (SQLITE_ROW != rv) {
                    fprintf(stderr, "sqlite3 failed to read unknown row: %d\n", rv);
                    goto done;
                }
                const char *grp = sqlite3_column_text(stmt, 0);
                const char *cnt = sqlite3_column_text(stmt, 1);
                char *face = g_strdup_printf("face:///_unk_:%s (%s)", grp, cnt);
                GFile *file = g_file_new_for_uri(face);
                GFileInfo *info = g_file_info_new();
                faces_file_source_update_file_info((GthFileSource*)state->ffs, file, info, cnt);
                _dbg("faces: file_source(%d): fec callback for: %s\n", state->ffs->id, face);
                state->fec(file, info, state->user);
                g_object_unref(info);
                g_object_unref(file);
                g_free(face);
            }
        }
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
        char *qry_lab =
                        "SELECT DISTINCT(p.path) " \
                        "FROM face_groups as g " \
                        "INNER JOIN face_data as d ON g.grp = d.grp " \
                        "INNER JOIN file_paths as p ON p.hash = d.hash " \
                        "WHERE g.label = ?1";
        char *qry_grp =
                        "SELECT DISTINCT(p.path) " \
                        "FROM face_data as d " \
                        "INNER JOIN file_paths as p ON p.hash = d.hash " \
                        "WHERE d.grp = ?1";
        int grp;
        char *qry = qry_lab;
        if (sscanf(face, "_unk_:%d ", &grp) > 0) {
            // unknown face label detected, use group query
            qry = qry_grp;
            _dbg("faces: file_source(%d): iterate face (%s): detected group: %d\n", state->ffs->id, uri, grp);
        }
        sqlite3_stmt *stmt;
        int rv = sqlite3_prepare_v2(db, qry, -1, &stmt, NULL);
        if (rv != SQLITE_OK) {
            fprintf(stderr, "faces: iterate_face: prepare failed: %d\n", rv);
            goto done;
        }
        if (qry == qry_lab)
            rv = sqlite3_bind_text(stmt, 1, face, -1, SQLITE_STATIC);
        else
            rv = sqlite3_bind_int(stmt, 1, grp);
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


// ** Hook into GthImageViewer rendering chain to add markers to images

// Here we declare a function in /another extension/, as it's the ONLY way to
// get hold of the GthImageViewer instance in use (apparantly - sigh).
// NB: We would include the extension's own header.. but it's not in the gThumb
// plug-in dev kit (as you would expect, it's a private plug in), so we hack
// it here. Wouldn't it be nice if this sort of coupling could be avoided by some
// sort of introspection type system, or hook registry.. oh wait :-/
extern GtkWidget * gth_image_viewer_page_get_image_viewer (GthViewerPage *self);

// Shared cache of facial data for a specific image
typedef struct _FaceInfo {
    struct _FaceInfo *next;
    int l, t, r, b, p;
    gchar *n, *g;
} FaceInfo;
typedef struct {
    gchar *path;
    FaceInfo *faces;
} FaceCache;
// callback from find_faces
static void cache_face(int l, int t, int r, int b, const char *n, const char *g, int p, gpointer user) {
    FaceCache *cache = (FaceCache *)user;
    FaceInfo *fi = malloc(sizeof(FaceInfo));
    fi->next = cache->faces;
    fi->l = l;
    fi->t = t;
    fi->r = r;
    fi->b = b;
    fi->p = p;
    fi->n = g_strdup(n);
    fi->g = g_strdup(g);
    cache->faces = fi;
    _dbg("faces: cache_face: %s\n", n);
}
// GLib signal handler, called when any viewer loads a file
// We use this as a conveniant moment to query for image metadata
static void faces_viewer_file_loaded(GthViewerPage *viewer, GthFileData *file, GFileInfo *info, gboolean success, gpointer user) {
    gchar *path = g_file_get_path(file->file);
    FaceCache *cache = (FaceCache *)user;
    _dbg("faces: viewer_file_loaded(%s): %s cache=%p\n", success ? "ok" : "fail", path, cache);
    if (success) {
        if (NULL != cache->path)
            g_free(cache->path);
        cache->path = g_strdup(path);
        if (NULL != cache->faces) {
            FaceInfo *fi, *n;
            for (fi = cache->faces; fi != NULL; fi = n) {
                if (fi->n != NULL)
                    g_free(fi->n);
                if (fi->g != NULL)
                    g_free(fi->g);
                n = fi->next;
                free(fi);
            }
            cache->faces = NULL;
        }
        find_faces(cache->path, cache_face, cache);
    }
    g_free(path);
}

// Scale and draw face metadata from the cache over the image
static gboolean _draw_faces = TRUE;
static void faces_paint_metadata(GthImageViewer *viewer, cairo_t *cr, gpointer user) {
    // We calculate co-ordinates in drawing space as follows:
    //   image (left,top) = transform(cr, (image_offset) - (scroll_offset))
    double il = (double)(viewer->image_area.x - viewer->visible_area.x);
    double it = (double)(viewer->image_area.y - viewer->visible_area.y);
    cairo_user_to_device(cr, &il, &it);
    // We create a fresh context, as the provided one is oddly transformed
    cairo_t *ourcr = cairo_create(cairo_get_target(cr));
    if (_draw_faces) {
        FaceCache *cache = (FaceCache *)user;
        double z = gth_image_viewer_get_zoom(viewer);
        FaceInfo *fi;
        for (fi = cache->faces; fi != NULL; fi = fi->next) {
            // rect (l,t,r,b) = (face (l,t,r,b) * z) + image (left,top)
            int l = (int)(((double)fi->l)*z + il);
            int t = (int)(((double)fi->t)*z + it);
            int r = (int)(((double)fi->r)*z + il);
            int b = (int)(((double)fi->b)*z + it);
            draw_to_context(ourcr, l, t, r, b, fi->n, fi->g, fi->p);
        }
    } else {
        // Mark corner to show faces are disabled
        cairo_save(ourcr);
        cairo_set_source_rgb(ourcr, 1.0, 0, 0);
        cairo_set_line_width(ourcr, 2.0);
        cairo_move_to(ourcr, il + 5, it + 15);
        cairo_set_font_size(ourcr, 12);
        cairo_set_line_width(ourcr, 1.0);
        cairo_text_path(ourcr, "(faces off)");
        cairo_stroke(ourcr);
        cairo_restore(ourcr);
    }
    cairo_destroy(ourcr);
}

static GtkWidget *_viewer = NULL;
static gpointer faces_keypress(GthBrowser *browser, GdkEventKey *ev) {
    gboolean rv = FALSE;
    if (GDK_KEY_F == ev->keyval) {
        _draw_faces = !_draw_faces;
        if (_viewer != NULL)
            gtk_widget_queue_draw(_viewer);
        rv = TRUE;
    }
    _dbg("faces_toggle_faces: state=%d, return=%d\n", _draw_faces, rv);
    return GINT_TO_POINTER(rv);
}

static void faces_viewer_activated(GthBrowser *browser) {
    GthViewerPage *page = GTH_VIEWER_PAGE(gth_browser_get_viewer_page(browser));
    GType vtype = G_OBJECT_TYPE(page);
    // Check we can use the method in the extension to get to the GthImageViewer..
    if (strcmp(g_type_name(vtype), "GthImageViewerPage") == 0) {
        // If so: then connect to the file loaded signal for this page and add a paint
        // handler to the GthImageViewer, both sharing a cache of face data.
        FaceCache *cache = calloc(1, sizeof(FaceCache));
        g_signal_connect(page, "file-loaded", G_CALLBACK(faces_viewer_file_loaded), cache);
        // Add our painting function to render face rectangles (if enabled)
        // keep a reference to the widget to invalidate when toggling enable/disable faces
        _viewer = gth_image_viewer_page_get_image_viewer(page);
        gth_image_viewer_add_painter(GTH_IMAGE_VIEWER(_viewer), faces_paint_metadata, cache);
        _dbg("faces: viewer_activated: hooked page type: %s cache=%p\n", g_type_name(vtype), cache);
    }
}

G_MODULE_EXPORT void
gthumb_extension_activate (void) {
    if (getenv("FACES_INTERCEPT") != NULL) {
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
    } else {
        // Hook into processing when browser key press event isn't handled elsewhere
        gth_hook_add_callback ("gth-browser-file-list-key-press", 10, G_CALLBACK (faces_keypress), NULL);
        // Hook into processing when browser viewer is activated
        gth_hook_add_callback("gth-browser-activate-viewer-page", 10, G_CALLBACK(faces_viewer_activated), NULL);
    }
    // Read our database path and open it
    GSettings *settings = g_settings_new(GTHUMB_FACES_SCHEMA);
    char *dbpath = g_settings_get_string(settings, PREF_FACES_DBPATH);
    g_object_unref(settings);
    _dbg("faces: org.gnome.gthumb.faces.dbpath=%s\n", dbpath);
    if (!dbpath || !dbpath[0])
        dbpath = dbfile;
    // save a copy of the path name
    dbfile = g_strdup(dbpath);
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
    // Display the current database path and threshold
    const char *thresh = "unknown";
    if (db) {
        sqlite3_stmt *stmt = NULL;
        int rv = sqlite3_prepare_v2(db, "SELECT key,value from face_scanner_config WHERE key = 'threshold'", -1, &stmt, NULL);
        if (SQLITE_OK != rv) {
            fprintf(stderr, "faces: unable to read config: %d\n", rv);
            return;
        }
        while ((rv = sqlite3_step(stmt)) != SQLITE_DONE) {
            thresh = sqlite3_column_text(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }
    gchar *msg = g_strdup_printf("Database: %s\nThreshold: %s", dbfile, thresh);
    GtkWidget *dialog = gtk_message_dialog_new(parent, 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, msg);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(msg);
}
