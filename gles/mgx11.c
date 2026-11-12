/* SPDX-License-Identifier: GPL-3.0-or-later
 * The EGL library SDL loads (SDL_EGL_LIBRARY) for a game MobileGlues draws on Android's own OpenGL ES driver
 * through libhybris. libhybris has no X11 window surfaces, so a window surface here is a pbuffer of the window's
 * size. At every eglSwapBuffers the GPU copies it into one of three Android hardware buffers, which the X server
 * (Termux:X11) took once over DRI3, as libhybris's Vulkan swapchain does in the app (runtime/hybris/patches/0001);
 * without them, the CPU copies it with XPutImage. Everything else goes to MobileGlues' EGL (MG_LIBRARY). */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default")))

static void *mg;
static Display *x;   /* our own connection: SDL's may be in use on another thread */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *real(const char *name)
{
    if (!mg) {
        const char *path = getenv("MG_LIBRARY");
        mg = dlopen(path ? path : "libmobileglues.so", RTLD_NOW | RTLD_LOCAL);
        if (!mg) { fprintf(stderr, "mgx11: %s\n", dlerror()); abort(); }
        x = XOpenDisplay(NULL);
        fprintf(stderr, "mgx11: MobileGlues loaded, X display %s\n", x ? "open" : "NOT OPEN");
    }
    void *f = dlsym(mg, name);
    if (!f) {
        void *(*get)(const char *) = (void *(*)(const char *))dlsym(mg, "eglGetProcAddress");
        f = get ? get(name) : NULL;
    }
    return f;
}
#define REAL(ret, name, args) static ret (*real_##name) args; \
    if (!real_##name) *(void **)&real_##name = real(#name);

/* GL, for the copy. */
typedef void (*getiv_t)(unsigned, int *);
typedef void (*bind_t)(unsigned, unsigned);
typedef void (*read_t)(int, int, int, int, unsigned, unsigned, void *);
typedef void (*store_t)(unsigned, int);
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

struct window_surface {
    EGLDisplay display;
    EGLConfig config;
    EGLSurface pbuffer;
    Window window;
    int width, height;
    XImage *image;
    GC gc;
    unsigned char *rgba;
    unsigned frames;
    double copy_ms, put_ms;
    /* DRI3: 0 untried, 1 in use, -1 not available. */
    int dri3;
    void *buffers[3], *images[3];
    unsigned textures[3], framebuffers[3];
    uint32_t pixmaps[3];
    unsigned next_buffer;
    unsigned stride;
    /* A frame goes to the X server once the GPU has drawn it, a frame later, so the CPU and GPU overlap. */
    void *fences[3];
    int waiting;   /* the buffer drawn last frame, -1 if none */
    uint32_t serial;
    double blit_ms, present_ms;
    double second_start;
    unsigned second_frames, fps;
    struct window_surface *next;
};
static struct window_surface *surfaces;
static void release_buffers(struct window_surface *s);
static __thread struct window_surface *current;

static struct window_surface *ours(EGLSurface surface)
{
    pthread_mutex_lock(&lock);
    struct window_surface *s = surfaces;
    while (s && (EGLSurface)s != surface) s = s->next;
    pthread_mutex_unlock(&lock);
    return s;
}
static EGLSurface backing(EGLSurface surface)
{
    struct window_surface *s = ours(surface);
    return s ? s->pbuffer : surface;
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static EGLSurface pbuffer(EGLDisplay display, EGLConfig config, int width, int height)
{
    REAL(EGLSurface, eglCreatePbufferSurface, (EGLDisplay, EGLConfig, const EGLint *))
    const EGLint size[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    return real_eglCreatePbufferSurface(display, config, size);
}

static void allocate(struct window_surface *s)
{
    if (s->image) XDestroyImage(s->image);   /* frees its data */
    free(s->rgba);
    s->rgba = malloc((size_t)s->width * s->height * 4);
    char *data = malloc((size_t)s->width * s->height * 4);
    Visual *visual = DefaultVisual(x, DefaultScreen(x));
    s->image = XCreateImage(x, visual, DefaultDepth(x, DefaultScreen(x)), ZPixmap, 0, data, s->width, s->height,
                            32, 0);
}

EXPORT EGLDisplay eglGetDisplay(EGLNativeDisplayType native)
{
    REAL(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType))
    (void)native;
    return real_eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

EXPORT const char *eglQueryString(EGLDisplay display, EGLint name)
{
    REAL(const char *, eglQueryString, (EGLDisplay, EGLint))
    /* Without a display: no client extensions and no version, so SDL takes eglGetDisplay, not the X11 platform. */
    if (display == EGL_NO_DISPLAY) return name == EGL_EXTENSIONS ? "" : NULL;
    return real_eglQueryString(display, name);
}

EXPORT EGLBoolean eglChooseConfig(EGLDisplay display, const EGLint *attributes, EGLConfig *configs, EGLint size,
                                  EGLint *count)
{
    REAL(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
    EGLint copy[128];
    int n = 0;
    for (const EGLint *a = attributes; a && *a != EGL_NONE && n < 124; a += 2) {
        if (a[0] == EGL_SURFACE_TYPE) continue;
        copy[n++] = a[0];
        copy[n++] = a[1];
    }
    copy[n++] = EGL_SURFACE_TYPE;
    copy[n++] = EGL_PBUFFER_BIT;
    copy[n] = EGL_NONE;
    return real_eglChooseConfig(display, copy, configs, size, count);
}

EXPORT EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config, EGLint attribute, EGLint *value)
{
    REAL(EGLBoolean, eglGetConfigAttrib, (EGLDisplay, EGLConfig, EGLint, EGLint *))
    /* No X visual of its own: SDL takes the screen's default one. */
    if (attribute == EGL_NATIVE_VISUAL_ID) { *value = 0; return EGL_TRUE; }
    EGLBoolean result = real_eglGetConfigAttrib(display, config, attribute, value);
    if (result && attribute == EGL_SURFACE_TYPE) *value |= EGL_WINDOW_BIT;
    return result;
}

EXPORT EGLSurface eglCreateWindowSurface(EGLDisplay display, EGLConfig config, EGLNativeWindowType window,
                                         const EGLint *attributes)
{
    (void)attributes;
    real("eglGetError");
    XWindowAttributes geometry;
    if (!x || !XGetWindowAttributes(x, (Window)window, &geometry)) return EGL_NO_SURFACE;
    struct window_surface *s = calloc(1, sizeof *s);
    s->display = display;
    s->config = config;
    s->window = (Window)window;
    s->width = geometry.width > 0 ? geometry.width : 1;
    s->height = geometry.height > 0 ? geometry.height : 1;
    s->pbuffer = pbuffer(display, config, s->width, s->height);
    if (s->pbuffer == EGL_NO_SURFACE) { free(s); return EGL_NO_SURFACE; }
    s->gc = XCreateGC(x, s->window, 0, NULL);
    allocate(s);
    pthread_mutex_lock(&lock);
    s->next = surfaces;
    surfaces = s;
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "mgx11: window 0x%lx, %dx%d through a pbuffer\n", s->window, s->width, s->height);
    return (EGLSurface)s;
}

EXPORT EGLBoolean eglDestroySurface(EGLDisplay display, EGLSurface surface)
{
    REAL(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface))
    struct window_surface *s = ours(surface);
    if (!s) return real_eglDestroySurface(display, surface);
    pthread_mutex_lock(&lock);
    for (struct window_surface **p = &surfaces; *p; p = &(*p)->next)
        if (*p == s) { *p = s->next; break; }
    pthread_mutex_unlock(&lock);
    if (s->dri3 > 0) release_buffers(s);
    EGLBoolean result = real_eglDestroySurface(display, s->pbuffer);
    if (s->image) XDestroyImage(s->image);
    XFreeGC(x, s->gc);
    free(s->rgba);
    free(s);
    return result;
}

EXPORT EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context)
{
    REAL(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
    EGLBoolean result = real_eglMakeCurrent(display, backing(draw), backing(read), context);
    if (result) current = ours(draw);
    return result;
}

EXPORT EGLSurface eglGetCurrentSurface(EGLint which)
{
    REAL(EGLSurface, eglGetCurrentSurface, (EGLint))
    EGLSurface surface = real_eglGetCurrentSurface(which);
    return current && current->pbuffer == surface ? (EGLSurface)current : surface;
}

EXPORT EGLBoolean eglQuerySurface(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint *value)
{
    REAL(EGLBoolean, eglQuerySurface, (EGLDisplay, EGLSurface, EGLint, EGLint *))
    return real_eglQuerySurface(display, backing(surface), attribute, value);
}

EXPORT EGLBoolean eglSurfaceAttrib(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint value)
{
    REAL(EGLBoolean, eglSurfaceAttrib, (EGLDisplay, EGLSurface, EGLint, EGLint))
    return real_eglSurfaceAttrib(display, backing(surface), attribute, value);
}


/* Android's hardware buffers and what draws into them: Android's libnativewindow through libhybris, and
 * libhybris's own EGL and GLES (their EGLImages are libhybris's, so its eglCreateImageKHR and
 * glEGLImageTargetTexture2DOES go together). The GL calls here bypass MobileGlues and put back what they change. */
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;
/* The X server reads the buffer as its own BGRA. */
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN 3ULL
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN (3ULL << 4)
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER (1ULL << 9)
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_SCISSOR_TEST 0x0C11
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_NEAREST 0x2600
/* The X server's DRI3 reads an Android hardware buffer from a socket passed as the buffer's fd. */
static const uint64_t AHARDWAREBUFFER_SOCKET_FD = 1255;

static struct {
    int ready;   /* 1 loaded, -1 not available */
    int (*allocate)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
    void (*release)(AHardwareBuffer *);
    void (*describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
    int (*send)(const AHardwareBuffer *, int);
    EGLClientBuffer (*client_buffer)(const AHardwareBuffer *);
    void *(*create_image)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
    EGLBoolean (*destroy_image)(EGLDisplay, void *);
    void (*image_texture)(unsigned, void *);
    void (*gen_textures)(int, unsigned *);
    void (*delete_textures)(int, const unsigned *);
    void (*bind_texture)(unsigned, unsigned);
    void (*gen_framebuffers)(int, unsigned *);
    void (*delete_framebuffers)(int, const unsigned *);
    void (*bind_framebuffer)(unsigned, unsigned);
    void (*framebuffer_texture)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*check_framebuffer)(unsigned);
    void (*blit)(int, int, int, int, int, int, int, int, unsigned, unsigned);
    void (*get_integer)(unsigned, int *);
    unsigned char (*is_enabled)(unsigned);
    void (*enable)(unsigned);
    void (*disable)(unsigned);
    void (*finish)(void);
    void *(*fence_sync)(unsigned, unsigned);
    unsigned (*client_wait)(void *, unsigned, uint64_t);
    void (*delete_sync)(void *);
    void (*flush)(void);
    void (*scissor)(int, int, int, int);
    void (*clear_color)(float, float, float, float);
    void (*clear)(unsigned);
    void (*get_float)(unsigned, float *);
    void (*get_boolean)(unsigned, unsigned char *);
    void (*color_mask)(unsigned char, unsigned char, unsigned char, unsigned char);
    xcb_connection_t *xcb;
} hw;

static int load_hw(void)
{
    if (hw.ready) return hw.ready > 0;
    hw.ready = -1;
    void *common = dlopen("libhybris-common.so.1", RTLD_NOW | RTLD_NOLOAD);
    void *egl = dlopen(getenv("MG_EGL_LIBRARY"), RTLD_NOW | RTLD_NOLOAD);
    void *gles = dlopen(getenv("MG_GLES_LIBRARY"), RTLD_NOW | RTLD_NOLOAD);
    if (!common || !egl || !gles) { fprintf(stderr, "mgx11: no libhybris loaded\n"); return 0; }
    void *(*android_open)(const char *, int) = (void *(*)(const char *, int))dlsym(common, "android_dlopen");
    void *(*android_sym)(void *, const char *) = (void *(*)(void *, const char *))dlsym(common, "android_dlsym");
    void *window = android_open ? android_open("libnativewindow.so", RTLD_NOW) : NULL;
    if (!window) { fprintf(stderr, "mgx11: no libnativewindow\n"); return 0; }
    *(void **)&hw.allocate = android_sym(window, "AHardwareBuffer_allocate");
    *(void **)&hw.release = android_sym(window, "AHardwareBuffer_release");
    *(void **)&hw.describe = android_sym(window, "AHardwareBuffer_describe");
    *(void **)&hw.send = android_sym(window, "AHardwareBuffer_sendHandleToUnixSocket");
    void *(*egl_proc)(const char *) = (void *(*)(const char *))dlsym(egl, "eglGetProcAddress");
    *(void **)&hw.client_buffer = egl_proc("eglGetNativeClientBufferANDROID");
    *(void **)&hw.create_image = egl_proc("eglCreateImageKHR");
    *(void **)&hw.destroy_image = egl_proc("eglDestroyImageKHR");
    *(void **)&hw.image_texture = egl_proc("glEGLImageTargetTexture2DOES");
#define GLES(field, name) *(void **)&hw.field = dlsym(gles, name)
    GLES(gen_textures, "glGenTextures");
    GLES(delete_textures, "glDeleteTextures");
    GLES(bind_texture, "glBindTexture");
    GLES(gen_framebuffers, "glGenFramebuffers");
    GLES(delete_framebuffers, "glDeleteFramebuffers");
    GLES(bind_framebuffer, "glBindFramebuffer");
    GLES(framebuffer_texture, "glFramebufferTexture2D");
    GLES(check_framebuffer, "glCheckFramebufferStatus");
    GLES(blit, "glBlitFramebuffer");
    GLES(get_integer, "glGetIntegerv");
    GLES(is_enabled, "glIsEnabled");
    GLES(enable, "glEnable");
    GLES(disable, "glDisable");
    GLES(finish, "glFinish");
    GLES(fence_sync, "glFenceSync");
    GLES(client_wait, "glClientWaitSync");
    GLES(delete_sync, "glDeleteSync");
    GLES(flush, "glFlush");
    GLES(scissor, "glScissor");
    GLES(clear_color, "glClearColor");
    GLES(clear, "glClear");
    GLES(get_float, "glGetFloatv");
    GLES(get_boolean, "glGetBooleanv");
    GLES(color_mask, "glColorMask");
    void **all = (void **)&hw.allocate;
    for (void **f = all; f < (void **)&hw.xcb; f++)
        if (!*f) { fprintf(stderr, "mgx11: a hardware buffer function is missing (%d)\n", (int)(f - all)); return 0; }
    hw.xcb = xcb_connect(NULL, NULL);
    const xcb_query_extension_reply_t *dri3 = xcb_get_extension_data(hw.xcb, &xcb_dri3_id);
    const xcb_query_extension_reply_t *present = xcb_get_extension_data(hw.xcb, &xcb_present_id);
    if (xcb_connection_has_error(hw.xcb) || !dri3 || !dri3->present || !present || !present->present) {
        fprintf(stderr, "mgx11: no DRI3 or Present in the X server\n");
        return 0;
    }
    hw.ready = 1;
    return 1;
}

static void release_buffers(struct window_surface *s)
{
    for (int i = 0; i < 3; i++) {
        if (s->pixmaps[i]) xcb_free_pixmap(hw.xcb, s->pixmaps[i]);
        if (s->framebuffers[i]) hw.delete_framebuffers(1, &s->framebuffers[i]);
        if (s->textures[i]) hw.delete_textures(1, &s->textures[i]);
        if (s->images[i]) hw.destroy_image(s->display, s->images[i]);
        if (s->buffers[i]) hw.release(s->buffers[i]);
        if (s->fences[i]) hw.delete_sync(s->fences[i]);
        s->fences[i] = NULL;
        s->pixmaps[i] = s->framebuffers[i] = s->textures[i] = 0;
        s->images[i] = s->buffers[i] = NULL;
    }
    if (hw.xcb) xcb_flush(hw.xcb);
}

static int make_buffers(struct window_surface *s)
{
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(hw.xcb, xcb_get_geometry(hw.xcb, s->window), NULL);
    if (!geometry) return 0;
    uint8_t depth = geometry->depth;
    free(geometry);
    int texture = 0, draw = 0;
    hw.get_integer(GL_TEXTURE_BINDING_2D, &texture);
    hw.get_integer(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    int made = 1;
    for (int i = 0; i < 3 && made; i++) {
        /* The X server copies a frame it doesn't flip with the CPU: for the GPU only, Mali's allocator compresses
         * the buffer (AFBC) and the copy showed its tiles as noise on a Mali-G52, and Google's on PowerVR refuses
         * the lock (as with the app's own Vulkan swapchain, runtime/hybris/patches/0006). CPU access makes them
         * plain pixels. */
        AHardwareBuffer_Desc desc = {(uint32_t)s->width, (uint32_t)s->height, 1, AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM,
                                     AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                                         AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                                     0, 0, 0};
        AHardwareBuffer *buffer = NULL;
        if (hw.allocate(&desc, &buffer)) { fprintf(stderr, "mgx11: no hardware buffer\n"); made = 0; break; }
        s->buffers[i] = buffer;
        hw.describe(buffer, &desc);
        s->stride = desc.stride;
        const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        s->images[i] = hw.create_image(s->display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, hw.client_buffer(buffer),
                                       attributes);
        if (!s->images[i]) { fprintf(stderr, "mgx11: no EGLImage\n"); made = 0; break; }
        hw.gen_textures(1, &s->textures[i]);
        hw.bind_texture(GL_TEXTURE_2D, s->textures[i]);
        hw.image_texture(GL_TEXTURE_2D, s->images[i]);
        hw.gen_framebuffers(1, &s->framebuffers[i]);
        hw.bind_framebuffer(GL_DRAW_FRAMEBUFFER, s->framebuffers[i]);
        hw.framebuffer_texture(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->textures[i], 0);
        unsigned status = hw.check_framebuffer(GL_DRAW_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "mgx11: framebuffer 0x%x\n", status); made = 0; break; }
        /* The server reads the buffer from the socket while it handles the request: keep our end until then. */
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) { made = 0; break; }
        if (hw.send(buffer, pair[0])) { close(pair[0]); close(pair[1]); made = 0; break; }
        s->pixmaps[i] = xcb_generate_id(hw.xcb);
        xcb_generic_error_t *error = xcb_request_check(hw.xcb, xcb_dri3_pixmap_from_buffers_checked(
            hw.xcb, s->pixmaps[i], s->window, 1, s->width, s->height, desc.stride * 4, 0, 0, 0, 0, 0, 0, 0, depth, 32,
            AHARDWAREBUFFER_SOCKET_FD, &pair[1]));
        close(pair[0]);
        if (error) {
            fprintf(stderr, "mgx11: DRI3 refused the buffer (error %d)\n", error->error_code);
            free(error);
            s->pixmaps[i] = 0;
            made = 0;
        }
    }
    hw.bind_texture(GL_TEXTURE_2D, texture);
    hw.bind_framebuffer(GL_DRAW_FRAMEBUFFER, draw);
    s->waiting = -1;
    if (!made) release_buffers(s);
    else fprintf(stderr, "mgx11: %dx%d through three hardware buffers over DRI3 (stride %u)\n", s->width, s->height,
                 s->stride);
    return made;
}


