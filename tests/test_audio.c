#include "muteguard.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <strsafe.h>
#include <stdio.h>

typedef struct Fixture { IAudioClient *client; ISimpleAudioVolume *volume; } Fixture;
typedef struct Child { PROCESS_INFORMATION process; HANDLE ready,stop; } Child;
static int checks,failures,skips;
static const GUID external_context={0x720217b1,0xe011,0x4dd0,{0x92,0x11,0x39,0x53,0xf4,0x60,0x33,0x10}};
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)
static HRESULT fixture_create(const wchar_t *id,Fixture *f) {
    IMMDeviceEnumerator *en=NULL; IMMDevice *device=NULL; WAVEFORMATEX *format=NULL; GUID session; HRESULT hr;
    IAudioRenderClient *render=NULL; UINT32 frames; BYTE *data=NULL;
    ZeroMemory(f,sizeof(*f));
    hr=CoCreateInstance(&CLSID_MMDeviceEnumerator,NULL,CLSCTX_INPROC_SERVER,&IID_IMMDeviceEnumerator,(void**)&en);
    if(FAILED(hr)) goto done;
    hr=IMMDeviceEnumerator_GetDevice(en,id,&device); if(FAILED(hr)) goto done;
    hr=IMMDevice_Activate(device,&IID_IAudioClient,CLSCTX_INPROC_SERVER,NULL,(void**)&f->client); if(FAILED(hr)) goto done;
    hr=IAudioClient_GetMixFormat(f->client,&format); if(FAILED(hr)) goto done;
    hr=CoCreateGuid(&session); if(FAILED(hr)) goto done;
    hr=IAudioClient_Initialize(f->client,AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_NOPERSIST,1000000,0,format,&session); if(FAILED(hr)) goto done;
    hr=IAudioClient_GetService(f->client,&IID_ISimpleAudioVolume,(void**)&f->volume); if(FAILED(hr)) goto done;
    hr=IAudioClient_GetService(f->client,&IID_IAudioRenderClient,(void**)&render); if(FAILED(hr)) goto done;
    hr=IAudioClient_GetBufferSize(f->client,&frames); if(FAILED(hr)) goto done;
    hr=IAudioRenderClient_GetBuffer(render,frames,&data); if(FAILED(hr)) goto done;
    hr=IAudioRenderClient_ReleaseBuffer(render,frames,AUDCLNT_BUFFERFLAGS_SILENT); if(FAILED(hr)) goto done;
    hr=IAudioClient_Start(f->client);
done:
    if(render) IAudioRenderClient_Release(render); CoTaskMemFree(format);
    if(device) IMMDevice_Release(device); if(en) IMMDeviceEnumerator_Release(en);
    if(FAILED(hr)) {
        if(f->volume) ISimpleAudioVolume_Release(f->volume); if(f->client) IAudioClient_Release(f->client); ZeroMemory(f,sizeof(*f));
    }
    return hr;
}
static void fixture_free(Fixture *f) {
    if(f->client) IAudioClient_Stop(f->client); if(f->volume) ISimpleAudioVolume_Release(f->volume);
    if(f->client) IAudioClient_Release(f->client); ZeroMemory(f,sizeof(*f));
}
static BOOL muted(ISimpleAudioVolume *v,BOOL desired) {
    BOOL mute=FALSE; return v && SUCCEEDED(ISimpleAudioVolume_GetMute(v,&mute)) && mute==desired;
}
static BOOL wait_mute(ISimpleAudioVolume *v,BOOL desired) {
    ULONGLONG end=GetTickCount64()+5000;
    do { if(muted(v,desired)) return TRUE; Sleep(10); } while(GetTickCount64()<end);
    return FALSE;
}
static ISimpleAudioVolume *pid_volume(const wchar_t *id,DWORD pid) {
    IMMDeviceEnumerator *en=NULL; IMMDevice *device=NULL; IAudioSessionManager2 *manager=NULL;
    IAudioSessionEnumerator *sessions=NULL; ISimpleAudioVolume *volume=NULL; int count=0,i;
    if(FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator,NULL,CLSCTX_INPROC_SERVER,&IID_IMMDeviceEnumerator,(void**)&en))) goto done;
    if(FAILED(IMMDeviceEnumerator_GetDevice(en,id,&device))) goto done;
    if(FAILED(IMMDevice_Activate(device,&IID_IAudioSessionManager2,CLSCTX_INPROC_SERVER,NULL,(void**)&manager))) goto done;
    if(FAILED(IAudioSessionManager2_GetSessionEnumerator(manager,&sessions)) || FAILED(IAudioSessionEnumerator_GetCount(sessions,&count))) goto done;
    for(i=0;i<count && !volume;++i) {
        IAudioSessionControl *c=NULL; IAudioSessionControl2 *c2=NULL; DWORD session_pid=0;
        if(SUCCEEDED(IAudioSessionEnumerator_GetSession(sessions,i,&c))) {
            if(SUCCEEDED(IAudioSessionControl_QueryInterface(c,&IID_IAudioSessionControl2,(void**)&c2))) {
                HRESULT phr=IAudioSessionControl2_GetProcessId(c2,&session_pid);
                if(phr==S_OK && session_pid==pid)
                    IAudioSessionControl2_QueryInterface(c2,&IID_ISimpleAudioVolume,(void**)&volume);
                IAudioSessionControl2_Release(c2);
            }
            IAudioSessionControl_Release(c);
        }
    }
