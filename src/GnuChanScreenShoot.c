/*
 * GnuChanScreenShoot.c — the GnuchanOS screenshot tool.
 *
 *     GnuChanScreenShoot          run: wait for Alt+P and Print/SysRq
 *     GnuChanScreenShoot --once   take one screenshot at once and exit
 *     GnuChanScreenShoot --settings   open the settings window and exit
 *     GnuChanScreenShoot --version  print the version and exit
 *
 * Two hotkeys, one job:
 *
 *   Alt+P        capture the focused window (or the whole screen) and write
 *                it to SaveDir, as named in the settings script.
 *   Print/SysRq  open an X11 window with the detailed settings: where
 *                screenshots go, which format, JPEG quality, whether Alt+P
 *                grabs the focused window or the whole screen.
 *
 * The settings live in a script at
 *   ~/.config/GnuChanScreenShoot/GnuChanScreenShoot.py
 * the same way every other GnuChan program keeps its settings, and
 * GnuChanSettings edits that same file.
 *
 * A screenshot is written as a PNG or JPEG with a timestamp in the name so
 * nothing is ever overwritten.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <setjmp.h>
#include <stdint.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <jpeglib.h>
#include <png.h>

#include "screenshot_config.h"

#define GNUCHANSCREENSHOOT_VERSION "0.1.0"

/* How long the settings window waits before closing itself when idle. */
#define SS_SETTINGS_IDLE_MS 30000

static volatile sig_atomic_t s_should_stop = 0;

static void handle_signal(int signum) {
    (void)signum;
    s_should_stop = 1;
}

static int s_xerror = 0;

static int error_handler(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    s_xerror = ev->error_code;
    return 0;
}

/* --- paths and filenames -------------------------------------------------- */

/* Expand a leading ~ into $HOME. Everything else is passed through. */
static void expand_tilde(char *out, unsigned int size, const char *in) {
    if (in && in[0] == '~' && (!in[1] || in[1] == '/')) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            unsigned int n = (unsigned int)snprintf(out, size, "%s%s", home,
                                                    in[1] ? in + 1 : "");
            if (n >= size) {
                n = size - 1;
            }
            out[n] = '\0';
            return;
        }
    }
    snprintf(out, size, "%s", in ? in : "");
}

/* Make every directory along a path that does not exist yet. */
static int ensure_directory(const char *path) {
    char tmp[SS_TEXT_LENGTH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Build a timestamped filename with the selected format's extension. */
static void screenshot_filename(char *out, unsigned int size,
                                const char *save_dir, SsFormat format) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm);
    snprintf(out, size, "%s/screenshot-%s.%s", save_dir, stamp,
             format == SS_FORMAT_JPEG ? "jpg" : "png");
}

static unsigned char component(unsigned long pixel, unsigned long mask) {
    if (!mask) {
        return 0;
    }
    unsigned int shift = 0;
    while (((mask >> shift) & 1UL) == 0UL) {
        shift++;
    }
    unsigned long value = (pixel & mask) >> shift;
    unsigned long maximum = mask >> shift;
    return (unsigned char)((value * 255UL + maximum / 2UL) / maximum);
}

static unsigned char *image_rgb(XImage *image) {
    if (image->width <= 0 || image->height <= 0 ||
        (size_t)image->width > SIZE_MAX / 3U / (size_t)image->height) {
        return NULL;
    }
    size_t row_size = (size_t)image->width * 3U;
    unsigned char *pixels = malloc(row_size * (size_t)image->height);
    if (!pixels) {
        return NULL;
    }
    for (int y = 0; y < image->height; y++) {
        unsigned char *row = pixels + (size_t)y * row_size;
        for (int x = 0; x < image->width; x++) {
            unsigned long pixel = XGetPixel(image, x, y);
            row[(size_t)x * 3U] = component(pixel, image->red_mask);
            row[(size_t)x * 3U + 1U] = component(pixel, image->green_mask);
            row[(size_t)x * 3U + 2U] = component(pixel, image->blue_mask);
        }
    }
    return pixels;
}

