
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include <string.h>

#define THUMB_WIDTH 480
#define THUMB_HEIGHT 300
#define MAX_CACHED_THUMBS 7

typedef struct {
    gchar *path;
    gchar *name;
    cairo_surface_t *thumbnail;
    guint64 last_used;
    gboolean loading;
} Wallpaper;

typedef struct {
    GtkWidget *window;
    GtkWidget *area;

    GPtrArray *wallpapers;
    GArray *pending_indices;

    gchar *folder;
    gchar *status;

    guint selected;
    guint cache_count;
    guint64 cache_clock;

    guint pending_pos;
    guint load_source_id;
} Carousel;


/* ---------- GESTIÓN DE ARCHIVOS ---------- */

static void wallpaper_free(gpointer data)
{
    Wallpaper *w = data;

    if (w->thumbnail)
        cairo_surface_destroy(w->thumbnail);

    g_free(w->path);
    g_free(w->name);
    g_free(w);
}

static gboolean supported_image(const gchar *name)
{
    gchar *lower = g_ascii_strdown(name, -1);

    gboolean result =
        g_str_has_suffix(lower, ".jpg")  ||
        g_str_has_suffix(lower, ".jpeg") ||
        g_str_has_suffix(lower, ".png")  ||
        g_str_has_suffix(lower, ".webp") ||
        g_str_has_suffix(lower, ".bmp")  ||
        g_str_has_suffix(lower, ".gif");

    g_free(lower);
    return result;
}

static gint compare_names(gconstpointer a, gconstpointer b)
{
    const gchar *name_a = *(gchar * const *)a;
    const gchar *name_b = *(gchar * const *)b;

    return g_strcmp0(name_a, name_b);
}

static GPtrArray *load_wallpapers(const gchar *folder)
{
    GPtrArray *result =
        g_ptr_array_new_with_free_func(wallpaper_free);

    GPtrArray *names =
        g_ptr_array_new_with_free_func(g_free);

    GError *error = NULL;
    GDir *dir = g_dir_open(folder, 0, &error);

    if (!dir) {
        g_warning("No se pudo abrir %s: %s",
                  folder, error->message);

        g_clear_error(&error);
        g_ptr_array_unref(names);
        return result;
    }

    const gchar *name;

    while ((name = g_dir_read_name(dir))) {
        if (!supported_image(name))
            continue;

        gchar *path =
            g_build_filename(folder, name, NULL);

        if (g_file_test(path, G_FILE_TEST_IS_REGULAR))
            g_ptr_array_add(names, g_strdup(name));

        g_free(path);
    }

    g_dir_close(dir);

    g_ptr_array_sort(names, compare_names);

    for (guint i = 0; i < names->len; i++) {
        const gchar *filename =
            g_ptr_array_index(names, i);

        Wallpaper *w = g_new0(Wallpaper, 1);

        w->name = g_strdup(filename);
        w->path = g_build_filename(
            folder, filename, NULL
        );

        g_ptr_array_add(result, w);
    }

    g_ptr_array_unref(names);

    return result;
}


/* ---------- CONVERSIÓN DE IMÁGENES ---------- */

/*
 * Convierte GdkPixbuf a Cairo ARGB32.
 * Así evitamos gdk_cairo_set_source_pixbuf(),
 * que está obsoleto en GTK4.
 */

