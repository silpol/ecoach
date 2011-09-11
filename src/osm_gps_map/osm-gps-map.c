/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*- */
/* vim:set et sw=4 ts=4 cino=t0,(0: */
/*
 * osm-gps-map.c
 * Copyright (C) Marcus Bauer 2008 <marcus.bauer@gmail.com>
 * Copyright (C) John Stowers 2009 <john.stowers@gmail.com>
 *
 * Contributions by
 * Everaldo Canuto 2009 <everaldo.canuto@gmail.com>
 *
 * osm-gps-map.c is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * osm-gps-map.c is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <fcntl.h>
#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gdk/gdk.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <libsoup/soup.h>

#include "converter.h"
#include "osm-gps-map-types.h"
#include "osm-gps-map.h"

/* i18n */
#include <glib/gi18n.h>

#ifdef USE_CAIRO
#include <cairo.h>
#endif

#define ENABLE_DEBUG 1

#define EXTRA_BORDER (TILESIZE / 2)

struct _OsmGpsMapPrivate
{
    GHashTable *tile_queue;
    GHashTable *missing_tiles;
    GHashTable *tile_cache;

    int map_zoom;
    int max_zoom;
    int min_zoom;
    gboolean map_auto_center;
    gboolean map_auto_download;
    int map_x;
    int map_y;

    /* Latitude and longitude of the center of the map, in radians */
    gfloat center_rlat;
    gfloat center_rlon;

    guint max_tile_cache_size;
    /* Incremented at each redraw */
    guint redraw_cycle;
    /* ID of the idle redraw operation */
    guint idle_map_redraw;

    //how we download tiles
    SoupSession *soup_session;
    char *proxy_uri;

    //where downloaded tiles are cached
    char *cache_dir;
    gboolean cache_dir_is_full_path;

    //contains flags indicating the various special characters
    //the uri string contains, that will be replaced when calculating
    //the uri to download.
    OsmGpsMapSource_t map_source;
    char *repo_uri;
    char *image_format;
    int uri_format;
    //flag indicating if the map source is located on the google
    gboolean the_google;

    //gps tracking state
    gboolean record_trip_history;
    gboolean show_trip_history;
    GSList *trip_history;
    coord_t *gps;
    gboolean gps_valid;
    
    gboolean draw_buttons;

    //additional images or tracks added to the map
    GSList *tracks;
    GSList *images;
    GSList *buttons;
    
    //Used for storing the joined tiles
    GdkPixmap *pixmap;
    GdkGC *gc_map;

    //The tile painted when one cannot be found
    GdkPixbuf *null_tile;

    //For tracking click and drag
    int drag_counter;
    int drag_mouse_dx;
    int drag_mouse_dy;
    int drag_start_mouse_x;
    int drag_start_mouse_y;
    int drag_start_map_x;
    int drag_start_map_y;

    //for customizing the redering of the gps track
    int ui_gps_track_width;
    int ui_gps_point_inner_radius;
    int ui_gps_point_outer_radius;

    guint is_disposed : 1;
    guint dragging : 1;
    guint center_coord_set : 1;
};

#define OSM_GPS_MAP_PRIVATE(o)  (OSM_GPS_MAP (o)->priv)

typedef struct
{
    GdkPixbuf *pixbuf;
    /* We keep track of the number of the redraw cycle this tile was last used,
     * so that osm_gps_map_purge_cache() can remove the older ones */
    guint redraw_cycle;
} OsmCachedTile;

enum
{
    PROP_0,

    PROP_AUTO_CENTER,
    PROP_RECORD_TRIP_HISTORY,
    PROP_SHOW_TRIP_HISTORY,
    PROP_AUTO_DOWNLOAD,
    PROP_REPO_URI,
    PROP_PROXY_URI,
    PROP_TILE_CACHE_DIR,
    PROP_TILE_CACHE_DIR_IS_FULL_PATH,
    PROP_ZOOM,
    PROP_MAX_ZOOM,
    PROP_MIN_ZOOM,
    PROP_LATITUDE,
    PROP_LONGITUDE,
    PROP_MAP_X,
    PROP_MAP_Y,
    PROP_TILES_QUEUED,
    PROP_GPS_TRACK_WIDTH,
    PROP_GPS_POINT_R1,
    PROP_GPS_POINT_R2,
    PROP_MAP_SOURCE,
    PROP_IMAGE_FORMAT
};

G_DEFINE_TYPE (OsmGpsMap, osm_gps_map, GTK_TYPE_DRAWING_AREA);

/*
 * Drawing function forward defintions
 */
static gchar    *replace_string(const gchar *src, const gchar *from, const gchar *to);
static void     inspect_map_uri(OsmGpsMap *map);
static gchar    *replace_map_uri(OsmGpsMap *map, const gchar *uri, int zoom, int x, int y);
static void     osm_gps_map_print_images (OsmGpsMap *map);
static void     osm_gps_map_draw_gps_point (OsmGpsMap *map);
static void     osm_gps_map_blit_tile(OsmGpsMap *map, GdkPixbuf *pixbuf, int offset_x, int offset_y);
static void     osm_gps_map_tile_download_complete (SoupSession *session, SoupMessage *msg, gpointer user_data);
static void     osm_gps_map_download_tile (OsmGpsMap *map, int zoom, int x, int y, gboolean redraw);
static void     osm_gps_map_load_tile (OsmGpsMap *map, int zoom, int x, int y, int offset_x, int offset_y);
static void     osm_gps_map_fill_tiles_pixel (OsmGpsMap *map);
static gboolean osm_gps_map_map_redraw (OsmGpsMap *map);
static void     osm_gps_map_map_redraw_idle (OsmGpsMap *map);

static void
cached_tile_free (OsmCachedTile *tile)
{
    g_object_unref (tile->pixbuf);
    g_slice_free (OsmCachedTile, tile);
}

/*
 * Description:
 *   Find and replace text within a string.
 *
 * Parameters:
 *   src  (in) - pointer to source string
 *   from (in) - pointer to search text
 *   to   (in) - pointer to replacement text
 *
 * Returns:
 *   Returns a pointer to dynamically-allocated memory containing string
 *   with occurences of the text pointed to by 'from' replaced by with the
 *   text pointed to by 'to'.
 */
static gchar *
replace_string(const gchar *src, const gchar *from, const gchar *to)
{
    size_t size    = strlen(src) + 1;
    size_t fromlen = strlen(from);
    size_t tolen   = strlen(to);

    /* Allocate the first chunk with enough for the original string. */
    gchar *value = g_malloc(size);


    /* We need to return 'value', so let's make a copy to mess around with. */
    gchar *dst = value;

    if ( value != NULL )
    {
        for ( ;; )
        {
            /* Try to find the search text. */
            const gchar *match = g_strstr_len(src, size, from);
            if ( match != NULL )
            {
                gchar *temp;
                /* Find out how many characters to copy up to the 'match'. */
                size_t count = match - src;


                /* Calculate the total size the string will be after the
                 * replacement is performed. */
                size += tolen - fromlen;

                temp = g_realloc(value, size);
                if ( temp == NULL )
                {
                    g_free(value);
                    return NULL;
                }

                /* we'll want to return 'value' eventually, so let's point it
                 * to the memory that we are now working with.
                 * And let's not forget to point to the right location in
                 * the destination as well. */
                dst = temp + (dst - value);
                value = temp;

                /*
                 * Copy from the source to the point where we matched. Then
                 * move the source pointer ahead by the amount we copied. And
                 * move the destination pointer ahead by the same amount.
                 */
                g_memmove(dst, src, count);
                src += count;
                dst += count;

                /* Now copy in the replacement text 'to' at the position of
                 * the match. Adjust the source pointer by the text we replaced.
                 * Adjust the destination pointer by the amount of replacement
                 * text. */
                g_memmove(dst, to, tolen);
                src += fromlen;
                dst += tolen;
            }
            else
            {
                /*
                 * Copy any remaining part of the string. This includes the null
                 * termination character.
                 */
                strcpy(dst, src);
                break;
            }
        }
    }
    return value;
}

static void
map_convert_coords_to_quadtree_string(OsmGpsMap *map, gint x, gint y, gint zoomlevel,
                                      gchar *buffer, const gchar initial,
                                      const gchar *const quadrant)
{
    gchar *ptr = buffer;
    gint n;

    if (initial)
        *ptr++ = initial;

    for(n = zoomlevel-1; n >= 0; n--)
    {
        gint xbit = (x >> n) & 1;
        gint ybit = (y >> n) & 1;
        *ptr++ = quadrant[xbit + 2 * ybit];
    }

    *ptr++ = '\0';
}


static void
inspect_map_uri(OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;

    if (g_strrstr(priv->repo_uri, URI_MARKER_X))
        priv->uri_format |= URI_HAS_X;

    if (g_strrstr(priv->repo_uri, URI_MARKER_Y))
        priv->uri_format |= URI_HAS_Y;

    if (g_strrstr(priv->repo_uri, URI_MARKER_Z))
        priv->uri_format |= URI_HAS_Z;

    if (g_strrstr(priv->repo_uri, URI_MARKER_S))
        priv->uri_format |= URI_HAS_S;

    if (g_strrstr(priv->repo_uri, URI_MARKER_Q))
        priv->uri_format |= URI_HAS_Q;

    if (g_strrstr(priv->repo_uri, URI_MARKER_Q0))
        priv->uri_format |= URI_HAS_Q0;

    if (g_strrstr(priv->repo_uri, URI_MARKER_YS))
        priv->uri_format |= URI_HAS_YS;

    if (g_strrstr(priv->repo_uri, URI_MARKER_R))
        priv->uri_format |= URI_HAS_R;

    if (g_strrstr(priv->repo_uri, "google.com"))
        priv->the_google = TRUE;

    g_debug("URI Format: 0x%X (google: %X)", priv->uri_format, priv->the_google);

}