static int write_png(const char *path, XImage *image) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                               NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    unsigned char *pixels = image_rgb(image);
    int ok = png && info && pixels;
    if (ok && setjmp(png_jmpbuf(png)) == 0) {
        png_init_io(png, f);
        png_set_IHDR(png, info, (png_uint_32)image->width,
                     (png_uint_32)image->height, 8, PNG_COLOR_TYPE_RGB,
                     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                     PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png, info);
        size_t row_size = (size_t)image->width * 3U;
        for (int y = 0; y < image->height; y++) {
            png_write_row(png, pixels + (size_t)y * row_size);
        }
        png_write_end(png, info);
    } else {
        ok = 0;
    }
    free(pixels);
    if (png) {
        png_destroy_write_struct(&png, info ? &info : NULL);
    }
    if (fclose(f) != 0) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

struct JpegError {
    struct jpeg_error_mgr base;
    jmp_buf jump;
};

static void jpeg_failure(j_common_ptr common) {
    struct JpegError *error = (struct JpegError *)common->err;
    longjmp(error->jump, 1);
}

static int write_jpeg(const char *path, XImage *image, int quality) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    unsigned char *pixels = image_rgb(image);
    if (!pixels) {
        fclose(f);
        return -1;
    }

    struct jpeg_compress_struct jpeg;
    struct JpegError error;
    jpeg.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_failure;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&jpeg);
        free(pixels);
        fclose(f);
        return -1;
    }
    jpeg_create_compress(&jpeg);
    jpeg_stdio_dest(&jpeg, f);
    jpeg.image_width = (JDIMENSION)image->width;
    jpeg.image_height = (JDIMENSION)image->height;
    jpeg.input_components = 3;
    jpeg.in_color_space = JCS_RGB;
    jpeg_set_defaults(&jpeg);
    jpeg_set_quality(&jpeg, quality, TRUE);
    jpeg_start_compress(&jpeg, TRUE);
    size_t row_size = (size_t)image->width * 3U;
    while (jpeg.next_scanline < jpeg.image_height) {
        JSAMPROW row = pixels + (size_t)jpeg.next_scanline * row_size;
        jpeg_write_scanlines(&jpeg, &row, 1);
    }
    jpeg_finish_compress(&jpeg);
    jpeg_destroy_compress(&jpeg);
    free(pixels);
    int ok = fclose(f) == 0;
    return ok ? 0 : -1;
}

/* --- capture -------------------------------------------------------------- */

/* Grab the focused window, or fall back to the root window. Returns the
   window and its geometry relative to the root. */
static Window focused_window(Display *display, int *x, int *y,
                             unsigned int *w, unsigned int *h,
                             unsigned int *border, unsigned int *depth) {
    Window root = DefaultRootWindow(display);
    Window focus = None;
    int revert = 0;
    Atom active_atom = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
    if (active_atom != None) {
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(display, root, active_atom, 0, 1, False,
                               XA_WINDOW, &actual_type, &actual_format,
                               &count, &remaining, &data) == Success &&
            data && actual_type == XA_WINDOW && actual_format == 32 &&
            count == 1) {
            focus = *(Window *)data;
        }
        if (data) {
            XFree(data);
        }
    }
    if (focus == None) {
        XGetInputFocus(display, &focus, &revert);
    }
    if (focus == None || focus == PointerRoot || focus == root) {
        focus = root;
        *x = 0;
        *y = 0;
        *w = (unsigned int)DisplayWidth(display, DefaultScreen(display));
        *h = (unsigned int)DisplayHeight(display, DefaultScreen(display));
        *border = 0;
        *depth = (unsigned int)DefaultDepth(display, DefaultScreen(display));
        return focus;
    }
    Window parent = None;
    Window *children = NULL;
    unsigned int nchildren = 0;
    while (XQueryTree(display, focus, &root, &parent, &children, &nchildren) &&
           parent != None && parent != root) {
        if (children) {
            XFree(children);
            children = NULL;
        }
        focus = parent;
    }
    if (children) {
        XFree(children);
    }
    if (focus == root ||
        !XGetGeometry(display, focus, &root, x, y, w, h, border, depth)) {
        *x = 0;
        *y = 0;
        *w = (unsigned int)DisplayWidth(display, DefaultScreen(display));
        *h = (unsigned int)DisplayHeight(display, DefaultScreen(display));
        *border = 0;
        *depth = (unsigned int)DefaultDepth(display, DefaultScreen(display));
        return root;
    }
    *x = 0;
    *y = 0;
    return focus;
}

