#include "muteguard.h"
#include <stdio.h>
/* Close only this project's tray application through its real Exit handler.
   Used by the Release test harness; no forced termination or audio writes. */
int wmain(int argc,wchar_t **argv) {
    HWND window; DWORD pid=0,length=32768; wchar_t path[32768]; HANDLE process;
    if(argc!=2) return 2;
    window=FindWindowW(L"MuteGuard.Tray",NULL); if(!window) return 0;
    GetWindowThreadProcessId(window,&pid); process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
    if(!process) return 3;
    if(!QueryFullProcessImageNameW(process,0,path,&length) || CompareStringOrdinal(path,-1,argv[1],-1,TRUE)!=CSTR_EQUAL) { CloseHandle(process); return 4; }
    if(!PostMessageW(window,WM_COMMAND,102,0) || WaitForSingleObject(process,10000)!=WAIT_OBJECT_0) { CloseHandle(process); return 5; }
    CloseHandle(process); printf("MuteGuard exited through its tray command handler.\n"); return 0;
}