static gchar *
replace_map_uri(OsmGpsMap *map, const gchar *uri, int zoom, int x, int y)
{
    OsmGpsMapPrivate *priv = map->priv;
    char *url;
    unsigned int i;
    char location[22];

    i = 1;
    url = g_strdup(uri);
    while (i < URI_FLAG_END)
    {
        char *s;
        char *old;

        old = url;
        switch(i & priv->uri_format)
        {
            case URI_HAS_X:
                s = g_strdup_printf("%d", x);
                url = replace_string(url, URI_MARKER_X, s);
                //g_debug("FOUND " URI_MARKER_X);
                break;
            case URI_HAS_Y:
                s = g_strdup_printf("%d", y);
                url = replace_string(url, URI_MARKER_Y, s);
                //g_debug("FOUND " URI_MARKER_Y);
                break;
            case URI_HAS_Z:
                s = g_strdup_printf("%d", zoom);
                url = replace_string(url, URI_MARKER_Z, s);
                //g_debug("FOUND " URI_MARKER_Z);
                break;
            case URI_HAS_S:
                s = g_strdup_printf("%d", priv->max_zoom-zoom);
                url = replace_string(url, URI_MARKER_S, s);
                //g_debug("FOUND " URI_MARKER_S);
                break;
            case URI_HAS_Q:
                map_convert_coords_to_quadtree_string(map,x,y,zoom,location,'t',"qrts");
                s = g_strdup_printf("%s", location);
                url = replace_string(url, URI_MARKER_Q, s);
                //g_debug("FOUND " URI_MARKER_Q);
                break;
            case URI_HAS_Q0:
                map_convert_coords_to_quadtree_string(map,x,y,zoom,location,'\0', "0123");
                s = g_strdup_printf("%s", location);
                url = replace_string(url, URI_MARKER_Q0, s);
                //g_debug("FOUND " URI_MARKER_Q0);
                break;
            case URI_HAS_YS:
                //              s = g_strdup_printf("%d", y);
                //              url = replace_string(url, URI_MARKER_YS, s);
                g_warning("FOUND " URI_MARKER_YS " NOT IMPLEMENTED");
                //            retval = g_strdup_printf(repo->url,
                //                    tilex,
                //                    (1 << (MAX_ZOOM - zoom)) - tiley - 1,
                //                    zoom - (MAX_ZOOM - 17));
                break;
            case URI_HAS_R:
                s = g_strdup_printf("%d", g_random_int_range(0,4));
                url = replace_string(url, URI_MARKER_R, s);
                //g_debug("FOUND " URI_MARKER_R);
                break;
            default:
                s = NULL;
                break;
        }

        if (s) {
            g_free(s);
            g_free(old);
        }

        i = (i << 1);

    }

    return url;
}

static void
my_log_handler (const gchar * log_domain, GLogLevelFlags log_level, const gchar * message, gpointer user_data)
{
    if (!(log_level & G_LOG_LEVEL_DEBUG) || ENABLE_DEBUG)
        g_log_default_handler (log_domain, log_level, message, user_data);
}

static float
osm_gps_map_get_scale_at_point(int zoom, float rlat, float rlon)
{
    /* This is a naieve implementation with values from
    http://wiki.openstreetmap.org/index.php/FAQ#What_is_the_map_scale_for_a_particular_zoom_level_of_the_map.3F  
    http://almien.co.uk/OSM/Tools/Scale/

    The true scale, becase of the mercator projection of all supported map 
    sources, depends on the lat and lon of the point */
    switch (zoom) {
        case 0:
            return 156412.0;
        case 1:
            return 78206.0;
        case 2:
            return 39135.758482;
        case 3:
            return 19567.879241;
        case 4:
            return 9783.939621;
        case 5:
            return 4891.969810;
        case 6:
            return 2445.984905;
        case 7:
            return 1222.992453;
        case 8:
            return 611.496226;
        case 9:
            return 305.748113;
        case 10:
            return 152.874057;
        case 11:
            return 76.437028;
        case 12:
            return 38.218514;
        case 13:
            return 19.109257;
        case 14:
            return 9.554629;
        case 15:
            return 4.777314;
        case 16:
            return 2.388657;
        case 17:
            return 1.194329;
        case 18:
            return 0.597164;
    }

    return 0.0;
}

/* clears the trip list and all resources */
static void
osm_gps_map_free_trip (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;
    if (priv->trip_history) {
        g_slist_foreach(priv->trip_history, (GFunc) g_free, NULL);
        g_slist_free(priv->trip_history);
        priv->trip_history = NULL;
    }
}

/* clears the tracks and all resources */
static void
osm_gps_map_free_tracks (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;
    if (priv->tracks)
    {
        GSList* tmp = priv->tracks;
        while (tmp != NULL)
        {
            g_slist_foreach(tmp->data, (GFunc) g_free, NULL);
            g_slist_free(tmp->data);
            tmp = g_slist_next(tmp);
        }
        g_slist_free(priv->tracks);
        priv->tracks = NULL;
    }
}

/* free the poi image lists */
static void
osm_gps_map_free_images (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;
    if (priv->images) {
        GSList *list;
        for(list = priv->images; list != NULL; list = list->next)
        {
            image_t *im = list->data;
            g_object_unref(im->image);
            g_free(im);
        }
        g_slist_free(priv->images);
        priv->images = NULL;
    }
}

static void
osm_gps_map_print_images (OsmGpsMap *map)
{
    GSList *list;
    int x,y,pixel_x,pixel_y;
    int min_x = 0,min_y = 0,max_x = 0,max_y = 0;
    int map_x0, map_y0;
    OsmGpsMapPrivate *priv = map->priv;

    map_x0 = priv->map_x - EXTRA_BORDER;
    map_y0 = priv->map_y - EXTRA_BORDER;
    for(list = priv->images; list != NULL; list = list->next)
    {
        image_t *im = list->data;

        // pixel_x,y, offsets
        pixel_x = lon2pixel(priv->map_zoom, im->pt.rlon);
        pixel_y = lat2pixel(priv->map_zoom, im->pt.rlat);

        g_debug("Image %dx%d @: %f,%f (%d,%d)",
                im->w, im->h,
                im->pt.rlat, im->pt.rlon,
                pixel_x, pixel_y);

        x = pixel_x - map_x0;
        y = pixel_y - map_y0;

        gdk_draw_pixbuf (
                         priv->pixmap,
                         priv->gc_map,
                         im->image,
                         0,0,
                         x-(im->w/2),y-(im->h/2),
                         im->w,im->h,
                         GDK_RGB_DITHER_NONE, 0, 0);

        max_x = MAX(x+im->w,max_x);
        min_x = MIN(x-im->w,min_x);
        max_y = MAX(y+im->h,max_y);
        min_y = MIN(y-im->h,min_y);
    }

    gtk_widget_queue_draw_area (
                                GTK_WIDGET(map),
                                min_x + EXTRA_BORDER, min_y + EXTRA_BORDER,
                                max_x + EXTRA_BORDER, max_y + EXTRA_BORDER);

}

static void
osm_gps_map_draw_gps_point (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;

    //incase we get called before we have got a gps point
    if (priv->gps_valid) {
        int map_x0, map_y0;
        int x, y;
        int r = priv->ui_gps_point_inner_radius;
        int r2 = priv->ui_gps_point_outer_radius;
        int lw = priv->ui_gps_track_width;
        int mr = MAX(r,r2);

        map_x0 = priv->map_x - EXTRA_BORDER;
        map_y0 = priv->map_y - EXTRA_BORDER;
        x = lon2pixel(priv->map_zoom, priv->gps->rlon) - map_x0;
        y = lat2pixel(priv->map_zoom, priv->gps->rlat) - map_y0;
#ifdef USE_CAIRO
        cairo_t *cr;
        cairo_pattern_t *pat;
#else
        GdkColor color;
        GdkGC *marker;
#endif

#ifdef USE_CAIRO
        cr = gdk_cairo_create(priv->pixmap);

        // draw transparent area
        if (r2 > 0) {
            cairo_set_line_width (cr, 1.5);
            cairo_set_source_rgba (cr, 0.75, 0.75, 0.75, 0.4);
            cairo_arc (cr, x, y, r2, 0, 2 * M_PI);
            cairo_fill (cr);
            // draw transparent area border
            cairo_set_source_rgba (cr, 0.55, 0.55, 0.55, 0.4);
            cairo_arc (cr, x, y, r2, 0, 2 * M_PI);
            cairo_stroke(cr);
        }

        // draw ball gradient
        if (r > 0) {
            pat = cairo_pattern_create_radial (x-(r/5), y-(r/5), (r/5), x,  y, r);
            cairo_pattern_add_color_stop_rgba (pat, 0, 1, 1, 1, 1.0);
            cairo_pattern_add_color_stop_rgba (pat, 1, 0, 0, 1, 1.0);
            cairo_set_source (cr, pat);
            cairo_arc (cr, x, y, r, 0, 2 * M_PI);
            cairo_fill (cr);
            cairo_pattern_destroy (pat);
            // draw ball border
            cairo_set_line_width (cr, 1.0);
            cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 1.0);
            cairo_arc (cr, x, y, r, 0, 2 * M_PI);
            cairo_stroke(cr);
        }

        cairo_destroy(cr);
        gtk_widget_queue_draw_area (GTK_WIDGET(map),
                                    x-mr,
                                    y-mr,
                                    mr*2,
                                    mr*2);
#else
        marker = gdk_gc_new(priv->pixmap);
        color.red = 5000;
        color.green = 5000;
        color.blue = 55000;
        gdk_gc_set_rgb_fg_color(marker, &color);
        gdk_gc_set_line_attributes(marker, lw, GDK_LINE_SOLID, GDK_CAP_ROUND, GDK_JOIN_ROUND);

        if (r2 > 0) {
            gdk_draw_arc (priv->pixmap,
                          marker,
                          FALSE,            //filled
                          x-r2, y-r2,       // x,y
                          r2*2,r2*2,        // width, height
                          0, 360*64);       // start-end angle 64th, from 3h, anti clockwise
        }
        if (r > 0) {
            gdk_draw_arc (priv->pixmap,
                          marker,
                          TRUE,         //filled
                          x-r, y-r,     // x,y
                          r*2,r*2,      // width, height
                          0, 360*64);   // start-end angle 64th, from 3h, anti clockwise
        }

        g_object_unref(marker);
        gtk_widget_queue_draw_area (GTK_WIDGET(map),
                                    x-(mr+lw),
                                    y-(mr+lw),
                                    (mr*2)+lw+lw,
                                    (mr*2)+lw+lw);
#endif
    }
}

static void
osm_gps_map_blit_tile(OsmGpsMap *map, GdkPixbuf *pixbuf, int offset_x, int offset_y)
{
    OsmGpsMapPrivate *priv = map->priv;

    g_debug("Queing redraw @ %d,%d (w:%d h:%d)", offset_x,offset_y, TILESIZE,TILESIZE);

    /* draw pixbuf onto pixmap */
    gdk_draw_pixbuf (priv->pixmap,
                     priv->gc_map,
                     pixbuf,
                     0,0,
                     offset_x,offset_y,
                     TILESIZE,TILESIZE,
                     GDK_RGB_DITHER_NONE, 0, 0);
}