done:
    if(sessions) IAudioSessionEnumerator_Release(sessions); if(manager) IAudioSessionManager2_Release(manager);
    if(device) IMMDevice_Release(device); if(en) IMMDeviceEnumerator_Release(en); return volume;
}
static BOOL child_start(Child *c,const wchar_t *id,BOOL target) {
    wchar_t self[MAX_PATH],path[MAX_PATH],prefix[128],ready[150],stop[150],command[4096],*slash;
    STARTUPINFOW startup={sizeof(startup)}; static unsigned serial; static BOOL copied[2];
    ZeroMemory(c,sizeof(*c)); GetModuleFileNameW(NULL,self,ARRAYSIZE(self)); StringCchCopyW(path,ARRAYSIZE(path),self);
    slash=wcsrchr(path,L'\\'); if(!slash) return FALSE; *slash=0;
    StringCchCatW(path,ARRAYSIZE(path),target?L"\\test_audio_child.exe":L"\\test_audio_unrelated.exe");
    if(!copied[target?1:0]) { if(!CopyFileW(self,path,FALSE)) return FALSE; copied[target?1:0]=TRUE; }
    StringCchPrintfW(prefix,ARRAYSIZE(prefix),L"Local\\MuteGuardTest.%lu.%u",GetCurrentProcessId(),++serial);
    StringCchPrintfW(ready,ARRAYSIZE(ready),L"%s.ready",prefix); StringCchPrintfW(stop,ARRAYSIZE(stop),L"%s.stop",prefix);
    c->ready=CreateEventW(NULL,TRUE,FALSE,ready); c->stop=CreateEventW(NULL,TRUE,FALSE,stop);
    if(!c->ready || !c->stop) return FALSE;
    StringCchPrintfW(command,ARRAYSIZE(command),L"\"%s\" --fixture \"%s\" \"%s\"",path,id,prefix);
    if(!CreateProcessW(path,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&c->process)) return FALSE;
    { HANDLE handles[2]={c->ready,c->process.hProcess}; return WaitForMultipleObjects(2,handles,FALSE,10000)==WAIT_OBJECT_0; }
}
static void child_stop(Child *c) {
    if(c->stop) SetEvent(c->stop);
    if(c->process.hProcess) {
        if(WaitForSingleObject(c->process.hProcess,10000)==WAIT_TIMEOUT) TerminateProcess(c->process.hProcess,1);
        CloseHandle(c->process.hThread); CloseHandle(c->process.hProcess);
    }
    if(c->ready) CloseHandle(c->ready); if(c->stop) CloseHandle(c->stop); ZeroMemory(c,sizeof(*c));
}
static int child_main(const wchar_t *id,const wchar_t *prefix) {
    wchar_t name[150]; HANDLE ready,stop; Fixture fixture={0}; HRESULT hr;
    StringCchPrintfW(name,ARRAYSIZE(name),L"%s.ready",prefix); ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,name);
    StringCchPrintfW(name,ARRAYSIZE(name),L"%s.stop",prefix); stop=OpenEventW(SYNCHRONIZE,FALSE,name);
    if(!ready || !stop) return 2;
    hr=fixture_create(id,&fixture); if(FAILED(hr)) { CloseHandle(ready); CloseHandle(stop); return 3; }
    SetEvent(ready); WaitForSingleObject(stop,30000); fixture_free(&fixture); CloseHandle(ready); CloseHandle(stop); return 0;
}
static ULONGLONG process_cpu(HANDLE process) {
    FILETIME created,exit,kernel,user; ULARGE_INTEGER k,u;
    if(!GetProcessTimes(process,&created,&exit,&kernel,&user)) return 0;
    k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime; return k.QuadPart+u.QuadPart;
}
/* Exercise only the application's own tray command IDs, with PID verification. */
static BOOL production_command(DWORD pid,UINT command) {
    HWND window=FindWindowW(L"MuteGuard.Tray",NULL); DWORD owner=0; DWORD_PTR result;
    if(!window) return FALSE;
    GetWindowThreadProcessId(window,&owner);
    return owner==pid && SendMessageTimeoutW(window,WM_COMMAND,command,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,10000,&result)!=0;
}
/* Verify the separately running Release executable. Lifecycle mode requires a
   temporary test-only config and exits that instance after testing restoration. */