/* Capture a window into an XImage. */
static XImage *capture_window(Display *display, Window window, int x, int y,
                              unsigned int w, unsigned int h) {
    return XGetImage(display, window, x, y, w, h, AllPlanes, ZPixmap);
}

/* Take one screenshot and write it. Returns 0 on success. */
static int take_screenshot(Display *display, const SsConfig *config) {
    char dir[SS_TEXT_LENGTH];
    expand_tilde(dir, sizeof(dir), config->save_dir);
    if (ensure_directory(dir) != 0) {
        fprintf(stderr,
                "gnuchanscreenshot: could not create save directory %s\n",
                dir);
        return -1;
    }

    int x = 0, y = 0;
    unsigned int w = 0, h = 0, border = 0, depth = 0;
    Window root = DefaultRootWindow(display);
    Window window = root;
    if (config->capture_focused) {
        window = focused_window(display, &x, &y, &w, &h, &border, &depth);
    }
    if (w == 0 || h == 0) {
        w = (unsigned int)DisplayWidth(display, DefaultScreen(display));
        h = (unsigned int)DisplayHeight(display, DefaultScreen(display));
    }
    if (window == root) {
        x = 0;
        y = 0;
    }

    XImage *image = capture_window(display, window, x, y, w, h);
    if (!image) {
        fprintf(stderr,
                "gnuchanscreenshot: could not read the screen\n");
        return -1;
    }

    char path[SS_TEXT_LENGTH];
    screenshot_filename(path, sizeof(path), dir, config->format);
    int rc = config->format == SS_FORMAT_JPEG
                 ? write_jpeg(path, image, config->quality)
                 : write_png(path, image);
    XDestroyImage(image);
    if (rc != 0) {
        fprintf(stderr,
                "gnuchanscreenshot: could not write %s\n", path);
        return -1;
    }
    printf("gnuchanscreenshot: wrote %s\n", path);
    fflush(stdout);
    return 0;
}

/* --- settings window ------------------------------------------------------ */

/* The whole settings window: one display, one window, a few labels and
   buttons, and the current settings so the window can show them. */
typedef struct SsSettingsUi {
    Display *display;
    Window window;
    GC gc;
    int screen;
    int width;
    int height;
    SsConfig config;
    int done;
    /* Fields the buttons edit: save_dir is a text buffer; everything else is
       a toggling button. */
    char save_dir_text[SS_TEXT_LENGTH];
    int format_png;    /* 1 when PNG, 0 when JPEG */
    int quality;       /* 1..100 */
    int capture_focused;
    int settings_window;
    int focused_field;
    char status[128];
} SsSettingsUi;

static void ss_settings_draw_text(SsSettingsUi *ui, int x, int y,
                                  const char *text, unsigned long pixel) {
    XSetForeground(ui->display, ui->gc, pixel);
    XDrawString(ui->display, ui->window, ui->gc, x, y, text,
                (int)strlen(text));
}

static void ss_settings_redraw(SsSettingsUi *ui) {
    XSetForeground(ui->display, ui->gc, 0x21102f);
    XFillRectangle(ui->display, ui->window, ui->gc, 0, 0,
                   (unsigned int)ui->width, (unsigned int)ui->height);
    XSetForeground(ui->display, ui->gc, 0xb98aff);
    XDrawString(ui->display, ui->window, ui->gc, 20, 34,
                "GnuChanScreenShoot settings", 28);
    int yy = 78;
    char line[SS_TEXT_LENGTH];
    const char *labels[] = {
        "Save directory",
        "Image format",
        "JPEG quality (click left/right)",
        "Alt+P capture",
        "Print opens settings",
    };
    const char *values[] = {
        ui->save_dir_text,
        ui->format_png ? "PNG" : "JPEG",
        NULL,
        ui->capture_focused ? "Focused window" : "Whole screen",
        ui->settings_window ? "Enabled" : "Disabled",
    };
    for (int i = 0; i < 5; i++) {
        int y = yy + i * 48;
        XSetForeground(ui->display, ui->gc,
                       i == ui->focused_field ? 0x392150 : 0x2b1740);
        XFillRectangle(ui->display, ui->window, ui->gc, 16, y - 22, 468, 38);
        XSetForeground(ui->display, ui->gc, 0x65428a);
        XDrawRectangle(ui->display, ui->window, ui->gc, 16, y - 22, 467, 37);
        ss_settings_draw_text(ui, 26, y, labels[i], 0xf5eaff);
        if (i == 2) {
            snprintf(line, sizeof(line), "%d", ui->quality);
            ss_settings_draw_text(ui, 350, y, line, 0xd8c8ed);
        } else {
            ss_settings_draw_text(ui, 280, y, values[i], 0xd8c8ed);
        }
    }

    XSetForeground(ui->display, ui->gc, 0x65428a);
    XFillRectangle(ui->display, ui->window, ui->gc, 280, 330, 100, 38);
    XFillRectangle(ui->display, ui->window, ui->gc, 390, 330, 94, 38);
    XSetForeground(ui->display, ui->gc, 0xf5eaff);
    XDrawString(ui->display, ui->window, ui->gc, 307, 354, "Cancel", 6);
    XDrawString(ui->display, ui->window, ui->gc, 416, 354, "Save", 4);
    if (ui->status[0]) {
        ss_settings_draw_text(ui, 20, 397, ui->status, 0xff6b8a);
    }
}

