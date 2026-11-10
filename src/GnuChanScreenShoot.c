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
 *   Alt+P        choose a window with the pointer, then save and copy it.
 *   Shift+Alt+P  drag a screen region, then save and copy it.
 *   Print/SysRq  open an X11 window with the detailed settings: where
 *                screenshots go, which format, JPEG quality, and the default
 *                target for non-interactive captures.
 *
 * The settings live in
 *   ~/.config/GnuChanScreenShoot/GnuChanScreenShoot.conf
 * as plain Key = value data. Older .py-named config files are parsed as data
 * for migration; they are never executed.
 *
 * A screenshot is written as a PNG or JPEG with a timestamp in the name so
 * nothing is ever overwritten.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <setjmp.h>
#include <stdint.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
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
static int s_grab_error = 0;

static int error_handler(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    s_xerror = ev->error_code;
    return 0;
}

static unsigned int hotkey_modifier_mask(unsigned int base, unsigned int variant) {
    unsigned int mask = base;
    if (variant & 1u) mask |= LockMask;
    if (variant & 2u) mask |= Mod2Mask;
    if (variant & 4u) mask |= Mod5Mask;
    return mask;
}

static int grab_key_variants(Display *display, KeyCode keycode,
                             unsigned int base, Window root) {
    if (keycode == 0) {
        return 0;
    }

    int grabbed = 0;
    for (unsigned int variant = 0; variant < 8; variant++) {
        unsigned int modifiers = hotkey_modifier_mask(base, variant);
        s_xerror = 0;
        XGrabKey(display, keycode, modifiers, root, True,
                 GrabModeAsync, GrabModeAsync);
        XSync(display, False);
        if (s_xerror == 0) {
            grabbed++;
        } else {
            s_grab_error = s_xerror;
        }
    }
    return grabbed;
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
    int written = snprintf(tmp, sizeof(tmp), "%s", path ? path : "");
    if (written <= 0 || (size_t)written >= sizeof(tmp)) {
        return -1;
    }
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

static int executable_in_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !path[0]) {
        return 0;
    }

    const char *entry = path;
    while (1) {
        const char *end = strchr(entry, ':');
        size_t directory_length = end ? (size_t)(end - entry) : strlen(entry);
        char candidate[PATH_MAX];
        int written;
        if (directory_length == 0) {
            written = snprintf(candidate, sizeof(candidate), "%s", name);
        } else {
            written = snprintf(candidate, sizeof(candidate), "%.*s/%s",
                               (int)directory_length, entry, name);
        }
        if (written > 0 && (size_t)written < sizeof(candidate) &&
            access(candidate, X_OK) == 0) {
            return 1;
        }
        if (!end) {
            break;
        }
        entry = end + 1;
    }
    return 0;
}

static int require_clipboard_helper(void) {
    if (executable_in_path("xclip")) {
        return 0;
    }
    fprintf(stderr,
            "gnuchanscreenshot: xclip is required to copy screenshots; "
            "install the xclip package and try again\n");
    return -1;
}

static int copy_to_clipboard(const char *path, SsFormat format) {
    const char *mime = format == SS_FORMAT_JPEG ? "image/jpeg" : "image/png";
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "gnuchanscreenshot: could not start xclip: %s\n",
                strerror(errno));
        return -1;
    }
    if (child == 0) {
        execlp("xclip", "xclip", "-selection", "clipboard", "-t", mime,
               "-i", path, (char *)NULL);
        _exit(127);
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "gnuchanscreenshot: could not wait for xclip: %s\n",
                    strerror(errno));
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr,
                "gnuchanscreenshot: xclip could not put the screenshot on "
                "the X clipboard\n");
        return -1;
    }
    return 0;
}

/* Create a unique timestamped file without ever truncating an earlier capture. */
static int open_screenshot_file(char *out, unsigned int size,
                                const char *save_dir, SsFormat format) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) {
        return -1;
    }
    char stamp[32];
    if (strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm) == 0) {
        return -1;
    }
    const char *extension = format == SS_FORMAT_JPEG ? "jpg" : "png";
    for (unsigned int suffix = 0;; suffix++) {
        int written = suffix == 0
                          ? snprintf(out, size, "%s/screenshot-%s.%s",
                                     save_dir, stamp, extension)
                          : snprintf(out, size, "%s/screenshot-%s-%u.%s",
                                     save_dir, stamp, suffix, extension);
        if (written < 0 || (unsigned int)written >= size) {
            return -1;
        }
        int fd = open(out, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd >= 0 || errno != EEXIST) {
            return fd;
        }
        if (suffix == UINT_MAX) {
            return -1;
        }
    }
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

