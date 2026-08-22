#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WIN32_LEAN_AND_MEAN

// 默认配置信息，在main或config.ini调整
typedef struct 
{
    int slide_ms;  // 滑行总时长
    int game_fps;  // 帧率，用于游戏中模拟位移滑行时尽量减少帧率不一致导致的抖动
    int base_hz;   // 连按频率，远高于游戏帧率可减少抖动感
    int min_hold_ms;  // 单次按键高电平最小保持时长
    int min_gap_ms;   // 脉冲最小间隔
    int max_slide_frames;  // 最慢时滑行帧数，比如为5就是走一帧停4帧
    int curve;        // 选择衰减曲线类型

    int keyW, keyA, keyS, keyD;
    int keyUp, keyDown;

    int enableReplay; // 是否同时注入"模拟按下"到游戏（始终true；0仅透传不重放）
} Config;

// 全局状态量
static Config g_cfg;
static volatile BOOL g_shouldExit = FALSE;

// 监听按键运行时状态
#define MAX_KEYS 8
static int g_keyList[MAX_KEYS];
static int g_keyCount = 0;

typedef struct 
{
    BOOL    isMoveKey;      
    BOOL    physicallyDown; 
    DWORD   releaseTime;    
    BOOL    injecting;      
    DWORD   injectUntil;    
    LARGE_INTEGER perfFreq; 
} KeyState;
static KeyState g_keyState[MAX_KEYS];

static HANDLE g_hHookThread;
static HHOOK g_hHook = NULL;
static HANDLE g_hInjectThread = NULL;
static volatile BOOL g_injectRunning = TRUE;

static CRITICAL_SECTION g_cs;

static double nowSec(void) 
{
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

static DWORD nowMs(void) 
{
    return (DWORD)(nowSec() * 1000.0);
}

static void loadConfig(const char *path) 
{
    FILE *fp = fopen(path, "r");
    char line[256];
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == 0) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p;
        char *val = eq + 1;
        int n = (int)strlen(val);
        while (n > 0 && (val[n-1] == '\n' || val[n-1] == '\r' || val[n-1] == ' ' || val[n-1] == '\t'))
            val[--n] = 0;
        n = (int)strlen(key);
        while (n > 0 && (key[n-1] == ' ' || key[n-1] == '\t')) key[--n] = 0;

        int iv = atoi(val);
        if (strcmp(key, "slide_ms") == 0)      g_cfg.slide_ms = iv;
        else if (strcmp(key, "game_fps") == 0) g_cfg.game_fps = iv;
        else if (strcmp(key, "base_hz") == 0)  g_cfg.base_hz = iv;
        else if (strcmp(key, "min_hold_ms")==0) g_cfg.min_hold_ms = iv;
        else if (strcmp(key, "min_gap_ms") ==0)g_cfg.min_gap_ms = iv;
        else if (strcmp(key, "max_slide_frames")==0) g_cfg.max_slide_frames = iv;
        else if (strcmp(key, "curve") == 0)    g_cfg.curve = iv;
        else if (strcmp(key, "keyW") == 0)     g_cfg.keyW = iv;
        else if (strcmp(key, "keyA") == 0)     g_cfg.keyA = iv;
        else if (strcmp(key, "keyS") == 0)     g_cfg.keyS = iv;
        else if (strcmp(key, "keyD") == 0)     g_cfg.keyD = iv;
        else if (strcmp(key, "keyUp") == 0)    g_cfg.keyUp = iv;
        else if (strcmp(key, "keyDown") == 0)  g_cfg.keyDown = iv;
        else if (strcmp(key, "enableReplay")==0) g_cfg.enableReplay = iv;
    }
    fclose(fp);
}

static const char* vkName(int vk) 
{
    switch (vk) {
        case 'W': return "W"; case 'A': return "A"; case 'S': return "S"; case 'D': return "D";
        case VK_SPACE: return "SPACE";
        case VK_LSHIFT: return "LSHIFT"; case VK_RSHIFT: return "RSHIFT";
        case VK_LCONTROL: return "LCTRL"; case VK_RCONTROL: return "RCTRL";
        case VK_LMENU: return "LALT"; case VK_RMENU: return "RALT";
        case VK_UP: return "UP"; case VK_DOWN: return "DOWN"; case VK_LEFT: return "LEFT"; case VK_RIGHT: return "RIGHT";
        default: return "?";
    }
}

static void injectKey(int vk, BOOL down) 
{
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    in.ki.time = 0;
    in.ki.dwExtraInfo = 0;
    SendInput(1, &in, sizeof(INPUT));
}

// 递归保护
static void markInject(int idx, DWORD holdMs) 
{
    EnterCriticalSection(&g_cs);
    g_keyState[idx].injecting = TRUE;
    g_keyState[idx].injectUntil = nowMs() + holdMs;
    LeaveCriticalSection(&g_cs);
}
static void clearInject(int idx) {
    EnterCriticalSection(&g_cs);
    g_keyState[idx].injecting = FALSE;
    LeaveCriticalSection(&g_cs);
}

