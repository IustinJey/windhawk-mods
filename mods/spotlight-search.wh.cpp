// ==WindhawkMod==
// @id              spotlight-search
// @name            Spotlight Search
// @description     A gorgeous macOS-style Spotlight launcher for Windows — frosted glass, fluid 60fps animations, instant app & settings search.
// @version         1.0.0
// @author          Jey
// @homepage        https://www.instagram.com/iustinjey
// @include         explorer.exe
// @compilerOptions -lgdi32 -lgdiplus -ldwmapi -lshell32 -lcomctl32 -lole32 -lwinmm
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Spotlight Search

A beautiful, blazing-fast macOS-style Spotlight launcher for Windows. Press
your shortcut, start typing, and instantly open any app, game, or settings page
— wrapped in real frosted glass with buttery 60fps animations.

![Demo](https://akfggpfvpiozrwcvvyxi.supabase.co/storage/v1/object/public/landing-assets/email-assets/1780213922995-screenrecording2026-05-31102943-ezgif.com-video-to-gif-converter.gif)

## Features
- **Searches everything** — every installed app (Win32, Microsoft Store, and
  games from Steam and other launchers) plus Windows Settings pages.
- **Real frosted-glass acrylic** with a soft ambient shadow.
- **Fluid, precisely-paced 60fps animations** — the bar springs open from a
  circle, results glide and stagger in, and the caret slides as you type.
- **Fully keyboard-driven** — arrows to navigate, Enter to launch, Esc to
  close, Ctrl+A to select. Clicking away or switching apps dismisses it.
- **Pick your own hotkey** in the mod settings.

## Coming soon
- Full customization and themes (colors, blur, sizes, light/dark)
- File and folder search
- Quick calculator and web search
- Recent and frequent apps

---
Made with ❤️ by Jey — follow [@iustinjey](https://www.instagram.com/iustinjey) for updates.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hotkey: alt_space
  $name: Activation shortcut
  $description: The keyboard shortcut that opens Spotlight Search.
  $options:
  - alt_space: Alt + Space
  - ctrl_space: Ctrl + Space
  - win_space: Win + Space
  - ctrl_alt_space: Ctrl + Alt + Space
  - ctrl_shift_space: Ctrl + Shift + Space
  - alt_s: Alt + S
  - win_s: Win + S
  - alt_space_caps: Caps Lock
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <objidl.h>
#include <timeapi.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <thread>
#include <cwctype>
#include <cmath>

using namespace Gdiplus;

// ── Layout (logical px) ───────────────────────────────────────────────────────
constexpr int   SHADOW       = 80;   // padding around pill for soft ambient shadow
constexpr float OW           = 800;  // full pill width
constexpr float SEARCH_H     = 102;  // search bar height (also circle diameter)
constexpr float RESULT_H     = 72;
constexpr int   MAX_RES      = 7;
constexpr float ICON_SZ      = 50;
constexpr float ROW_GAP      = 12;   // gap between search row and first result
constexpr float BOTTOM_PAD   = 14;
constexpr float LEFT_PAD     = 40;
constexpr float ICON_TXT_GAP = 22;
constexpr float CORNER       = SEARCH_H / 2;  // fully-rounded
constexpr float CIRCLE_W     = SEARCH_H;      // collapsed circle width
constexpr int   ICON_SRC     = 96;   // resolution to extract icons at

// Largest the layered surface can ever need (full pill + spring overshoot +
// all rows + shadow margins). Allocated once and reused every frame.
constexpr int   MAXW = (int)OW + 140 + SHADOW * 2;
constexpr int   MAXH = (int)(SEARCH_H + ROW_GAP + MAX_RES * RESULT_H + BOTTOM_PAD)
                       + SHADOW * 2 + 40;

// ── App entry ─────────────────────────────────────────────────────────────────
struct AppEntry {
    std::wstring     name;
    PIDLIST_ABSOLUTE pidl     = nullptr;  // shell item (apps): launch + icon
    std::wstring     launch;              // alt launch string (ms-settings: etc.)
    std::wstring     iconPath;            // file to pull an icon from (settings)
    Bitmap*          gbmp     = nullptr;  // lazy clean ARGB icon
    bool             tried    = false;    // already attempted icon load
};

// ── Animation state ───────────────────────────────────────────────────────────
enum class St { Hidden, Opening, Open, Closing };

static std::vector<AppEntry>        g_allApps;
static std::vector<const AppEntry*> g_results;
static std::wstring g_query;
static int          g_sel       = 0;
static bool         g_selectAll = false;  // Ctrl+A: whole query selected

// configurable activation hotkey (from mod settings)
enum { HK_ALT = 1, HK_CTRL = 2, HK_WIN = 4, HK_SHIFT = 8 };
static UINT g_hkMods = HK_ALT;
static UINT g_hkVk   = VK_SPACE;

static HWND   g_wnd       = nullptr;
static HHOOK  g_kbHook    = nullptr;
static HHOOK  g_mouseHook = nullptr;
static RECT   g_pillScreen = {};          // pill bounds in screen coords (for click-out)
static HDC    g_screen    = nullptr;
static std::thread g_thread;
static ULONG_PTR   g_gdipTok = 0;

static St     g_state    = St::Hidden;
static double g_tOpen     = 0;   // QPC seconds at open
static double g_tClose    = 0;   // QPC seconds at close start
static double g_lastFrame = 0;

// smoothed animated values
static double g_heightCur = SEARCH_H;
static double g_selY      = 0;
static double g_selAlpha  = 0;
static double g_caretX    = -1;  // animated caret x (-1 = snap on next frame)
static double g_caretTgt  = 0;   // caret target x
static double g_lastType  = 0;   // time of last keystroke (keeps fps up to animate)

// frosted-glass backdrop: a blurred snapshot of the screen behind the window
static Bitmap* g_frost      = nullptr;
constexpr int  FROST_SCALE  = 8;   // pre-downscale before the box blur (cheaper + frostier)

// cached GDI+ fonts (creating these every frame was a real cost)
static FontFamily* g_ff   = nullptr;
static Font*       g_fBig = nullptr;
static Font*       g_fSm  = nullptr;

// cached render surface — allocated ONCE at max size, reused every frame
static HBITMAP  g_dib    = nullptr;
static HDC      g_memDC  = nullptr;
static HBITMAP  g_oldBmp = nullptr;
static void*    g_dibBits = nullptr;
static Bitmap*  g_surface = nullptr;
static Graphics* g_gfx   = nullptr;
static bool     g_hiRes  = false;  // 1ms timer period active?

// Per-row animation state, keyed by app identity, so narrowing the query
// reflows the surviving rows instead of restarting every row's entrance.
struct Row {
    const AppEntry* app;
    double y;       // animated center Y (pill-local)
    double alpha;   // animated 0..1
    double tAlpha;  // target alpha (0 = leaving)
    int    index;   // target slot (-1 = leaving)
    double born;    // creation time (for entrance stagger)
};
static std::vector<Row> g_rows;

constexpr UINT WM_TOGGLE  = WM_USER + 1;
constexpr UINT WM_DISMISS = WM_USER + 2;  // posted by mouse hook on outside click

// ── Timing ────────────────────────────────────────────────────────────────────
static double Now() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

// ── Easing ────────────────────────────────────────────────────────────────────
static double clamp01(double t)      { return t < 0 ? 0 : (t > 1 ? 1 : t); }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }
static double easeOutCubic(double t) { double u = 1 - t; return 1 - u * u * u; }
static double easeInCubic(double t)  { return t * t * t; }
static double easeOutExpo(double t)  { return t >= 1 ? 1 : 1 - std::pow(2, -10 * t); }
static double easeOutBack(double t) {
    const double c1 = 1.70158, c3 = c1 + 1;
    double u = t - 1;
    return 1 + c3 * u * u * u + c1 * u * u;
}
// frame-rate-independent exponential smoothing
static double smooth(double cur, double target, double dt, double speed) {
    return cur + (target - cur) * (1 - std::exp(-dt * speed));
}

// GUIDs defined inline so we don't depend on libuuid (which Windhawk's linker
// doesn't pull in).
static const GUID IID_ShellItemImageFactory =
    {0xbcc18b79, 0xba16, 0x442f, {0x80, 0xc4, 0x8a, 0x59, 0xc3, 0x0c, 0x46, 0x3b}};
static const GUID IID_IShellFolder_ =
    {0x000214e6, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID IID_IEnumIDList_ =
    {0x000214f2, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// ── Icon loading (clean transparent ARGB, no shortcut overlay) ────────────────
// Copy a 32-bit HBITMAP into a GDI+ bitmap while preserving the alpha channel.
// (Bitmap::FromHBITMAP discards alpha, which is what caused the black boxes.)
static Bitmap* HBitmapToARGB(HBITMAP hbmp) {
    BITMAP bm = {};
    if (!GetObjectW(hbmp, sizeof(bm), &bm)) return nullptr;
    int w = bm.bmWidth, h = bm.bmHeight;
    if (w <= 0 || h <= 0) return nullptr;

    Bitmap* out = new Bitmap(w, h, PixelFormat32bppARGB);
    Rect rc(0, 0, w, h);
    BitmapData bd = {};
    if (out->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Ok) {
        delete out;
        return nullptr;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;  // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    GetDIBits(dc, hbmp, 0, h, bd.Scan0, &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    out->UnlockBits(&bd);
    return out;
}

// Clean transparent ARGB icon for any app/setting, from its PIDL or icon file.
static Bitmap* LoadAppIcon(const AppEntry& e) {
    IShellItemImageFactory* factory = nullptr;
    HRESULT hr = E_FAIL;
    if (e.pidl)
        hr = SHCreateItemFromIDList(e.pidl, IID_ShellItemImageFactory,
                                    (void**)&factory);
    else if (!e.iconPath.empty())
        hr = SHCreateItemFromParsingName(e.iconPath.c_str(), nullptr,
                                         IID_ShellItemImageFactory,
                                         (void**)&factory);
    if (FAILED(hr) || !factory) return nullptr;

    SIZE    sz   = {ICON_SRC, ICON_SRC};
    HBITMAP hbmp = nullptr;
    hr = factory->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp);
    factory->Release();
    if (FAILED(hr) || !hbmp) return nullptr;

    Bitmap* bmp = HBitmapToARGB(hbmp);
    DeleteObject(hbmp);
    return bmp;
}

// ── App / settings catalog ────────────────────────────────────────────────────
static std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

static void FreeApps() {
    for (auto& a : g_allApps) {
        if (a.pidl) { ILFree(a.pidl); a.pidl = nullptr; }
        if (a.gbmp) { delete a.gbmp; a.gbmp = nullptr; }
    }
    g_allApps.clear();
}

// Common Windows Settings pages (deep links). Icon comes from SystemSettings.exe.
static void AddSettings(std::set<std::wstring>& seen) {
    struct S { const wchar_t* name; const wchar_t* uri; };
    static const S kSettings[] = {
        {L"Settings",                 L"ms-settings:"},
        {L"System",                   L"ms-settings:system"},
        {L"Display",                  L"ms-settings:display"},
        {L"Sound",                    L"ms-settings:sound"},
        {L"Notifications",            L"ms-settings:notifications"},
        {L"Power & battery",          L"ms-settings:powersleep"},
        {L"Storage",                  L"ms-settings:storagesense"},
        {L"Bluetooth & devices",      L"ms-settings:bluetooth"},
        {L"Mouse",                    L"ms-settings:mousetouchpad"},
        {L"Keyboard",                 L"ms-settings:typing"},
        {L"Network & internet",       L"ms-settings:network"},
        {L"Wi-Fi",                    L"ms-settings:network-wifi"},
        {L"VPN",                      L"ms-settings:network-vpn"},
        {L"Personalization",          L"ms-settings:personalization"},
        {L"Background",               L"ms-settings:personalization-background"},
        {L"Colors",                   L"ms-settings:colors"},
        {L"Themes",                   L"ms-settings:themes"},
        {L"Lock screen",              L"ms-settings:lockscreen"},
        {L"Taskbar",                  L"ms-settings:taskbar"},
        {L"Apps & features",          L"ms-settings:appsfeatures"},
        {L"Default apps",             L"ms-settings:defaultapps"},
        {L"Startup apps",             L"ms-settings:startupapps"},
        {L"Accounts",                 L"ms-settings:accounts"},
        {L"Date & time",              L"ms-settings:dateandtime"},
        {L"Language & region",        L"ms-settings:regionlanguage"},
        {L"Windows Update",           L"ms-settings:windowsupdate"},
        {L"Privacy & security",       L"ms-settings:privacy"},
        {L"Windows Security",         L"windowsdefender:"},
        {L"About",                    L"ms-settings:about"},
        {L"Developers",               L"ms-settings:developers"},
    };

    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring icon = std::wstring(win) +
        L"\\ImmersiveControlPanel\\SystemSettings.exe";

    for (auto& s : kSettings) {
        std::wstring key = ToLower(s.name);
        if (seen.count(key)) continue;
        seen.insert(key);
        AppEntry ae;
        ae.name     = s.name;
        ae.launch   = s.uri;
        ae.iconPath = icon;
        g_allApps.push_back(std::move(ae));
    }
}

// Enumerate the AppsFolder — the same catalog Start's search uses. Includes
// Win32 apps, Microsoft Store / UWP apps, and registered games (Steam, etc.).
static void LoadApps() {
    FreeApps();
    std::set<std::wstring> seen;
    int appCount = 0;

    PIDLIST_ABSOLUTE appsPidl = nullptr;
    HRESULT hr = SHParseDisplayName(L"shell:AppsFolder", nullptr,
                                    &appsPidl, 0, nullptr);
    Wh_Log(L"Spotlight: AppsFolder parse hr=0x%08lX", (unsigned long)hr);

    if (SUCCEEDED(hr) && appsPidl) {
        IShellFolder* desktop = nullptr;
        if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && desktop) {
            IShellFolder* folder = nullptr;
            HRESULT hb = desktop->BindToObject(appsPidl, nullptr,
                                               IID_IShellFolder_,
                                               (void**)&folder);
            Wh_Log(L"Spotlight: AppsFolder bind hr=0x%08lX", (unsigned long)hb);
            if (SUCCEEDED(hb) && folder) {
                IEnumIDList* en = nullptr;
                if (folder->EnumObjects(nullptr,
                        SHCONTF_NONFOLDERS | SHCONTF_FOLDERS | SHCONTF_INCLUDEHIDDEN,
                        &en) == S_OK && en) {
                    PITEMID_CHILD child = nullptr;
                    ULONG fetched = 0;
                    while (en->Next(1, &child, &fetched) == S_OK && fetched) {
                        PIDLIST_ABSOLUTE abs = ILCombine(appsPidl, child);
                        CoTaskMemFree(child);
                        if (!abs) continue;
                        PWSTR disp = nullptr;
                        if (SUCCEEDED(SHGetNameFromIDList(abs,
                                SIGDN_NORMALDISPLAY, &disp)) && disp) {
                            std::wstring name = disp;
                            CoTaskMemFree(disp);
                            std::wstring key = ToLower(name);
                            if (!key.empty() && !seen.count(key)) {
                                seen.insert(key);
                                AppEntry ae;
                                ae.name = std::move(name);
                                ae.pidl = abs;
                                g_allApps.push_back(std::move(ae));
                                appCount++;
                                abs = nullptr;  // ownership transferred
                            }
                        }
                        if (abs) ILFree(abs);
                    }
                    en->Release();
                }
                folder->Release();
            }
            desktop->Release();
        }
        ILFree(appsPidl);
    }

    AddSettings(seen);

    std::sort(g_allApps.begin(), g_allApps.end(),
              [](const AppEntry& a, const AppEntry& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });
    Wh_Log(L"Spotlight: %d apps from AppsFolder, %zu total",
           appCount, g_allApps.size());
}

static double RowCenterY(int index) {
    return SEARCH_H + ROW_GAP + index * RESULT_H + RESULT_H / 2;
}

static void FilterApps() {
    g_results.clear();
    if (!g_query.empty()) {
        std::wstring q = ToLower(g_query);
        for (auto& a : g_allApps) {
            if ((int)g_results.size() >= MAX_RES) break;
            std::wstring n = ToLower(a.name);
            if (n.size() >= q.size() && n.compare(0, q.size(), q) == 0)
                g_results.push_back(&a);
        }
        for (auto& a : g_allApps) {
            if ((int)g_results.size() >= MAX_RES) break;
            if (ToLower(a.name).find(q) == std::wstring::npos) continue;
            if (std::find(g_results.begin(), g_results.end(), &a) ==
                g_results.end())
                g_results.push_back(&a);
        }
    }
    g_sel = 0;

    // ── reconcile g_rows against the new result set ──
    double now = Now();
    // mark every existing row as leaving by default
    for (auto& r : g_rows) { r.index = -1; r.tAlpha = 0; }

    for (int i = 0; i < (int)g_results.size(); i++) {
        const AppEntry* app = g_results[i];
        auto it = std::find_if(g_rows.begin(), g_rows.end(),
                               [&](const Row& r) { return r.app == app; });
        if (it != g_rows.end()) {       // survivor — keep its motion, retarget
            it->index  = i;
            it->tAlpha = 1.0;
        } else {                        // newcomer — fades/slides in
            Row r;
            r.app    = app;
            r.index  = i;
            r.y      = RowCenterY(i);
            r.alpha  = 0.0;
            r.tAlpha = 1.0;
            r.born   = now;
            g_rows.push_back(r);
        }
    }
}

// ── Geometry helper ───────────────────────────────────────────────────────────
static void RoundRect(GraphicsPath& p, RectF r, REAL rad) {
    REAL m = (r.Width < r.Height ? r.Width : r.Height) / 2;
    if (rad > m) rad = m;
    REAL d = rad * 2;
    p.Reset();
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

static float ContentTargetH() {
    int n = (int)g_results.size();
    return n > 0 ? SEARCH_H + ROW_GAP + n * RESULT_H + BOTTOM_PAD : SEARCH_H;
}

// Is anything still moving? When fully settled we can stop redrawing every
// frame and only tick slowly (for the blinking caret) — that kills idle lag.
static bool Animating() {
    if (g_state != St::Open) return true;             // opening / closing
    if (Now() - g_lastType < 0.4) return true;        // caret slide after typing
    if (std::fabs(g_caretX - g_caretTgt) > 0.5) return true;
    if (std::fabs(g_heightCur - ContentTargetH()) > 0.5) return true;
    double selTarget = g_results.empty() ? 0.0 : 1.0;
    if (std::fabs(g_selAlpha - selTarget) > 0.01) return true;
    if (!g_results.empty() &&
        std::fabs(g_selY - RowCenterY(g_sel)) > 0.5) return true;
    for (auto& r : g_rows) {
        double ty = (r.index >= 0) ? RowCenterY(r.index) : r.y;
        if (std::fabs(r.y - ty) > 0.5)        return true;
        if (std::fabs(r.alpha - r.tAlpha) > 0.01) return true;
    }
    return false;
}

// ── Magnifier glyph ───────────────────────────────────────────────────────────
static void DrawMagnifier(Graphics& g, float cx, float cy, BYTE a) {
    Pen pen(Color(a, 200, 200, 205), 3.0f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    float r = 12;
    g.DrawEllipse(&pen, cx - r, cy - r, r * 2, r * 2);
    Pen handle(Color(a, 200, 200, 205), 3.5f);
    handle.SetStartCap(LineCapRound);
    handle.SetEndCap(LineCapRound);
    float o = r * 0.72f;
    g.DrawLine(&handle, cx + o, cy + o, cx + o + 8, cy + o + 8);
}

// Separable box blur (run once per open, on the small downscaled snapshot, so
// cost is negligible). A couple of passes approximates a Gaussian — this is
// what smears the desktop into soft acrylic color, not the tint.
static void BoxBlur(Bitmap* bmp, int radius, int passes) {
    int w = (int)bmp->GetWidth(), h = (int)bmp->GetHeight();
    if (w < 2 || h < 2 || radius < 1) return;

    Rect rc(0, 0, w, h);
    BitmapData bd = {};
    if (bmp->LockBits(&rc, ImageLockModeRead | ImageLockModeWrite,
                      PixelFormat32bppARGB, &bd) != Ok)
        return;
    BYTE* base   = (BYTE*)bd.Scan0;
    int   stride = bd.Stride;
    std::vector<BYTE> tmp((size_t)stride * h);

    for (int p = 0; p < passes; p++) {
        // horizontal: base -> tmp
        for (int y = 0; y < h; y++) {
            BYTE* row  = base + (size_t)y * stride;
            BYTE* trow = tmp.data() + (size_t)y * stride;
            for (int x = 0; x < w; x++) {
                int x0 = x - radius; if (x0 < 0) x0 = 0;
                int x1 = x + radius; if (x1 > w - 1) x1 = w - 1;
                int b = 0, gg = 0, r = 0, cnt = 0;
                for (int xx = x0; xx <= x1; xx++) {
                    BYTE* px = row + xx * 4;
                    b += px[0]; gg += px[1]; r += px[2]; cnt++;
                }
                BYTE* o = trow + x * 4;
                o[0] = (BYTE)(b / cnt); o[1] = (BYTE)(gg / cnt);
                o[2] = (BYTE)(r / cnt); o[3] = 255;
            }
        }
        // vertical: tmp -> base
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < h; y++) {
                int y0 = y - radius; if (y0 < 0) y0 = 0;
                int y1 = y + radius; if (y1 > h - 1) y1 = h - 1;
                int b = 0, gg = 0, r = 0, cnt = 0;
                for (int yy = y0; yy <= y1; yy++) {
                    BYTE* px = tmp.data() + (size_t)yy * stride + x * 4;
                    b += px[0]; gg += px[1]; r += px[2]; cnt++;
                }
                BYTE* o = base + (size_t)y * stride + x * 4;
                o[0] = (BYTE)(b / cnt); o[1] = (BYTE)(gg / cnt);
                o[2] = (BYTE)(r / cnt); o[3] = 255;
            }
        }
    }
    bmp->UnlockBits(&bd);
}

// ── Frosted backdrop ──────────────────────────────────────────────────────────
// Grab the desktop behind us, downscale it, then blur it into soft acrylic.
// Captured once per open (before our own window is shown).
static void CaptureFrost() {
    if (g_frost) { delete g_frost; g_frost = nullptr; }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int dw = sw / FROST_SCALE, dh = sh / FROST_SCALE;
    if (dw < 1 || dh < 1) return;

    HDC sdc = GetDC(nullptr);
    if (!sdc) return;
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, dw, dh);  // screen-format DDB
    if (mdc && bmp) {
        HBITMAP old = (HBITMAP)SelectObject(mdc, bmp);
        SetStretchBltMode(mdc, HALFTONE);
        SetBrushOrgEx(mdc, 0, 0, nullptr);
        StretchBlt(mdc, 0, 0, dw, dh, sdc, 0, 0, sw, sh, SRCCOPY);
        SelectObject(mdc, old);
        g_frost = Bitmap::FromHBITMAP(bmp, nullptr);  // opaque copy, no alpha
        if (g_frost && g_frost->GetLastStatus() != Ok) {
            delete g_frost;
            g_frost = nullptr;
        }
    }
    if (bmp) DeleteObject(bmp);
    if (mdc) DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);

    // smear it into soft acrylic (cheap now — the image is already tiny)
    if (g_frost) BoxBlur(g_frost, 5, 2);
}