static int write_png(FILE *f, XImage *image) {
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

static int write_jpeg(FILE *f, XImage *image, int quality) {
    unsigned char *pixels = image_rgb(image);
    if (!pixels) {
        return -1;
    }

    struct jpeg_compress_struct jpeg;
    struct JpegError error;
    jpeg.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_failure;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&jpeg);
        free(pixels);
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
    return 0;
}

/* --- capture -------------------------------------------------------------- */

/* Grab the focused window, or fall back to the root window. Returns the
   window and its geometry relative to the root. */
static Window root_child(Display *display, Window candidate) {
    Window root = DefaultRootWindow(display);
    if (candidate == None || candidate == PointerRoot || candidate == root) {
        return root;
    }

    Window parent = None;
    Window *children = NULL;
    unsigned int nchildren = 0;
    while (XQueryTree(display, candidate, &root, &parent, &children,
                      &nchildren)) {
        if (children) {
            XFree(children);
            children = NULL;
        }
        if (parent == None || parent == root || parent == candidate) {
            return parent == root ? candidate : root;
        }
        candidate = parent;
    }
    return root;
}

static Window current_focus(Display *display) {
    Window root = DefaultRootWindow(display);
    Window focus = None;
    int revert = 0;
    XGetInputFocus(display, &focus, &revert);
    if (focus == None || focus == PointerRoot || focus == root) {
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
    }
    return root_child(display, focus);
}

static Window focused_window(Display *display, int *x, int *y,
                             unsigned int *w, unsigned int *h,
                             unsigned int *border, unsigned int *depth) {
    Window root = DefaultRootWindow(display);
    XSync(display, False);
    Window focus = current_focus(display);
    if (focus == root) {
        focus = root;
        *x = 0;
        *y = 0;
        *w = (unsigned int)DisplayWidth(display, DefaultScreen(display));
        *h = (unsigned int)DisplayHeight(display, DefaultScreen(display));
        *border = 0;
        *depth = (unsigned int)DefaultDepth(display, DefaultScreen(display));
        return focus;
    }
    XWindowAttributes attributes;
    if (!XGetWindowAttributes(display, focus, &attributes) ||
        attributes.map_state != IsViewable ||
        attributes.width <= 0 || attributes.height <= 0) {
        *x = 0;
        *y = 0;
        *w = (unsigned int)DisplayWidth(display, DefaultScreen(display));
        *h = (unsigned int)DisplayHeight(display, DefaultScreen(display));
        *border = 0;
        *depth = (unsigned int)DefaultDepth(display, DefaultScreen(display));
        return root;
    }
    *w = (unsigned int)attributes.width;
    *h = (unsigned int)attributes.height;
    *border = (unsigned int)attributes.border_width;
    *depth = (unsigned int)attributes.depth;
    *x = 0;
    *y = 0;
    return focus;
}

static Window chosen_window_geometry(Display *display, Window chosen,
                                     unsigned int *w, unsigned int *h,
                                     unsigned int *border,
                                     unsigned int *depth) {
    Window root = DefaultRootWindow(display);
    Window target = root_child(display, chosen);
    XWindowAttributes attributes;
    XSync(display, False);
    if (target == root || !XGetWindowAttributes(display, target, &attributes) ||
        attributes.map_state != IsViewable ||
        attributes.width <= 0 || attributes.height <= 0) {
        return None;
    }
    *w = (unsigned int)attributes.width;
    *h = (unsigned int)attributes.height;
    *border = (unsigned int)attributes.border_width;
    *depth = (unsigned int)attributes.depth;
    return target;
}

/* Capture a window into an XImage. */
static XImage *capture_window(Display *display, Window window, int x, int y,
                              unsigned int w, unsigned int h) {
    return XGetImage(display, window, x, y, w, h, AllPlanes, ZPixmap);
}