static cairo_surface_t *pixbuf_to_surface(GdkPixbuf *pixbuf)
{
    if (gdk_pixbuf_get_bits_per_sample(pixbuf) != 8)
        return NULL;

    gint width = gdk_pixbuf_get_width(pixbuf);
    gint height = gdk_pixbuf_get_height(pixbuf);

    gint channels = gdk_pixbuf_get_n_channels(pixbuf);
    gint source_stride = gdk_pixbuf_get_rowstride(pixbuf);

    gboolean has_alpha =
        gdk_pixbuf_get_has_alpha(pixbuf);

    if (channels < 3)
        return NULL;

    cairo_surface_t *surface =
        cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32,
            width,
            height
        );

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }

    cairo_surface_flush(surface);

    const guchar *src = gdk_pixbuf_get_pixels(pixbuf);
    guchar *dst = cairo_image_surface_get_data(surface);

    gint destination_stride =
        cairo_image_surface_get_stride(surface);

    for (gint y = 0; y < height; y++) {
        const guchar *source_row =
            src + y * source_stride;

        guchar *destination_row =
            dst + y * destination_stride;

        for (gint x = 0; x < width; x++) {
            const guchar *pixel =
                source_row + x * channels;

            guint32 alpha =
                has_alpha ? pixel[3] : 255;

            guint32 red =
                (pixel[0] * alpha + 127) / 255;

            guint32 green =
                (pixel[1] * alpha + 127) / 255;

            guint32 blue =
                (pixel[2] * alpha + 127) / 255;

            guint32 argb =
                (alpha << 24) |
                (red << 16) |
                (green << 8) |
                blue;

            memcpy(
                destination_row + x * 4,
                &argb,
                sizeof(argb)
            );
        }
    }

    cairo_surface_mark_dirty(surface);

    return surface;
}


/* ---------- CACHÉ DE MINIATURAS ---------- */

static gboolean index_is_visible(
    Carousel *c,
    guint index
) {
    gint count = (gint)c->wallpapers->len;

    if (count == 0)
        return FALSE;

    for (gint offset = -2; offset <= 2; offset++) {
        gint candidate =
            ((gint)c->selected + offset + count) % count;

        if (candidate == (gint)index)
            return TRUE;
    }

    return FALSE;
}