static int production_test(DWORD pid,BOOL lifecycle) {
    Config c; wchar_t path[MAX_PATH]; Fixture burst[24]={0},control={0},premuted={0},*matrix=NULL;
    OutputList outputs={0}; const wchar_t *selected=NULL; size_t i,active=0; HANDLE process=NULL;
    ULONGLONG cpu,start; config_init(&c);
    CHECK(config_path(path,ARRAYSIZE(path))); CHECK(config_load(&c,path)==S_OK);
    CHECK(c.enabled && c.app_count==1 && config_matches_app(&c,L"test_audio.exe"));
    if(!c.enabled || c.app_count!=1 || !config_matches_app(&c,L"test_audio.exe")) goto done;
    CHECK(SUCCEEDED(outputs_enumerate(&outputs)));
    for(i=0;i<outputs.count;++i) if(outputs.items[i].state==DEVICE_STATE_ACTIVE && config_output_mode(&c,outputs.items[i].id)==OUTPUT_MUTE) { selected=outputs.items[i].id; break; }
    CHECK(selected!=NULL); if(!selected) goto done;
    matrix=mg_alloc(outputs.count*sizeof(*matrix)); CHECK(matrix!=NULL); if(!matrix) goto done;
    process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid); CHECK(process!=NULL);
    CHECK(SUCCEEDED(fixture_create(selected,&control))); CHECK(wait_mute(control.volume,TRUE));
    if(lifecycle) {
        CHECK(production_command(pid,101)); CHECK(wait_mute(control.volume,FALSE));
        CHECK(SUCCEEDED(fixture_create(selected,&premuted)));
        if(premuted.volume) CHECK(SUCCEEDED(ISimpleAudioVolume_SetMute(premuted.volume,TRUE,&external_context)));
        CHECK(production_command(pid,101)); CHECK(wait_mute(control.volume,TRUE));
    }
    cpu=process?process_cpu(process):0; start=GetTickCount64();
    for(i=0;i<outputs.count;++i) if(outputs.items[i].state==DEVICE_STATE_ACTIVE) {
        ++active; CHECK(SUCCEEDED(fixture_create(outputs.items[i].id,&matrix[i])));
    }
    for(i=0;i<ARRAYSIZE(burst);++i) CHECK(SUCCEEDED(fixture_create(selected,&burst[i])));
    for(i=0;i<outputs.count;++i) if(matrix[i].volume) CHECK(wait_mute(matrix[i].volume,config_output_mode(&c,outputs.items[i].id)==OUTPUT_MUTE));
    for(i=0;i<ARRAYSIZE(burst);++i) CHECK(wait_mute(burst[i].volume,TRUE));
    Sleep(250);
    printf("Release session-change sample: %zu render endpoints plus 24 matching sessions, %llu ms wall time, %.3f ms process CPU\n",active,GetTickCount64()-start,process?(double)(process_cpu(process)-cpu)/10000.0:0.0);
    if(lifecycle) {
        CHECK(production_command(pid,101)); CHECK(wait_mute(control.volume,FALSE));
        for(i=0;i<outputs.count;++i) if(matrix[i].volume) CHECK(wait_mute(matrix[i].volume,FALSE));
        for(i=0;i<ARRAYSIZE(burst);++i) CHECK(wait_mute(burst[i].volume,FALSE));
        CHECK(muted(premuted.volume,TRUE));
        CHECK(production_command(pid,101)); CHECK(wait_mute(control.volume,TRUE));
        for(i=0;i<outputs.count;++i) if(matrix[i].volume) CHECK(wait_mute(matrix[i].volume,config_output_mode(&c,outputs.items[i].id)==OUTPUT_MUTE));
        for(i=0;i<ARRAYSIZE(burst);++i) CHECK(wait_mute(burst[i].volume,TRUE));
        CHECK(production_command(pid,102)); CHECK(process && WaitForSingleObject(process,10000)==WAIT_OBJECT_0);
        for(i=0;i<outputs.count;++i) if(matrix[i].volume) CHECK(muted(matrix[i].volume,FALSE));
        for(i=0;i<ARRAYSIZE(burst);++i) CHECK(muted(burst[i].volume,FALSE));
        CHECK(muted(control.volume,FALSE)); CHECK(muted(premuted.volume,TRUE));
    }
