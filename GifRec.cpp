// GifRec.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include "GifRec.h"
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <ctime>
#include "gif.h"

#pragma comment(lib, "comctl32.lib")

enum AppState { IDLE, SELECTING, RECORDING };

// 全局变量
HINSTANCE hInst;
HWND hHiddenWnd = NULL;
HWND hOverlayWnd = NULL;
NOTIFYICONDATA nid = {};
AppState currentState = IDLE;

// 设置相关
struct Settings {
    int fps = 10;
    int quality = 10;
    wchar_t path[MAX_PATH] = L"C:\\";
} g_settings;

// 选区和录制控制
RECT g_selRect = { 0 };
POINT g_startPt = { 0 };
bool g_isDragging = false;
std::atomic<bool> g_stopRecording(false);
std::thread g_recordThread;

// 声明函数
LRESULT CALLBACK HiddenWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK OverlayWndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK SettingsDlgProc(HWND, UINT, WPARAM, LPARAM);
void LoadSettings();
void SaveSettings();
void ShowTrayBalloon(const wchar_t* title, const wchar_t* text);
void StartRecording();
void StopRecording();

// 加载/保存配置文件
std::wstring GetIniPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L"\\/")) + L"\\config.ini";
}

void LoadSettings() {
    std::wstring ini = GetIniPath();
    g_settings.fps = GetPrivateProfileInt(L"Settings", L"FPS", 10, ini.c_str());
    g_settings.quality = GetPrivateProfileInt(L"Settings", L"Quality", 10, ini.c_str());

    // 将默认路径设为当前 exe 所在的目录，而不是 C:\，避免权限拒绝
    wchar_t defaultPath[MAX_PATH];
    GetModuleFileName(NULL, defaultPath, MAX_PATH);
    std::wstring p(defaultPath);
    p = p.substr(0, p.find_last_of(L"\\/")); // 截取目录部分

    GetPrivateProfileString(L"Settings", L"Path", p.c_str(), g_settings.path, MAX_PATH, ini.c_str());
}

void SaveSettings() {
    std::wstring ini = GetIniPath();
    WritePrivateProfileString(L"Settings", L"FPS", std::to_wstring(g_settings.fps).c_str(), ini.c_str());
    WritePrivateProfileString(L"Settings", L"Quality", std::to_wstring(g_settings.quality).c_str(), ini.c_str());
    WritePrivateProfileString(L"Settings", L"Path", g_settings.path, ini.c_str());
}

void RecordingThreadWorker(RECT rect, Settings settings) {
    // 1. 确保宽度和高度为正数且偶数（偶数对编码器更友好）
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;

    // 强制宽度 4 字节对齐（可选，但推荐）
    // width = (width / 4) * 4; 

    // 2. 生成文件名
    time_t now = time(nullptr);
    tm tinfo;
    localtime_s(&tinfo, &now);
    wchar_t filename[MAX_PATH];
    swprintf_s(filename, L"%s\\record_%04d%02d%02d_%02d%02d%02d.gif",
        settings.path, tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday,
        tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec);

    char utf8FileName[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, filename, -1, utf8FileName, MAX_PATH, NULL, NULL);

    // 3. 在堆上分配 GifWriter，防止栈溢出摧毁指针
    auto writer = std::make_unique<GifWriter>();
    memset(writer.get(), 0, sizeof(GifWriter));

    int delay = 100 / settings.fps;

    // 注意：GifBegin 的最后一个参数是 bitDepth，必须 <= 8
    if (!GifBegin(writer.get(), utf8FileName, width, height, delay, 8)) {
        ShowTrayBalloon(L"Error", L"Could not create file. Check path/permissions.");
        currentState = IDLE;
        return;
    }

    // 4. GDI 环境准备
    HDC hScreenDC = GetDC(NULL);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemDC, hBitmap);

    // 设置位图信息
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height; // 负值表示 Top-Down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> bgraPixels(width * height * 4);
    std::vector<uint8_t> rgbaPixels(width * height * 4);

    int sleepTimeMs = 1000 / settings.fps;

    // 5. 录制循环
    while (!g_stopRecording) {
        DWORD frameStart = GetTickCount();

        // 截取屏幕指定区域
        BitBlt(hMemDC, 0, 0, width, height, hScreenDC, rect.left, rect.top, SRCCOPY);

        // 获取像素数据
        if (GetDIBits(hMemDC, hBitmap, 0, height, bgraPixels.data(), &bi, DIB_RGB_COLORS)) {
            // 手动转换颜色通道：BGRA -> RGBA (GIF.h 需要 RGBA)
            for (int i = 0; i < width * height; ++i) {
                int base = i * 4;
                rgbaPixels[base] = bgraPixels[base + 2]; // R
                rgbaPixels[base + 1] = bgraPixels[base + 1]; // G
                rgbaPixels[base + 2] = bgraPixels[base];     // B
                rgbaPixels[base + 3] = 255;                  // A (不透明)
            }

            // 【核心修复】无论设置里写多少，这里的 bitDepth 必须传 8
            // 第三个参数是 delay，最后一个是 bitDepth，倒数第二个是是否开启抖动(dither)
            GifWriteFrame(writer.get(), rgbaPixels.data(), width, height, delay, 8, false);
        }

        DWORD elapsed = GetTickCount() - frameStart;
        if (elapsed < (DWORD)sleepTimeMs) {
            Sleep(sleepTimeMs - elapsed);
        }
    }

    GifEnd(writer.get());

    // 6. 清理资源
    SelectObject(hMemDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreenDC);
}

