/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */

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

// where we store our prefs (in dconf-editor)
#define GTHUMB_FACES_SCHEMA GTHUMB_SCHEMA ".faces"
#define PREF_FACES_DBPATH "dbpath"

// Database handle and default location
static sqlite3 *db = NULL;
static char *dbfile = "/home/shared/photos/faces.db";

// local debug messages
static void _dbg(const char *fmt, ...)
{
    if (getenv("FACE_DEBUG")!=NULL) {
        va_list va;
        va_start(va, fmt);
        vprintf(fmt, va);
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
        "SELECT DISTINCT d.left, d.top, d.right, d.bottom, g.label, g.grp from file_paths f inner join face_data as d on d.hash = f.hash inner join face_groups as g on g.grp = d.grp where f.path = ?1",
        -1 , &stmt, NULL);
    if (SQLITE_OK != rv)
        fprintf(stderr, "faces: sqlite_prepare error: %d\n", rv);
    rv = sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    if (SQLITE_OK != rv)
        fprintf(stderr, "faces: sqlite_bind error: %d\n", rv);
    while ((rv = sqlite3_step(stmt)) != SQLITE_DONE)
    {
        int l, t, r, b;
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
	        cairo_set_source_rgb(cr, 1.0, 0, 0);
	        cairo_rectangle(cr, l, t, r-l, b-t);
            cairo_move_to(cr, l+5, b-5);
            cairo_set_font_size(cr, 20/sw);
            cairo_text_path(cr, n);
            cairo_text_path(cr, g);
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

G_MODULE_EXPORT void
gthumb_extension_activate (void)
{
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
}


G_MODULE_EXPORT void
gthumb_extension_deactivate (void)
{
    if (db)
        sqlite3_close(db);
}


G_MODULE_EXPORT gboolean
gthumb_extension_is_configurable (void)
{
	return TRUE;
}


G_MODULE_EXPORT void
gthumb_extension_configure (GtkWindow *parent)
{
    GtkWidget *dialog = gtk_message_dialog_new(parent, 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Hello config!");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}