static int ss_settings_save(SsSettingsUi *ui) {
    /* Write the settings back as a script file. */
    char path[SS_TEXT_LENGTH];
    ss_config_path(path, sizeof(path));
    if (!path[0]) {
        snprintf(ui->status, sizeof(ui->status), "Cannot find config directory");
        return -1;
    }
    if (strchr(ui->save_dir_text, '"') || strchr(ui->save_dir_text, '\\')) {
        snprintf(ui->status, sizeof(ui->status),
                 "Path cannot contain quotes or backslashes");
        return -1;
    }

    /* Make sure the directory exists. */
    char dir[SS_TEXT_LENGTH];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (ensure_directory(dir) != 0) {
            snprintf(ui->status, sizeof(ui->status),
                     "Cannot create settings directory");
            return -1;
        }
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(ui->status, sizeof(ui->status), "Cannot write settings file");
        return -1;
    }

    fprintf(f, "# ~/.config/GnuChanScreenShoot/GnuChanScreenShoot.py\n");
    fprintf(f, "# Written by GnuChanScreenShoot settings window.\n");
    fprintf(f, "\n");
    fprintf(f, "gcl_Screenshot.Settings(\n");
    fprintf(f, "    SaveDir=\"%s\",\n", ui->save_dir_text);
    fprintf(f, "    Format=\"%s\",\n",
            ui->format_png ? "png" : "jpeg");
    fprintf(f, "    Quality=%d,\n", ui->quality);
    fprintf(f, "    CaptureFocused=%s,\n",
            ui->capture_focused ? "True" : "False");
    fprintf(f, "    SettingsWindow=%s,\n",
            ui->settings_window ? "True" : "False");
    fprintf(f, ")\n");
    if (fclose(f) != 0) {
        snprintf(ui->status, sizeof(ui->status), "Cannot finish settings file");
        return -1;
    }
    ui->status[0] = '\0';
    return 0;
}

/* Open the settings window, run until the user closes it or the idle timer
   expires. Returns 0 on a normal close. */