static void
osm_gps_map_tile_download_complete (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
    FILE *file;
    tile_download_t *dl = (tile_download_t *)user_data;
    OsmGpsMap *map = OSM_GPS_MAP(dl->map);
    OsmGpsMapPrivate *priv = map->priv;

    if (SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
        if (g_mkdir_with_parents(dl->folder,0700) == 0)
        {
            file = g_fopen(dl->filename, "wb");
            if (file != NULL)
            {
                fwrite (msg->response_body->data, 1, msg->response_body->length, file);
                g_debug("Wrote %lld bytes to %s", msg->response_body->length, dl->filename);
                fclose (file);

                if (dl->redraw)
                {
                    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file (dl->filename, NULL);

                    /* Store the tile into the cache */
                    if (G_LIKELY (pixbuf))
                    {
                        OsmCachedTile *tile = g_slice_new (OsmCachedTile);
                        tile->pixbuf = pixbuf;
                        tile->redraw_cycle = priv->redraw_cycle;
                        /* if the tile is already in the cache (it could be one
                         * rendered from another zoom level), it will be
                         * overwritten */
                        g_hash_table_insert (priv->tile_cache, dl->filename, tile);
                        /* NULL-ify dl->filename so that it won't be freed, as
                         * we are using it as a key in the hash table */
                        dl->filename = NULL;
                    }
                    osm_gps_map_map_redraw_idle (map);
                }
            }
        }
        else
        {
            g_warning("Error creating tile download directory: %s", dl->folder);
        }

        g_hash_table_remove(priv->tile_queue, dl->uri);

        g_free(dl->uri);
        g_free(dl->folder);
        g_free(dl->filename);
        g_free(dl);
    }
    else
    {
        g_warning("Error downloading tile: %d - %s", msg->status_code, msg->reason_phrase);
        if (msg->status_code == SOUP_STATUS_NOT_FOUND)
        {
            g_hash_table_insert(priv->missing_tiles, dl->uri, NULL);
            g_hash_table_remove(priv->tile_queue, dl->uri);
        }
        else if (msg->status_code == SOUP_STATUS_CANCELLED)
        {
            ;//application exiting
        }
        else
        {
            soup_session_requeue_message(session, msg);
            return;
        }
    }


}

static void
osm_gps_map_download_tile (OsmGpsMap *map, int zoom, int x, int y, gboolean redraw)
{
    SoupMessage *msg;
    OsmGpsMapPrivate *priv = map->priv;
    tile_download_t *dl = g_new0(tile_download_t,1);

    //calculate the uri to download
    dl->uri = replace_map_uri(map, priv->repo_uri, zoom, x, y);

    //check the tile has not already been queued for download,
    //or has been attempted, and its missing
    if (g_hash_table_lookup_extended(priv->tile_queue, dl->uri, NULL, NULL) ||
        g_hash_table_lookup_extended(priv->missing_tiles, dl->uri, NULL, NULL) )
    {
        g_debug("Tile already downloading (or missing)");
        g_free(dl->uri);
        g_free(dl);
    } else {
        dl->folder = g_strdup_printf("%s%c%d%c%d%c",
                            priv->cache_dir, G_DIR_SEPARATOR,
                            zoom, G_DIR_SEPARATOR,
                            x, G_DIR_SEPARATOR);
        dl->filename = g_strdup_printf("%s%c%d%c%d%c%d.%s",
                            priv->cache_dir, G_DIR_SEPARATOR,
                            zoom, G_DIR_SEPARATOR,
                            x, G_DIR_SEPARATOR,
                            y,
                            priv->image_format);
        dl->map = map;
        dl->redraw = redraw;

        g_debug("Download tile: %d,%d z:%d\n\t%s --> %s", x, y, zoom, dl->uri, dl->filename);

        msg = soup_message_new (SOUP_METHOD_GET, dl->uri);
        if (msg) {
            if (priv->the_google) {
                //Set maps.google.com as the referrer
                g_debug("Setting Google Referrer");
                soup_message_headers_append(msg->request_headers, "Referer", "http://maps.google.com/");
                //For google satelite also set the appropriate cookie value
                if (priv->uri_format & URI_HAS_Q) {
                    const char *cookie = g_getenv("GOOGLE_COOKIE");
                    if (cookie) {
                        g_debug("Adding Google Cookie");
                        soup_message_headers_append(msg->request_headers, "Cookie", cookie);
                    }
                }
            }

            g_hash_table_insert (priv->tile_queue, dl->uri, msg);
            soup_session_queue_message (priv->soup_session, msg, osm_gps_map_tile_download_complete, dl);
        } else {
            g_warning("Could not create soup message");
            g_free(dl->uri);
            g_free(dl->folder);
            g_free(dl->filename);
            g_free(dl);
        }
    }
}

static GdkPixbuf *
osm_gps_map_load_cached_tile (OsmGpsMap *map, int zoom, int x, int y)
{
    OsmGpsMapPrivate *priv = map->priv;
    gchar *filename;
    GdkPixbuf *pixbuf;
    OsmCachedTile *tile;

    filename = g_strdup_printf("%s%c%d%c%d%c%d.%s",
                priv->cache_dir, G_DIR_SEPARATOR,
                zoom, G_DIR_SEPARATOR,
                x, G_DIR_SEPARATOR,
                y,
                priv->image_format);

    tile = g_hash_table_lookup (priv->tile_cache, filename);
    if (tile)
    {
        g_free (filename);
    }
    else
    {
        pixbuf = gdk_pixbuf_new_from_file (filename, NULL);
        if (pixbuf)
        {
            tile = g_slice_new (OsmCachedTile);
            tile->pixbuf = pixbuf;
            g_hash_table_insert (priv->tile_cache, filename, tile);
        }
    }

    /* set/update the redraw_cycle timestamp on the tile */
    if (tile)
    {
        tile->redraw_cycle = priv->redraw_cycle;
        pixbuf = g_object_ref (tile->pixbuf);
    }

    return pixbuf;
}

static GdkPixbuf *
osm_gps_map_find_bigger_tile (OsmGpsMap *map, int zoom, int x, int y,
                              int *zoom_found)
{
    GdkPixbuf *pixbuf;
    int next_zoom, next_x, next_y;

    if (zoom == 0) return NULL;
    next_zoom = zoom - 1;
    next_x = x / 2;
    next_y = y / 2;
    pixbuf = osm_gps_map_load_cached_tile (map, next_zoom, next_x, next_y);
    if (pixbuf)
        *zoom_found = next_zoom;
    else
        pixbuf = osm_gps_map_find_bigger_tile (map, next_zoom, next_x, next_y,
                                               zoom_found);
    return pixbuf;
}

static GdkPixbuf *
osm_gps_map_render_missing_tile_upscaled (OsmGpsMap *map, int zoom,
                                          int x, int y)
{
    GdkPixbuf *pixbuf, *big, *area;
    int zoom_big, zoom_diff, area_size, area_x, area_y;
    int modulo;

    big = osm_gps_map_find_bigger_tile (map, zoom, x, y, &zoom_big);
    if (!big) return NULL;

    g_debug ("Found bigger tile (zoom = %d, wanted = %d)", zoom_big, zoom);

    /* get a Pixbuf for the area to magnify */
    zoom_diff = zoom - zoom_big;
    area_size = TILESIZE >> zoom_diff;
    modulo = 1 << zoom_diff;
    area_x = (x % modulo) * area_size;
    area_y = (y % modulo) * area_size;
    area = gdk_pixbuf_new_subpixbuf (big, area_x, area_y,
                                     area_size, area_size);
    g_object_unref (big);
    pixbuf = gdk_pixbuf_scale_simple (area, TILESIZE, TILESIZE,
                                      GDK_INTERP_NEAREST);
    g_object_unref (area);
    return pixbuf;
}

static GdkPixbuf *
osm_gps_map_render_missing_tile (OsmGpsMap *map, int zoom, int x, int y)
{
    /* maybe TODO: render from downscaled tiles, if the following fails */
    return osm_gps_map_render_missing_tile_upscaled (map, zoom, x, y);
}

static void
osm_gps_map_load_tile (OsmGpsMap *map, int zoom, int x, int y, int offset_x, int offset_y)
{
    OsmGpsMapPrivate *priv = map->priv;
    gchar *filename;
    GdkPixbuf *pixbuf;

    g_debug("Load tile %d,%d (%d,%d) z:%d", x, y, offset_x, offset_y, zoom);

    if (priv->map_source == OSM_GPS_MAP_SOURCE_NULL) {
        osm_gps_map_blit_tile(map, priv->null_tile, offset_x,offset_y);
        return;
    }

    filename = g_strdup_printf("%s%c%d%c%d%c%d.%s",
                priv->cache_dir, G_DIR_SEPARATOR,
                zoom, G_DIR_SEPARATOR,
                x, G_DIR_SEPARATOR,
                y,
                priv->image_format);

    pixbuf = gdk_pixbuf_new_from_file (filename, NULL);
    if(pixbuf)
    {
        g_debug("Found tile %s", filename);
        osm_gps_map_blit_tile(map, pixbuf, offset_x,offset_y);
        g_object_unref (pixbuf);
    }
    else
    {
        if (priv->map_auto_download)
            osm_gps_map_download_tile(map, zoom, x, y, TRUE);

        /* try to render the tile by scaling cached tiles from other zoom
         * levels */
        pixbuf = osm_gps_map_render_missing_tile (map, zoom, x, y);
        if (pixbuf)
        {
            gdk_draw_pixbuf (priv->pixmap,
                             priv->gc_map,
                             pixbuf,
                             0,0,
                             offset_x,offset_y,
                             TILESIZE,TILESIZE,
                             GDK_RGB_DITHER_NONE, 0, 0);
            g_object_unref (pixbuf);
        }
        else
        {
            //prevent some artifacts when drawing not yet loaded areas.
            gdk_draw_rectangle (priv->pixmap,
                                GTK_WIDGET(map)->style->white_gc,
                                TRUE, offset_x, offset_y, TILESIZE, TILESIZE);
        }
    }
    g_free(filename);
}