static int save_screenshot(const SsConfig *config, XImage *image) {
    char dir[SS_TEXT_LENGTH];
    expand_tilde(dir, sizeof(dir), config->save_dir);
    if (ensure_directory(dir) != 0) {
        fprintf(stderr,
                "gnuchanscreenshot: could not create save directory %s\n",
                dir);
        XDestroyImage(image);
        return -1;
    }

    char path[SS_TEXT_LENGTH];
    int fd = open_screenshot_file(path, sizeof(path), dir, config->format);
    if (fd < 0) {
        fprintf(stderr,
                "gnuchanscreenshot: could not create a unique screenshot file "
                "in %s\n", dir);
        XDestroyImage(image);
        return -1;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(path);
        fprintf(stderr,
                "gnuchanscreenshot: could not open the screenshot file\n");
        XDestroyImage(image);
        return -1;
    }
    int rc = config->format == SS_FORMAT_JPEG
                 ? write_jpeg(file, image, config->quality)
                 : write_png(file, image);
    if (fclose(file) != 0) {
        rc = -1;
    }
    XDestroyImage(image);
    if (rc != 0) {
        unlink(path);
        fprintf(stderr,
                "gnuchanscreenshot: could not write %s\n", path);
        return -1;
    }
    printf("gnuchanscreenshot: wrote %s\n", path);
    fflush(stdout);
    if (copy_to_clipboard(path, config->format) != 0) {
        fprintf(stderr,
                "gnuchanscreenshot: screenshot was saved but could not be "
                "copied to the clipboard\n");
        return -1;
    }
    printf("gnuchanscreenshot: copied to clipboard\n");
    fflush(stdout);
    return 0;
}