static gboolean load_next_thumbnail(gpointer data)
{
    Carousel *c = data;

    if (c->pending_pos >= c->pending_indices->len) {
        c->load_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    guint index = g_array_index(
        c->pending_indices,
        guint,
        c->pending_pos++
    );

    Wallpaper *w = g_ptr_array_index(
        c->wallpapers,
        index
    );

    GError *error = NULL;

    GdkPixbuf *pixbuf =
        gdk_pixbuf_new_from_file_at_scale(
            w->path,
            THUMB_WIDTH,
            THUMB_HEIGHT,
            TRUE,
            &error
        );

    w->loading = FALSE;

    if (!pixbuf) {
        g_warning(
            "Error cargando %s: %s",
            w->name,
            error->message
        );

        g_clear_error(&error);
    } else {
        cairo_surface_t *surface =
            pixbuf_to_surface(pixbuf);

        g_object_unref(pixbuf);

        if (surface) {
            w->thumbnail = surface;
            w->last_used = ++c->cache_clock;
            c->cache_count++;
        }
    }

    gtk_widget_queue_draw(c->area);

    if (c->pending_pos >= c->pending_indices->len) {
        c->load_source_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void request_visible_thumbnails(Carousel *c)
{
    if (c->wallpapers->len == 0)
        return;

    if (c->load_source_id) {
        g_source_remove(c->load_source_id);
        c->load_source_id = 0;
    }

    g_array_set_size(c->pending_indices, 0);
    c->pending_pos = 0;

    for (guint i = 0; i < c->wallpapers->len; i++) {
        Wallpaper *w = g_ptr_array_index(c->wallpapers, i);
        w->loading = FALSE;

        if (index_is_visible(c, i) && w->thumbnail)
            w->last_used = ++c->cache_clock;
    }

    /* Conservamos como máximo siete miniaturas. */
    while (c->cache_count > MAX_CACHED_THUMBS) {
        Wallpaper *victim = NULL;

        for (guint i = 0; i < c->wallpapers->len; i++) {
            Wallpaper *w = g_ptr_array_index(
                c->wallpapers, i
            );

            if (!w->thumbnail || index_is_visible(c, i))
                continue;

            if (!victim || w->last_used < victim->last_used)
                victim = w;
        }

        if (!victim)
            break;

        cairo_surface_destroy(victim->thumbnail);
        victim->thumbnail = NULL;
        c->cache_count--;
    }

    /* Solicitamos únicamente las miniaturas visibles. */
    for (guint i = 0; i < c->wallpapers->len; i++) {
        Wallpaper *w = g_ptr_array_index(
            c->wallpapers, i
        );

        if (index_is_visible(c, i) && !w->thumbnail) {
            w->loading = TRUE;
            g_array_append_val(c->pending_indices, i);
        }
    }

    if (c->pending_indices->len > 0) {
        c->load_source_id = g_timeout_add(
            16,
            load_next_thumbnail,
            c
        );
    }

    gtk_widget_queue_draw(c->area);
}


/* ---------- DIBUJO DEL CARRUSEL ---------- */

static void rounded_rectangle(
    cairo_t *cr,
    double x,
    double y,
    double width,
    double height,
    double radius
) {
    double r = MIN(radius, MIN(width, height) / 2.0);

    cairo_new_sub_path(cr);

    cairo_move_to(cr, x + r, y);

    cairo_line_to(cr, x + width - r, y);
    cairo_arc(cr, x + width - r, y + r,
              r, -G_PI / 2, 0);

    cairo_line_to(cr, x + width, y + height - r);
    cairo_arc(cr, x + width - r, y + height - r,
              r, 0, G_PI / 2);

    cairo_line_to(cr, x + r, y + height);
    cairo_arc(cr, x + r, y + height - r,
              r, G_PI / 2, G_PI);

    cairo_line_to(cr, x, y + r);
    cairo_arc(cr, x + r, y + r,
              r, G_PI, 3 * G_PI / 2);

    cairo_close_path(cr);
}

static void draw_centered_text(
    cairo_t *cr,
    const gchar *text,
    double center_x,
    double baseline,
    double font_size
) {
    cairo_select_font_face(
        cr,
        "Sans",
        CAIRO_FONT_SLANT_NORMAL,
        CAIRO_FONT_WEIGHT_BOLD
    );

    cairo_set_font_size(cr, font_size);

    cairo_text_extents_t extents;
    cairo_text_extents(cr, text, &extents);

    double x =
        center_x - extents.width / 2.0 - extents.x_bearing;

    cairo_move_to(cr, x, baseline);
    cairo_show_text(cr, text);
}

static void draw_card(
    cairo_t *cr,
    Wallpaper *w,
    double center_x,
    double center_y,
    double width,
    double height,
    gboolean selected,
    double opacity
) {
    double x = center_x - width / 2.0;
    double y = center_y - height / 2.0;

    cairo_save(cr);

    rounded_rectangle(cr, x, y, width, height, 15);

    cairo_set_source_rgba(cr, 0.05, 0.09, 0.15, opacity);
    cairo_fill(cr);

    if (w->thumbnail) {
        double image_width =
            cairo_image_surface_get_width(w->thumbnail);

        double image_height =
            cairo_image_surface_get_height(w->thumbnail);

        double scale = MIN(
            width / image_width,
            height / image_height
        );

        double image_x =
            center_x - image_width * scale / 2.0;

        double image_y =
            center_y - image_height * scale / 2.0;

        cairo_translate(cr, image_x, image_y);
        cairo_scale(cr, scale, scale);

        cairo_set_source_surface(
            cr,
            w->thumbnail,
            0,
            0
        );

        cairo_pattern_set_filter(
            cairo_get_source(cr),
            CAIRO_FILTER_BILINEAR
        );

        cairo_paint_with_alpha(cr, opacity);
    }

    cairo_restore(cr);

    rounded_rectangle(cr, x, y, width, height, 15);

    if (selected) {
        cairo_set_source_rgba(cr, 0.0, 0.85, 1.0, 0.95);
        cairo_set_line_width(cr, 3.0);
    } else {
        cairo_set_source_rgba(cr, 0.25, 0.55, 0.75, opacity);
        cairo_set_line_width(cr, 1.2);
    }

    cairo_stroke(cr);
}


/* ---------- INTERFAZ ---------- */

static void draw_carousel(
    GtkDrawingArea *area,
    cairo_t *cr,
    int width,
    int height,
    gpointer data
) {
    (void)area;

    Carousel *c = data;

    cairo_set_source_rgb(cr, 0.012, 0.020, 0.045);
    cairo_paint(cr);

    double center_x = width / 2.0;
    double center_y = height / 2.0;

    cairo_set_source_rgb(cr, 0.0, 0.85, 1.0);

    draw_centered_text(
        cr,
        "WALLPAPER SELECTOR",
        center_x,
        48,
        23
    );

    if (c->wallpapers->len == 0) {
        cairo_set_source_rgb(cr, 0.85, 0.88, 0.95);

        draw_centered_text(
            cr,
            "NO SE ENCONTRARON IMAGENES",
            center_x,
            center_y,
            19
        );

        draw_centered_text(
            cr,
            c->folder,
            center_x,
            center_y + 30,
            12
        );

        return;
    }

    /*
     * Dibujamos primero las tarjetas lejanas,
     * después las cercanas y finalmente la central.
     */

    const gint order[] = { -2, 2, -1, 1, 0 };

    for (guint i = 0; i < G_N_ELEMENTS(order); i++) {
        gint offset = order[i];

        gint count = (gint)c->wallpapers->len;

        gint index =
            ((gint)c->selected + offset + count) % count;

        Wallpaper *w = g_ptr_array_index(
            c->wallpapers,
            index
        );

        double card_x;
        double card_width;
        double card_height;
        double opacity;

        if (offset == 0) {
            card_x = center_x;
            card_width = 420;
            card_height = 285;
            opacity = 1.0;
        } else if (offset == -1 || offset == 1) {
            card_x = center_x + offset * 245.0;
            card_width = 270;
            card_height = 190;
            opacity = 0.80;
        } else {
            card_x = center_x + offset * 210.0;
            card_width = 170;
            card_height = 120;
            opacity = 0.50;
        }

        draw_card(
            cr,
            w,
            card_x,
            center_y,
            card_width,
            card_height,
            offset == 0,
            opacity
        );
    }

    Wallpaper *current = g_ptr_array_index(
        c->wallpapers,
        c->selected
    );

    gchar *display_name;

    if (g_utf8_strlen(current->name, -1) > 48) {
        gchar *prefix = g_utf8_substring(
            current->name, 0, 45
        );

        display_name = g_strconcat(prefix, "...", NULL);
        g_free(prefix);
    } else {
        display_name = g_strdup(current->name);
    }

    cairo_set_source_rgb(cr, 0.88, 0.93, 1.0);

    draw_centered_text(
        cr,
        display_name,
        center_x,
        center_y + 180,
        14
    );

    g_free(display_name);

    gchar *counter = g_strdup_printf(
        "%u / %u",
        c->selected + 1,
        c->wallpapers->len
    );

    cairo_set_source_rgb(cr, 0.0, 0.85, 1.0);

    draw_centered_text(
        cr,
        counter,
        center_x,
        center_y + 205,
        12
    );

    g_free(counter);

    if (c->status) {
        cairo_set_source_rgb(cr, 0.60, 0.70, 0.82);

        draw_centered_text(
            cr,
            c->status,
            center_x,
            height - 48,
            11
        );
    }

    cairo_set_source_rgb(cr, 0.0, 0.85, 1.0);

    draw_centered_text(
        cr,
        "←  →  NAVEGAR       ENTER  APLICAR       ESC  SALIR",
        center_x,
        height - 22,
        12
    );
}


/* ---------- APLICAR WALLPAPER ---------- */

static void set_status(Carousel *c, gchar *message)
{
    g_free(c->status);
    c->status = message;

    gtk_widget_queue_draw(c->area);
}

static void apply_selected(Carousel *c)
{
    if (c->wallpapers->len == 0)
        return;

    Wallpaper *w = g_ptr_array_index(
        c->wallpapers,
        c->selected
    );

    gchar *argv[] = {
        "awww",
        "img",
        "--transition-type",
        "fade",
        "--transition-duration",
        "1",
        w->path,
        NULL
    };

    GError *error = NULL;

    gboolean spawned = g_spawn_async(
        NULL,
        argv,
        NULL,
        G_SPAWN_SEARCH_PATH,
        NULL,
        NULL,
        NULL,
        &error
    );

    if (!spawned) {
        set_status(
            c,
            g_strdup_printf(
                "Error iniciando awww: %s",
                error->message
            )
        );

        g_clear_error(&error);
        return;
    }

    set_status(
        c,
        g_strdup_printf(
            "Comando enviado a awww: %s",
            w->name
        )
    );
}


/* ---------- TECLADO ---------- */

static gboolean on_key_pressed(
    GtkEventControllerKey *controller,
    guint keyval,
    guint keycode,
    GdkModifierType state,
    gpointer data
) {
    (void)controller;
    (void)keycode;
    (void)state;

    Carousel *c = data;

    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(GTK_WINDOW(c->window));
        return TRUE;
    }

    if (c->wallpapers->len == 0)
        return FALSE;

    if (keyval == GDK_KEY_Right) {
        c->selected =
            (c->selected + 1) % c->wallpapers->len;

        request_visible_thumbnails(c);
        return TRUE;
    }

    if (keyval == GDK_KEY_Left) {
        c->selected =
            (c->selected + c->wallpapers->len - 1)
            % c->wallpapers->len;

        request_visible_thumbnails(c);
        return TRUE;
    }

    if (keyval == GDK_KEY_Return ||
        keyval == GDK_KEY_KP_Enter) {

        apply_selected(c);
        return TRUE;
    }

    return FALSE;
}


/* ---------- LIMPIEZA ---------- */

static void carousel_free(gpointer data)
{
    Carousel *c = data;

    if (c->load_source_id)
        g_source_remove(c->load_source_id);

    g_array_unref(c->pending_indices);
    g_ptr_array_unref(c->wallpapers);

    g_free(c->folder);
    g_free(c->status);
    g_free(c);
}


/* ---------- INICIO ---------- */

static void activate(
    GtkApplication *app,
    gpointer user_data
) {
    (void)user_data;

    Carousel *c = g_new0(Carousel, 1);

    c->folder = g_build_filename(
        g_get_home_dir(),
        "Pictures",
        "Wallpapers",
        NULL
    );

    c->wallpapers = load_wallpapers(c->folder);

    c->pending_indices = g_array_new(
        FALSE,
        FALSE,
        sizeof(guint)
    );

    c->status = g_strdup(
        "Flechas para navegar · Enter para aplicar"
    );

    GtkWidget *window =
        gtk_application_window_new(app);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);

    c->window = window;

    gtk_window_set_title(
        GTK_WINDOW(window),
        "Wallpaper Selector"
    );

    gtk_window_set_default_size(
        GTK_WINDOW(window),
        1100,
        600
    );

    GtkWidget *area = gtk_drawing_area_new();

    c->area = area;

    gtk_drawing_area_set_content_width(
        GTK_DRAWING_AREA(area),
        1000
    );

    gtk_drawing_area_set_content_height(
        GTK_DRAWING_AREA(area),
        560
    );

    gtk_widget_set_focusable(area, TRUE);

    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(area),
        draw_carousel,
        c,
        NULL
    );

    GtkEventController *keys =
        gtk_event_controller_key_new();

    g_signal_connect(
        keys,
        "key-pressed",
        G_CALLBACK(on_key_pressed),
        c
    );

    gtk_widget_add_controller(area, keys);

    gtk_window_set_child(
        GTK_WINDOW(window),
        area
    );

    g_object_set_data_full(
        G_OBJECT(window),
        "wallpaper-selector",
        c,
        carousel_free
    );
    GtkCssProvider *css = gtk_css_provider_new();

gtk_css_provider_load_from_data(
    css,
    "window.background {"
    " background: transparent;"
    " box-shadow: none;"
    "}"
    "drawingarea {"
    " background: transparent;"
    "}",
    -1
);

gtk_style_context_add_provider_for_display(
    gdk_display_get_default(),
    GTK_STYLE_PROVIDER(css),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
);

g_object_unref(css);

    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(area);

    request_visible_thumbnails(c);
}

int main(int argc, char **argv)
{
    GtkApplication *app =
        gtk_application_new(
            "com.wallpaper.selector",
            G_APPLICATION_DEFAULT_FLAGS
        );

    g_signal_connect(
        app,
        "activate",
        G_CALLBACK(activate),
        NULL
    );

    int status = g_application_run(
        G_APPLICATION(app),
        argc,
        argv
    );

    g_object_unref(app);

    return status;
}