done:
    for(i=0;i<ARRAYSIZE(burst);++i) fixture_free(&burst[i]);
    if(matrix) for(i=0;i<outputs.count;++i) fixture_free(&matrix[i]);
    mg_free(matrix); fixture_free(&control); fixture_free(&premuted);
    if(process) CloseHandle(process); outputs_free(&outputs); config_free(&c);
    printf("Production integration: %d checks, %d failures, %d skipped scenarios\n",checks,failures,skips); return failures?1:0;
}
int wmain(int argc,wchar_t **argv) {
    OutputList outputs={0}; Config c; Engine *e=NULL; size_t i,selected=SIZE_MAX,protected=SIZE_MAX;
    Fixture existing={0},premuted={0},new_session={0},guarded={0}; Child child={0}; ISimpleAudioVolume *other=NULL;
    HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED); wchar_t self[MAX_PATH],*base; ULONGLONG begin;
    setvbuf(stdout,NULL,_IONBF,0);
    if(FAILED(hr)) return 1;
    if(argc==4 && !wcscmp(argv[1],L"--fixture")) { int result=child_main(argv[2],argv[3]); CoUninitialize(); return result; }
    if(argc==3 && (!wcscmp(argv[1],L"--production") || !wcscmp(argv[1],L"--production-lifecycle"))) {
        int result=production_test(wcstoul(argv[2],NULL,10),!wcscmp(argv[1],L"--production-lifecycle")); CoUninitialize(); return result;
    }
    config_init(&c); c.all_outputs=FALSE; hr=outputs_enumerate(&outputs); CHECK(SUCCEEDED(hr));
    for(i=0;i<outputs.count;++i) if(outputs.items[i].state==DEVICE_STATE_ACTIVE) {
        if(selected==SIZE_MAX) selected=i; else if(protected==SIZE_MAX) protected=i;
    }
    if(selected==SIZE_MAX) { printf("SKIP: no active render endpoint; live audio tests unavailable\n"); ++skips; goto done; }
    printf("Live Core Audio integration on an active playback endpoint.\n");
    GetModuleFileNameW(NULL,self,ARRAYSIZE(self)); base=wcsrchr(self,L'\\'); base=base?base+1:self;
    CHECK(config_add_app(&c,base)); CHECK(config_set_output(&c,outputs.items[selected].id,L"Test output",OUTPUT_MUTE));
    CHECK(SUCCEEDED(fixture_create(outputs.items[selected].id,&existing)));
    CHECK(SUCCEEDED(fixture_create(outputs.items[selected].id,&premuted)));
    if(!existing.volume || !premuted.volume) goto done;
    CHECK(SUCCEEDED(ISimpleAudioVolume_SetMute(premuted.volume,TRUE,&external_context)));
    if(protected!=SIZE_MAX) {
        CHECK(config_set_output(&c,outputs.items[protected].id,L"Protected test output",OUTPUT_PROTECT));
        CHECK(SUCCEEDED(fixture_create(outputs.items[protected].id,&guarded)));
    } else { printf("SKIP: no second active output for protected-endpoint live test\n"); ++skips; }
    begin=GetTickCount64(); e=engine_start(&c,NULL); CHECK(e!=NULL); if(!e) goto done;
    CHECK(wait_mute(existing.volume,TRUE)); CHECK(muted(premuted.volume,TRUE));
    printf("Existing-session mute observed after %llu ms (10 ms observation interval).\n",GetTickCount64()-begin);
    if(guarded.volume) CHECK(muted(guarded.volume,FALSE));
    begin=GetTickCount64(); CHECK(SUCCEEDED(fixture_create(outputs.items[selected].id,&new_session)));
    CHECK(wait_mute(new_session.volume,TRUE)); printf("New-session mute observed after %llu ms including fixture creation.\n",GetTickCount64()-begin);
    CHECK(muted(existing.volume,TRUE));
    CHECK(child_start(&child,outputs.items[selected].id,FALSE));
    if(child.process.hProcess) { other=pid_volume(outputs.items[selected].id,child.process.dwProcessId); CHECK(other!=NULL); Sleep(150); CHECK(muted(other,FALSE)); }
    if(other) { ISimpleAudioVolume_Release(other); other=NULL; } child_stop(&child);
    c.enabled=FALSE; CHECK(engine_update(e,&c)); CHECK(wait_mute(existing.volume,FALSE)); CHECK(wait_mute(new_session.volume,FALSE)); CHECK(muted(premuted.volume,TRUE));
    c.enabled=TRUE; CHECK(engine_update(e,&c)); CHECK(wait_mute(existing.volume,TRUE)); CHECK(wait_mute(new_session.volume,TRUE));
    /* External volume adjustment invalidates ownership, even while still muted. */
    CHECK(SUCCEEDED(ISimpleAudioVolume_SetMasterVolume(existing.volume,0.73f,&external_context))); Sleep(200);
    CHECK(SUCCEEDED(ISimpleAudioVolume_SetMute(new_session.volume,FALSE,&external_context))); Sleep(300); CHECK(muted(new_session.volume,FALSE));
    c.enabled=FALSE; CHECK(engine_update(e,&c)); Sleep(200);
    CHECK(muted(existing.volume,TRUE)); CHECK(muted(new_session.volume,FALSE)); CHECK(muted(premuted.volume,TRUE));
    CHECK(config_add_app(&c,L"test_audio_child.exe")); c.enabled=TRUE; CHECK(engine_update(e,&c)); CHECK(wait_mute(new_session.volume,TRUE));
    for(i=0;i<2;++i) {
        CHECK(child_start(&child,outputs.items[selected].id,TRUE));
        if(child.process.hProcess) { other=pid_volume(outputs.items[selected].id,child.process.dwProcessId); CHECK(other!=NULL); CHECK(wait_mute(other,TRUE)); }
        if(other) { ISimpleAudioVolume_Release(other); other=NULL; } child_stop(&child);
    }
    {
        Child twin={0}; ISimpleAudioVolume *second=NULL;
        CHECK(child_start(&child,outputs.items[selected].id,TRUE)); CHECK(child_start(&twin,outputs.items[selected].id,TRUE));
        other=pid_volume(outputs.items[selected].id,child.process.dwProcessId); second=pid_volume(outputs.items[selected].id,twin.process.dwProcessId);
        CHECK(other!=NULL); CHECK(second!=NULL); CHECK(wait_mute(other,TRUE)); CHECK(wait_mute(second,TRUE));
        if(other) { ISimpleAudioVolume_Release(other); other=NULL; } if(second) ISimpleAudioVolume_Release(second);
        child_stop(&child); child_stop(&twin);
    }
    CHECK(config_set_output(&c,L"MuteGuard-test-missing-endpoint",L"Missing test output",OUTPUT_MUTE)); CHECK(engine_update(e,&c));
    begin=GetTickCount64(); while(engine_status(e)!=S_FALSE && GetTickCount64()-begin<5000) Sleep(10); CHECK(engine_status(e)==S_FALSE);
    CHECK(config_set_output(&c,L"MuteGuard-test-missing-endpoint",L"Missing test output",OUTPUT_IGNORE)); CHECK(engine_update(e,&c));
    begin=GetTickCount64(); while(engine_status(e)!=S_OK && GetTickCount64()-begin<5000) Sleep(10); CHECK(engine_status(e)==S_OK);
    /* Protecting a previously selected output safely releases owned mutes. */
    CHECK(config_set_output(&c,outputs.items[selected].id,L"Now protected",OUTPUT_PROTECT)); CHECK(engine_update(e,&c));
    CHECK(wait_mute(new_session.volume,FALSE)); CHECK(muted(existing.volume,TRUE)); CHECK(muted(premuted.volume,TRUE));
    CHECK(config_set_output(&c,outputs.items[selected].id,L"Selected again",OUTPUT_MUTE)); CHECK(engine_update(e,&c)); CHECK(wait_mute(new_session.volume,TRUE));
    /* Repeated changes exercise notifier lifetime and callback teardown. */
    for(i=0;i<20;++i) { c.enabled=FALSE; CHECK(engine_update(e,&c)); CHECK(wait_mute(new_session.volume,FALSE)); c.enabled=TRUE; CHECK(engine_update(e,&c)); CHECK(wait_mute(new_session.volume,TRUE)); }
    CHECK(SUCCEEDED(engine_status(e))); engine_stop(e); e=NULL;
    CHECK(muted(new_session.volume,FALSE)); CHECK(muted(premuted.volume,TRUE)); CHECK(muted(existing.volume,TRUE));
    if(guarded.volume) CHECK(muted(guarded.volume,FALSE));
done:
    if(e) engine_stop(e); if(other) ISimpleAudioVolume_Release(other); child_stop(&child);
    fixture_free(&existing); fixture_free(&premuted); fixture_free(&new_session); fixture_free(&guarded);
    config_free(&c); outputs_free(&outputs); CoUninitialize();
    printf("Audio integration: %d checks, %d failures, %d skipped scenarios\n",checks,failures,skips); return failures?1:0;
}
