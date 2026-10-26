#include "muteguard.h"
#include <stdio.h>
/* Exercise the actual tray command handlers in an isolated native test host. */
#define wWinMain mg_production_entry
#include "../src/main.c"
#undef wWinMain

static int checks,failures;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)
int wmain(void) {
    WNDCLASSEXW wc={sizeof(wc)}; INITCOMMONCONTROLSEX common={sizeof(common),ICC_LISTVIEW_CLASSES}; Config saved;
    HRESULT hr=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED); MSG message; unsigned i;
    if(FAILED(hr)) return 1; setvbuf(stdout,NULL,_IONBF,0);
    g_instance=GetModuleHandleW(NULL); config_init(&g_config); config_init(&saved); InitCommonControlsEx(&common);
    StringCchPrintfW(g_config_path,ARRAYSIZE(g_config_path),L"build\\tray-test-%lu.dat",GetCurrentProcessId());
    wc.hInstance=g_instance; wc.lpfnWndProc=owner_proc; wc.lpszClassName=L"MuteGuard.TrayTest"; CHECK(RegisterClassExW(&wc)!=0);
    g_owner=CreateWindowExW(0,wc.lpszClassName,L"MuteGuard tray test",0,0,0,0,0,NULL,NULL,g_instance,NULL); CHECK(g_owner!=NULL);
    g_engine=engine_start(&g_config,NULL); CHECK(g_engine!=NULL); if(!g_engine) return 1;
    for(i=0;i<100 && engine_status(g_engine)==E_PENDING;++i) Sleep(10);
    CHECK(SUCCEEDED(engine_status(g_engine))); CHECK(FindWindowW(L"MuteGuard.Settings",NULL)==NULL);
    tray.cbSize=sizeof(tray); tray.hWnd=g_owner; tray.uID=1; tray.uFlags=NIF_MESSAGE|NIF_TIP|NIF_ICON; tray.uCallbackMessage=MG_TRAY;
    tray.hIcon=LoadIconW(g_instance,MAKEINTRESOURCEW(1)); taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
    tray_add(); CHECK(tray_attempts==0);
    /* Simulate loss of the icon followed by Explorer's recreation message. */
    CHECK(Shell_NotifyIconW(NIM_DELETE,&tray)); owner_proc(g_owner,taskbar_created,0,0); CHECK(tray_attempts==0);
    owner_proc(g_owner,WM_COMMAND,MENU_ENABLE,0); CHECK(!g_config.enabled); CHECK(config_load(&saved,g_config_path)==S_OK && !saved.enabled);
    owner_proc(g_owner,WM_COMMAND,MENU_ENABLE,0); CHECK(g_config.enabled); CHECK(config_load(&saved,g_config_path)==S_OK && saved.enabled);
    owner_proc(g_owner,WM_COMMAND,MENU_OPEN,0); CHECK(FindWindowW(L"MuteGuard.Settings",NULL)!=NULL);
    settings_close(); CHECK(g_engine!=NULL && IsWindow(g_owner));
    owner_proc(g_owner,WM_COMMAND,MENU_EXIT,0); CHECK(g_engine==NULL && !IsWindow(g_owner));
    { BOOL quit=FALSE; while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)) { if(message.message==WM_QUIT) quit=TRUE; else { TranslateMessage(&message); DispatchMessageW(&message); } } CHECK(quit); }
    DeleteFileW(g_config_path); config_free(&saved); config_free(&g_config); CoUninitialize();
    printf("Tray commands/recreation/clean shutdown: %d checks, %d failures\n",checks,failures); return failures?1:0;
}