// 衰减曲线
static double curveWeight(double t, int curve)
{
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    double w;
    switch (curve) {
        case 0: // 线性缓动
            w = 1.0 - t;
            break;
        case 1: // 指数缓动
            w = exp(-5.0 * t);
            break;
        case 2: // 前后平滑
            w = 1.0 - t;
            w = w * w * (3.0 - 2.0 * w);
            break;
        case 3: // 余弦缓动
            w = cos(t * 3.14159265358979323846 / 2.0);
            break;
        case 4: // 缓出平滑
            w = (1.0 - t) * (1.0 - t);
            break;
        default:
            w = 1.0 - t;
    }
    return w;
}

// 按键重放驱动
static DWORD WINAPI injectThread(LPVOID param) 
{
    (void)param;
    DWORD lastTick = nowMs();
    while (g_injectRunning) {
        DWORD cur = nowMs();
        DWORD dt = cur - lastTick;
        lastTick = cur;
        if (dt > 16) dt = 16;  // 防止掉帧跳变

        for (int i = 0; i < g_keyCount; i++) {
            KeyState *ks = &g_keyState[i];
            if (!ks->isMoveKey) continue;
            if (!g_cfg.enableReplay) continue;

            EnterCriticalSection(&g_cs);
            BOOL inSlide = ks->physicallyDown == FALSE && ks->releaseTime != 0;
            BOOL injecting = ks->injecting;
            DWORD rtime = ks->releaseTime;
            LeaveCriticalSection(&g_cs);

            if (!inSlide) continue;
            if (injecting) continue;  // 正在注入，跳过

            double elapsed = (double)(cur - rtime);          // 已滑行时间 ms
            if (elapsed >= g_cfg.slide_ms) {                 // 结束滑行
                EnterCriticalSection(&g_cs);
                ks->releaseTime = 0;
                LeaveCriticalSection(&g_cs);
                continue;
            }
            double t = elapsed / (double)g_cfg.slide_ms;     
            double w = curveWeight(t, g_cfg.curve);          

            double period = 1000.0 / (g_cfg.base_hz > 0 ? g_cfg.base_hz : 200);
            double baseHold = w * period;

            double holdMs, gapMs;
            if (baseHold >= g_cfg.min_hold_ms) {
                holdMs = baseHold;
                gapMs = period - holdMs;
                if (gapMs < 0) gapMs = 0;
            } else {
                holdMs = (double)g_cfg.min_hold_ms;
                double idealGap = holdMs * (1.0 - w) / (w > 0.0001 ? w : 0.0001);
                double maxGap = 25.0; 
                gapMs = idealGap < maxGap ? idealGap : maxGap;
            }

            int vk = g_keyList[i];
            markInject(i, (DWORD)(holdMs + 3));
            injectKey(vk, TRUE);
            Sleep((DWORD)holdMs);
            injectKey(vk, FALSE);
            clearInject(i);

            // 若重新按下该键则立即中止本轮滑行
            DWORD remain = (DWORD)gapMs;
            while (remain > 0 && g_injectRunning) {
                EnterCriticalSection(&g_cs);
                BOOL userPressed = ks->physicallyDown; 
                LeaveCriticalSection(&g_cs);
                if (userPressed) {
                    EnterCriticalSection(&g_cs);
                    ks->releaseTime = 0;
                    LeaveCriticalSection(&g_cs);
                    break;
                }
                DWORD sl = remain > 2 ? 2 : remain;
                Sleep(sl);
                remain -= sl;
            }
        }
        Sleep(1); 
    }
    return 0;
}

// hook
static LRESULT CALLBACK lowLevelKeyboard(int nCode, WPARAM wParam, LPARAM lParam) 
{
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT*)lParam;
        int vk = (int)kb->vkCode;
        BOOL down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

        for (int i = 0; i < g_keyCount; i++) {
            if (vk == g_keyList[i]) {
                KeyState *ks = &g_keyState[i];
                EnterCriticalSection(&g_cs);
                BOOL selfInject = ks->injecting && nowMs() < ks->injectUntil;
                if (selfInject) {
                    LeaveCriticalSection(&g_cs);
                    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
                }
                if (down && !ks->physicallyDown) {
                    ks->physicallyDown = TRUE;
                    ks->releaseTime = 0;
                    LeaveCriticalSection(&g_cs);
                    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
                }
                if (!down && ks->physicallyDown) {
                    ks->physicallyDown = FALSE;
                    ks->releaseTime = nowMs();
                    LeaveCriticalSection(&g_cs);
                    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
                }
                LeaveCriticalSection(&g_cs);
                break;
            }
        }
    }
    return CallNextHookEx(g_hHook, nCode, wParam, lParam);
}