static int run_settings_window(Display *display, int screen,
                               const SsConfig *config) {
    int width = 500;
    int height = 420;
    Window root = RootWindow(display, screen);
    int x = (DisplayWidth(display, screen) - width) / 2;
    int y = (DisplayHeight(display, screen) - height) / 2;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }

    SsSettingsUi ui;
    memset(&ui, 0, sizeof(ui));
    ui.display = display;
    ui.screen = screen;
    ui.width = width;
    ui.height = height;
    ui.config = *config;
    snprintf(ui.save_dir_text, sizeof(ui.save_dir_text), "%s",
             config->save_dir);
    ui.format_png = (config->format == SS_FORMAT_PNG);
    ui.quality = config->quality;
    ui.capture_focused = config->capture_focused;
    ui.settings_window = config->settings_window;
    ui.focused_field = 0;

    ui.window = XCreateSimpleWindow(display, root, x, y,
                                    (unsigned int)width, (unsigned int)height,
                                    1, BlackPixel(display, screen),
                                    WhitePixel(display, screen));
    /* The transient settings form has fixed control coordinates; keep the
       window fixed-size rather than offering a maximize state it cannot use. */
    XSizeHints size_hints;
    memset(&size_hints, 0, sizeof(size_hints));
    size_hints.flags = PMinSize | PMaxSize;
    size_hints.min_width = size_hints.max_width = width;
    size_hints.min_height = size_hints.max_height = height;
    XSetWMNormalHints(display, ui.window, &size_hints);
    ui.gc = XCreateGC(display, ui.window, 0, NULL);
    XSelectInput(display, ui.window,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                 StructureNotifyMask);
    XMapWindow(display, ui.window);
    XStoreName(display, ui.window, "GnuChanScreenShoot — settings");
    XFlush(display);

    /* Idle timer: close after SS_SETTINGS_IDLE_MS of no input. */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += SS_SETTINGS_IDLE_MS * 1000000L;
    while (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (!ui.done && !s_should_stop) {
        long now_ms, until_ms;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        now_ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;
        until_ms = deadline.tv_sec * 1000 + deadline.tv_nsec / 1000000;
        long wait_ms = until_ms - now_ms;
        if (wait_ms <= 0) {
            break;
        }

        if (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            deadline.tv_sec = now.tv_sec;
            deadline.tv_nsec = now.tv_nsec;
            deadline.tv_nsec += SS_SETTINGS_IDLE_MS * 1000000L;
            while (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }

            switch (event.type) {
            case Expose:
                ss_settings_redraw(&ui);
                break;
            case KeyPress: {
                char input[32];
                KeySym ks = NoSymbol;
                int count = XLookupString(&event.xkey, input,
                                          sizeof(input), &ks, NULL);
                if (ks == XK_Escape || ks == XK_q || ks == XK_Q) {
                    ui.done = 1;
                } else if (ks == XK_Tab) {
                    ui.focused_field = (ui.focused_field + 1) % 5;
                } else if (ui.focused_field == 0 && ks == XK_BackSpace) {
                    size_t length = strlen(ui.save_dir_text);
                    if (length > 0) {
                        ui.save_dir_text[length - 1] = '\0';
                    }
                } else if (ui.focused_field == 0 && count > 0) {
                    size_t length = strlen(ui.save_dir_text);
                    if (length + (size_t)count < sizeof(ui.save_dir_text)) {
                        for (int i = 0; i < count; i++) {
                            if ((unsigned char)input[i] >= 0x20 &&
                                input[i] != '"' && input[i] != '\\') {
                                ui.save_dir_text[length++] = input[i];
                            }
                        }
                        ui.save_dir_text[length] = '\0';
                    }
                }
                ss_settings_redraw(&ui);
                break;
            }
            case ButtonPress: {
                int y = event.xbutton.y;
                if (y >= 330 && event.xbutton.x >= 390) {
                    if (ss_settings_save(&ui) == 0) {
                        ui.done = 1;
                    }
                } else if (y >= 330 && event.xbutton.x >= 280) {
                    ui.done = 1;
                } else if (y >= 56 && y < 104) {
                    ui.focused_field = 0;
                } else if (y < 152 && y >= 104) {
                    ui.focused_field = 1;
                    ui.format_png = !ui.format_png;
                } else if (y < 200 && y >= 152) {
                    ui.focused_field = 2;
                    if (event.xbutton.x < 250 && ui.quality > 1) {
                        ui.quality--;
                    } else if (ui.quality < 100) {
                        ui.quality++;
                    }
                } else if (y < 248 && y >= 200) {
                    ui.focused_field = 3;
                    ui.capture_focused = !ui.capture_focused;
                } else if (y < 296 && y >= 248) {
                    ui.focused_field = 4;
                    ui.settings_window = !ui.settings_window;
                }
                ss_settings_redraw(&ui);
                break;
            }
            default:
                break;
            }
        } else {
            struct timespec pause = {0, 100 * 1000 * 1000L}; /* 100 ms */
            nanosleep(&pause, NULL);
        }
    }

    XDestroyWindow(display, ui.window);
    XFreeGC(display, ui.gc);
    XFlush(display);
    return 0;
}

/* --- the program ---------------------------------------------------------- */

