#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <memory> 

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wsock32.lib")

#define WM_TRAYICON (WM_USER + 1)
#define IDI_ICON1 101
#define TIMER_ID_UI_UPDATE 1001

const int PING_INTERVAL_MS = 1000;
const int UI_REFRESH_INTERVAL_MS = 500; 

class IconManager {
private:
    std::unordered_map<int, HICON> cache;
    HFONT hFontArial = NULL;
    HFONT hFontArialNarrow = NULL;
    int size;

public:
    IconManager() {
        size = GetSystemMetrics(SM_CXSMICON);
    }

    ~IconManager() {
        for (auto& pair : cache) DestroyIcon(pair.second);
        if (hFontArial) DeleteObject(hFontArial);
        if (hFontArialNarrow) DeleteObject(hFontArialNarrow);
    }

    HICON GetIcon(int ping) {
        if (cache.count(ping)) return cache[ping];

        HDC hdcScrn = GetDC(NULL);
        HDC hdc = CreateCompatibleDC(hdcScrn);
        HBITMAP hbmp = CreateCompatibleBitmap(hdcScrn, size, size);
        HBITMAP hbmpOld = (HBITMAP)SelectObject(hdc, hbmp);

        COLORREF bgColor;
        if (ping < 0) bgColor = RGB(220, 20, 20); 
        else if (ping <= 50) bgColor = RGB(30, 160, 30);
        else if (ping <= 100) bgColor = RGB(200, 180, 20);
        else bgColor = RGB(220, 110, 20);

        HBRUSH hBrush = CreateSolidBrush(bgColor);
        RECT rect = {0, 0, size, size};
        FillRect(hdc, &rect, hBrush);
        DeleteObject(hBrush);

        char text[8];
        if (ping < 0) snprintf(text, sizeof(text), "X");
        else if (ping > 999) snprintf(text, sizeof(text), ">1k");
        else snprintf(text, sizeof(text), "%d", ping);
        
        int len = (int)strlen(text);

        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        
        int fontSize = - (size * (len >= 3 ? 55 : 75) / 100);
        
        HFONT* phFont = (len >= 3) ? &hFontArialNarrow : &hFontArial;
        if (*phFont == NULL) {
            *phFont = CreateFontA(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, 
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, len >= 3 ? "Arial Narrow" : "Arial");
        }
        
        HFONT hFontOld = (HFONT)SelectObject(hdc, *phFont);
        DrawTextA(hdc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, hFontOld);

        HBITMAP hbmpMask = CreateCompatibleBitmap(hdcScrn, size, size);
        HDC hdcMask = CreateCompatibleDC(hdcScrn);
        HBITMAP hbmpMaskOld = (HBITMAP)SelectObject(hdcMask, hbmpMask);
        FillRect(hdcMask, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SelectObject(hdcMask, hbmpMaskOld);
        DeleteDC(hdcMask);

        SelectObject(hdc, hbmpOld);
        DeleteDC(hdc);
        ReleaseDC(NULL, hdcScrn);

        ICONINFO ii = {0};
        ii.fIcon = TRUE;
        ii.hbmMask = hbmpMask;
        ii.hbmColor = hbmp;

        HICON hIcon = CreateIconIndirect(&ii);
        DeleteObject(hbmp);
        DeleteObject(hbmpMask);

        cache[ping] = hIcon;
        return hIcon;
    }
};

struct Target {
    std::string host = "8.8.8.8";
    IN_ADDR ip;
};

struct PingResult {
    int ping = -1;
    std::string host = "8.8.8.8";
    bool isResolving = false;
    
    bool operator==(const PingResult& o) const { 
        return ping == o.ping && isResolving == o.isResolving && host == o.host; 
    }
    bool operator!=(const PingResult& o) const { return !(*this == o); }
};

struct AppContext {
    std::shared_ptr<Target> currentTarget;
    std::shared_ptr<PingResult> latestResult;

    PingResult lastDrawnResult; 

    std::atomic<bool> running{true};
    std::condition_variable pingCv;
    std::mutex pingCvMutex;

    IconManager iconMgr;
    NOTIFYICONDATAA nid = {};
    HWND hwndMain = NULL;
    HWND hInputWnd = NULL;
    HWND hEditInput = NULL;
};

AppContext g_ctx;

void ResolveAndApplyAsync(std::string host) {
    auto intermediateRes = std::make_shared<PingResult>();
    intermediateRes->ping = -1;
    intermediateRes->host = host;
    intermediateRes->isResolving = true;
    std::atomic_store(&g_ctx.latestResult, intermediateRes);

    auto newTarget = std::make_shared<Target>();
    newTarget->host = host;
    newTarget->ip.s_addr = INADDR_NONE;

    if (InetPtonA(AF_INET, host.c_str(), &newTarget->ip) != 1) {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0) {
            if (result && result->ai_family == AF_INET) {
                newTarget->ip = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
            }
            if (result) freeaddrinfo(result);
        }
    }