/* Take one screenshot and write it. Returns 0 on success. */
static int take_screenshot(Display *display, const SsConfig *config,
                           Window chosen_window_id) {
    if (require_clipboard_helper() != 0) {
        return -1;
    }

    int x = 0, y = 0;
    unsigned int w = 0, h = 0, border = 0, depth = 0;
    Window root = DefaultRootWindow(display);
    Window window = root;
    if (config->capture_focused) {
        if (chosen_window_id != None) {
            window = chosen_window_geometry(display, chosen_window_id,
                                            &w, &h, &border, &depth);
            if (window == None) {
                fprintf(stderr,
                        "gnuchanscreenshot: the selected window is no longer "
                        "available; no screenshot was taken\n");
                return -1;
            }
        } else {
            window = focused_window(display, &x, &y, &w, &h, &border, &depth);
        }
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
    return save_screenshot(config, image);
}

static int take_region_screenshot(Display *display, const SsConfig *config,
                                  int x, int y, unsigned int width,
                                  unsigned int height) {
    if (require_clipboard_helper() != 0) {
        return -1;
    }
    XImage *image = capture_window(display, DefaultRootWindow(display),
                                   x, y, width, height);
    if (!image) {
        fprintf(stderr,
                "gnuchanscreenshot: could not read the selected region; "
                "no screenshot was taken\n");
        return -1;
    }
    return save_screenshot(config, image);
}

static int normalize_region(int start_x, int start_y, int end_x, int end_y,
                            int screen_width, int screen_height,
                            int *x, int *y, unsigned int *width,
                            unsigned int *height) {
    int left = start_x < end_x ? start_x : end_x;
    int top = start_y < end_y ? start_y : end_y;
    int right = start_x > end_x ? start_x : end_x;
    int bottom = start_y > end_y ? start_y : end_y;
    if (left < 0 || top < 0 || right > screen_width ||
        bottom > screen_height || right <= left || bottom <= top) {
        return -1;
    }
    *x = left;
    *y = top;
    *width = (unsigned int)(right - left);
    *height = (unsigned int)(bottom - top);
    return 0;
}

static void region_overlay_update(Display *display, Window overlay,
                                  int screen_width, int screen_height,
                                  int pointer_x, int pointer_y,
                                  int start_x, int start_y, int dragging) {
    XRectangle rectangles[6];
    int count = 0;
    if (pointer_x < 0) pointer_x = 0;
    if (pointer_y < 0) pointer_y = 0;
    if (pointer_x >= screen_width) pointer_x = screen_width - 1;
    if (pointer_y >= screen_height) pointer_y = screen_height - 1;
    rectangles[count++] = (XRectangle){0, (short)pointer_y,
                                        (unsigned short)screen_width, 1};
    rectangles[count++] = (XRectangle){(short)pointer_x, 0, 1,
                                        (unsigned short)screen_height};
    if (dragging) {
        int left = start_x < pointer_x ? start_x : pointer_x;
        int top = start_y < pointer_y ? start_y : pointer_y;
        int right = start_x > pointer_x ? start_x : pointer_x;
        int bottom = start_y > pointer_y ? start_y : pointer_y;
        unsigned short horizontal = (unsigned short)(right - left + 1);
        unsigned short vertical = (unsigned short)(bottom - top + 1);
        rectangles[count++] = (XRectangle){(short)left, (short)top,
                                            horizontal, 2};
        rectangles[count++] = (XRectangle){(short)left, (short)bottom,
                                            horizontal, 2};
        rectangles[count++] = (XRectangle){(short)left, (short)top,
                                            2, vertical};
        rectangles[count++] = (XRectangle){(short)right, (short)top,
                                            2, vertical};
    }
    XShapeCombineRectangles(display, overlay, ShapeBounding, 0, 0, rectangles,
                            count, ShapeSet, Unsorted);
    XClearWindow(display, overlay);
}

static Window selection_key_window(Display *display, Window root) {
    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.event_mask = KeyPressMask;
    return XCreateWindow(display, root, 0, 0, 1, 1, 0, 0, InputOnly,
                         CopyFromParent, CWEventMask, &attributes);
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
    int capture_mode;
    int focused_field;
    Window initial_target;
    Window chosen_target;
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
    const char *title = ui->capture_mode
                            ? "GnuChanScreenShoot capture"
                            : "GnuChanScreenShoot settings";
    XDrawString(ui->display, ui->window, ui->gc, 20, 34, title,
                (int)strlen(title));
    int yy = 78;
    char line[SS_TEXT_LENGTH];
    const char *labels[] = {
        "Save directory",
        "Image format",
        "JPEG quality (click left/right)",
        "Default capture target",
        "Print opens settings",
    };
    const char *values[] = {
        ui->save_dir_text,
        ui->format_png ? "PNG" : "JPEG",
        NULL,
        ui->chosen_target != None ? "Chosen window" :
            (ui->capture_focused ? "Focused window" : "Whole screen"),
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
    XFillRectangle(ui->display, ui->window, ui->gc, 16, 330, 112, 38);
    XFillRectangle(ui->display, ui->window, ui->gc, 140, 330, 112, 38);
    XFillRectangle(ui->display, ui->window, ui->gc, 280, 330, 100, 38);
    XFillRectangle(ui->display, ui->window, ui->gc, 390, 330, 94, 38);
    XSetForeground(ui->display, ui->gc, 0xf5eaff);
    XDrawString(ui->display, ui->window, ui->gc, 26, 354, "Choose window", 13);
    XDrawString(ui->display, ui->window, ui->gc, 169, 354, "Capture", 7);
    XDrawString(ui->display, ui->window, ui->gc, 307, 354, "Cancel", 6);
    XDrawString(ui->display, ui->window, ui->gc, 416, 354, "Save", 4);
    if (ui->status[0]) {
        ss_settings_draw_text(ui, 20, 397, ui->status,
                              ui->chosen_target != None ? 0x8fe6a2 : 0xff6b8a);
    }
}

static int ss_settings_save(SsSettingsUi *ui) {
    /* Write a Key = value config, atomically, so a save cannot be lost to a
       partial write or silently reset by the next install. */
    char path[SS_TEXT_LENGTH];
    ss_config_path(path, sizeof(path));
    if (!path[0]) {
        snprintf(ui->status, sizeof(ui->status), "Cannot find config directory");
        return -1;
    }
    if (!ui->save_dir_text[0]) {
        snprintf(ui->status, sizeof(ui->status),
                 "Save directory cannot be empty");
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

    char temporary[SS_TEXT_LENGTH];
    if (snprintf(temporary, sizeof(temporary), "%s.new", path) >=
        (int)sizeof(temporary)) {
        snprintf(ui->status, sizeof(ui->status), "Settings path is too long");
        return -1;
    }
    FILE *f = fopen(temporary, "w");
    if (!f) {
        snprintf(ui->status, sizeof(ui->status), "Cannot write settings file");
        return -1;
    }

    fprintf(f, "# ~/.config/GnuChanScreenShoot/GnuChanScreenShoot.conf\n");
    fprintf(f, "SaveDir = \"%s\"\n", ui->save_dir_text);
    fprintf(f, "Format = \"%s\"\n",
            ui->format_png ? "png" : "jpeg");
    fprintf(f, "Quality = %d\n", ui->quality);
    fprintf(f, "CaptureFocused = %s\n",
            ui->capture_focused ? "True" : "False");
    fprintf(f, "SettingsWindow = %s\n",
            ui->settings_window ? "True" : "False");
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) {
        ok = 0;
    }
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        snprintf(ui->status, sizeof(ui->status),
                 "Cannot finish settings file");
        return -1;
    }
    ui->status[0] = '\0';
    return 0;
}

static Window select_window(Display *display, Window root, Window settings) {
    Window settings_root = root_child(display, settings);
    Cursor cursor = XCreateFontCursor(display, XC_crosshair);
    Window key_window = selection_key_window(display, root);
    if (key_window != None) {
        XMapWindow(display, key_window);
    }
    if (cursor == None || key_window == None ||
        XGrabPointer(display, root, False, ButtonPressMask,
                     GrabModeAsync, GrabModeAsync, None, cursor,
                     CurrentTime) != GrabSuccess ||
        XGrabKeyboard(display, key_window, False, GrabModeAsync,
                      GrabModeAsync, CurrentTime) != GrabSuccess) {
        XUngrabPointer(display, CurrentTime);
        XUngrabKeyboard(display, CurrentTime);
        if (key_window != None) XDestroyWindow(display, key_window);
        if (cursor != None) XFreeCursor(display, cursor);
        fprintf(stderr,
                "gnuchanscreenshot: could not start window selection\n");
        return None;
    }
    XFlush(display);
    Window target = None;
    int done = 0;
    while (!done && !s_should_stop) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == KeyPress &&
            XLookupKeysym(&event.xkey, 0) == XK_Escape) {
            done = 1;
            continue;
        }
        if (event.type != ButtonPress) {
            continue;
        }
        if (event.xbutton.button != Button1) {
            done = 1;
            continue;
        }
        Window root_return = None;
        Window child = None;
        int root_x, root_y, window_x, window_y;
        unsigned int mask;
        if (XQueryPointer(display, root, &root_return, &child,
                          &root_x, &root_y, &window_x, &window_y, &mask) &&
            child != None) {
            target = root_child(display, child);
        }
        if (target == settings_root || target == root) {
            target = None;
        }
        done = 1;
    }
    XUngrabPointer(display, CurrentTime);
    XUngrabKeyboard(display, CurrentTime);
    XDestroyWindow(display, key_window);
    XFreeCursor(display, cursor);
    XFlush(display);
    return target;
}