/* The app's FPS counter (GALLIUM_HUD) is Mesa's, which isn't in this path: the frame rate as seven-segment digits
 * in the top right corner, from scissored clears into the hardware buffer (its first row is the screen's top). */
#define GL_SCISSOR_BOX 0x0C10
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_WRITEMASK 0x0C23
static void box(int x, int y, int w, int h)
{
    hw.scissor(x, y, w, h);
    hw.clear(GL_COLOR_BUFFER_BIT);
}
static void draw_fps(struct window_surface *s)
{
    static const unsigned char segments[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    char digits[8];
    int count = snprintf(digits, sizeof digits, "%u", s->fps);
    int u = s->height / 160 > 2 ? s->height / 160 : 2;
    int x = s->width - count * 4 * u - 3 * u - s->height / 40, y = s->height / 40;
    int scissor_box[4];
    float clear_color[4];
    unsigned char mask[4];
    hw.get_integer(GL_SCISSOR_BOX, scissor_box);
    hw.get_float(GL_COLOR_CLEAR_VALUE, clear_color);
    hw.get_boolean(GL_COLOR_WRITEMASK, mask);
    hw.color_mask(1, 1, 1, 1);
    hw.enable(GL_SCISSOR_TEST);
    hw.clear_color(0, 0, 0, 1);
    box(x - u, y - u, count * 4 * u + u, 7 * u);
    hw.clear_color(1, 0.85f, 0.2f, 1);
    for (int d = 0; d < count; d++, x += 4 * u) {
        unsigned bits = segments[digits[d] - '0'];
        if (bits & 1) box(x, y, 3 * u, u);
        if (bits & 2) box(x + 2 * u, y, u, 3 * u);
        if (bits & 4) box(x + 2 * u, y + 2 * u, u, 3 * u);
        if (bits & 8) box(x, y + 4 * u, 3 * u, u);
        if (bits & 16) box(x, y + 2 * u, u, 3 * u);
        if (bits & 32) box(x, y, u, 3 * u);
        if (bits & 64) box(x, y + 2 * u, 3 * u, u);
    }
    hw.scissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
    hw.clear_color(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    hw.color_mask(mask[0], mask[1], mask[2], mask[3]);
}

/* The pbuffer, flipped (GL's rows go up, the buffer's down), into the next hardware buffer, which the X server shows. */
static int present_dri3(struct window_surface *s)
{
    if (!s->dri3) s->dri3 = load_hw() && make_buffers(s) ? 1 : -1;
    if (s->dri3 < 0) return 0;
    double start = now_ms();
    int i = s->next_buffer++ % 3;
    int read = 0, draw = 0;
    hw.get_integer(GL_READ_FRAMEBUFFER_BINDING, &read);
    hw.get_integer(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    unsigned char scissor = hw.is_enabled(GL_SCISSOR_TEST);
    hw.bind_framebuffer(GL_READ_FRAMEBUFFER, 0);
    hw.bind_framebuffer(GL_DRAW_FRAMEBUFFER, s->framebuffers[i]);
    if (scissor) hw.disable(GL_SCISSOR_TEST);
    hw.blit(0, 0, s->width, s->height, 0, s->height, s->width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (start - s->second_start >= 1000) {
        s->fps = s->second_frames;
        s->second_frames = 0;
        s->second_start = start;
    }
    s->second_frames++;
    if (getenv("GALLIUM_HUD")) draw_fps(s);
    if (scissor) hw.enable(GL_SCISSOR_TEST);
    else hw.disable(GL_SCISSOR_TEST);
    hw.bind_framebuffer(GL_READ_FRAMEBUFFER, read);
    hw.bind_framebuffer(GL_DRAW_FRAMEBUFFER, draw);
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
    /* Each frame is shown one later, once the GPU has finished it, so the CPU works on the next frame while the GPU
     * draws (patches/mobileglues-discard-new-storage.patch makes that safe): on a Mali-G52, 45-60 FPS instead of
     * 20-30. MGX11_OVERLAP=0 waits for the GPU and shows the frame at once, which showed more frames where the GPU is
     * the limit: 57 FPS on screen against 53 on the vivo. MGX11_VSYNC=1 lets the X server hold a frame for its next
     * vblank, where a newer one replaces it: 30 FPS on screen of 60. */
    static int finish = -1, vsync = -1;
    if (finish < 0) finish = getenv("MGX11_OVERLAP") && !strcmp(getenv("MGX11_OVERLAP"), "0");
    if (vsync < 0) vsync = getenv("MGX11_VSYNC") && !strcmp(getenv("MGX11_VSYNC"), "1");
    const uint32_t options = vsync ? XCB_PRESENT_OPTION_NONE : XCB_PRESENT_OPTION_ASYNC;
    int shown;
    if (finish) {
        hw.finish();
        shown = i;
    } else {
        if (s->fences[i]) hw.delete_sync(s->fences[i]);
        s->fences[i] = hw.fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        hw.flush();
        /* Last frame's buffer, now that the GPU has finished it. */
        shown = s->waiting;
        s->waiting = i;
        if (shown >= 0 && s->fences[shown])
            hw.client_wait(s->fences[shown], GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ULL);
    }
    double blitted = now_ms();
    if (shown >= 0) {
        xcb_present_pixmap(hw.xcb, s->window, s->pixmaps[shown], ++s->serial, 0, 0, 0, 0, 0, 0, 0, options,
                           0, 0, 0, 0, NULL);
        xcb_flush(hw.xcb);
    }
    s->blit_ms += blitted - start;
    s->present_ms += now_ms() - blitted;
    if (++s->frames % 300 == 0) {
        fprintf(stderr, "mgx11: %dx%d, last 300 frames: GPU copy %.1f ms, wait and present %.2f ms a frame\n",
                s->width, s->height, s->blit_ms / 300, s->present_ms / 300);
        s->blit_ms = s->present_ms = 0;
    }
    return 1;
}

static void present(struct window_surface *s)
{
    if (present_dri3(s)) return;
    static getiv_t getiv;
    static bind_t bind_framebuffer, bind_buffer;
    static read_t read_pixels;
    static store_t store;
    if (!read_pixels) {
        getiv = (getiv_t)real("glGetIntegerv");
        bind_framebuffer = (bind_t)real("glBindFramebuffer");
        bind_buffer = (bind_t)real("glBindBuffer");
        read_pixels = (read_t)real("glReadPixels");
        store = (store_t)real("glPixelStorei");
    }
    double start = now_ms();
    int framebuffer = 0, pack_buffer = 0, alignment = 4, row_length = 0;
    getiv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
    getiv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    getiv(GL_PACK_ALIGNMENT, &alignment);
    getiv(GL_PACK_ROW_LENGTH, &row_length);
    bind_framebuffer(GL_READ_FRAMEBUFFER, 0);
    bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
    store(GL_PACK_ALIGNMENT, 4);
    store(GL_PACK_ROW_LENGTH, 0);
    read_pixels(0, 0, s->width, s->height, GL_RGBA, GL_UNSIGNED_BYTE, s->rgba);
    bind_framebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    bind_buffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
    store(GL_PACK_ALIGNMENT, alignment);
    store(GL_PACK_ROW_LENGTH, row_length);
    /* GL's rows go up, X's down; RGBA bytes become the X server's little-endian BGRX. */
    for (int y = 0; y < s->height; y++) {
        const uint8_t *from = s->rgba + (size_t)(s->height - 1 - y) * s->width * 4;
        uint32_t *to = (uint32_t *)(s->image->data + (size_t)y * s->image->bytes_per_line);
        for (int i = 0; i < s->width; i++)
            to[i] = (uint32_t)from[i * 4] << 16 | (uint32_t)from[i * 4 + 1] << 8 | from[i * 4 + 2] | 0xff000000u;
    }
    double copied = now_ms();
    XPutImage(x, s->window, s->gc, s->image, 0, 0, 0, 0, s->width, s->height);
    XFlush(x);
    double put = now_ms();
    s->copy_ms += copied - start;
    s->put_ms += put - copied;
    if (++s->frames % 300 == 0) {
        fprintf(stderr, "mgx11: %ux%u, last 300 frames: read+convert %.1f ms, XPutImage %.1f ms a frame\n",
                s->width, s->height, s->copy_ms / 300, s->put_ms / 300);
        s->copy_ms = s->put_ms = 0;
    }
}

/* A window that changed size gets a pbuffer of the new size, current in place of the old one. */
static void follow_size(struct window_surface *s)
{
    if (s->frames % 15) return;
    XWindowAttributes geometry;
    if (!XGetWindowAttributes(x, s->window, &geometry) || geometry.width <= 0 || geometry.height <= 0 ||
        (geometry.width == s->width && geometry.height == s->height)) return;
    REAL(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
    REAL(EGLBoolean, eglDestroySurface, (EGLDisplay, EGLSurface))
    REAL(EGLContext, eglGetCurrentContext, (void))
    EGLSurface resized = pbuffer(s->display, s->config, geometry.width, geometry.height);
    if (resized == EGL_NO_SURFACE) return;
    real_eglMakeCurrent(s->display, resized, resized, real_eglGetCurrentContext());
    real_eglDestroySurface(s->display, s->pbuffer);
    s->pbuffer = resized;
    s->width = geometry.width;
    s->height = geometry.height;
    allocate(s);
    if (s->dri3 > 0) release_buffers(s);
    s->dri3 = 0;
    fprintf(stderr, "mgx11: window now %dx%d\n", s->width, s->height);
}

EXPORT EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    REAL(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface))
    struct window_surface *s = ours(surface);
    if (!s) return real_eglSwapBuffers(display, surface);
    present(s);
    EGLBoolean result = real_eglSwapBuffers(display, s->pbuffer);
    follow_size(s);
    return result;
}

#define PASS(ret, name, params, args) \
    EXPORT ret name params { REAL(ret, name, params) return real_##name args; }
PASS(EGLBoolean, eglInitialize, (EGLDisplay d, EGLint *a, EGLint *b), (d, a, b))
PASS(EGLBoolean, eglTerminate, (EGLDisplay d), (d))
PASS(EGLContext, eglCreateContext, (EGLDisplay d, EGLConfig c, EGLContext s, const EGLint *a), (d, c, s, a))
PASS(EGLBoolean, eglDestroyContext, (EGLDisplay d, EGLContext c), (d, c))
PASS(EGLSurface, eglCreatePbufferSurface, (EGLDisplay d, EGLConfig c, const EGLint *a), (d, c, a))
PASS(EGLBoolean, eglSwapInterval, (EGLDisplay d, EGLint i), (d, i))
PASS(EGLBoolean, eglWaitNative, (EGLint e), (e))
PASS(EGLBoolean, eglWaitGL, (void), ())
PASS(EGLBoolean, eglBindAPI, (EGLenum a), (a))
PASS(EGLenum, eglQueryAPI, (void), ())
PASS(EGLint, eglGetError, (void), ())
PASS(EGLBoolean, eglGetConfigs, (EGLDisplay d, EGLConfig *c, EGLint s, EGLint *n), (d, c, s, n))
PASS(EGLContext, eglGetCurrentContext, (void), ())
PASS(EGLDisplay, eglGetCurrentDisplay, (void), ())
PASS(EGLBoolean, eglReleaseThread, (void), ())
PASS(EGLBoolean, eglQueryContext, (EGLDisplay d, EGLContext c, EGLint a, EGLint *v), (d, c, a, v))

EXPORT __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name)
{
    static const struct { const char *name; void *fn; } here[] = {
        {"eglGetDisplay", (void *)eglGetDisplay}, {"eglQueryString", (void *)eglQueryString},
        {"eglChooseConfig", (void *)eglChooseConfig}, {"eglGetConfigAttrib", (void *)eglGetConfigAttrib},
        {"eglCreateWindowSurface", (void *)eglCreateWindowSurface},
        {"eglDestroySurface", (void *)eglDestroySurface}, {"eglMakeCurrent", (void *)eglMakeCurrent},
        {"eglGetCurrentSurface", (void *)eglGetCurrentSurface}, {"eglQuerySurface", (void *)eglQuerySurface},
        {"eglSurfaceAttrib", (void *)eglSurfaceAttrib}, {"eglSwapBuffers", (void *)eglSwapBuffers},
        {"eglGetProcAddress", (void *)eglGetProcAddress},
    };
    for (size_t i = 0; i < sizeof here / sizeof *here; i++)
        if (!strcmp(name, here[i].name)) return (__eglMustCastToProperFunctionPointerType)here[i].fn;
    REAL(__eglMustCastToProperFunctionPointerType, eglGetProcAddress, (const char *))
    return real_eglGetProcAddress(name);
}