static void
osm_gps_map_fill_tiles_pixel (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;
    int i,j, width, height, tile_x0, tile_y0, tiles_nx, tiles_ny;
    int offset_xn = 0;
    int offset_yn = 0;
    int offset_x;
    int offset_y;

    g_debug("Fill tiles: %d,%d z:%d", priv->map_x, priv->map_y, priv->map_zoom);

    offset_x = - priv->map_x % TILESIZE;
    offset_y = - priv->map_y % TILESIZE;
    if (offset_x > 0) offset_x -= TILESIZE;
    if (offset_y > 0) offset_y -= TILESIZE;

    offset_xn = offset_x + EXTRA_BORDER;
    offset_yn = offset_y + EXTRA_BORDER;

    width  = GTK_WIDGET(map)->allocation.width;
    height = GTK_WIDGET(map)->allocation.height;

    tiles_nx = (width  - offset_x) / TILESIZE + 1;
    tiles_ny = (height - offset_y) / TILESIZE + 1;

    tile_x0 =  floor((float)priv->map_x / (float)TILESIZE);
    tile_y0 =  floor((float)priv->map_y / (float)TILESIZE);

    //TODO: implement wrap around
    for (i=tile_x0; i<(tile_x0+tiles_nx);i++)
    {
        for (j=tile_y0;  j<(tile_y0+tiles_ny); j++)
        {
            if( j<0 || i<0 || i>=exp(priv->map_zoom * M_LN2) || j>=exp(priv->map_zoom * M_LN2))
            {
                gdk_draw_rectangle (priv->pixmap,
                                    GTK_WIDGET(map)->style->white_gc,
                                    TRUE,
                                    offset_xn, offset_yn,
                                    TILESIZE,TILESIZE);
            }
            else
            {
                osm_gps_map_load_tile(map,
                                      priv->map_zoom,
                                      i,j,
                                      offset_xn,offset_yn);
            }
            offset_yn += TILESIZE;
        }
        offset_xn += TILESIZE;
        offset_yn = offset_y + EXTRA_BORDER;
    }
}

static void
osm_gps_map_print_track (OsmGpsMap *map, GSList *trackpoint_list)
{
    OsmGpsMapPrivate *priv = map->priv;

    GSList *list;
    int x,y;
    int min_x = 0,min_y = 0,max_x = 0,max_y = 0;
    int lw = priv->ui_gps_track_width;
    int map_x0, map_y0;
#ifdef USE_CAIRO
    cairo_t *cr;
#else
    int last_x = 0, last_y = 0;
    GdkColor color;
    GdkGC *gc;
#endif

#ifdef USE_CAIRO
    cr = gdk_cairo_create(priv->pixmap);
    cairo_set_line_width (cr, lw);
    cairo_set_source_rgba (cr, 60000.0/65535.0, 0.0, 0.0, 0.6);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
#else
    gc = gdk_gc_new(priv->pixmap);
    color.green = 0;
    color.blue = 0;
    color.red = 60000;
    gdk_gc_set_rgb_fg_color(gc, &color);
    gdk_gc_set_line_attributes(gc, lw, GDK_LINE_SOLID, GDK_CAP_ROUND, GDK_JOIN_ROUND);
#endif

    map_x0 = priv->map_x - EXTRA_BORDER;
    map_y0 = priv->map_y - EXTRA_BORDER;
    for(list = trackpoint_list; list != NULL; list = list->next)
    {
        coord_t *tp = list->data;

        x = lon2pixel(priv->map_zoom, tp->rlon) - map_x0;
        y = lat2pixel(priv->map_zoom, tp->rlat) - map_y0;

        // first time through loop
        if (list == trackpoint_list) {
#ifdef USE_CAIRO
            cairo_move_to(cr, x, y);
#else
            last_x = x;
            last_y = y;
#endif
        }

#ifdef USE_CAIRO
        cairo_line_to(cr, x, y);
#else
        gdk_draw_line (priv->pixmap, gc, x, y, last_x, last_y);
        last_x = x;
        last_y = y;
#endif

        max_x = MAX(x,max_x);
        min_x = MIN(x,min_x);
        max_y = MAX(y,max_y);
        min_y = MIN(y,min_y);
    }

    gtk_widget_queue_draw_area (
                                GTK_WIDGET(map),
                                min_x - lw,
                                min_y - lw,
                                max_x + (lw * 2),
                                max_y + (lw * 2));

#ifdef USE_CAIRO
    cairo_stroke(cr);
    cairo_destroy(cr);
#else
    g_object_unref(gc);
#endif
}

/* Prints the gps trip history, and any other tracks */
static void
osm_gps_map_print_tracks (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;

    if (priv->show_trip_history)
        osm_gps_map_print_track (map, priv->trip_history);

    if (priv->tracks)
    {
        GSList* tmp = priv->tracks;
        while (tmp != NULL)
        {
            osm_gps_map_print_track (map, tmp->data);
            tmp = g_slist_next(tmp);
        }
    }
}

static gboolean
osm_gps_map_purge_cache_check(gpointer key, gpointer value, gpointer user)
{
   return (((OsmCachedTile*)value)->redraw_cycle != ((OsmGpsMapPrivate*)user)->redraw_cycle);
}

static void
osm_gps_map_purge_cache (OsmGpsMap *map)
{
   OsmGpsMapPrivate *priv = map->priv;

   if (g_hash_table_size (priv->tile_cache) < priv->max_tile_cache_size)
       return;

   /* run through the cache, and remove the tiles which have not been used
    * during the last redraw operation */
   g_hash_table_foreach_remove(priv->tile_cache, osm_gps_map_purge_cache_check, priv);
}

static gboolean
osm_gps_map_map_redraw (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;

    priv->idle_map_redraw = 0;

    /* the motion_notify handler uses priv->pixmap to redraw the area; if we
     * change it while we are dragging, we will end up showing it in the wrong
     * place. This could be fixed by carefully recompute the coordinates, but
     * for now it's easier just to disable redrawing the map while dragging */
    if (priv->dragging)
        return FALSE;

    priv->redraw_cycle++;

    /* draw white background to initialise pixmap */
    gdk_draw_rectangle (
                        priv->pixmap,
                        GTK_WIDGET(map)->style->white_gc,
                        TRUE,
                        0, 0,
                        GTK_WIDGET(map)->allocation.width + EXTRA_BORDER * 2,
                        GTK_WIDGET(map)->allocation.height + EXTRA_BORDER * 2);

    osm_gps_map_fill_tiles_pixel(map);

    osm_gps_map_print_tracks(map);
    osm_gps_map_draw_gps_point(map);
    osm_gps_map_print_images(map);

    //osm_gps_map_osd_speed(map, 1.5);
    osm_gps_map_purge_cache(map);
    gtk_widget_queue_draw (GTK_WIDGET (map));

    return FALSE;
}

static void
osm_gps_map_map_redraw_idle (OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv = map->priv;

    if (priv->idle_map_redraw == 0)
        priv->idle_map_redraw = g_idle_add ((GSourceFunc)osm_gps_map_map_redraw, map);
}

static void
osm_gps_map_init (OsmGpsMap *object)
{
    OsmGpsMapPrivate *priv;

    priv = G_TYPE_INSTANCE_GET_PRIVATE (object, OSM_TYPE_GPS_MAP, OsmGpsMapPrivate);
    object->priv = priv;

    priv->pixmap = NULL;

    priv->trip_history = NULL;
    priv->gps = g_new0(coord_t, 1);
    priv->gps_valid = FALSE;

    priv->tracks = NULL;
    priv->images = NULL;

    priv->drag_counter = 0;
    priv->drag_mouse_dx = 0;
    priv->drag_mouse_dy = 0;
    priv->drag_start_mouse_x = 0;
    priv->drag_start_mouse_y = 0;

    priv->uri_format = 0;
    priv->the_google = FALSE;

    priv->map_source = -1;

    //Change naumber of concurrent connections option?
    priv->soup_session = soup_session_async_new_with_options(
                                                             SOUP_SESSION_USER_AGENT,
                                                             "Mozilla/5.0 (Windows; U; Windows NT 5.1; en-US; rv:1.8.1.11) Gecko/20071127 Firefox/2.0.0.11",
                                                             NULL);

    //Hash table which maps tile d/l URIs to SoupMessage requests
    priv->tile_queue = g_hash_table_new (g_str_hash, g_str_equal);

    //Some mapping providers (Google) have varying degrees of tiles at multiple
    //zoom levels
    priv->missing_tiles = g_hash_table_new (g_str_hash, g_str_equal);

    /* memory cache for most recently used tiles */
    priv->tile_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                              g_free, (GDestroyNotify)cached_tile_free);
    priv->max_tile_cache_size = 20;

    gtk_widget_add_events (GTK_WIDGET (object),
                           GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                           GDK_POINTER_MOTION_MASK |
                           GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    GTK_WIDGET_SET_FLAGS (object, GTK_CAN_FOCUS);

    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MASK, my_log_handler, NULL);
}

static GObject *
osm_gps_map_constructor (GType gtype, guint n_properties, GObjectConstructParam *properties)
{
    GObject *object;
    OsmGpsMapPrivate *priv;
    OsmGpsMap *map;
    const char *uri, *name;

    //Always chain up to the parent constructor
    object = G_OBJECT_CLASS(osm_gps_map_parent_class)->constructor(gtype, n_properties, properties);
    map = OSM_GPS_MAP(object);
    priv = OSM_GPS_MAP_PRIVATE(object);

    //user can specify a map source ID, or a repo URI as the map source
    uri = osm_gps_map_source_get_repo_uri(OSM_GPS_MAP_SOURCE_NULL);
    if ( (priv->map_source == 0) || (strcmp(priv->repo_uri, uri) == 0) ) {
        g_debug("Using null source");
        priv->map_source = OSM_GPS_MAP_SOURCE_NULL;

        priv->null_tile = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 256, 256);
        gdk_pixbuf_fill(priv->null_tile, 0xcccccc00);
    }
    else if (priv->map_source >= 0) {
        //check if the source given is valid
        uri = osm_gps_map_source_get_repo_uri(priv->map_source);
        if (uri) {
            g_debug("Setting map source from ID");
            g_free(priv->repo_uri);

            priv->repo_uri = g_strdup(uri);
            priv->image_format = g_strdup(
                osm_gps_map_source_get_image_format(priv->map_source));
            priv->max_zoom = osm_gps_map_source_get_max_zoom(priv->map_source);
            priv->min_zoom = osm_gps_map_source_get_min_zoom(priv->map_source);
        }
    }

    if (!priv->cache_dir_is_full_path) {
        char *md5 = g_compute_checksum_for_string (G_CHECKSUM_MD5, priv->repo_uri, -1);

        if (priv->cache_dir) {
            char *old = priv->cache_dir;
            //the new cachedir is the given cache dir + the md5 of the repo_uri
            priv->cache_dir = g_strdup_printf("%s%c%s", old, G_DIR_SEPARATOR, md5);
            g_debug("Adjusting cache dir %s -> %s", old, priv->cache_dir);
            g_free(old);
        } else {
            //the new cachedir is the current dir + the md5 of the repo_uri
            priv->cache_dir = g_strdup(md5);
        }

        g_free(md5);
    }

    inspect_map_uri(map);

    return object;
}