static int select_region(Display *display, Window root, int screen,
                         int *x, int *y, unsigned int *width,
                         unsigned int *height) {
    int event_base, error_base;
    if (!XShapeQueryExtension(display, &event_base, &error_base)) {
        fprintf(stderr,
                "gnuchanscreenshot: the X Shape extension is required for "
                "region selection\n");
        return -1;
    }

    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);
    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.override_redirect = True;
    attributes.background_pixel = WhitePixel(display, screen);
    Window overlay = XCreateWindow(
        display, root, 0, 0, (unsigned int)screen_width,
        (unsigned int)screen_height, 0, CopyFromParent, InputOutput,
        CopyFromParent, CWOverrideRedirect | CWBackPixel, &attributes);
    Cursor cursor = XCreateFontCursor(display, XC_crosshair);
    Window key_window = selection_key_window(display, root);
    if (overlay == None || cursor == None || key_window == None) {
        if (overlay != None) XDestroyWindow(display, overlay);
        if (key_window != None) XDestroyWindow(display, key_window);
        if (cursor != None) XFreeCursor(display, cursor);
        fprintf(stderr,
                "gnuchanscreenshot: could not create the region selector\n");
        return -1;
    }

    XColor color, exact;
    if (XAllocNamedColor(display, DefaultColormap(display, screen),
                         "#ff4fd8", &color, &exact)) {
        XSetWindowBackground(display, overlay, color.pixel);
    }
    int initial_x = screen_width / 2;
    int initial_y = screen_height / 2;
    region_overlay_update(display, overlay, screen_width, screen_height,
                          initial_x, initial_y, 0, 0, 0);
    XMapRaised(display, overlay);
    XMapWindow(display, key_window);
    XFlush(display);

    int pointer_grab = XGrabPointer(
        display, root, False,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, cursor, CurrentTime);
    int keyboard_grab = pointer_grab == GrabSuccess
                            ? XGrabKeyboard(display, key_window, False,
                                            GrabModeAsync, GrabModeAsync,
                                            CurrentTime)
                            : GrabNotViewable;
    if (pointer_grab != GrabSuccess || keyboard_grab != GrabSuccess) {
        XUngrabPointer(display, CurrentTime);
        XUngrabKeyboard(display, CurrentTime);
        XDestroyWindow(display, overlay);
        XDestroyWindow(display, key_window);
        XFreeCursor(display, cursor);
        XFlush(display);
        fprintf(stderr,
                "gnuchanscreenshot: could not start region selection\n");
        return -1;
    }

    int start_x = 0, start_y = 0, end_x = 0, end_y = 0;
    int dragging = 0;
    int selected = 0;
    while (!s_should_stop) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == KeyPress &&
            XLookupKeysym(&event.xkey, 0) == XK_Escape) {
            break;
        }
        if (event.type == ButtonPress) {
            if (event.xbutton.button != Button1) {
                break;
            }
            start_x = event.xbutton.x_root;
            start_y = event.xbutton.y_root;
            end_x = start_x;
            end_y = start_y;
            dragging = 1;
        } else if (event.type == MotionNotify) {
            end_x = event.xmotion.x_root;
            end_y = event.xmotion.y_root;
        } else if (event.type == ButtonRelease && dragging) {
            end_x = event.xbutton.x_root;
            end_y = event.xbutton.y_root;
            selected = normalize_region(start_x, start_y, end_x, end_y,
                                        screen_width, screen_height,
                                        x, y, width, height) == 0;
            break;
        } else {
            continue;
        }
        region_overlay_update(display, overlay, screen_width, screen_height,
                              end_x, end_y, start_x, start_y, dragging);
        XFlush(display);
    }

    XUngrabPointer(display, CurrentTime);
    XUngrabKeyboard(display, CurrentTime);
    XDestroyWindow(display, overlay);
    XDestroyWindow(display, key_window);
    XFreeCursor(display, cursor);
    XFlush(display);
    return selected ? 1 : 0;
}