// Allocate the layered surface once, at max size. Reused for every frame so we
// never pay for a per-frame DIB allocation again.
static bool EnsureBuffers() {
    if (g_dib) return true;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = MAXW;
    bi.bmiHeader.biHeight      = -MAXH;  // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_dib = CreateDIBSection(g_screen, &bi, DIB_RGB_COLORS, &g_dibBits, nullptr, 0);
    if (!g_dib) return false;
    g_memDC  = CreateCompatibleDC(g_screen);
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dib);

    g_surface = new Bitmap(MAXW, MAXH, MAXW * 4, PixelFormat32bppPARGB,
                           (BYTE*)g_dibBits);
    g_gfx = new Graphics(g_surface);
    g_gfx->SetSmoothingMode(SmoothingModeAntiAlias);
    g_gfx->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g_gfx->SetPixelOffsetMode(PixelOffsetModeHalf);
    return true;
}

// ── Render one frame ──────────────────────────────────────────────────────────
static void Render() {
    if (!EnsureBuffers()) return;
    double now = Now();
    double dt  = g_lastFrame ? (now - g_lastFrame) : 0.016;
    if (dt > 0.1) dt = 0.1;
    g_lastFrame = now;

    // ── timeline values ──
    double winAlpha = 1.0, closeScale = 1.0;
    double openEl = now - g_tOpen;

    if (g_state == St::Opening) {
        winAlpha = easeOutCubic(clamp01(openEl / 0.20));
        if (openEl > 0.65) g_state = St::Open;
    } else if (g_state == St::Closing) {
        double cf = clamp01((now - g_tClose) / 0.22);
        winAlpha   = 1 - easeInCubic(cf);
        closeScale = lerp(1.0, 0.90, easeInCubic(cf));
        if (cf >= 1.0) {
            ShowWindow(g_wnd, SW_HIDE);
            g_state = St::Hidden;
            if (g_hiRes) { timeEndPeriod(1); g_hiRes = false; }
            return;
        }
    }

    // width expansion: circle -> full pill (springy)
    double wFrac  = clamp01((openEl - 0.04) / 0.55);
    double widthT = (g_state == St::Closing) ? 1.0 : easeOutBack(wFrac);
    double pillW  = lerp(CIRCLE_W, OW, widthT);
    double content = clamp01((widthT - 0.45) / 0.55);  // when inner content shows

    // height follows results
    double targetH = ContentTargetH();
    g_heightCur = smooth(g_heightCur, targetH, dt, 16.0);
    double totalH = g_heightCur;

    // selection indicator glide
    if (!g_results.empty()) {
        double rowTop = SEARCH_H + ROW_GAP + g_sel * RESULT_H;
        g_selY     = smooth(g_selY, rowTop + RESULT_H / 2, dt, 22.0);
        g_selAlpha = smooth(g_selAlpha, 1.0, dt, 18.0);
    } else {
        g_selAlpha = smooth(g_selAlpha, 0.0, dt, 18.0);
    }

    // ── window placement ──
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int W  = (int)std::ceil(pillW) + SHADOW * 2;
    int H  = (int)std::ceil(totalH) + SHADOW * 2;
    int winX = (int)(sw / 2.0 - W / 2.0);
    int winY = (int)(sh * 0.27);

    // remember the visible pill bounds (screen coords) for click-outside detection
    g_pillScreen = { winX + SHADOW, winY + SHADOW,
                     winX + SHADOW + (int)std::ceil(pillW),
                     winY + SHADOW + (int)std::ceil(totalH) };

    if (W > MAXW) W = MAXW;
    if (H > MAXH) H = MAXH;

    {
        Graphics& g = *g_gfx;
        g.ResetTransform();
        g.ResetClip();

        // clear only the W×H region we'll actually blit (SourceCopy = overwrite)
        g.SetCompositingMode(CompositingModeSourceCopy);
        SolidBrush clear(Color(0, 0, 0, 0));
        g.FillRectangle(&clear, 0.0f, 0.0f, (REAL)W, (REAL)H);
        g.SetCompositingMode(CompositingModeSourceOver);

        // close-scale around center
        if (closeScale != 1.0) {
            g.TranslateTransform((REAL)(W / 2.0), (REAL)(H / 2.0));
            g.ScaleTransform((REAL)closeScale, (REAL)closeScale);
            g.TranslateTransform(-(REAL)(W / 2.0), -(REAL)(H / 2.0));
        }

        RectF pill((REAL)SHADOW, (REAL)SHADOW, (REAL)pillW, (REAL)totalH);

        // ── glass body silhouette ──
        GraphicsPath body;
        RoundRect(body, pill, CORNER);

        Region oldClip;
        g.GetClip(&oldClip);

        // ── soft ambient shadow ── (no clip — overdraw under the body is far
        // cheaper than maintaining an excluded region, and the opaque glass
        // paints right over it anyway)
        for (int i = 10; i >= 1; i--) {
            REAL infl = (REAL)i * 5.8f;                 // spread up to ~58px
            double f  = (double)(11 - i) / 10.0;        // 0 (outer) -> 1 (inner)
            BYTE a    = (BYTE)(8.0 * f * f);            // very faint, eased falloff
            if (a == 0) continue;
            GraphicsPath sp;
            RectF sr(pill.X - infl, pill.Y - infl + 3,
                     pill.Width + infl * 2, pill.Height + infl * 2);
            RoundRect(sp, sr, CORNER + infl);
            SolidBrush sb(Color(a, 0, 0, 0));
            g.FillPath(&sb, &sp);
        }

        // ── frosted glass body (near-opaque: background shows only as a hint) ──
        g.SetClip(&body);
        if (g_frost) {
            float sx = (winX + SHADOW) / (float)FROST_SCALE;
            float sy = (winY + SHADOW) / (float)FROST_SCALE;
            float swd = (float)pillW / (float)FROST_SCALE;
            float shd = (float)totalH / (float)FROST_SCALE;
            g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
            g.DrawImage(g_frost, pill, sx, sy, swd, shd, UnitPixel);
            // the blur does the work — only a ~6% black wash for slight depth
            SolidBrush tint(Color(16, 0, 0, 0));
            g.FillRectangle(&tint, pill);
        } else {
            SolidBrush glass(Color(220, 32, 32, 36));
            g.FillRectangle(&glass, pill);
        }

        // frosted top sheen — soft, restrained
        RectF sheenR(pill.X, pill.Y, pill.Width, pill.Height * 0.55f);
        LinearGradientBrush sheen(
            PointF(0, pill.Y), PointF(0, pill.Y + pill.Height * 0.55f),
            Color(30, 255, 255, 255), Color(0, 255, 255, 255));
        g.FillRectangle(&sheen, sheenR);
        g.SetClip(&oldClip, CombineModeReplace);

        // refined hairline borders: subtle bright rim + faint inner line
        Pen outer(Color(55, 255, 255, 255), 1.2f);
        g.DrawPath(&outer, &body);
        GraphicsPath innerP;
        RectF innerR(pill.X + 1.4f, pill.Y + 1.4f,
                     pill.Width - 2.8f, pill.Height - 2.8f);
        RoundRect(innerP, innerR, CORNER - 1.4f);
        Pen inner(Color(18, 255, 255, 255), 1.0f);
        g.DrawPath(&inner, &innerP);

        // ── search icon (slides center -> left as it expands) ──
        float iconCenterX = pill.X + (REAL)pillW / 2;
        float iconLeftX   = pill.X + LEFT_PAD + 12;
        float iconCX      = (float)lerp(iconCenterX, iconLeftX, widthT);
        float iconCY      = pill.Y + SEARCH_H / 2;
        DrawMagnifier(g, iconCX, iconCY, 255);

        // ── query / placeholder ── (fonts are cached globally)
        Font* fBig = g_fBig;
        Font* fSm  = g_fSm;
        // typographic format = no character overhang, so the caret can sit
        // tight against the last glyph and measurement is exact.
        StringFormat sf(StringFormat::GenericTypographic());
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsNoWrap |
                          StringFormatFlagsMeasureTrailingSpaces);

        float textX = pill.X + LEFT_PAD + 40;
        RectF textR(textX, pill.Y, pill.Width - (textX - pill.X) - LEFT_PAD,
                    SEARCH_H);
        BYTE ta = (BYTE)(255 * content);

        if (g_query.empty()) {
            SolidBrush hint(Color(ta, 150, 150, 158));
            g.DrawString(L"Spotlight Search", -1, fBig, textR, &sf, &hint);
            g_caretX = -1;  // reset so the caret snaps in on the first letter
        } else {
            // measure first so we can place the selection highlight / caret
            RectF meas;
            g.MeasureString(g_query.c_str(), (INT)g_query.size(), fBig,
                            PointF(textX, 0), &sf, &meas);

            // Ctrl+A selection highlight (drawn behind the text)
            if (g_selectAll) {
                RectF selR(textX - 4, pill.Y + SEARCH_H / 2 - 24,
                           meas.Width + 8, 48);
                GraphicsPath selP;
                RoundRect(selP, selR, 8);
                SolidBrush selBr(Color(150, 40, 110, 230));  // translucent blue
                g.FillPath(&selBr, &selP);
            }

            SolidBrush txt(Color(ta, 245, 245, 250));
            g.DrawString(g_query.c_str(), (INT)g_query.size(), fBig, textR,
                         &sf, &txt);

            // caret glides to just-after-text; hidden while a selection is active
            g_caretTgt = textX + meas.Width + 2;
            if (g_caretX < 0) g_caretX = g_caretTgt;
            else              g_caretX = smooth(g_caretX, g_caretTgt, dt, 30.0);
            if (!g_selectAll) {
                double blink = 0.40 + 0.60 * (0.5 + 0.5 * std::sin(now * 6.5));
                SolidBrush cb(Color((BYTE)(ta * blink), 235, 235, 240));
                g.FillRectangle(&cb, (REAL)g_caretX, pill.Y + SEARCH_H / 2 - 20,
                                2.4f, 40.0f);
            }
        }

        // ── advance per-row animation (reflow survivors, fade newcomers) ──
        for (auto& r : g_rows) {
            double targetY = (r.index >= 0) ? RowCenterY(r.index) : r.y;
            r.y = smooth(r.y, targetY, dt, 20.0);
            double gate = r.born + (r.index >= 0 ? r.index : 0) * 0.04;
            double tA   = (now >= gate) ? r.tAlpha : 0.0;
            r.alpha = smooth(r.alpha, tA, dt, 16.0);
        }

        // clip results & selection to the body so reflow never spills out
        g.SetClip(&body);

        // ── selection indicator (glassmorphism: frosted white) ──
        if (g_selAlpha > 0.01 && !g_results.empty()) {
            float selH = RESULT_H - 12;
            RectF selR(pill.X + 14, pill.Y + (REAL)g_selY - selH / 2,
                       pill.Width - 28, selH);
            GraphicsPath selP;
            RoundRect(selP, selR, selH / 2);  // fully-rounded pill highlight
            BYTE fa = (BYTE)(40 * g_selAlpha);
            LinearGradientBrush selBr(
                PointF(0, selR.Y), PointF(0, selR.Y + selR.Height),
                Color(fa, 255, 255, 255), Color((BYTE)(fa * 0.5f), 255, 255, 255));
            g.FillPath(&selBr, &selP);
            Pen selPen(Color((BYTE)(50 * g_selAlpha), 255, 255, 255), 1.0f);
            g.DrawPath(&selPen, &selP);
        }

        // ── results (animated rows keyed by app identity) ──
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);  // crisp icons
        for (auto& r : g_rows) {
            if (r.alpha <= 0.004) continue;
            const AppEntry* ae = r.app;

            float slide = (r.index >= 0) ? (float)((1 - r.alpha) * 18) : 0.0f;
            float rowCY = pill.Y + (REAL)r.y + slide;
            BYTE  rowA  = (BYTE)(255 * clamp01(r.alpha));

            // icon — clean transparent ARGB, loaded once on first display
            if (!ae->tried) {
                const_cast<AppEntry*>(ae)->gbmp  = LoadAppIcon(*ae);
                const_cast<AppEntry*>(ae)->tried = true;
            }
            if (ae->gbmp) {
                RectF ir(pill.X + LEFT_PAD, rowCY - ICON_SZ / 2, ICON_SZ, ICON_SZ);
                if (r.alpha > 0.99) {  // fully visible → no per-pixel alpha pass
                    g.DrawImage(ae->gbmp, ir, 0, 0,
                                (REAL)ae->gbmp->GetWidth(),
                                (REAL)ae->gbmp->GetHeight(), UnitPixel);
                } else {
                    ColorMatrix cm = {};
                    cm.m[0][0] = cm.m[1][1] = cm.m[2][2] = 1.0f;
                    cm.m[3][3] = (REAL)r.alpha;
                    cm.m[4][4] = 1.0f;
                    ImageAttributes ia;
                    ia.SetColorMatrix(&cm);
                    g.DrawImage(ae->gbmp, ir, 0, 0,
                                (REAL)ae->gbmp->GetWidth(),
                                (REAL)ae->gbmp->GetHeight(), UnitPixel, &ia);
                }
            }

            // name
            float nx = pill.X + LEFT_PAD + ICON_SZ + ICON_TXT_GAP;
            RectF nr(nx, rowCY - RESULT_H / 2,
                     pill.Width - (nx - pill.X) - LEFT_PAD, RESULT_H);
            StringFormat nf;
            nf.SetLineAlignment(StringAlignmentCenter);
            nf.SetTrimming(StringTrimmingEllipsisCharacter);
            nf.SetFormatFlags(StringFormatFlagsNoWrap);
            bool seld = (r.index == g_sel);
            SolidBrush nb(seld ? Color(rowA, 255, 255, 255)
                               : Color((BYTE)(rowA * 0.92), 228, 228, 234));
            g.DrawString(ae->name.c_str(), (INT)ae->name.size(), fSm, nr,
                         &nf, &nb);
        }

        g.SetClip(&oldClip, CombineModeReplace);

        // drop rows that have fully faded out
        g_rows.erase(std::remove_if(g_rows.begin(), g_rows.end(),
                         [](const Row& r) {
                             return r.index < 0 && r.alpha < 0.01;
                         }),
                     g_rows.end());
    }

    // ── push frame ──
    g_gfx->Flush(FlushIntentionSync);  // ensure GDI+ writes hit the DIB memory
    POINT ptSrc = {0, 0};
    SIZE  sz    = {W, H};
    POINT ptDst = {winX, winY};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)(255 * winAlpha), AC_SRC_ALPHA};
    UpdateLayeredWindow(g_wnd, g_screen, &ptDst, &sz, g_memDC, &ptSrc, 0, &bf,
                        ULW_ALPHA);
}