static void
osm_gps_map_dispose (GObject *object)
{
    OsmGpsMap *map = OSM_GPS_MAP(object);
    OsmGpsMapPrivate *priv = map->priv;

    if (priv->is_disposed)
        return;

    priv->is_disposed = TRUE;

    soup_session_abort(priv->soup_session);
    g_object_unref(priv->soup_session);

    g_hash_table_destroy(priv->tile_queue);
    g_hash_table_destroy(priv->missing_tiles);
    g_hash_table_destroy(priv->tile_cache);

    osm_gps_map_free_images(map);

    if(priv->pixmap)
        g_object_unref (priv->pixmap);

    if (priv->null_tile)
        g_object_unref (priv->null_tile);

    if(priv->gc_map)
        g_object_unref(priv->gc_map);

    if (priv->idle_map_redraw != 0)
        g_source_remove (priv->idle_map_redraw);

    G_OBJECT_CLASS (osm_gps_map_parent_class)->dispose (object);
}

static void
osm_gps_map_finalize (GObject *object)
{
    OsmGpsMap *map = OSM_GPS_MAP(object);
    OsmGpsMapPrivate *priv = map->priv;

    g_free(priv->cache_dir);
    g_free(priv->repo_uri);
    g_free(priv->image_format);

    osm_gps_map_free_trip(map);
    osm_gps_map_free_tracks(map);

    G_OBJECT_CLASS (osm_gps_map_parent_class)->finalize (object);
}

static void
osm_gps_map_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    g_return_if_fail (OSM_IS_GPS_MAP (object));
    OsmGpsMap *map = OSM_GPS_MAP(object);
    OsmGpsMapPrivate *priv = map->priv;

    switch (prop_id)
    {
        case PROP_AUTO_CENTER:
            priv->map_auto_center = g_value_get_boolean (value);
            break;
        case PROP_RECORD_TRIP_HISTORY:
            priv->record_trip_history = g_value_get_boolean (value);
            break;
        case PROP_SHOW_TRIP_HISTORY:
            priv->show_trip_history = g_value_get_boolean (value);
            break;
        case PROP_AUTO_DOWNLOAD:
            priv->map_auto_download = g_value_get_boolean (value);
            break;
        case PROP_REPO_URI:
            priv->repo_uri = g_value_dup_string (value);
            break;
        case PROP_PROXY_URI:
            if ( g_value_get_string(value) ) {
                GValue val = {0};

                priv->proxy_uri = g_value_dup_string (value);
                g_debug("Setting proxy server: %s", priv->proxy_uri);

                SoupURI* uri = soup_uri_new(priv->proxy_uri);
                g_value_init(&val, SOUP_TYPE_URI);
                g_value_take_boxed(&val, uri);

                g_object_set_property(G_OBJECT(priv->soup_session),SOUP_SESSION_PROXY_URI,&val);
            } else
                priv->proxy_uri = NULL;

            break;
        case PROP_TILE_CACHE_DIR:
            if ( g_value_get_string(value) )
                priv->cache_dir = g_value_dup_string (value);
            else {
                priv->cache_dir = g_build_filename(
                                    g_get_tmp_dir(),
                                    "osmgpsmap",
                                    NULL);
            }
            break;
        case PROP_TILE_CACHE_DIR_IS_FULL_PATH:
            priv->cache_dir_is_full_path = g_value_get_boolean (value);
            break;
        case PROP_ZOOM:
            priv->map_zoom = g_value_get_int (value);
            break;
        case PROP_MAX_ZOOM:
            priv->max_zoom = g_value_get_int (value);
            break;
        case PROP_MIN_ZOOM:
            priv->min_zoom = g_value_get_int (value);
            break;
        case PROP_MAP_X:
            priv->map_x = g_value_get_int (value);
            priv->center_coord_set = FALSE;
            break;
        case PROP_MAP_Y:
            priv->map_y = g_value_get_int (value);
            priv->center_coord_set = FALSE;
            break;
        case PROP_GPS_TRACK_WIDTH:
            priv->ui_gps_track_width = g_value_get_int (value);
            break;
        case PROP_GPS_POINT_R1:
            priv->ui_gps_point_inner_radius = g_value_get_int (value);
            break;
        case PROP_GPS_POINT_R2:
            priv->ui_gps_point_outer_radius = g_value_get_int (value);
            break;
        case PROP_MAP_SOURCE:
            priv->map_source = g_value_get_int (value);
            break;
        case PROP_IMAGE_FORMAT:
            priv->image_format = g_value_dup_string (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
osm_gps_map_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    g_return_if_fail (OSM_IS_GPS_MAP (object));
    float lat,lon;
    OsmGpsMap *map = OSM_GPS_MAP(object);
    OsmGpsMapPrivate *priv = map->priv;

    switch (prop_id)
    {
        case PROP_AUTO_CENTER:
            g_value_set_boolean(value, priv->map_auto_center);
            break;
        case PROP_RECORD_TRIP_HISTORY:
            g_value_set_boolean(value, priv->record_trip_history);
            break;
        case PROP_SHOW_TRIP_HISTORY:
            g_value_set_boolean(value, priv->show_trip_history);
            break;
        case PROP_AUTO_DOWNLOAD:
            g_value_set_boolean(value, priv->map_auto_download);
            break;
        case PROP_REPO_URI:
            g_value_set_string(value, priv->repo_uri);
            break;
        case PROP_PROXY_URI:
            g_value_set_string(value, priv->proxy_uri);
            break;
        case PROP_TILE_CACHE_DIR:
            g_value_set_string(value, priv->cache_dir);
            break;
        case PROP_TILE_CACHE_DIR_IS_FULL_PATH:
            g_value_set_boolean(value, priv->cache_dir_is_full_path);
            break;
        case PROP_ZOOM:
            g_value_set_int(value, priv->map_zoom);
            break;
        case PROP_MAX_ZOOM:
            g_value_set_int(value, priv->max_zoom);
            break;
        case PROP_MIN_ZOOM:
            g_value_set_int(value, priv->min_zoom);
            break;
        case PROP_LATITUDE:
            lat = pixel2lat(priv->map_zoom,
                            priv->map_y + (GTK_WIDGET(map)->allocation.height / 2));
            g_value_set_float(value, rad2deg(lat));
            break;
        case PROP_LONGITUDE:
            lon = pixel2lon(priv->map_zoom,
                            priv->map_x + (GTK_WIDGET(map)->allocation.width / 2));
            g_value_set_float(value, rad2deg(lon));
            break;
        case PROP_MAP_X:
            g_value_set_int(value, priv->map_x);
            break;
        case PROP_MAP_Y:
            g_value_set_int(value, priv->map_y);
            break;
        case PROP_TILES_QUEUED:
            g_value_set_int(value, g_hash_table_size(priv->tile_queue));
            break;
        case PROP_GPS_TRACK_WIDTH:
            g_value_set_int(value, priv->ui_gps_track_width);
            break;
        case PROP_GPS_POINT_R1:
            g_value_set_int(value, priv->ui_gps_point_inner_radius);
            break;
        case PROP_GPS_POINT_R2:
            g_value_set_int(value, priv->ui_gps_point_outer_radius);
            break;
        case PROP_MAP_SOURCE:
            g_value_set_int(value, priv->map_source);
            break;
        case PROP_IMAGE_FORMAT:
            g_value_set_string(value, priv->image_format);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static gboolean
osm_gps_map_scroll_event (GtkWidget *widget, GdkEventScroll  *event)
{
    OsmGpsMap *map = OSM_GPS_MAP(widget);
    OsmGpsMapPrivate *priv = map->priv;

    if (event->direction == GDK_SCROLL_UP)
    {
        osm_gps_map_set_zoom(map, priv->map_zoom+1);
    }
    else
    {
        osm_gps_map_set_zoom(map, priv->map_zoom-1);
    }

    return FALSE;
}

static gboolean
osm_gps_map_button_press (GtkWidget *widget, GdkEventButton *event)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    priv->drag_counter = 0;
    priv->drag_start_mouse_x = (int) event->x;
    priv->drag_start_mouse_y = (int) event->y;
    priv->drag_start_map_x = priv->map_x;
    priv->drag_start_map_y = priv->map_y;

    return FALSE;
}

static gboolean
osm_gps_map_button_release (GtkWidget *widget, GdkEventButton *event)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    if (priv->dragging)
    {
        priv->dragging = FALSE;

        priv->map_x = priv->drag_start_map_x;
        priv->map_y = priv->drag_start_map_y;

        priv->map_x += (priv->drag_start_mouse_x - (int) event->x);
        priv->map_y += (priv->drag_start_mouse_y - (int) event->y);

        priv->center_coord_set = FALSE;

        osm_gps_map_map_redraw_idle(OSM_GPS_MAP(widget));
    }

    priv->drag_mouse_dx = 0;
    priv->drag_mouse_dy = 0;
    priv->drag_counter = 0;

    return FALSE;
}

static gboolean
osm_gps_map_motion_notify (GtkWidget *widget, GdkEventMotion  *event)
{
    int x, y;
    GdkModifierType state;
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    if (event->is_hint)
        gdk_window_get_pointer (event->window, &x, &y, &state);
    else
    {
        x = event->x;
        y = event->y;
        state = event->state;
    }

    // are we being dragged
    if (!(state & GDK_BUTTON1_MASK))
        return FALSE;

    priv->drag_counter++;

    // we havent dragged more than 6 pixels
    if (priv->drag_counter < 6)
        return FALSE;

    priv->dragging = TRUE;

    if (priv->map_auto_center)
        g_object_set(G_OBJECT(widget), "auto-center", FALSE, NULL);

    priv->drag_mouse_dx = x - priv->drag_start_mouse_x;
    priv->drag_mouse_dy = y - priv->drag_start_mouse_y;

    gdk_draw_drawable (
                       widget->window,
                       widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
                       priv->pixmap,
                       0,0,
                       priv->drag_mouse_dx - EXTRA_BORDER, priv->drag_mouse_dy - EXTRA_BORDER,
                       -1,-1);

    //Paint white outside of the map if dragging. Its less
    //ugly than painting the corrupted map
    if(priv->drag_mouse_dx>EXTRA_BORDER) {
        gdk_draw_rectangle (
                            widget->window,
                            widget->style->white_gc,
                            TRUE,
                            0, 0,
                            priv->drag_mouse_dx - EXTRA_BORDER,
                            widget->allocation.height);
    }
    else if (-priv->drag_mouse_dx > EXTRA_BORDER)
    {
        gdk_draw_rectangle (
                            widget->window,
                            widget->style->white_gc,
                            TRUE,
                            priv->drag_mouse_dx + widget->allocation.width + EXTRA_BORDER, 0,
                            -priv->drag_mouse_dx - EXTRA_BORDER,
                            widget->allocation.height);
    }

    if (priv->drag_mouse_dy>EXTRA_BORDER) {
        gdk_draw_rectangle (
                            widget->window,
                            widget->style->white_gc,
                            TRUE,
                            0, 0,
                            widget->allocation.width,
                            priv->drag_mouse_dy - EXTRA_BORDER);
    }
    else if (-priv->drag_mouse_dy > EXTRA_BORDER)
    {
        gdk_draw_rectangle (
                            widget->window,
                            widget->style->white_gc,
                            TRUE,
                            0, priv->drag_mouse_dy + widget->allocation.height + EXTRA_BORDER,
                            widget->allocation.width,
                            -priv->drag_mouse_dy - EXTRA_BORDER);
    }

    return FALSE;
}

static gboolean
osm_gps_map_configure (GtkWidget *widget, GdkEventConfigure *event)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    /* create pixmap */
    if (priv->pixmap)
        g_object_unref (priv->pixmap);

    priv->pixmap = gdk_pixmap_new (
                                   widget->window,
                                   widget->allocation.width + EXTRA_BORDER * 2,
                                   widget->allocation.height + EXTRA_BORDER * 2,
                                   -1);

    /* and gc, used for clipping (I think......) */
    if(priv->gc_map)
        g_object_unref(priv->gc_map);

    priv->gc_map = gdk_gc_new(priv->pixmap);

    osm_gps_map_map_redraw(OSM_GPS_MAP(widget));

    return FALSE;
}