    std::atomic_store(&g_ctx.currentTarget, newTarget);
    
    auto doneRes = std::make_shared<PingResult>();
    doneRes->ping = -1;
    doneRes->host = host;
    doneRes->isResolving = false;
    std::atomic_store(&g_ctx.latestResult, doneRes);

    g_ctx.pingCv.notify_all();
}

void PingServiceThread() {
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) return;

    const DWORD replySize = sizeof(ICMP_ECHO_REPLY) + 32;
    std::vector<char> replyBuffer(replySize);
    char sendData[32] = "PingRequest";

    while (g_ctx.running.load()) {
        auto t = std::atomic_load(&g_ctx.currentTarget);
        auto currentRes = std::atomic_load(&g_ctx.latestResult);

        if (!g_ctx.running.load()) break;

        int ping = -1;
        if (t && !currentRes->isResolving && t->ip.s_addr != INADDR_NONE) {
            DWORD dwRetVal = IcmpSendEcho(hIcmp, t->ip.s_addr, sendData, sizeof(sendData),
                NULL, replyBuffer.data(), replySize, 1000);

            if (dwRetVal != 0) {
                PICMP_ECHO_REPLY pEchoReply = (PICMP_ECHO_REPLY)replyBuffer.data();
                if (pEchoReply->Status == IP_SUCCESS) {
                    ping = pEchoReply->RoundTripTime;
                }
            }
        }

        auto currentT = std::atomic_load(&g_ctx.currentTarget);
        if (currentT && currentT->host == t->host) {
            auto freshResult = std::make_shared<PingResult>();
            freshResult->ping = ping;
            freshResult->host = t->host;
            freshResult->isResolving = false;
            std::atomic_store(&g_ctx.latestResult, freshResult);
        }

        std::unique_lock<std::mutex> lock(g_ctx.pingCvMutex);
        g_ctx.pingCv.wait_for(lock, std::chrono::milliseconds(PING_INTERVAL_MS), [] { return !g_ctx.running.load(); });
    }

    IcmpCloseHandle(hIcmp);
}

void UpdateTrayIndicator() {
    auto currentResPtr = std::atomic_load(&g_ctx.latestResult);
    if (!currentResPtr) return;
    
    PingResult res = *currentResPtr;

    if (res == g_ctx.lastDrawnResult) {
        return;
    }
    g_ctx.lastDrawnResult = res;

    int pingVal = res.isResolving ? -1 : res.ping;
    g_ctx.nid.hIcon = g_ctx.iconMgr.GetIcon(pingVal);
    g_ctx.nid.uFlags = NIF_ICON | NIF_TIP;

    char hostTip[256];
    int written = 0;
    
    if (res.isResolving) written = snprintf(hostTip, sizeof(hostTip), "Resolving DNS: %s...", res.host.c_str());
    else if (pingVal < 0) written = snprintf(hostTip, sizeof(hostTip), "Ping: err to %s", res.host.c_str());
    else written = snprintf(hostTip, sizeof(hostTip), "Ping: %d ms to %s", pingVal, res.host.c_str());

    if (written >= (int)sizeof(g_ctx.nid.szTip)) {
        hostTip[sizeof(g_ctx.nid.szTip) - 4] = '.';
        hostTip[sizeof(g_ctx.nid.szTip) - 3] = '.';
        hostTip[sizeof(g_ctx.nid.szTip) - 2] = '.';
        hostTip[sizeof(g_ctx.nid.szTip) - 1] = '\0';
    }

    strncpy(g_ctx.nid.szTip, hostTip, sizeof(g_ctx.nid.szTip) - 1);
    g_ctx.nid.szTip[sizeof(g_ctx.nid.szTip) - 1] = '\0';

    Shell_NotifyIconA(NIM_MODIFY, &g_ctx.nid);
}


