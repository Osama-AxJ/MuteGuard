#include "muteguard.h"
#include <shellapi.h>
#include <commctrl.h>
#include <strsafe.h>

HINSTANCE g_instance;
HWND g_owner;
Config g_config;
Engine *g_engine;
wchar_t g_config_path[MAX_PATH];
static UINT taskbar_created;
static NOTIFYICONDATAW tray;
static UINT tray_attempts;
#define MG_TRAY (WM_APP + 3)
#define MENU_OPEN 100
#define MENU_ENABLE 101
#define MENU_EXIT 102

BOOL app_commit(const Config *c) {
    Config copy; HRESULT hr; config_init(&copy);
    if(!config_copy(&copy,c)) goto memory_error;
    hr=config_save(&copy,g_config_path);
    if(FAILED(hr)) {
        wchar_t text[200]; StringCchPrintfW(text,ARRAYSIZE(text),L"Settings could not be saved (0x%08lX). Your current rules are still active.",hr);
        MessageBoxW(g_owner,text,L"MuteGuard",MB_OK|MB_ICONERROR); config_free(&copy); return FALSE;
    }
    if(!engine_update(g_engine,&copy)) {
        /* Keep disk and active rules aligned if an allocation fails after save. */
        hr=config_save(&g_config,g_config_path);
        if(FAILED(hr)) MessageBoxW(g_owner,L"Not enough memory to apply settings. The saved file contains the new rules; restart MuteGuard to load them.",L"MuteGuard",MB_OK|MB_ICONERROR);
        config_free(&copy); goto memory_error;
    }
    config_free(&g_config); g_config=copy;
    StringCchCopyW(tray.szTip,ARRAYSIZE(tray.szTip),g_config.enabled?L"MuteGuard - Muting enabled":L"MuteGuard - Muting disabled");
    Shell_NotifyIconW(NIM_MODIFY,&tray); return TRUE;
memory_error:
    MessageBoxW(g_owner,L"Not enough memory to apply settings. Current rules remain active.",L"MuteGuard",MB_OK|MB_ICONERROR); return FALSE;
}
static void tray_add(void) {
    if(Shell_NotifyIconW(NIM_ADD,&tray)) {
        tray.uVersion=NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION,&tray); KillTimer(g_owner,1); tray_attempts=0;
    } else {
        if(++tray_attempts<=12) SetTimer(g_owner,1,5000,NULL);
        else { KillTimer(g_owner,1); MessageBoxW(g_owner,L"The notification area is unavailable. MuteGuard will exit. Please launch it again after Explorer is running.",L"MuteGuard",MB_OK|MB_ICONERROR); DestroyWindow(g_owner); }
    }
}
static void tray_menu(void) {
    HMENU menu=CreatePopupMenu(); POINT point;
    if(!menu) return;
    AppendMenuW(menu,MF_STRING,MENU_OPEN,L"Open Settings");
    AppendMenuW(menu,MF_STRING,MENU_ENABLE,g_config.enabled?L"Disable Muting":L"Enable Muting");
    AppendMenuW(menu,MF_STRING,MENU_EXIT,L"Exit");
    SetMenuDefaultItem(menu,MENU_OPEN,FALSE); GetCursorPos(&point); SetForegroundWindow(g_owner);
    TrackPopupMenu(menu,TPM_RIGHTBUTTON,point.x,point.y,0,g_owner,NULL); DestroyMenu(menu); PostMessageW(g_owner,WM_NULL,0,0);
}
static LRESULT CALLBACK owner_proc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    if(message==taskbar_created && taskbar_created) { tray_add(); return 0; }
    switch(message) {
    case MG_TRAY:
        if(LOWORD(l)==WM_CONTEXTMENU) tray_menu();
        else if(LOWORD(l)==NIN_SELECT || LOWORD(l)==NIN_KEYSELECT) settings_open();
        return 0;
    case MG_SETTINGS: settings_open(); return 0;
    case MG_CHANGED: settings_changed(w!=0,l!=0); return 0;
    case WM_TIMER: if(w==1) tray_add(); return 0;
    case WM_COMMAND:
        switch(LOWORD(w)) {
        case MENU_OPEN: settings_open(); break;
        case MENU_ENABLE: {
            Config copy; config_init(&copy);
            if(config_copy(&copy,&g_config)) {
                copy.enabled=!copy.enabled;
                if(app_commit(&copy)) SendMessageW(hwnd,MG_CHANGED,0,1);
            }
            config_free(&copy); break;
        }
        case MENU_EXIT: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if(w) DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd,1); settings_close(); Shell_NotifyIconW(NIM_DELETE,&tray);
        engine_stop(g_engine); g_engine=NULL; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
int WINAPI wWinMain(_In_ HINSTANCE instance,_In_opt_ HINSTANCE previous,_In_ PWSTR command,_In_ int show) {
    WNDCLASSEXW wc={sizeof(wc)}; MSG message; HANDLE mutex; HRESULT hr; INITCOMMONCONTROLSEX common={sizeof(common),ICC_LISTVIEW_CLASSES};
    (void)previous; (void)command; (void)show; g_instance=instance; config_init(&g_config);
    mutex=CreateMutexW(NULL,FALSE,L"Local\\MuteGuard.5F20322F-AC84-49DC-B3D5-7534757B7BD2");
    if(!mutex) return 1;
    if(GetLastError()==ERROR_ALREADY_EXISTS) {
        HWND existing=FindWindowW(L"MuteGuard.Tray",NULL);
        if(existing) PostMessageW(existing,MG_SETTINGS,0,0);
        else MessageBoxW(NULL,L"MuteGuard is already running on another desktop or is starting. Use its notification-area icon to open Settings.",L"MuteGuard",MB_OK|MB_ICONINFORMATION);
        CloseHandle(mutex); return 0;
    }
    hr=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    if(FAILED(hr)) { CloseHandle(mutex); return 1; }
    if(!config_path(g_config_path,ARRAYSIZE(g_config_path))) {
        MessageBoxW(NULL,L"MuteGuard could not open its local settings folder.",L"MuteGuard",MB_OK|MB_ICONERROR); goto fail;
    }
    hr=config_load(&g_config,g_config_path);
    if(FAILED(hr)) {
        /* Fail closed: no applications or outputs selected. Preserve the file
           until the user explicitly saves a replacement from Settings. */
        config_free(&g_config);
        MessageBoxW(NULL,L"MuteGuard could not read its settings. No audio will be changed. Open Settings from the tray and save your rules again.",L"MuteGuard",MB_OK|MB_ICONWARNING);
    }
    InitCommonControlsEx(&common); taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
    wc.hInstance=instance; wc.lpfnWndProc=owner_proc; wc.lpszClassName=L"MuteGuard.Tray";
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1)); wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
    if(!RegisterClassExW(&wc)) goto fail;
    /* A hidden top-level window receives Explorer's TaskbarCreated broadcast;
       a message-only window would not. No settings controls exist yet. */
    g_owner=CreateWindowExW(0,wc.lpszClassName,L"MuteGuard",0,0,0,0,0,NULL,NULL,instance,NULL);
    if(!g_owner) goto fail;
    g_engine=engine_start(&g_config,g_owner);
    if(!g_engine) { MessageBoxW(NULL,L"MuteGuard could not start its audio worker.",L"MuteGuard",MB_OK|MB_ICONERROR); DestroyWindow(g_owner); goto fail; }
    tray.cbSize=sizeof(tray); tray.hWnd=g_owner; tray.uID=1; tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_SHOWTIP;
    tray.uCallbackMessage=MG_TRAY; tray.hIcon=wc.hIcon;
    StringCchCopyW(tray.szTip,ARRAYSIZE(tray.szTip),g_config.enabled?L"MuteGuard - Muting enabled":L"MuteGuard - Muting disabled");
    tray_add();
    while(GetMessageW(&message,NULL,0,0)>0) {
        if(!settings_message(&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if(g_engine) DestroyWindow(g_owner);
    config_free(&g_config); CoUninitialize(); CloseHandle(mutex); return 0;
fail: config_free(&g_config); CoUninitialize(); CloseHandle(mutex); return 1;
}