// ── Show / hide ───────────────────────────────────────────────────────────────
static void StartClose() {
    if (g_state == St::Closing || g_state == St::Hidden) return;
    g_state  = St::Closing;
    g_tClose = Now();
}

// Reliably pull our window to the foreground from a background process
static void ForceForeground(HWND h) {
    DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD myTid = GetCurrentThreadId();
    if (fgTid && fgTid != myTid) AttachThreadInput(fgTid, myTid, TRUE);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    SetFocus(h);
    if (fgTid && fgTid != myTid) AttachThreadInput(fgTid, myTid, FALSE);
}

static void ShowOverlay() {
    g_query.clear();
    g_results.clear();
    g_rows.clear();
    g_sel       = 0;
    g_heightCur = SEARCH_H;
    g_selAlpha  = 0;
    g_selY      = SEARCH_H + ROW_GAP + RESULT_H / 2;
    g_state     = St::Opening;
    g_tOpen     = Now();
    g_lastFrame = 0;
    g_caretX    = -1;
    g_lastType  = 0;
    g_selectAll = false;

    Wh_Log(L"Spotlight: opening");
    if (!g_hiRes) { timeBeginPeriod(1); g_hiRes = true; }  // crisp frame pacing
    CaptureFrost();  // snapshot the desktop behind us before we appear
    ShowWindow(g_wnd, SW_SHOW);
    Render();        // first frame; the render loop takes over from here
    ForceForeground(g_wnd);
}