void StartRecording() {
    if (g_selRect.right - g_selRect.left <= 0 || g_selRect.bottom - g_selRect.top <= 0) {
        currentState = IDLE;
        return;
    }
    currentState = RECORDING;
    g_stopRecording = false;
    g_recordThread = std::thread(RecordingThreadWorker, g_selRect, g_settings);
    ShowTrayBalloon(L"Recording Started", L"Press Ctrl+Shift+G to stop recording.");
}

void StopRecording() {
    if (currentState == RECORDING) {
        g_stopRecording = true;
        if (g_recordThread.joinable()) {
            g_recordThread.join();
        }
        currentState = IDLE;
        ShowTrayBalloon(L"Recording Saved", L"GIF has been saved to your directory.");
    }
}

// 隐藏主窗口过程（处理托盘和热键）
LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        RegisterHotKey(hwnd, HOTKEY_ID, MOD_CONTROL | MOD_SHIFT, 'G');
        break;
    case WM_HOTKEY:
        if (wParam == HOTKEY_ID) {
            if (currentState == IDLE) {
                // 开启覆盖层进行框选
                currentState = SELECTING;
                int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
                int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
                int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

                SetWindowPos(hOverlayWnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
                SetForegroundWindow(hOverlayWnd);
            }
            else if (currentState == RECORDING) {
                StopRecording();
            }
        }
        break;
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = LoadMenu(hInst, MAKEINTRESOURCE(IDR_MENU1));
            HMENU hSubMenu = GetSubMenu(hMenu, 0);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hSubMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT) {
            StopRecording();
            PostQuitMessage(0);
        }
        else if (LOWORD(wParam) == ID_TRAY_SETTINGS) {
            DialogBox(hInst, MAKEINTRESOURCE(IDD_SETTINGS), NULL, SettingsDlgProc);
        }
        break;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, HOTKEY_ID);
        Shell_NotifyIcon(NIM_DELETE, &nid);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 覆盖层窗口过程（处理鼠标框选）
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        g_isDragging = true;
        g_startPt.x = (short)LOWORD(lParam);
        g_startPt.y = (short)HIWORD(lParam);
        g_selRect = { g_startPt.x, g_startPt.y, g_startPt.x, g_startPt.y };
        SetCapture(hwnd);
        break;
    case WM_MOUSEMOVE:
        if (g_isDragging) {
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            g_selRect.left = min(g_startPt.x, pt.x);
            g_selRect.top = min(g_startPt.y, pt.y);
            g_selRect.right = max(g_startPt.x, pt.x);
            g_selRect.bottom = max(g_startPt.y, pt.y);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_LBUTTONUP:
        if (g_isDragging) {
            g_isDragging = false;
            ReleaseCapture();
            ShowWindow(hwnd, SW_HIDE);

            // 转换为屏幕绝对坐标 (针对多显示器)
            POINT topleft = { g_selRect.left, g_selRect.top };
            POINT bottomright = { g_selRect.right, g_selRect.bottom };
            ClientToScreen(hwnd, &topleft);
            ClientToScreen(hwnd, &bottomright);
            g_selRect = { topleft.x, topleft.y, bottomright.x, bottomright.y };

            StartRecording();
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);

        // 双缓冲避免闪烁
        HDC hMemDC = CreateCompatibleDC(hdc);
        HBITMAP hBmp = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hMemDC, hBmp);

        // 填充半透明黑色背景
        HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hMemDC, &clientRect, bgBrush);
        DeleteObject(bgBrush);

        // 挖空选区
        if (g_isDragging) {
            ExcludeClipRect(hMemDC, g_selRect.left, g_selRect.top, g_selRect.right, g_selRect.bottom);
            FillRect(hMemDC, &clientRect, (HBRUSH)GetStockObject(BLACK_BRUSH)); // 仅用于确保裁剪外区域正确
            SelectClipRgn(hMemDC, NULL);

            // 画红色边框
            HBRUSH borderBrush = CreateSolidBrush(RGB(255, 0, 0));
            FrameRect(hMemDC, &g_selRect, borderBrush);
            DeleteObject(borderBrush);
        }

        BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, hMemDC, 0, 0, SRCCOPY);

        SelectObject(hMemDC, hOldBmp);
        DeleteObject(hBmp);
        DeleteDC(hMemDC);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_ERASEBKGND:
        return 1; // 配合双缓冲防止闪烁
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 设置对话框过程
INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemInt(hwnd, IDC_EDIT_FPS, g_settings.fps, FALSE);
        SetDlgItemInt(hwnd, IDC_EDIT_QUALITY, g_settings.quality, FALSE);
        SetDlgItemText(hwnd, IDC_EDIT_PATH, g_settings.path);
        return (INT_PTR)TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            g_settings.fps = GetDlgItemInt(hwnd, IDC_EDIT_FPS, NULL, FALSE);
            g_settings.quality = GetDlgItemInt(hwnd, IDC_EDIT_QUALITY, NULL, FALSE);
            GetDlgItemText(hwnd, IDC_EDIT_PATH, g_settings.path, MAX_PATH);
            SaveSettings();
            EndDialog(hwnd, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwnd, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

void ShowTrayBalloon(const wchar_t* title, const wchar_t* text) {
    wcscpy_s(nid.szInfoTitle, title);
    wcscpy_s(nid.szInfo, text);
    nid.uFlags = NIF_INFO;
    Shell_NotifyIcon(NIM_MODIFY, &nid);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    // 强制声明 DPI 感知，防止在缩放的显示器上截屏不完整
    SetProcessDPIAware();

    hInst = hInstance;
    LoadSettings();

    // 注册隐藏主窗口类
    WNDCLASSEX wcHidden = { sizeof(WNDCLASSEX), 0, HiddenWndProc, 0, 0, hInstance, NULL, NULL, NULL, NULL, L"HiddenClass", NULL };
    RegisterClassEx(&wcHidden);

    // 注册覆盖层窗口类
    WNDCLASSEX wcOverlay = { sizeof(WNDCLASSEX), 0, OverlayWndProc, 0, 0, hInstance, LoadCursor(NULL, IDC_CROSS), NULL, NULL, NULL, L"OverlayClass", NULL };
    RegisterClassEx(&wcOverlay);

    // 创建隐藏窗口
    hHiddenWnd = CreateWindow(L"HiddenClass", L"GifRecorderApp", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    // 创建覆盖层窗口 (WS_EX_LAYERED 用于实现半透明)
    hOverlayWnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"OverlayClass", L"",
        WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    SetLayeredWindowAttributes(hOverlayWnd, 0, 100, LWA_ALPHA); // Alpha值100 (0-255)

    // 托盘图标设置
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hHiddenWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION); // 默认图标
    wcscpy_s(nid.szTip, L"GIF Recorder (Ctrl+Shift+G)");
    Shell_NotifyIcon(NIM_ADD, &nid);

    ShowTrayBalloon(L"GIF Recorder Running", L"Press Ctrl+Shift+G to select screen region.");

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}