LRESULT CALLBACK InputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
        case WM_CREATE: {
            if (!CreateWindowExA(0, "STATIC", "Enter new host (IP or domain):", WS_CHILD | WS_VISIBLE, 10, 10, 220, 20, hwnd, NULL, NULL, NULL)) return -1;
            g_ctx.hEditInput = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 30, 200, 25, hwnd, (HMENU)101, NULL, NULL);
            if (!g_ctx.hEditInput) return -1;
            if (!CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 60, 65, 100, 30, hwnd, (HMENU)IDOK, NULL, NULL)) return -1;
            
            auto t = std::atomic_load(&g_ctx.currentTarget);
            if (t) SetWindowTextA(g_ctx.hEditInput, t->host.c_str());

            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            EnumChildWindows(hwnd, [](HWND child, LPARAM font) -> BOOL {
                SendMessage(child, WM_SETFONT, font, TRUE);
                return TRUE;
            }, (LPARAM)hFont);
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDOK) {
                char buf[256] = {0};
                GetWindowTextA(g_ctx.hEditInput, buf, sizeof(buf));
                if (strlen(buf) > 0) {
                    std::string newTarget = buf;
                    std::thread(ResolveAndApplyAsync, newTarget).detach();
                }
                DestroyWindow(hwnd);
            }
            break;
        }
        case WM_DESTROY: 
            g_ctx.hInputWnd = NULL; 
            break;
        default: 
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

void OpenChangeHostDialog(HINSTANCE hInstance, HWND parent) {
    if (g_ctx.hInputWnd != NULL) { 
        SetForegroundWindow(g_ctx.hInputWnd); 
        return; 
    }
    const char DLG_CLASS_NAME[] = "PingHostDialogClass";
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSA wc = { };
        wc.lpfnWndProc = InputWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = DLG_CLASS_NAME;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
        if (!RegisterClassA(&wc)) return;
        classRegistered = true;
    }
    RECT rcParent;
    GetWindowRect(GetDesktopWindow(), &rcParent);
    int x = (rcParent.right - rcParent.left - 240) / 2;
    int y = (rcParent.bottom - rcParent.top - 140) / 2;
    g_ctx.hInputWnd = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, DLG_CLASS_NAME, "Change Host", 
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, 240, 140, parent, NULL, hInstance, NULL);
}

void AddTrayIcon(HWND hwnd) {
    g_ctx.nid.cbSize = sizeof(NOTIFYICONDATAA);
    g_ctx.nid.hWnd = hwnd;
    g_ctx.nid.uID = 1;
    g_ctx.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_ctx.nid.uCallbackMessage = WM_TRAYICON;
    g_ctx.nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1));
    strcpy(g_ctx.nid.szTip, "Latency: Ready");
    Shell_NotifyIconA(NIM_ADD, &g_ctx.nid);
}


LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_TIMER:
            if (wParam == TIMER_ID_UI_UPDATE) UpdateTrayIndicator();
            break;
        case WM_TRAYICON:
            if (LOWORD(lParam) == WM_RBUTTONUP) {
                POINT pt; GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, 2, "Change Host");
                AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hMenu, MF_STRING, 1, "Exit");
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
                if (cmd == 1) DestroyWindow(hwnd);
                else if (cmd == 2) OpenChangeHostDialog((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), hwnd);
            }
            break;
        case WM_DESTROY:
            g_ctx.running = false; 
            g_ctx.pingCv.notify_all();
            KillTimer(hwnd, TIMER_ID_UI_UPDATE);
            Shell_NotifyIconA(NIM_DELETE, &g_ctx.nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WSADATA wsaData; 
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;
    
    auto initTarget = std::make_shared<Target>();
    InetPtonA(AF_INET, initTarget->host.c_str(), &initTarget->ip);
    std::atomic_store(&g_ctx.currentTarget, initTarget);

    auto initRes = std::make_shared<PingResult>();
    std::atomic_store(&g_ctx.latestResult, initRes);

    const char CLASS_NAME[] = "PingTrayWindowClass";
    WNDCLASSA wc = { }; 
    wc.lpfnWndProc = WindowProc; 
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME; 
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    
    if (!RegisterClassA(&wc)) {
        WSACleanup();
        return 1;
    }

    g_ctx.hwndMain = CreateWindowExA(0, CLASS_NAME, "PingTray", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    if (g_ctx.hwndMain == NULL) {
        WSACleanup();
        return 1;
    }

    AddTrayIcon(g_ctx.hwndMain);
    SetTimer(g_ctx.hwndMain, TIMER_ID_UI_UPDATE, UI_REFRESH_INTERVAL_MS, NULL);

    std::thread pingThread(PingServiceThread);

    MSG msg = { };
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (g_ctx.hInputWnd == NULL || !IsDialogMessageA(g_ctx.hInputWnd, &msg)) {
            TranslateMessage(&msg); 
            DispatchMessage(&msg);
        }
    }

    g_ctx.running = false;
    g_ctx.pingCv.notify_all();
    if (pingThread.joinable()) pingThread.join();
    
    WSACleanup();
    return 0;
}