static void LaunchSelected() {
    if (g_sel < 0 || g_sel >= (int)g_results.size()) return;
    const AppEntry* e = g_results[g_sel];
    if (e->pidl) {                       // shell app / UWP / game — invoke the item
        SHELLEXECUTEINFOW si = {};
        si.cbSize   = sizeof(si);
        si.fMask    = SEE_MASK_INVOKEIDLIST | SEE_MASK_IDLIST;
        si.lpIDList = (void*)e->pidl;
        si.lpVerb   = L"open";
        si.nShow    = SW_SHOW;
        ShellExecuteExW(&si);
    } else if (!e->launch.empty()) {     // settings deep link, etc.
        ShellExecuteW(nullptr, L"open", e->launch.c_str(), nullptr, nullptr, SW_SHOW);
    }
    StartClose();  // normal graceful fade-out
}

// ── Settings ──────────────────────────────────────────────────────────────────
static void LoadSettings() {
    struct HK { const wchar_t* id; UINT mods; UINT vk; };
    static const HK kHotkeys[] = {
        {L"alt_space",        HK_ALT,            VK_SPACE},
        {L"ctrl_space",       HK_CTRL,           VK_SPACE},
        {L"win_space",        HK_WIN,            VK_SPACE},
        {L"ctrl_alt_space",   HK_CTRL | HK_ALT,  VK_SPACE},
        {L"ctrl_shift_space", HK_CTRL | HK_SHIFT, VK_SPACE},
        {L"alt_s",            HK_ALT,            'S'},
        {L"win_s",            HK_WIN,            'S'},
        {L"alt_space_caps",   0,                 VK_CAPITAL},
    };

    g_hkMods = HK_ALT;  // safe default
    g_hkVk   = VK_SPACE;

    PCWSTR sel = Wh_GetStringSetting(L"hotkey");
    if (sel) {
        for (auto& h : kHotkeys)
            if (wcscmp(sel, h.id) == 0) { g_hkMods = h.mods; g_hkVk = h.vk; break; }
        Wh_FreeStringSetting(sel);
    }
    Wh_Log(L"Spotlight: hotkey mods=%u vk=%u", g_hkMods, g_hkVk);
}