static gboolean
osm_gps_map_expose (GtkWidget *widget, GdkEventExpose  *event)
{
    OsmGpsMapPrivate *priv = OSM_GPS_MAP_PRIVATE(widget);

    gdk_draw_drawable (
                       widget->window,
                       widget->style->fg_gc[GTK_WIDGET_STATE (widget)],
                       priv->pixmap,
                       event->area.x + EXTRA_BORDER, event->area.y + EXTRA_BORDER,
                       event->area.x, event->area.y,
                       event->area.width, event->area.height);
		       
		       
   if(priv->draw_buttons){
    GSList *list;
    for(list = priv->buttons; list != NULL; list = list->next)
    {
        button_t *im = list->data;

        // pixel_x,y, offsets
     /*   pixel_x = lon2pixel(priv->map_zoom, im->pt.rlon);
        pixel_y = lat2pixel(priv->map_zoom, im->pt.rlat);

        g_debug("Image %dx%d @: %f,%f (%d,%d)",
                im->w, im->h,
                im->pt.rlat, im->pt.rlon,
                pixel_x, pixel_y);

        x = pixel_x - map_x0;
        y = pixel_y - map_y0;
*/
      /*  gdk_draw_pixbuf (
                         priv->pixmap,
                         priv->gc_map,
                         im->image,
                         0,0,
                       im->x,im->y,
                         -1,-1,
                         GDK_RGB_DITHER_NONE, 0, 0);
*/
      
      gdk_draw_pixbuf(widget->window,widget->style->fg_gc[GTK_WIDGET_STATE (widget)],im->image,0,0,im->x,im->y,-1,-1,GDK_RGB_DITHER_NONE, 0, 0);

    }
   }
		       
		       
		       
		       
		       

    return FALSE;
}