/* Open the settings window, run until the user closes it or the idle timer
   expires. Returns 0 on a normal close. */
static int run_settings_window(Display *display, int screen,
                               const SsConfig *config, int capture_mode,
                               Window initial_target) {
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
    ui.capture_mode = capture_mode;
    ui.focused_field = 0;
    ui.initial_target = initial_target;

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
    XStoreName(display, ui.window, capture_mode
                                     ? "GnuChanScreenShoot — capture"
                                     : "GnuChanScreenShoot — settings");
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
                if (y >= 330 && event.xbutton.x < 128) {
                    snprintf(ui.status, sizeof(ui.status),
                             "Click a window to choose it");
                    ss_settings_redraw(&ui);
                    XFlush(display);
                    Window selected = select_window(display, root, ui.window);
                    if (selected != None) {
                        ui.chosen_target = selected;
                        snprintf(ui.status, sizeof(ui.status),
                                 "Window chosen; size is read at capture time");
                    } else {
                        snprintf(ui.status, sizeof(ui.status),
                                 "Could not choose a window");
                    }
                } else if (y >= 330 && event.xbutton.x < 260 &&
                           event.xbutton.x >= 140) {
                    SsConfig capture = *config;
                    snprintf(capture.save_dir, sizeof(capture.save_dir), "%s",
                             ui.save_dir_text);
                    capture.format = ui.format_png ? SS_FORMAT_PNG
                                                   : SS_FORMAT_JPEG;
                    capture.quality = ui.quality;
                    capture.capture_focused = ui.capture_focused ||
                                              ui.chosen_target != None;
                    capture.settings_window = ui.settings_window;
                    Window target = ui.chosen_target != None
                                        ? ui.chosen_target
                                        : (ui.capture_focused
                                               ? ui.initial_target
                                               : None);
                    if (require_clipboard_helper() != 0) {
                        snprintf(ui.status, sizeof(ui.status),
                                 "Install xclip for clipboard capture");
                    } else if (take_screenshot(display, &capture, target) == 0) {
                        ui.done = 1;
                    } else {
                        snprintf(ui.status, sizeof(ui.status),
                                 "Capture failed; see error output");
                    }
                } else if (y >= 330 && event.xbutton.x >= 390) {
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
                    ui.chosen_target = None;
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
    int do_capture = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            do_once = 1;
        } else if (strcmp(argv[i], "--settings") == 0) {
            do_settings = 1;
        } else if (strcmp(argv[i], "--capture") == 0) {
            do_capture = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("GnuChanScreenShoot %s\n", GNUCHANSCREENSHOOT_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: GnuChanScreenShoot [--once] [--settings] "
                   "[--capture] "
                   "[--version]\n"
                   "\n"
                   "Alt+P selects a window; Shift+Alt+P selects a region.\n"
                   "Print/SysRq opens settings. Use --capture for the\n"
                   "settings capture chooser.\n"
                   "Settings live in\n"
                   "~/.config/GnuChanScreenShoot/GnuChanScreenShoot.conf.\n");
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
        Window initial_target = current_focus(display);
        run_settings_window(display, screen, &config, 0, initial_target);
        XCloseDisplay(display);
        return 0;
    }

    if (do_capture) {
        Display *display = XOpenDisplay(NULL);
        if (!display) {
            fprintf(stderr,
                    "gnuchanscreenshot: cannot open the X display\n");
            return 1;
        }
        int screen = DefaultScreen(display);
        Window initial_target = current_focus(display);
        run_settings_window(display, screen, &config, 1, initial_target);
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
        int rc = take_screenshot(display, &config, None);
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
    s_grab_error = 0;
    XSetErrorHandler(error_handler);

    /* NumLock and CapsLock alter X11's modifier state even when they are not
       part of the shortcut. Grab every lock-state variant so the keys remain
       reliable regardless of the user's keyboard state. */
    int grab_alt_p = grab_key_variants(display, kc_p, Mod1Mask, root);
    int grab_shift_alt_p =
        grab_key_variants(display, kc_p, Mod1Mask | ShiftMask, root);
    int grab_print = config.settings_window
                         ? grab_key_variants(display, kc_print, 0, root)
                         : 0;

    if (grab_alt_p == 0 && grab_shift_alt_p == 0) {
        fprintf(stderr,
                "gnuchanscreenshot: could not grab Alt+P or Shift+Alt+P; "
                "another program may hold both shortcuts\n");
        XCloseDisplay(display);
        return 1;
    }
    if (grab_alt_p == 0 || grab_shift_alt_p == 0) {
        fprintf(stderr,
                "gnuchanscreenshot: one screenshot shortcut could not be "
                "grabbed; X error %d\n", s_grab_error);
    }
    if (config.settings_window && grab_print == 0) {
        fprintf(stderr,
                "gnuchanscreenshot: Print could not be grabbed; another "
                "program may own that key (X error %d)\n", s_grab_error);
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
                if (require_clipboard_helper() == 0) {
                    if (state & ShiftMask) {
                        int x = 0, y = 0;
                        unsigned int width = 0, height = 0;
                        int selected = select_region(display, root, screen,
                                                     &x, &y, &width, &height);
                        if (selected > 0) {
                            (void)take_region_screenshot(
                                display, &config, x, y, width, height);
                        } else if (selected == 0) {
                            fprintf(stderr,
                                    "gnuchanscreenshot: region selection "
                                    "cancelled; no screenshot was taken\n");
                        }
                    } else {
                        Window selected = select_window(display, root, None);
                        if (selected != None) {
                            SsConfig capture = config;
                            capture.capture_focused = 1;
                            (void)take_screenshot(display, &capture, selected);
                        } else {
                            fprintf(stderr,
                                    "gnuchanscreenshot: window selection "
                                    "cancelled; no screenshot was taken\n");
                        }
                    }
                }
                SsConfig fresh;
                if (ss_config_load_default(&fresh) == 0) {
                    config = fresh;
                }
                continue;
            }
            if (code == kc_print && config.settings_window) {
                /* Ignore Alt modifier for Print — it is the plain key. */
                Window initial_target = current_focus(display);
                run_settings_window(display, screen, &config, 0,
                                    initial_target);
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

    for (unsigned int variant = 0; variant < 8; variant++) {
        XUngrabKey(display, kc_p,
                   hotkey_modifier_mask(Mod1Mask, variant), root);
        XUngrabKey(display, kc_p,
                   hotkey_modifier_mask(Mod1Mask | ShiftMask, variant), root);
        if (config.settings_window) {
            XUngrabKey(display, kc_print,
                       hotkey_modifier_mask(0, variant), root);
        }
    }
    XCloseDisplay(display);
    return 0;
}