// ── Window proc ───────────────────────────────────────────────────────────────
static int RowAtY(int localY) {
    float base = (float)SHADOW + SEARCH_H + ROW_GAP;
    if (localY < base) return -1;
    int i = (int)((localY - base) / RESULT_H);
    return (i >= 0 && i < (int)g_results.size()) ? i : -1;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TOGGLE:
        Wh_Log(L"Spotlight: toggle (state=%d)", (int)g_state);
        if (g_state == St::Hidden || g_state == St::Closing) ShowOverlay();
        else StartClose();
        return 0;

    case WM_CHAR: {
        wchar_t c = (wchar_t)w;
        if (c == L'\r' || c == L'\n') {
            LaunchSelected();
        } else if (c == 27) {
            StartClose();
        } else if (c == 1) {                 // Ctrl+A → select all
            if (!g_query.empty()) { g_selectAll = true; g_lastType = Now(); }
        } else if (c == L'\b') {             // backspace
            if (g_selectAll)      { g_query.clear(); g_selectAll = false; }
            else if (!g_query.empty()) g_query.pop_back();
            FilterApps(); g_lastType = Now();
        } else if (c >= 32) {                // printable: typing replaces selection
            if (g_selectAll) { g_query.clear(); g_selectAll = false; }
            g_query += c; FilterApps(); g_lastType = Now();
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (w == VK_UP   && g_sel > 0) g_sel--;
        if (w == VK_DOWN && g_sel + 1 < (int)g_results.size()) g_sel++;
        if (w == VK_DELETE && g_selectAll) {       // delete the selected text
            g_query.clear(); g_selectAll = false; FilterApps(); g_lastType = Now();
        }
        return 0;

    case WM_MOUSEMOVE: {
        int r = RowAtY(HIWORD(l));
        if (r >= 0) g_sel = r;
        return 0;
    }

    case WM_LBUTTONUP: {
        int r = RowAtY(HIWORD(l));
        if (r >= 0) { g_sel = r; LaunchSelected(); }
        return 0;
    }

    case WM_DISMISS:   // outside click (from the mouse hook) or focus loss
        if (g_state != St::Hidden && g_state != St::Closing &&
            (Now() - g_tOpen) > 0.25)
            StartClose();
        return 0;

    case WM_ACTIVATE:
        // Secondary dismissal path (alt-tab etc.). The mouse hook handles
        // click-away reliably; this catches keyboard focus changes.
        if (LOWORD(w) == WA_INACTIVE)
            PostMessageW(h, WM_DISMISS, 0, 0);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ── Low-level keyboard hook (configurable activation shortcut) ────────────────
static LRESULT CALLBACK LLKey(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)l;
        bool down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);

        bool alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
        bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool win   = (GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000;
        bool shift = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
        UINT mods  = (alt ? HK_ALT : 0) | (ctrl ? HK_CTRL : 0) |
                     (win ? HK_WIN : 0) | (shift ? HK_SHIFT : 0);

        // activation shortcut → toggle (swallow so the OS doesn't also react)
        if (down && k->vkCode == g_hkVk && (mods & g_hkMods) == g_hkMods &&
            (g_hkMods != 0 || mods == 0)) {
            PostMessageW(g_wnd, WM_TOGGLE, 0, 0);
            return 1;
        }
        // While open, any "I'm doing something else" key dismisses it:
        // Windows key, Alt+Tab, Esc. (Not swallowed — the OS still acts.)
        if (down && g_state != St::Hidden && g_state != St::Closing) {
            if (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN ||
                k->vkCode == VK_ESCAPE || (k->vkCode == VK_TAB && alt))
                PostMessageW(g_wnd, WM_DISMISS, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// ── Low-level mouse hook (click outside → dismiss) ────────────────────────────
static LRESULT CALLBACK LLMouse(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && g_wnd &&
        g_state != St::Hidden && g_state != St::Closing &&
        (w == WM_LBUTTONDOWN || w == WM_RBUTTONDOWN || w == WM_MBUTTONDOWN)) {
        POINT p = ((MSLLHOOKSTRUCT*)l)->pt;
        if (!PtInRect(&g_pillScreen, p))      // clicked anywhere off the pill
            PostMessageW(g_wnd, WM_DISMISS, 0, 0);
        // never swallow — the click still goes to whatever is underneath
    }
    return CallNextHookEx(nullptr, code, w, l);
}

// ── Worker thread ─────────────────────────────────────────────────────────────
static void Worker() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // for IShellItemImageFactory
    GdiplusStartupInput in;
    GdiplusStartup(&g_gdipTok, &in, nullptr);
    g_screen = GetDC(nullptr);

    // create fonts once, not every frame
    g_ff   = new FontFamily(L"Segoe UI");
    g_fBig = new Font(g_ff, 35, FontStyleRegular, UnitPixel);
    g_fSm  = new Font(g_ff, 25, FontStyleRegular, UnitPixel);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindhawkSpotlight";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    // A previous load may have leaked this class (with a now-dangling
    // WndProc pointing into our unloaded DLL). Drop it first if possible.
    UnregisterClassW(L"WindhawkSpotlight", wc.hInstance);
    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            Wh_Log(L"Spotlight: RegisterClass failed (%lu)", err);
            return;
        }
        Wh_Log(L"Spotlight: reusing existing window class");
    }

    g_wnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"WindhawkSpotlight", nullptr, WS_POPUP,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_wnd) {
        Wh_Log(L"Spotlight: CreateWindow failed (%lu)", GetLastError());
        return;
    }
    // Ensure messages go to *this* module's WndProc, even if the window
    // class was reused from a stale prior registration.
    SetWindowLongPtrW(g_wnd, GWLP_WNDPROC, (LONG_PTR)WndProc);

    LoadSettings();
    LoadApps();

    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKey, nullptr, 0);
    if (!g_kbHook) Wh_Log(L"Spotlight: keyboard hook failed (%lu)", GetLastError());
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LLMouse, nullptr, 0);
    if (!g_mouseHook) Wh_Log(L"Spotlight: mouse hook failed (%lu)", GetLastError());

    Wh_Log(L"Spotlight: ready");

    // Precisely-paced render loop. We drive frames ourselves with high-res
    // timing instead of WM_TIMER (which is coalesced/jittery and never gives
    // a steady 60fps). Input is pumped every iteration for zero-latency typing.
    MSG msg;
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        if (g_state != St::Hidden) {
            bool anim = Animating();
            double t0 = Now();
            Render();
            double renderMs = (Now() - t0) * 1000.0;

            // pace to ~60fps while animating; idle just blinks the caret
            double frameMs = anim ? 16.6 : 66.0;
            int wait = (int)(frameMs - renderMs);
            if (wait < 1) wait = 1;
            MsgWaitForMultipleObjectsEx(0, nullptr, (DWORD)wait,
                                        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else {
            WaitMessage();  // fully idle — zero CPU until something happens
        }
    }

    if (g_hiRes) { timeEndPeriod(1); g_hiRes = false; }
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_gfx)     { delete g_gfx;     g_gfx = nullptr; }
    if (g_surface) { delete g_surface; g_surface = nullptr; }
    if (g_memDC)   { if (g_oldBmp) SelectObject(g_memDC, g_oldBmp); DeleteDC(g_memDC); g_memDC = nullptr; }
    if (g_dib)     { DeleteObject(g_dib); g_dib = nullptr; }
    if (g_frost) { delete g_frost; g_frost = nullptr; }
    if (g_fBig) { delete g_fBig; g_fBig = nullptr; }
    if (g_fSm)  { delete g_fSm;  g_fSm  = nullptr; }
    if (g_ff)   { delete g_ff;   g_ff   = nullptr; }
    FreeApps();
    if (g_screen) ReleaseDC(nullptr, g_screen);
    GdiplusShutdown(g_gdipTok);
    UnregisterClassW(L"WindhawkSpotlight", GetModuleHandleW(nullptr));
    CoUninitialize();
}

// ── Windhawk entry points ─────────────────────────────────────────────────────
BOOL Wh_ModInit() {
    Wh_Log(L"Spotlight Search 1.0: init");
    g_thread = std::thread(Worker);
    return TRUE;
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Spotlight Search: settings changed");
    LoadSettings();  // pick up the new hotkey live
}

void Wh_ModUninit() {
    Wh_Log(L"Spotlight Search 1.0: uninit");
    // WM_CLOSE -> DefWindowProc calls DestroyWindow -> WM_DESTROY -> quit.
    // (Posting WM_DESTROY directly never destroys the window, which would
    //  leave the window class registered and break the next load.)
    if (g_wnd) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    if (g_thread.joinable()) g_thread.join();
}
