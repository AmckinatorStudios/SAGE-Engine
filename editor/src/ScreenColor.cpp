#include "ScreenColor.h"

#include <GLFW/glfw3.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__) && __has_include(<X11/Xlib.h>)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#define SAGE_SCREENCOLOR_X11 1
#endif

namespace sage::editor::screencolor {

#if defined(_WIN32)

bool GlobalAvailable() { return true; }

bool SampleAtCursor(float rgb[3], int* x, int* y) {
    POINT p{};
    if (!GetCursorPos(&p)) return false;
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const COLORREF c = GetPixel(dc, p.x, p.y);
    ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID) return false;
    rgb[0] = GetRValue(c) / 255.0f;
    rgb[1] = GetGValue(c) / 255.0f;
    rgb[2] = GetBValue(c) / 255.0f;
    if (x) *x = p.x;
    if (y) *y = p.y;
    return true;
}

bool GlobalMouseDown(int button) {
    // Кнопки, переставленные для левши, — это «главная» и «вторая», а не
    // физическая левая: GetAsyncKeyState отдаёт физическую.
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const int vk = (button == 0) != swapped ? VK_LBUTTON : VK_RBUTTON;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool GlobalEscapeDown() { return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0; }

#elif defined(SAGE_SCREENCOLOR_X11)

namespace {
// Таблица функций libX11: берём ровно то, что нужно, во время работы.
struct X11 {
    bool Tried = false, Ok = false;
    Display* Dpy = nullptr;
    Display* (*OpenDisplay)(const char*) = nullptr;
    Bool (*QueryPointer)(Display*, Window, Window*, Window*, int*, int*, int*, int*, unsigned*) = nullptr;
    XImage* (*GetImage)(Display*, Drawable, int, int, unsigned, unsigned, unsigned long, int) = nullptr;
    Window (*RootWindow_)(Display*, int) = nullptr;
    int (*DefaultScreen_)(Display*) = nullptr;
    Bool (*QueryKeymap)(Display*, char[32]) = nullptr;
    KeyCode (*KeysymToKeycode)(Display*, KeySym) = nullptr;
};

X11& Lib() {
    static X11 x;
    if (x.Tried) return x;
    x.Tried = true;
    // Под Wayland у GLFW своё окно, а XWayland (если он есть) чужие окна не
    // показывает — читать там было бы нечего, и честнее сказать «нельзя».
    if (glfwGetPlatform() != GLFW_PLATFORM_X11) return x;
    void* h = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!h) h = dlopen("libX11.so", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return x;
    x.OpenDisplay = (decltype(x.OpenDisplay))dlsym(h, "XOpenDisplay");
    x.QueryPointer = (decltype(x.QueryPointer))dlsym(h, "XQueryPointer");
    x.GetImage = (decltype(x.GetImage))dlsym(h, "XGetImage");
    x.RootWindow_ = (decltype(x.RootWindow_))dlsym(h, "XRootWindow");
    x.DefaultScreen_ = (decltype(x.DefaultScreen_))dlsym(h, "XDefaultScreen");
    x.QueryKeymap = (decltype(x.QueryKeymap))dlsym(h, "XQueryKeymap");
    x.KeysymToKeycode = (decltype(x.KeysymToKeycode))dlsym(h, "XKeysymToKeycode");
    if (!x.OpenDisplay || !x.QueryPointer || !x.GetImage || !x.RootWindow_ || !x.DefaultScreen_)
        return x;
    // Своё соединение, а не соединение GLFW: запросы пипетки не должны
    // вклиниваться в очередь событий окна.
    x.Dpy = x.OpenDisplay(nullptr);
    x.Ok = x.Dpy != nullptr;
    return x;
}

bool Pointer(int* rx, int* ry, unsigned* mask) {
    X11& x = Lib();
    if (!x.Ok) return false;
    Window root = x.RootWindow_(x.Dpy, x.DefaultScreen_(x.Dpy)), r, c;
    int wx, wy;
    return x.QueryPointer(x.Dpy, root, &r, &c, rx, ry, &wx, &wy, mask) != 0;
}

// Доля канала по маске визуала: 0xff0000 -> сдвиг 16, 8 бит.
float Channel(unsigned long pixel, unsigned long mask) {
    if (!mask) return 0.0f;
    int shift = 0;
    while (!((mask >> shift) & 1ul)) ++shift;
    const unsigned long max = mask >> shift;
    return (float)((pixel & mask) >> shift) / (float)max;
}
} // namespace

bool GlobalAvailable() { return Lib().Ok; }

bool SampleAtCursor(float rgb[3], int* x, int* y) {
    X11& lib = Lib();
    int rx = 0, ry = 0;
    unsigned mask = 0;
    if (!Pointer(&rx, &ry, &mask)) return false;
    const Window root = lib.RootWindow_(lib.Dpy, lib.DefaultScreen_(lib.Dpy));
    XImage* img = lib.GetImage(lib.Dpy, root, rx, ry, 1, 1, AllPlanes, ZPixmap);
    if (!img) return false;
    const unsigned long px = XGetPixel(img, 0, 0);
    rgb[0] = Channel(px, img->red_mask);
    rgb[1] = Channel(px, img->green_mask);
    rgb[2] = Channel(px, img->blue_mask);
    XDestroyImage(img);
    if (x) *x = rx;
    if (y) *y = ry;
    return true;
}

bool GlobalMouseDown(int button) {
    int rx, ry;
    unsigned mask = 0;
    if (!Pointer(&rx, &ry, &mask)) return false;
    return (mask & (button == 0 ? Button1Mask : Button3Mask)) != 0;
}

bool GlobalEscapeDown() {
    X11& x = Lib();
    if (!x.Ok || !x.QueryKeymap || !x.KeysymToKeycode) return false;
    char keys[32] = {};
    x.QueryKeymap(x.Dpy, keys);
    const KeyCode code = x.KeysymToKeycode(x.Dpy, 0xff1b /* XK_Escape */);
    return code && (keys[code / 8] & (1 << (code % 8)));
}

#else

bool GlobalAvailable() { return false; }
bool SampleAtCursor(float*, int*, int*) { return false; }
bool GlobalMouseDown(int) { return false; }
bool GlobalEscapeDown() { return false; }

#endif

} // namespace sage::editor::screencolor