static void
osm_gps_map_class_init (OsmGpsMapClass *klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    g_type_class_add_private (klass, sizeof (OsmGpsMapPrivate));

    object_class->dispose = osm_gps_map_dispose;
    object_class->finalize = osm_gps_map_finalize;
    object_class->constructor = osm_gps_map_constructor;
    object_class->set_property = osm_gps_map_set_property;
    object_class->get_property = osm_gps_map_get_property;

    widget_class->expose_event = osm_gps_map_expose;
    widget_class->configure_event = osm_gps_map_configure;
    widget_class->button_press_event = osm_gps_map_button_press;
    widget_class->button_release_event = osm_gps_map_button_release;
    widget_class->motion_notify_event = osm_gps_map_motion_notify;
    widget_class->scroll_event = osm_gps_map_scroll_event;

    g_object_class_install_property (object_class,
                                     PROP_AUTO_CENTER,
                                     g_param_spec_boolean ("auto-center",
                                                           "auto center",
                                                           "map auto center",
                                                           TRUE,
                                                           G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_RECORD_TRIP_HISTORY,
                                     g_param_spec_boolean ("record-trip-history",
                                                           "record trip history",
                                                           "should all gps points be recorded in a trip history",
                                                           TRUE,
                                                           G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_SHOW_TRIP_HISTORY,
                                     g_param_spec_boolean ("show-trip-history",
                                                           "show trip history",
                                                           "should the recorded trip history be shown on the map",
                                                           TRUE,
                                                           G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_AUTO_DOWNLOAD,
                                     g_param_spec_boolean ("auto-download",
                                                           "auto download",
                                                           "map auto download",
                                                           TRUE,
                                                           G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_REPO_URI,
                                     g_param_spec_string ("repo-uri",
                                                          "repo uri",
                                                          "map source tile repository uri",
                                                          OSM_REPO_URI,
                                                          G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

     g_object_class_install_property (object_class,
                                     PROP_PROXY_URI,
                                     g_param_spec_string ("proxy-uri",
                                                          "proxy uri",
                                                          "http proxy uri on NULL",
                                                          NULL,
                                                          G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_TILE_CACHE_DIR,
                                     g_param_spec_string ("tile-cache",
                                                          "tile cache",
                                                          "osm local tile cache dir",
                                                          NULL,
                                                          G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_TILE_CACHE_DIR_IS_FULL_PATH,
                                     g_param_spec_boolean ("tile-cache-is-full-path",
                                                           "tile cache is full path",
                                                           "if true, the path passed to tile-cache is interpreted as the full cache path",
                                                           FALSE,
                                                           G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_ZOOM,
                                     g_param_spec_int ("zoom",
                                                       "zoom",
                                                       "zoom level",
                                                       MIN_ZOOM, /* minimum property value */
                                                       MAX_ZOOM, /* maximum property value */
                                                       3,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_MAX_ZOOM,
                                     g_param_spec_int ("max-zoom",
                                                       "max zoom",
                                                       "maximum zoom level",
                                                       MIN_ZOOM, /* minimum property value */
                                                       MAX_ZOOM, /* maximum property value */
                                                       OSM_MAX_ZOOM,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_MIN_ZOOM,
                                     g_param_spec_int ("min-zoom",
                                                       "min zoom",
                                                       "minimum zoom level",
                                                       MIN_ZOOM, /* minimum property value */
                                                       MAX_ZOOM, /* maximum property value */
                                                       OSM_MIN_ZOOM,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_LATITUDE,
                                     g_param_spec_float ("latitude",
                                                         "latitude",
                                                         "latitude in degrees",
                                                         -90.0, /* minimum property value */
                                                         90.0, /* maximum property value */
                                                         0,
                                                         G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_LONGITUDE,
                                     g_param_spec_float ("longitude",
                                                         "longitude",
                                                         "longitude in degrees",
                                                         -180.0, /* minimum property value */
                                                         180.0, /* maximum property value */
                                                         0,
                                                         G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_MAP_X,
                                     g_param_spec_int ("map-x",
                                                       "map-x",
                                                       "initial map x location",
                                                       G_MININT, /* minimum property value */
                                                       G_MAXINT, /* maximum property value */
                                                       890,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_MAP_Y,
                                     g_param_spec_int ("map-y",
                                                       "map-y",
                                                       "initial map y location",
                                                       G_MININT, /* minimum property value */
                                                       G_MAXINT, /* maximum property value */
                                                       515,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_TILES_QUEUED,
                                     g_param_spec_int ("tiles-queued",
                                                       "tiles-queued",
                                                       "number of tiles currently waiting to download",
                                                       G_MININT, /* minimum property value */
                                                       G_MAXINT, /* maximum property value */
                                                       0,
                                                       G_PARAM_READABLE));

    g_object_class_install_property (object_class,
                                     PROP_GPS_TRACK_WIDTH,
                                     g_param_spec_int ("gps-track-width",
                                                       "gps-track-width",
                                                       "width of the lines drawn for the gps track",
                                                       1,           /* minimum property value */
                                                       G_MAXINT,    /* maximum property value */
                                                       4,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_GPS_POINT_R1,
                                     g_param_spec_int ("gps-track-point-radius",
                                                       "gps-track-point-radius",
                                                       "radius of the gps point inner circle",
                                                       0,           /* minimum property value */
                                                       G_MAXINT,    /* maximum property value */
                                                       5,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_GPS_POINT_R2,
                                     g_param_spec_int ("gps-track-highlight-radius",
                                                       "gps-track-highlight-radius",
                                                       "radius of the gps point highlight circle",
                                                       0,           /* minimum property value */
                                                       G_MAXINT,    /* maximum property value */
                                                       20,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_MAP_SOURCE,
                                     g_param_spec_int ("map-source",
                                                       "map source",
                                                       "map source ID",
                                                       -1,           /* minimum property value */
                                                       G_MAXINT,    /* maximum property value */
                                                       -1,
                                                       G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property (object_class,
                                     PROP_IMAGE_FORMAT,
                                     g_param_spec_string ("image-format",
                                                          "image format",
                                                          "map source tile repository image format (jpg, png)",
                                                          OSM_IMAGE_FORMAT,
                                                          G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));
}

const char* 
osm_gps_map_source_get_friendly_name(OsmGpsMapSource_t source)
{
    switch(source)
    {
        case OSM_GPS_MAP_SOURCE_NULL:
            return _("None");
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP:
            return _("OpenStreetMap");
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP_RENDERER:
            return _("OpenStreetMap Renderer");
        case OSM_GPS_MAP_SOURCE_OPENAERIALMAP:
            return _("OpenAerialMap");
        case OSM_GPS_MAP_SOURCE_MAPS_FOR_FREE:
            return _("Maps-For-Free");
        case OSM_GPS_MAP_SOURCE_GOOGLE_STREET:
            return _("Google Maps");
        case OSM_GPS_MAP_SOURCE_GOOGLE_SATELLITE:
            return _("Google Satellite");
        case OSM_GPS_MAP_SOURCE_GOOGLE_HYBRID:
            return _("Google Hybrid");
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_STREET:
            return _("Virtual Earth");
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_SATELLITE:
            return _("Virtual Earth Satellite");
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_HYBRID:
            return _("Virtual Earth Hybrid");
        case OSM_GPS_MAP_SOURCE_YAHOO_STREET:
            return _("Yahoo Maps");
        case OSM_GPS_MAP_SOURCE_YAHOO_SATELLITE:
            return _("Yahoo Satellite");
        case OSM_GPS_MAP_SOURCE_YAHOO_HYBRID:
            return _("Yahoo Hybrid");
        case OSM_GPS_MAP_SOURCE_OVIMAPS_NORMAL:
            return _("OviMaps");
        case OSM_GPS_MAP_SOURCE_OVIMAPS_HYBRID:
            return _("OviMaps Hybrid");
        case OSM_GPS_MAP_SOURCE_OVIMAPS_TRANSIT:
            return _("OviMaps Public Transport");
        default:
            return NULL;
    }
    return NULL;
}

//http://www.internettablettalk.com/forums/showthread.php?t=5209
//https://garage.maemo.org/plugins/scmsvn/viewcvs.php/trunk/src/maps.c?root=maemo-mapper&view=markup
//http://www.ponies.me.uk/maps/GoogleTileUtils.java
//http://www.mgmaps.com/cache/MapTileCacher.perl
const char* 
osm_gps_map_source_get_repo_uri(OsmGpsMapSource_t source)
{
    switch(source)
    {
        case OSM_GPS_MAP_SOURCE_NULL:
            return "none://";
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP:
            return OSM_REPO_URI;
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP_RENDERER:
            return "http://tah.openstreetmap.org/Tiles/tile/#Z/#X/#Y.png";
        case OSM_GPS_MAP_SOURCE_OPENAERIALMAP:
            return "http://tile.openaerialmap.org/tiles/1.0.0/openaerialmap-900913/#Z/#X/#Y.jpg";
        case OSM_GPS_MAP_SOURCE_MAPS_FOR_FREE:
            return "http://maps-for-free.com/layer/relief/z#Z/row#Y/#Z_#X-#Y.jpg";
        case OSM_GPS_MAP_SOURCE_GOOGLE_STREET:
            return "http://mt#R.google.com/vt/v=w2.97&x=#X&y=#Y&z=#Z";
        case OSM_GPS_MAP_SOURCE_GOOGLE_SATELLITE:
            return "http://khm#R.google.com/kh?n=404&v=3&t=#Q";
        case OSM_GPS_MAP_SOURCE_GOOGLE_HYBRID:
            return NULL; /* No longer working  "http://mt#R.google.com/mt?n=404&v=w2t.99&x=#X&y=#Y&zoom=#S" */
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_STREET:
            return "http://a#R.ortho.tiles.virtualearth.net/tiles/r#W.jpeg?g=50";
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_SATELLITE:
            return "http://a#R.ortho.tiles.virtualearth.net/tiles/a#W.jpeg?g=50";
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_HYBRID:
            return "http://a#R.ortho.tiles.virtualearth.net/tiles/h#W.jpeg?g=50";
        case OSM_GPS_MAP_SOURCE_YAHOO_STREET:
        case OSM_GPS_MAP_SOURCE_YAHOO_SATELLITE:
        case OSM_GPS_MAP_SOURCE_YAHOO_HYBRID:
            /* TODO: Implement signed Y, aka U
             * http://us.maps3.yimg.com/aerial.maps.yimg.com/ximg?v=1.7&t=a&s=256&x=%d&y=%-d&z=%d 
             *  x = tilex,
             *  y = (1 << (MAX_ZOOM - zoom)) - tiley - 1,
             *  z = zoom - (MAX_ZOOM - 17));
             */
            return NULL;
        case OSM_GPS_MAP_SOURCE_OVIMAPS_NORMAL:
            return "http://maptile.maps.svc.ovi.com/maptiler/v2/maptile/newest/normal.day/#Z/#X/#Y/256/png8";
        case OSM_GPS_MAP_SOURCE_OVIMAPS_HYBRID:
            return "http://maptile.maps.svc.ovi.com/maptiler/v2/maptile/newest/hybrid.day/#Z/#X/#Y/256/png8";
        case OSM_GPS_MAP_SOURCE_OVIMAPS_TRANSIT:
            return "http://maptile.maps.svc.ovi.com/maptiler/v2/maptile/newest/normal.day.transit/#Z/#X/#Y/256/png8";
        default:
            return NULL;
    }
    return NULL;
}

const char *
osm_gps_map_source_get_image_format(OsmGpsMapSource_t source)
{
    switch(source) {
        case OSM_GPS_MAP_SOURCE_NULL:
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP:
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP_RENDERER:
        case OSM_GPS_MAP_SOURCE_OVIMAPS_NORMAL:
        case OSM_GPS_MAP_SOURCE_OVIMAPS_HYBRID:
        case OSM_GPS_MAP_SOURCE_OVIMAPS_TRANSIT:
            return "png";
        case OSM_GPS_MAP_SOURCE_OPENAERIALMAP:
        case OSM_GPS_MAP_SOURCE_GOOGLE_STREET:
        case OSM_GPS_MAP_SOURCE_GOOGLE_HYBRID:
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_STREET:
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_SATELLITE:
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_HYBRID:
        case OSM_GPS_MAP_SOURCE_YAHOO_STREET:
        case OSM_GPS_MAP_SOURCE_YAHOO_SATELLITE:
        case OSM_GPS_MAP_SOURCE_YAHOO_HYBRID:
        case OSM_GPS_MAP_SOURCE_MAPS_FOR_FREE:
        case OSM_GPS_MAP_SOURCE_GOOGLE_SATELLITE:
            return "jpg";
        default:
            return "bin";
    }
    return "bin";
}


int 
osm_gps_map_source_get_min_zoom(OsmGpsMapSource_t source)
{
    return 1;
}

int 
osm_gps_map_source_get_max_zoom(OsmGpsMapSource_t source)
{
    switch(source) {
        case OSM_GPS_MAP_SOURCE_NULL:
            return 18;
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP:
            return OSM_MAX_ZOOM;
        case OSM_GPS_MAP_SOURCE_OPENSTREETMAP_RENDERER:
        case OSM_GPS_MAP_SOURCE_OPENAERIALMAP:
        case OSM_GPS_MAP_SOURCE_GOOGLE_STREET:
        case OSM_GPS_MAP_SOURCE_GOOGLE_HYBRID:
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_STREET:
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_SATELLITE:
        case OSM_GPS_MAP_SOURCE_OVIMAPS_NORMAL:
        case OSM_GPS_MAP_SOURCE_OVIMAPS_HYBRID:
        case OSM_GPS_MAP_SOURCE_OVIMAPS_TRANSIT:
	  return 19;
        case OSM_GPS_MAP_SOURCE_VIRTUAL_EARTH_HYBRID:
	  return 19;
        case OSM_GPS_MAP_SOURCE_YAHOO_STREET:
        case OSM_GPS_MAP_SOURCE_YAHOO_SATELLITE:
        case OSM_GPS_MAP_SOURCE_YAHOO_HYBRID:
            return 17;
        case OSM_GPS_MAP_SOURCE_MAPS_FOR_FREE:
            return 11;
        case OSM_GPS_MAP_SOURCE_GOOGLE_SATELLITE:
            return 18;
        default:
            return 17;
    }
    return 17;
}

void
osm_gps_map_download_maps (OsmGpsMap *map, coord_t *pt1, coord_t *pt2, int zoom_start, int zoom_end)
{
    int i,j,zoom,num_tiles;
    OsmGpsMapPrivate *priv = map->priv;

    if (pt1 && pt2)
    {
        gchar *filename;
        num_tiles = 0;
        zoom_end = CLAMP(zoom_end, priv->min_zoom, priv->max_zoom);
        g_debug("Download maps: z:%d->%d",zoom_start, zoom_end);

        for(zoom=zoom_start; zoom<=zoom_end; zoom++)
        {
            int x1,y1,x2,y2;

            x1 = (int)floor((float)lon2pixel(zoom, pt1->rlon) / (float)TILESIZE);
            y1 = (int)floor((float)lat2pixel(zoom, pt1->rlat) / (float)TILESIZE);

            x2 = (int)floor((float)lon2pixel(zoom, pt2->rlon) / (float)TILESIZE);
            y2 = (int)floor((float)lat2pixel(zoom, pt2->rlat) / (float)TILESIZE);

            // loop x1-x2
            for(i=x1; i<=x2; i++)
            {
                // loop y1 - y2
                for(j=y1; j<=y2; j++)
                {
                    // x = i, y = j
                    filename = g_strdup_printf("%s%c%d%c%d%c%d.%s",
                                    priv->cache_dir, G_DIR_SEPARATOR,
                                    zoom, G_DIR_SEPARATOR,
                                    i, G_DIR_SEPARATOR,
                                    j,
                                    priv->image_format);
                    if (!g_file_test(filename, G_FILE_TEST_EXISTS))
                    {
                        osm_gps_map_download_tile(map, zoom, i, j, FALSE);
                        num_tiles++;
                    }
                    g_free(filename);
                }
            }
            g_debug("DL @Z:%d = %d tiles",zoom,num_tiles);
        }
    }
}

void
osm_gps_map_get_bbox (OsmGpsMap *map, coord_t *pt1, coord_t *pt2)
{
    OsmGpsMapPrivate *priv = map->priv;

    if (pt1 && pt2) {
        pt1->rlat = pixel2lat(priv->map_zoom, priv->map_y);
        pt1->rlon = pixel2lon(priv->map_zoom, priv->map_x);
        pt2->rlat = pixel2lat(priv->map_zoom, priv->map_y + GTK_WIDGET(map)->allocation.height);
        pt2->rlon = pixel2lon(priv->map_zoom, priv->map_x + GTK_WIDGET(map)->allocation.width);

        g_debug("BBOX: %f %f %f %f", pt1->rlat, pt1->rlon, pt2->rlat, pt2->rlon);
    }
}

void
osm_gps_map_set_mapcenter (OsmGpsMap *map, float latitude, float longitude, int zoom)
{
    osm_gps_map_set_center (map, latitude, longitude);
    osm_gps_map_set_zoom (map, zoom);
}

void
osm_gps_map_set_center (OsmGpsMap *map, float latitude, float longitude)
{
    int pixel_x, pixel_y;
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    priv->center_rlat = deg2rad(latitude);
    priv->center_rlon = deg2rad(longitude);
    priv->center_coord_set = TRUE;

    // pixel_x,y, offsets
    pixel_x = lon2pixel(priv->map_zoom, priv->center_rlon);
    pixel_y = lat2pixel(priv->map_zoom, priv->center_rlat);

    priv->map_x = pixel_x - GTK_WIDGET(map)->allocation.width/2;
    priv->map_y = pixel_y - GTK_WIDGET(map)->allocation.height/2;

    osm_gps_map_map_redraw_idle(map);
}

int 
osm_gps_map_set_zoom (OsmGpsMap *map, int zoom)
{
    int zoom_old;
    double factor;
    int width_center, height_center;
    OsmGpsMapPrivate *priv;

    g_return_val_if_fail (OSM_IS_GPS_MAP (map), 0);
    priv = map->priv;

    if (zoom != priv->map_zoom)
    {
        width_center  = GTK_WIDGET(map)->allocation.width / 2;
        height_center = GTK_WIDGET(map)->allocation.height / 2;

        zoom_old = priv->map_zoom;
        //constrain zoom min_zoom -> max_zoom
        priv->map_zoom = CLAMP(zoom, priv->min_zoom, priv->max_zoom);

        if (priv->center_coord_set)
        {
            priv->map_x = lon2pixel(priv->map_zoom, priv->center_rlon) - width_center;
            priv->map_y = lat2pixel(priv->map_zoom, priv->center_rlat) - height_center;
        }
        else
        {
            factor = exp(priv->map_zoom * M_LN2)/exp(zoom_old * M_LN2);
            priv->map_x = ((priv->map_x + width_center) * factor) - width_center;
            priv->map_y = ((priv->map_y + height_center) * factor) - height_center;
        }

        g_debug("Zoom changed from %d to %d factor:%f x:%d",
                zoom_old, priv->map_zoom, factor, priv->map_x);

        osm_gps_map_map_redraw_idle(map);
    }
    return priv->map_zoom;
}

void
osm_gps_map_add_track (OsmGpsMap *map, GSList *track)
{
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    if (track) {
        priv->tracks = g_slist_append(priv->tracks, track);
        osm_gps_map_map_redraw_idle(map);
    }
}

void
osm_gps_map_clear_tracks (OsmGpsMap *map)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));

    osm_gps_map_free_tracks(map);
    osm_gps_map_map_redraw_idle(map);
}
void
osm_gps_map_add_button(OsmGpsMap *map,int x, int y, GdkPixbuf *image)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));

    if (image) {
        OsmGpsMapPrivate *priv = map->priv;
        button_t *bt;
	priv->draw_buttons = TRUE;
        //cache w/h for speed, and add image to list
        bt = g_new0(button_t,1);
        bt->w = gdk_pixbuf_get_width(image);
        bt->h = gdk_pixbuf_get_height(image);
        bt->x = x;
	bt->y = y;
        g_object_ref(image);
        bt->image = image;

        priv->buttons = g_slist_append(priv->buttons, bt);

        osm_gps_map_map_redraw_idle(map);
    }
    
}

void
osm_gps_map_add_image (OsmGpsMap *map, float latitude, float longitude, GdkPixbuf *image)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));

    if (image) {
        OsmGpsMapPrivate *priv = map->priv;
        image_t *im;

        //cache w/h for speed, and add image to list
        im = g_new0(image_t,1);
        im->w = gdk_pixbuf_get_width(image);
        im->h = gdk_pixbuf_get_height(image);
        im->pt.rlat = deg2rad(latitude);
        im->pt.rlon = deg2rad(longitude);

        g_object_ref(image);
        im->image = image;

        priv->images = g_slist_append(priv->images, im);

        osm_gps_map_map_redraw_idle(map);
    }
}

gboolean
osm_gps_map_remove_image (OsmGpsMap *map, GdkPixbuf *image)
{
    OsmGpsMapPrivate *priv = map->priv;
    if (priv->images) {
        GSList *list;
        for(list = priv->images; list != NULL; list = list->next)
        {
            image_t *im = list->data;
	        if (im->image == image)
	        {
		        priv->images = g_slist_remove_link(priv->images, list);
		        g_object_unref(im->image);
		        g_free(im);
		        osm_gps_map_map_redraw_idle(map);
		        return TRUE;
	        }
        }
    }
    return FALSE;
}

void
osm_gps_map_clear_images (OsmGpsMap *map)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));

    osm_gps_map_free_images(map);
    osm_gps_map_map_redraw_idle(map);
}

void
osm_gps_map_osd_speed (OsmGpsMap *map, float speed)
{
    OsmGpsMapPrivate *priv;

    PangoContext        *context = NULL;
    PangoLayout     *layout  = NULL;
    PangoFontDescription    *desc    = NULL;

    GdkColor color;
    GdkGC *gc;

    gchar *buffer;
    //static int x = 10, y = 10;
    static int width = 0, height = 0;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    buffer = g_strdup_printf("%.0f", speed);

    /* pango initialisation */
    context = gtk_widget_get_pango_context (GTK_WIDGET(map));
    layout  = pango_layout_new (context);
    desc    = pango_font_description_new();

    pango_font_description_set_size (desc, 40 * PANGO_SCALE);
    pango_layout_set_font_description (layout, desc);
    pango_layout_set_text (layout, buffer, strlen(buffer));

    gc = gdk_gc_new (GTK_WIDGET(map)->window);

    color.red = (0 > 50) ? 0xffff : 0;
    color.green = 0;
    color.blue = 0;

    gdk_gc_set_rgb_fg_color (gc, &color);

    /* faster / less flicker alternative:*/
    gdk_draw_drawable (
                       GTK_WIDGET(map)->window,
                       GTK_WIDGET(map)->style->fg_gc[GTK_WIDGET_STATE(map)],
                       priv->pixmap,
                       0,0,
                       0,0,
                       width+10,width+10);

    gdk_draw_layout(GTK_WIDGET(map)->window,
                    gc,
                    0, 0,
                    layout);

    /* set width and height */
    pango_layout_get_pixel_size(layout, &width, &height);

    g_free(buffer);
    pango_font_description_free (desc);
    g_object_unref (layout);
    g_object_unref (gc);
}

void
osm_gps_map_draw_gps (OsmGpsMap *map, float latitude, float longitude, float heading)
{
    int pixel_x, pixel_y;
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    priv->gps->rlat = deg2rad(latitude);
    priv->gps->rlon = deg2rad(longitude);
    priv->gps_valid = TRUE;

    // pixel_x,y, offsets
    pixel_x = lon2pixel(priv->map_zoom, priv->gps->rlon);
    pixel_y = lat2pixel(priv->map_zoom, priv->gps->rlat);

    //If trip marker add to list of gps points.
    if (priv->record_trip_history) {
        coord_t *tp = g_new0(coord_t,1);
        tp->rlat = priv->gps->rlat;
        tp->rlon = priv->gps->rlon;
        priv->trip_history = g_slist_append(priv->trip_history, tp);
    }

    // dont draw anything if we are dragging
    if (priv->dragging) {
        g_debug("Dragging");
        return;
    }

    //Automatically center the map if the track approaches the edge
    if(priv->map_auto_center)   {
        int x = pixel_x - priv->map_x;
        int y = pixel_y - priv->map_y;
        int width = GTK_WIDGET(map)->allocation.width;
        int height = GTK_WIDGET(map)->allocation.height;
        if( x < (width/2 - width/8)     || x > (width/2 + width/8)  ||
            y < (height/2 - height/8)   || y > (height/2 + height/8)) {

            priv->map_x = pixel_x - GTK_WIDGET(map)->allocation.width/2;
            priv->map_y = pixel_y - GTK_WIDGET(map)->allocation.height/2;
            priv->center_coord_set = FALSE;
        }
    }

    // this redraws the map (including the gps track, and adjusts the
    // map center if it was changed
    osm_gps_map_map_redraw_idle(map);
}

void
osm_gps_map_clear_gps (OsmGpsMap *map)
{
    osm_gps_map_free_trip(map);
    osm_gps_map_map_redraw_idle(map);
}

coord_t
osm_gps_map_get_co_ordinates (OsmGpsMap *map, int pixel_x, int pixel_y)
{
    coord_t coord;
    OsmGpsMapPrivate *priv = map->priv;

    coord.rlat = pixel2lat(priv->map_zoom, priv->map_y + pixel_y);
    coord.rlon = pixel2lon(priv->map_zoom, priv->map_x + pixel_x);
    return coord;
}

GtkWidget *
osm_gps_map_new (void)
{
    return g_object_new (OSM_TYPE_GPS_MAP, NULL);
}

void
osm_gps_map_screen_to_geographic (OsmGpsMap *map, gint pixel_x, gint pixel_y,
                                  gfloat *latitude, gfloat *longitude)
{
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    if (latitude)
        *latitude = rad2deg(pixel2lat(priv->map_zoom, priv->map_y + pixel_y));
    if (longitude)
        *longitude = rad2deg(pixel2lon(priv->map_zoom, priv->map_x + pixel_x));
}

void
osm_gps_map_geographic_to_screen (OsmGpsMap *map,
                                  gfloat latitude, gfloat longitude,
                                  gint *pixel_x, gint *pixel_y)
{
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    if (pixel_x)
        *pixel_x = lon2pixel(priv->map_zoom, deg2rad(longitude)) - priv->map_x;
    if (pixel_y)
        *pixel_y = lat2pixel(priv->map_zoom, deg2rad(latitude)) - priv->map_y;
}

void
osm_gps_map_scroll (OsmGpsMap *map, gint dx, gint dy)
{
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    priv->center_coord_set = FALSE;
    priv->map_x += dx;
    priv->map_y += dy;

    osm_gps_map_map_redraw_idle (map);
}

float
osm_gps_map_get_scale(OsmGpsMap *map)
{
    OsmGpsMapPrivate *priv;

    g_return_if_fail (OSM_IS_GPS_MAP (map));
    priv = map->priv;

    return osm_gps_map_get_scale_at_point(priv->map_zoom, priv->center_rlat, priv->center_rlon);
}
void osm_gps_map_hide_buttons(OsmGpsMap *map)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));

    OsmGpsMapPrivate *priv = map->priv;
    priv->draw_buttons = FALSE;
  
}
void osm_gps_map_show_buttons(OsmGpsMap *map)
{
    g_return_if_fail (OSM_IS_GPS_MAP (map));

    OsmGpsMapPrivate *priv = map->priv;
    priv->draw_buttons = TRUE;
  
}
/* remove button by x,y */
void
osm_gps_map_remove_button (OsmGpsMap *map,int x, int y)
{
    OsmGpsMapPrivate *priv = map->priv;
    if (priv->buttons) {
        GSList *list;
        for(list = priv->buttons; list != NULL; list = list->next)
        {
	   button_t *im = list->data;
           if(im->x == x && im->y ==y)
	   {
	    priv->buttons = g_slist_remove_link(priv->buttons, list);
            g_object_unref(im->image);
            g_free(im);
	   }
        }
  
    }
}