static DWORD WINAPI hookThread(LPVOID param)
{
    (void)param;
    g_hHook = SetWindowsHookExW(WH_KEYBOARD_LL, lowLevelKeyboard, GetModuleHandleW(NULL), 0);
    if (!g_hHook) {
        printf("[错误] 设置低级键盘钩子失败，错误码=%lu\n", (unsigned long)GetLastError());
        g_shouldExit = TRUE;
        return 1;
    }
    printf("[信息] 低级键盘钩子已安装。按 ESC 退出...\n");
    MSG msg;
    while (!g_shouldExit && GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(g_hHook);
    g_hHook = NULL;
    return 0;
}

// 状态显示
static DWORD WINAPI statusThread(LPVOID param) 
{
    (void)param;
    DWORD last = nowMs();
    while (!g_shouldExit) {
        DWORD cur = nowMs();
        if (cur - last >= 100)  // 100ms刷新一次
        {  
            last = cur;
            static int printed = 0;
            char line[256]; line[0] = 0;
            for (int i = 0; i < g_keyCount; i++) {
                KeyState *ks = &g_keyState[i];
                if (ks->isMoveKey) {
                    const char *st;
                    EnterCriticalSection(&g_cs);
                    if (ks->physicallyDown) st = "[按住]";
                    else if (ks->releaseTime != 0) {
                        double el = (double)(cur - ks->releaseTime);
                        if (el < g_cfg.slide_ms) st = "[滑行]";
                        else st = "[停]";
                    } else st = "[停]";
                    LeaveCriticalSection(&g_cs);
                    char part[32];
                    snprintf(part, sizeof(part), "%s:%s ", vkName(g_keyList[i]), st);
                    strncat(line, part, sizeof(line)-strlen(line)-1);
                }
            }
            if (printed) printf("\r");
            printf("%-48s", line);
            fflush(stdout);
            printed = 1;
        }
        Sleep(40);
    }
    printf("\n");
    return 0;
}



int main(int argc, char **argv) 
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.slide_ms   = 1200; 
    g_cfg.game_fps   = 60;   // 60帧
    g_cfg.base_hz    = 180;  
    g_cfg.min_hold_ms= 2;    // 按住的最小持续时间
    g_cfg.min_gap_ms = 1;    // 空挡
    g_cfg.max_slide_frames = 5; 
    g_cfg.curve      = 4;  
    g_cfg.keyW = 'W'; g_cfg.keyA = 'A'; g_cfg.keyS = 'S'; g_cfg.keyD = 'D';
    g_cfg.keyUp = VK_SPACE; g_cfg.keyDown = VK_LSHIFT;
    g_cfg.enableReplay = 1;

    const char *cfgPath = (argc > 1) ? argv[1] : "config.ini";
    loadConfig(cfgPath);

    int tmp[MAX_KEYS]; int cnt = 0;
    tmp[cnt++] = g_cfg.keyW;
    tmp[cnt++] = g_cfg.keyA;
    tmp[cnt++] = g_cfg.keyS;
    tmp[cnt++] = g_cfg.keyD;
    if (g_cfg.keyUp > 0)   tmp[cnt++] = g_cfg.keyUp;
    if (g_cfg.keyDown > 0) tmp[cnt++] = g_cfg.keyDown;
    g_keyCount = cnt;
    for (int i = 0; i < cnt; i++) {
        g_keyList[i] = tmp[i];
        memset(&g_keyState[i], 0, sizeof(g_keyState[i]));
        g_keyState[i].isMoveKey = TRUE;
    }

    InitializeCriticalSection(&g_cs);

    printf("~~~~~ O原神启动O ~~~~~\n");
    printf("LDD_ROG 2026.8.22\n");
    printf("配置: slide_ms=%d  game_fps=%d  curve=%d  maxFrames=%d\n",
           g_cfg.slide_ms, g_cfg.game_fps, g_cfg.curve, g_cfg.max_slide_frames);
    printf("受控键: W/A/S/D");
    if (g_cfg.keyUp > 0) printf(" + 空格(上)");
    if (g_cfg.keyDown > 0) printf(" + Shift(下)");
    printf("\n");

    g_injectRunning = TRUE;
    g_hInjectThread = CreateThread(NULL, 0, injectThread, NULL, 0, NULL);

    CreateThread(NULL, 0, statusThread, NULL, 0, NULL);

    g_hHookThread = CreateThread(NULL, 0, hookThread, NULL, 0, NULL);
    WaitForSingleObject(g_hHookThread, INFINITE);

    g_injectRunning = FALSE;
    if (g_hInjectThread) { WaitForSingleObject(g_hInjectThread, 2000); CloseHandle(g_hInjectThread); }
    DeleteCriticalSection(&g_cs);
    printf("已退出。\n");
    return 0;
}