int main(int argc, char **argv) {
    int do_once = 0;
    int do_settings = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            do_once = 1;
        } else if (strcmp(argv[i], "--settings") == 0) {
            do_settings = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("GnuChanScreenShoot %s\n", GNUCHANSCREENSHOOT_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: GnuChanScreenShoot [--once] [--settings] "
                   "[--version]\n"
                   "\n"
                   "Alt+P captures the focused window (or the whole screen)\n"
                   "and writes it to the save directory. Print/SysRq opens\n"
                   "the settings window. The settings live in\n"
                   "~/.config/GnuChanScreenShoot/GnuChanScreenShoot.py\n"
                   "and are edited by GnuChanSettings.\n");
            return 0;
        } else {
            fprintf(stderr, "gnuchanscreenshot: unknown argument '%s'\n",
                    argv[i]);
            return 2;
        }
    }

    SsConfig config;
    ss_config_defaults(&config);
    if (ss_config_load_default(&config) != 0 && config.error[0]) {
        fprintf(stderr, "gnuchanscreenshot: %s\n", config.error);
    }

    if (do_settings) {
        Display *display = XOpenDisplay(NULL);
        if (!display) {
            fprintf(stderr,
                    "gnuchanscreenshot: cannot open the X display\n");
            return 1;
        }
        int screen = DefaultScreen(display);
        run_settings_window(display, screen, &config);
        XCloseDisplay(display);
        return 0;
    }

    if (do_once) {
        Display *display = XOpenDisplay(NULL);
        if (!display) {
            fprintf(stderr,
                    "gnuchanscreenshot: cannot open the X display\n");
            return 1;
        }
        int rc = take_screenshot(display, &config);
        XCloseDisplay(display);
        return rc;
    }

    /* THE SESSION DAEMON. It stays up for the whole session and answers the
       two hotkeys. */
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr,
                "gnuchanscreenshot: cannot open the X display\n");
        return 1;
    }
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);

    /* Resolve the hotkey keycodes. */
    KeySym ks_p = XStringToKeysym("p");
    KeySym ks_print = XStringToKeysym("Print");
    if (ks_p == NoSymbol || ks_print == NoSymbol) {
        fprintf(stderr,
                "gnuchanscreenshot: could not resolve hotkey keysyms\n");
        XCloseDisplay(display);
        return 1;
    }
    KeyCode kc_p = XKeysymToKeycode(display, ks_p);
    KeyCode kc_print = XKeysymToKeycode(display, ks_print);

    /* Set up error handler so BadAccess from XGrabKey is captured. */
    s_xerror = 0;
    XSetErrorHandler(error_handler);

    /* Grab Alt+P for screenshot. Also grab Alt+Shift+P as a fallback. */
    int grabbed = 0;
    unsigned int mods_p[] = {Mod1Mask, Mod1Mask | ShiftMask};
    for (int m = 0; m < 2; m++) {
        if (XGrabKey(display, kc_p, mods_p[m], root, True,
                     GrabModeAsync, GrabModeAsync) == GrabSuccess) {
            grabbed = 1;
        }
    }
    /* Grab Print/SysRq for settings window (no Alt required). */
    if (XGrabKey(display, kc_print, 0, root, True,
                 GrabModeAsync, GrabModeAsync) == GrabSuccess) {
        grabbed = 1;
    }

    if (s_xerror != 0 || !grabbed) {
        fprintf(stderr,
                "gnuchanscreenshot: could not grab the hotkeys; another "
                "program may hold them (X error %d)\n", s_xerror);
        XCloseDisplay(display);
        return 1;
    }

    /* Signals let the session end the daemon cleanly. */
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    XFlush(display);

    while (!s_should_stop) {
        XEvent event;
        XNextEvent(display, &event);

        if (event.type == KeyPress) {
            KeyCode code = event.xkey.keycode;
            unsigned int state = event.xkey.state;

            int is_alt = (state & Mod1Mask) != 0;
            if (code == kc_p && is_alt) {
                take_screenshot(display, &config);
                continue;
            }
            if (code == kc_print && config.settings_window) {
                /* Ignore Alt modifier for Print — it is the plain key. */
                run_settings_window(display, screen, &config);
                /* Re-read the settings after the window closes, so an edit
                   takes effect at once. */
                SsConfig fresh;
                if (ss_config_load_default(&fresh) == 0) {
                    config = fresh;
                }
                continue;
            }
        }
    }

    XUngrabKey(display, kc_p, Mod1Mask, root);
    XUngrabKey(display, kc_p, Mod1Mask | ShiftMask, root);
    XUngrabKey(display, kc_print, 0, root);
    XCloseDisplay(display);
    return 0;
}
