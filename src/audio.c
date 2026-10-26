#include "muteguard.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <strsafe.h>

/* The SDK's C headers declare these MIDL IDs but uuid.lib does not define them.
   Values are the interface UUIDs published in the Windows SDK headers. */
DEFINE_GUID(CLSID_MMDeviceEnumerator,0xbcde0395,0xe52f,0x467c,0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e);
DEFINE_GUID(IID_IMMDeviceEnumerator,0xa95664d2,0x9614,0x4f35,0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6);
DEFINE_GUID(IID_IMMEndpoint,0x1be09788,0x6894,0x4089,0x85,0x86,0x9a,0x2a,0x6c,0x26,0x5a,0xc5);
DEFINE_GUID(IID_IMMNotificationClient,0x7991eec9,0x7e89,0x4d85,0x83,0x90,0x6c,0x70,0x3c,0xec,0x60,0xc0);
DEFINE_GUID(IID_IAudioSessionEvents,0x24918acc,0x64b3,0x37c1,0x8c,0xa9,0x74,0xa6,0x6e,0x99,0x57,0xa8);
DEFINE_GUID(IID_IAudioSessionControl2,0xbfb7ff88,0x7239,0x4fc9,0x8f,0xa2,0x07,0xc9,0x50,0xbe,0x9c,0x6d);
DEFINE_GUID(IID_IAudioSessionNotification,0x641dd20b,0x4d41,0x49cc,0xab,0xa3,0x17,0x4b,0x94,0x77,0xbb,0x08);
DEFINE_GUID(IID_IAudioSessionManager2,0x77aa99a0,0x1bd6,0x484f,0x8b,0xc7,0x2c,0x65,0x4c,0x9a,0x9b,0x6f);
DEFINE_GUID(IID_ISimpleAudioVolume,0x87ce5498,0x68d6,0x44e5,0x92,0x15,0x6d,0xa4,0x7e,0xf8,0x83,0xd8);
DEFINE_GUID(IID_IAudioClient,0x1cb9ad4c,0xdbfa,0x4c32,0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2);
DEFINE_GUID(IID_IAudioRenderClient,0xf294acfc,0x3146,0x4483,0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2);

#define WAKE_NEW 1
#define WAKE_SESSION 2
#define WAKE_DEVICE 4
#define WAKE_CONFIG 8
#define WAKE_STOP 16
#define WAKE_REBUILD 32
#define WAKE_SCAN 64
#define RECOVERY_MS 60000

typedef struct Packet {
    SLIST_ENTRY entry;
    IAudioSessionControl *control;
    DWORD generation;
    wchar_t id[MG_ID];
} Packet;
/* HeapAlloc on x64 provides the 16-byte alignment required by SLIST_HEADER. */
typedef struct Hub {
    SLIST_HEADER queue;
    LONG refs;
    volatile LONG accepting, producers, flags;
    HANDLE event;
    GUID context;
} Hub;
typedef struct DeviceNotice { IMMNotificationClient iface; LONG refs; Hub *hub; } DeviceNotice;
typedef struct NewNotice {
    IAudioSessionNotification iface; LONG refs; Hub *hub;
    DWORD generation; wchar_t id[MG_ID];
} NewNotice;
typedef struct SessionNotice {
    IAudioSessionEvents iface; LONG refs; Hub *hub;
    volatile LONG external_version, unmuted, dead;
} SessionNotice;
typedef struct Session {
    struct Session *next;
    IAudioSessionControl2 *control;
    ISimpleAudioVolume *volume;
    SessionNotice *notice;
    wchar_t *instance;
    wchar_t app[MG_NAME];
    LONG claimed_version, seen_version;
    BOOL owned, suppressed;
} Session;
typedef struct Endpoint {
    struct Endpoint *next;
    wchar_t *id;
    DWORD generation;
    IAudioSessionManager2 *manager;
    NewNotice *notice;
    Session *sessions;
} Endpoint;
struct Engine {
    HANDLE thread;
    Hub *hub;
    SRWLOCK lock;
    Config *pending;
    Config current;
    Endpoint *endpoints;
    IMMDeviceEnumerator *enumerator;
    DeviceNotice *notice;
    DWORD generation;
    HWND notify;
    volatile LONG status;
#ifdef MG_TESTING
    wchar_t held_endpoint[MG_ID];
    volatile LONG held;
#endif
};

static void hub_addref(Hub *h) { InterlockedIncrement(&h->refs); }
static void hub_release(Hub *h) {
    if (!InterlockedDecrement(&h->refs)) { CloseHandle(h->event); mg_free(h); }
}
static void wake(Hub *h,LONG flags) { InterlockedOr(&h->flags,flags); SetEvent(h->event); }
static BOOL iid_is(REFIID a,REFIID b) { return IsEqualGUID(a,b); }

static HRESULT STDMETHODCALLTYPE dn_query(IMMNotificationClient *p,REFIID iid,void **out) {
    if(!out) return E_POINTER; *out=NULL;
    if(!iid_is(iid,&IID_IUnknown) && !iid_is(iid,&IID_IMMNotificationClient)) return E_NOINTERFACE;
    *out=p; IMMNotificationClient_AddRef(p); return S_OK;
}
static ULONG STDMETHODCALLTYPE dn_addref(IMMNotificationClient *p) { return (ULONG)InterlockedIncrement(&((DeviceNotice*)p)->refs); }
static ULONG STDMETHODCALLTYPE dn_release(IMMNotificationClient *p) {
    DeviceNotice *n=(DeviceNotice*)p; LONG refs=InterlockedDecrement(&n->refs);
    if(!refs) { hub_release(n->hub); mg_free(n); } return (ULONG)refs;
}
static HRESULT STDMETHODCALLTYPE dn_state(IMMNotificationClient *p,LPCWSTR id,DWORD state) {
    (void)id; (void)state; wake(((DeviceNotice*)p)->hub,WAKE_DEVICE); return S_OK;
}
static HRESULT STDMETHODCALLTYPE dn_added(IMMNotificationClient *p,LPCWSTR id) { return dn_state(p,id,0); }
static HRESULT STDMETHODCALLTYPE dn_removed(IMMNotificationClient *p,LPCWSTR id) { return dn_state(p,id,0); }
static HRESULT STDMETHODCALLTYPE dn_default(IMMNotificationClient *p,EDataFlow flow,ERole role,LPCWSTR id) {
    (void)role; return flow==eRender?dn_state(p,id,0):S_OK;
}
static HRESULT STDMETHODCALLTYPE dn_property(IMMNotificationClient *p,LPCWSTR id,const PROPERTYKEY key) {
    if(key.pid==PKEY_Device_FriendlyName.pid && IsEqualGUID(&key.fmtid,&PKEY_Device_FriendlyName.fmtid)) return dn_state(p,id,0);
    return S_OK;
}
static const IMMNotificationClientVtbl device_vtbl={dn_query,dn_addref,dn_release,dn_state,dn_added,dn_removed,dn_default,dn_property};

static HRESULT STDMETHODCALLTYPE nn_query(IAudioSessionNotification *p,REFIID iid,void **out) {
    if(!out) return E_POINTER; *out=NULL;
    if(!iid_is(iid,&IID_IUnknown) && !iid_is(iid,&IID_IAudioSessionNotification)) return E_NOINTERFACE;
    *out=p; IAudioSessionNotification_AddRef(p); return S_OK;
}
static ULONG STDMETHODCALLTYPE nn_addref(IAudioSessionNotification *p) { return (ULONG)InterlockedIncrement(&((NewNotice*)p)->refs); }
static ULONG STDMETHODCALLTYPE nn_release(IAudioSessionNotification *p) {
    NewNotice *n=(NewNotice*)p; LONG refs=InterlockedDecrement(&n->refs);
    if(!refs) { hub_release(n->hub); mg_free(n); } return (ULONG)refs;
}
static HRESULT STDMETHODCALLTYPE nn_created(IAudioSessionNotification *p,IAudioSessionControl *control) {
    NewNotice *n=(NewNotice*)p; Hub *h=n->hub; Packet *packet;
    /* No waits, enumeration, unregistering, or final WASAPI releases in callbacks.
       Retain the supplied session: a later enumerator can omit it entirely. */
    InterlockedIncrement(&h->producers);
    if(InterlockedCompareExchange(&h->accepting,0,0)) {
        packet=mg_alloc(sizeof(*packet));
        if(packet) {
            IAudioSessionControl_AddRef(control); packet->control=control; packet->generation=n->generation;
            StringCchCopyW(packet->id,ARRAYSIZE(packet->id),n->id);
            InterlockedPushEntrySList(&h->queue,&packet->entry);
            wake(h,WAKE_NEW);
        } else wake(h,WAKE_SCAN); /* Slow reconciliation retries on allocation failure. */
    }
    InterlockedDecrement(&h->producers); SetEvent(h->event); return S_OK;
}
static const IAudioSessionNotificationVtbl new_vtbl={nn_query,nn_addref,nn_release,nn_created};

static HRESULT STDMETHODCALLTYPE sn_query(IAudioSessionEvents *p,REFIID iid,void **out) {
    if(!out) return E_POINTER; *out=NULL;
    if(!iid_is(iid,&IID_IUnknown) && !iid_is(iid,&IID_IAudioSessionEvents)) return E_NOINTERFACE;
    *out=p; IAudioSessionEvents_AddRef(p); return S_OK;
}
static ULONG STDMETHODCALLTYPE sn_addref(IAudioSessionEvents *p) { return (ULONG)InterlockedIncrement(&((SessionNotice*)p)->refs); }
static ULONG STDMETHODCALLTYPE sn_release(IAudioSessionEvents *p) {
    SessionNotice *n=(SessionNotice*)p; LONG refs=InterlockedDecrement(&n->refs);
    if(!refs) { hub_release(n->hub); mg_free(n); } return (ULONG)refs;
}
static HRESULT STDMETHODCALLTYPE sn_text(IAudioSessionEvents *p,LPCWSTR text,LPCGUID ctx) { (void)p; (void)text; (void)ctx; return S_OK; }
static HRESULT STDMETHODCALLTYPE sn_volume(IAudioSessionEvents *p,float volume,BOOL mute,LPCGUID ctx) {
    SessionNotice *n=(SessionNotice*)p; (void)volume;
    if(!ctx || !IsEqualGUID(ctx,&n->hub->context)) {
        InterlockedIncrement(&n->external_version);
        if(!mute) InterlockedExchange(&n->unmuted,1);
        wake(n->hub,WAKE_SESSION);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sn_channel(IAudioSessionEvents *p,DWORD count,float volumes[],DWORD changed,LPCGUID ctx) {
    (void)count; (void)volumes; (void)changed;
    return sn_volume(p,0,TRUE,ctx); /* Conservatively relinquish ownership. */
}
static HRESULT STDMETHODCALLTYPE sn_group(IAudioSessionEvents *p,LPCGUID group,LPCGUID ctx) { (void)p;(void)group;(void)ctx;return S_OK; }
static HRESULT STDMETHODCALLTYPE sn_state(IAudioSessionEvents *p,AudioSessionState state) {
    SessionNotice *n=(SessionNotice*)p;
    if(state==AudioSessionStateExpired) InterlockedExchange(&n->dead,1);
    wake(n->hub,state==AudioSessionStateActive?WAKE_SCAN:WAKE_SESSION); return S_OK;
}
static HRESULT STDMETHODCALLTYPE sn_disconnected(IAudioSessionEvents *p,AudioSessionDisconnectReason why) {
    SessionNotice *n=(SessionNotice*)p; (void)why;
    InterlockedExchange(&n->dead,1); wake(n->hub,WAKE_REBUILD); return S_OK;
}
static const IAudioSessionEventsVtbl session_vtbl={sn_query,sn_addref,sn_release,sn_text,sn_text,sn_volume,sn_channel,sn_group,sn_state,sn_disconnected};

void outputs_free(OutputList *list) {
    size_t i; for(i=0;i<list->count;++i) { mg_free(list->items[i].id); mg_free(list->items[i].name); }
    mg_free(list->items); ZeroMemory(list,sizeof(*list));
}
/* The SDK's C COM declaration omits GetId's documented returned-string
   annotation. Express that contract once for callers and static analysis. */
_Ret_maybenull_z_ static wchar_t *device_id(IMMDevice *device) {
    LPWSTR id=NULL;
    if(FAILED(IMMDevice_GetId(device,&id))) { CoTaskMemFree(id); return NULL; }
    return id;
}
HRESULT outputs_enumerate(OutputList *list) {
    IMMDeviceEnumerator *en=NULL; IMMDeviceCollection *collection=NULL; UINT count=0,i; HRESULT hr;
    ZeroMemory(list,sizeof(*list));
    hr=CoCreateInstance(&CLSID_MMDeviceEnumerator,NULL,CLSCTX_INPROC_SERVER,&IID_IMMDeviceEnumerator,(void**)&en);
    if(FAILED(hr)) return hr;
    hr=IMMDeviceEnumerator_EnumAudioEndpoints(en,eRender,DEVICE_STATE_ACTIVE|DEVICE_STATE_DISABLED|DEVICE_STATE_UNPLUGGED,&collection);
    if(SUCCEEDED(hr)) hr=IMMDeviceCollection_GetCount(collection,&count);
    if(SUCCEEDED(hr) && count) {
        list->items=mg_alloc(count*sizeof(*list->items)); if(!list->items) hr=E_OUTOFMEMORY;
    }
    for(i=0;SUCCEEDED(hr) && i<count;++i) {
        IMMDevice *device=NULL; IPropertyStore *store=NULL; LPWSTR id=NULL; PROPVARIANT name; DWORD state=0;
        PropVariantInit(&name);
        if(SUCCEEDED(IMMDeviceCollection_Item(collection,i,&device)) && (id=device_id(device))!=NULL && wcslen(id)<MG_ID) {
            OutputInfo *out=&list->items[list->count];
            IMMDevice_GetState(device,&state);
            if(SUCCEEDED(IMMDevice_OpenPropertyStore(device,STGM_READ,&store))) IPropertyStore_GetValue(store,&PKEY_Device_FriendlyName,&name);
            out->id=mg_string(id); out->name=mg_string(name.vt==VT_LPWSTR && name.pwszVal && *name.pwszVal?name.pwszVal:id); out->state=state;
            if(!out->id || !out->name) { mg_free(out->id); mg_free(out->name); hr=E_OUTOFMEMORY; }
            else ++list->count;
        }
        PropVariantClear(&name); if(store) IPropertyStore_Release(store); CoTaskMemFree(id); if(device) IMMDevice_Release(device);
    }
    if(collection) IMMDeviceCollection_Release(collection); IMMDeviceEnumerator_Release(en);
    if(FAILED(hr)) outputs_free(list); return hr;
}

static BOOL process_name(DWORD pid,wchar_t *name) {
    HANDLE process; DWORD count=MG_NAME,code; wchar_t local[MG_NAME],*path=local,*base; BOOL ok=FALSE,queried=FALSE;
    if(!pid) return FALSE;
    process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid); if(!process) return FALSE;
    if(GetExitCodeProcess(process,&code) && code==STILL_ACTIVE) {
        queried=QueryFullProcessImageNameW(process,0,path,&count);
        if(!queried && GetLastError()==ERROR_INSUFFICIENT_BUFFER) {
            count=32768; path=mg_alloc(count*sizeof(*path));
            if(path) queried=QueryFullProcessImageNameW(process,0,path,&count);
        }
    }
    if(queried) {
        base=wcsrchr(path,L'\\'); base=base?base+1:path;
        ok=SUCCEEDED(StringCchCopyW(name,MG_NAME,base)) && config_valid_app(name);
    }
    if(path!=local) mg_free(path); CloseHandle(process); return ok;
}
static void session_restore(Session *s,Hub *h) {
    BOOL mute; LONG version=InterlockedCompareExchange(&s->notice->external_version,0,0);
    /* Do not unmute pre-muted or externally touched sessions. There is no atomic
       compare-and-set in Core Audio; an external change can still race SetMute. */
    if(s->owned && version==s->claimed_version && !s->notice->dead &&
       SUCCEEDED(ISimpleAudioVolume_GetMute(s->volume,&mute)) && mute &&
       InterlockedCompareExchange(&s->notice->external_version,0,0)==version)
        ISimpleAudioVolume_SetMute(s->volume,FALSE,&h->context);
    s->owned=FALSE;
}
static void session_free(Session *s,Hub *h,BOOL restore) {
    if(restore) session_restore(s,h);
    IAudioSessionControl2_UnregisterAudioSessionNotification(s->control,&s->notice->iface);
    IAudioSessionEvents_Release(&s->notice->iface);
    ISimpleAudioVolume_Release(s->volume); IAudioSessionControl2_Release(s->control); CoTaskMemFree(s->instance); mg_free(s);
}
static void endpoint_free(Endpoint *p,Hub *h) {
    Session *s;
    IAudioSessionManager2_UnregisterSessionNotification(p->manager,&p->notice->iface);
    while((s=p->sessions)!=NULL) { p->sessions=s->next; session_free(s,h,TRUE); }
    IAudioSessionNotification_Release(&p->notice->iface); IAudioSessionManager2_Release(p->manager); mg_free(p->id); mg_free(p);
}
static void session_apply(Engine *e,Endpoint *p,Session *s) {
    BOOL mute; LONG version=InterlockedCompareExchange(&s->notice->external_version,0,0);
    if(version!=s->seen_version) { s->owned=FALSE; s->seen_version=version; }
    if(InterlockedExchange(&s->notice->unmuted,0)) s->suppressed=TRUE;
    /* Re-check exact endpoint permission immediately before every mute write. */
    if(!e->current.enabled || config_output_mode(&e->current,p->id)!=OUTPUT_MUTE ||
       !config_matches_app(&e->current,s->app) || s->suppressed || s->notice->dead) return;
    if(SUCCEEDED(ISimpleAudioVolume_GetMute(s->volume,&mute)) && !mute &&
       version==InterlockedCompareExchange(&s->notice->external_version,0,0)) {
        if(SUCCEEDED(ISimpleAudioVolume_SetMute(s->volume,TRUE,&e->hub->context)) &&
           version==InterlockedCompareExchange(&s->notice->external_version,0,0)) {
            s->owned=TRUE; s->claimed_version=version;
        }
    }
}
static void session_add(Engine *e,Endpoint *p,IAudioSessionControl *control) {
    IAudioSessionControl2 *c=NULL; ISimpleAudioVolume *v=NULL; LPWSTR instance=NULL; Session *s,*other;
    DWORD pid=0; wchar_t app[MG_NAME]; HRESULT hr; AudioSessionState state;
    if(!e->current.enabled || config_output_mode(&e->current,p->id)!=OUTPUT_MUTE) return;
    if(FAILED(IAudioSessionControl_QueryInterface(control,&IID_IAudioSessionControl2,(void**)&c))) return;
    hr=IAudioSessionControl2_GetProcessId(c,&pid);
    /* Cross-process sessions (AUDCLNT_S_NO_SINGLE_PROCESS) are ambiguous: skip. */
    if(hr!=S_OK || IAudioSessionControl2_IsSystemSoundsSession(c)!=S_FALSE ||
       FAILED(IAudioSessionControl2_GetState(c,&state)) || state==AudioSessionStateExpired ||
       !process_name(pid,app) || !config_matches_app(&e->current,app) ||
       FAILED(IAudioSessionControl2_GetSessionInstanceIdentifier(c,&instance))) goto done;
    for(other=p->sessions;other;other=other->next) if(!wcscmp(other->instance,instance)) goto done;
    if(FAILED(IAudioSessionControl2_QueryInterface(c,&IID_ISimpleAudioVolume,(void**)&v))) goto done;
    s=mg_alloc(sizeof(*s)); if(!s) goto done;
    s->notice=mg_alloc(sizeof(*s->notice)); if(!s->notice) { mg_free(s); goto done; }
    s->notice->iface.lpVtbl=&session_vtbl; s->notice->refs=1; s->notice->hub=e->hub; hub_addref(e->hub);
    hr=IAudioSessionControl2_RegisterAudioSessionNotification(c,&s->notice->iface);
    if(FAILED(hr)) { IAudioSessionEvents_Release(&s->notice->iface); mg_free(s); goto done; }
    s->control=c; c=NULL; s->volume=v; v=NULL; s->instance=instance; instance=NULL;
    StringCchCopyW(s->app,ARRAYSIZE(s->app),app); s->next=p->sessions; p->sessions=s;
    session_apply(e,p,s);
done: CoTaskMemFree(instance); if(v) ISimpleAudioVolume_Release(v); if(c) IAudioSessionControl2_Release(c);
}
static HRESULT endpoint_scan(Engine *e,Endpoint *p) {
    IAudioSessionEnumerator *sessions=NULL; int count=0,i; HRESULT hr=IAudioSessionManager2_GetSessionEnumerator(p->manager,&sessions);
    if(SUCCEEDED(hr)) hr=IAudioSessionEnumerator_GetCount(sessions,&count);
    /* GetCount is required even with zero sessions, to arm new-session delivery. */
    for(i=0;SUCCEEDED(hr) && i<count;++i) {
        IAudioSessionControl *control=NULL;
        if(SUCCEEDED(IAudioSessionEnumerator_GetSession(sessions,i,&control))) { session_add(e,p,control); IAudioSessionControl_Release(control); }
    }
    if(sessions) IAudioSessionEnumerator_Release(sessions); return hr;
}
static Endpoint *endpoint_create(Engine *e,const wchar_t *id) {
    IMMDevice *device=NULL; IMMEndpoint *endpoint=NULL; LPWSTR actual=NULL; EDataFlow flow;
    Endpoint *p=NULL; DWORD state=0; HRESULT hr;
    hr=IMMDeviceEnumerator_GetDevice(e->enumerator,id,&device);
    if(FAILED(hr)) return NULL;
    /* Never trust an unverified ID or a stored capture endpoint. Bind the
       manager only after Windows confirms both exact identity and render flow. */
    actual=device_id(device);
    if(!actual || wcscmp(actual,id) ||
       FAILED(IMMDevice_QueryInterface(device,&IID_IMMEndpoint,(void**)&endpoint)) ||
       FAILED(IMMEndpoint_GetDataFlow(endpoint,&flow)) || flow!=eRender) goto done;
    if(FAILED(IMMDevice_GetState(device,&state)) || state!=DEVICE_STATE_ACTIVE) goto done;
    p=mg_alloc(sizeof(*p)); if(!p) goto done;
    p->id=mg_string(id); p->notice=mg_alloc(sizeof(*p->notice));
    if(!p->id || !p->notice) goto fail;
    hr=IMMDevice_Activate(device,&IID_IAudioSessionManager2,CLSCTX_INPROC_SERVER,NULL,(void**)&p->manager);
    if(FAILED(hr)) goto fail;
    p->generation=++e->generation; p->notice->iface.lpVtbl=&new_vtbl; p->notice->refs=1;
    p->notice->hub=e->hub; hub_addref(e->hub); p->notice->generation=p->generation;
    StringCchCopyW(p->notice->id,ARRAYSIZE(p->notice->id),id);
    hr=IAudioSessionManager2_RegisterSessionNotification(p->manager,&p->notice->iface);
    if(FAILED(hr)) { IAudioSessionNotification_Release(&p->notice->iface); p->notice=NULL; goto fail; }
    if(FAILED(endpoint_scan(e,p))) { endpoint_free(p,e->hub); p=NULL; }
    goto done;
fail:
    if(p->manager) IAudioSessionManager2_Release(p->manager);
    mg_free(p->notice); mg_free(p->id); mg_free(p); p=NULL;
done: if(endpoint) IMMEndpoint_Release(endpoint); CoTaskMemFree(actual); IMMDevice_Release(device); return p;
}
static void drain_packets(Engine *e,BOOL use) {
    PSLIST_ENTRY entry=InterlockedFlushSList(&e->hub->queue);
    while(entry) {
        Packet *packet=(Packet*)entry; Endpoint *p; entry=entry->Next;
        if(use) for(p=e->endpoints;p;p=p->next) if(p->generation==packet->generation && !wcscmp(p->id,packet->id)) {
            session_add(e,p,packet->control); break;
        }
        IAudioSessionControl_Release(packet->control); mg_free(packet);
    }
}
static BOOL has_work(const Config *c) {
    size_t i; if(!c->enabled || !c->app_count) return FALSE;
    if(c->all_outputs) return TRUE;
    for(i=0;i<c->output_count;++i) if(config_output_mode(c,c->outputs[i].id)==OUTPUT_MUTE) return TRUE;
    return FALSE;
}
static BOOL add_allowed_endpoint(Engine *e,const wchar_t *id) {
    Endpoint *p;
    if(config_output_mode(&e->current,id)!=OUTPUT_MUTE) return TRUE;
#ifdef MG_TESTING
    if(InterlockedCompareExchange(&e->held,0,0) && !wcscmp(e->held_endpoint,id)) return TRUE;
#endif
    for(p=e->endpoints;p;p=p->next) if(!wcscmp(p->id,id)) return TRUE;
    p=endpoint_create(e,id);
    if(!p) return FALSE;
    p->next=e->endpoints; e->endpoints=p; return TRUE;
}
static HRESULT ensure_enumerator(Engine *e) {
    HRESULT hr;
    if(e->enumerator) return S_OK;
    hr=CoCreateInstance(&CLSID_MMDeviceEnumerator,NULL,CLSCTX_INPROC_SERVER,&IID_IMMDeviceEnumerator,(void**)&e->enumerator);
    if(FAILED(hr)) return hr;
    e->notice=mg_alloc(sizeof(*e->notice));
    if(!e->notice) { IMMDeviceEnumerator_Release(e->enumerator); e->enumerator=NULL; return E_OUTOFMEMORY; }
    e->notice->iface.lpVtbl=&device_vtbl; e->notice->refs=1; e->notice->hub=e->hub; hub_addref(e->hub);
    hr=IMMDeviceEnumerator_RegisterEndpointNotificationCallback(e->enumerator,&e->notice->iface);
    if(FAILED(hr)) { IMMNotificationClient_Release(&e->notice->iface); e->notice=NULL; IMMDeviceEnumerator_Release(e->enumerator); e->enumerator=NULL; }
    return hr;
}
static HRESULT reconcile(Engine *e,BOOL reset) {
    Endpoint **link=&e->endpoints,*p; size_t i; BOOL missing=FALSE; HRESULT hr=ensure_enumerator(e);
    if(FAILED(hr)) return hr;
    while((p=*link)!=NULL) {
        IMMDevice *device=NULL; DWORD state=0;
        BOOL keep=has_work(&e->current) && config_output_mode(&e->current,p->id)==OUTPUT_MUTE;
        if(keep) {
            keep=SUCCEEDED(IMMDeviceEnumerator_GetDevice(e->enumerator,p->id,&device)) && SUCCEEDED(IMMDevice_GetState(device,&state)) && state==DEVICE_STATE_ACTIVE;
            if(device) IMMDevice_Release(device);
        }
        if(keep && FAILED(endpoint_scan(e,p))) keep=FALSE;
        if(!keep) { *link=p->next; endpoint_free(p,e->hub); }
        else link=&p->next;
    }
    if(has_work(&e->current)) {
        if(e->current.all_outputs) {
            IMMDeviceCollection *devices=NULL; UINT count=0,index;
            hr=IMMDeviceEnumerator_EnumAudioEndpoints(e->enumerator,eRender,DEVICE_STATE_ACTIVE,&devices);
            if(SUCCEEDED(hr)) hr=IMMDeviceCollection_GetCount(devices,&count);
            if(FAILED(hr)) missing=TRUE;
            for(index=0;SUCCEEDED(hr) && index<count;++index) {
                IMMDevice *device=NULL; LPWSTR id=NULL;
                if(FAILED(IMMDeviceCollection_Item(devices,index,&device))) { missing=TRUE; continue; }
                id=device_id(device);
                if(!id || !*id || wcslen(id)>=MG_ID || !add_allowed_endpoint(e,id)) missing=TRUE;
                CoTaskMemFree(id); IMMDevice_Release(device);
            }
            if(devices) IMMDeviceCollection_Release(devices);
        } else for(i=0;i<e->current.output_count;++i)
            if(!add_allowed_endpoint(e,e->current.outputs[i].id)) missing=TRUE;
    }
    for(p=e->endpoints;p;p=p->next) {
        Session **sl=&p->sessions,*s;
        while((s=*sl)!=NULL) {
            AudioSessionState state; HRESULT sh=IAudioSessionControl2_GetState(s->control,&state);
            if(FAILED(sh) || state==AudioSessionStateExpired || s->notice->dead || !config_matches_app(&e->current,s->app)) {
                *sl=s->next; session_free(s,e->hub,TRUE); continue;
            }
            if(reset) { s->suppressed=FALSE; InterlockedExchange(&s->notice->unmuted,0); }
            session_apply(e,p,s); sl=&s->next;
        }
    }
    return missing?(e->endpoints?S_FALSE:HRESULT_FROM_WIN32(ERROR_NOT_READY)):S_OK;
}
static void apply_session_events(Engine *e) {
    Endpoint *p;
    for(p=e->endpoints;p;p=p->next) {
        Session **link=&p->sessions,*s;
        while((s=*link)!=NULL) {
            if(s->notice->dead) { *link=s->next; session_free(s,e->hub,FALSE); }
            else { session_apply(e,p,s); link=&s->next; }
        }
    }
}
static void set_status(Engine *e,HRESULT hr,BOOL devices) {
    HRESULT old=(HRESULT)InterlockedExchange(&e->status,hr);
    if(e->notify && (old!=hr || devices)) PostMessageW(e->notify,MG_CHANGED,devices,0);
}
static DWORD WINAPI audio_thread(void *arg) {
    Engine *e=arg; HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED); LONG flags=WAKE_CONFIG|WAKE_DEVICE;
    ULONGLONG next_recovery=0;
    if(FAILED(hr)) { set_status(e,hr,FALSE); return 0; }
    for(;;) {
        Config *pending=NULL;
        if(flags&WAKE_STOP) break;
        if(flags&WAKE_CONFIG) {
            AcquireSRWLockExclusive(&e->lock); pending=e->pending; e->pending=NULL; ReleaseSRWLockExclusive(&e->lock);
            if(pending) { config_free(&e->current); e->current=*pending; mg_free(pending); }
        }
        if(flags&WAKE_REBUILD) {
            Endpoint *p; while((p=e->endpoints)!=NULL) { e->endpoints=p->next; endpoint_free(p,e->hub); }
        }
        if(flags&(WAKE_CONFIG|WAKE_DEVICE|WAKE_REBUILD|WAKE_SCAN)) {
            hr=reconcile(e,(flags&WAKE_CONFIG)!=0); set_status(e,hr,(flags&WAKE_DEVICE)!=0);
        }
        drain_packets(e,TRUE);
        if(flags&WAKE_SESSION) apply_session_events(e);
        {
            BOOL work=has_work(&e->current); DWORD timeout=INFINITE,result; ULONGLONG now=GetTickCount64();
            /* A fixed deadline prevents unrelated notifications from postponing
               recovery indefinitely. Disabled/empty configurations have no timer. */
            if(work) {
                if(!next_recovery) next_recovery=now+RECOVERY_MS;
                timeout=now>=next_recovery?0:(DWORD)(next_recovery-now);
            } else next_recovery=0;
            result=WaitForSingleObject(e->hub->event,timeout);
            flags=InterlockedExchange(&e->hub->flags,0);
            now=GetTickCount64();
            if(work && now>=next_recovery) { flags|=WAKE_SCAN; next_recovery=now+RECOVERY_MS; }
            if(result==WAIT_FAILED) break;
        }
    }
    InterlockedExchange(&e->hub->accepting,0);
    while(e->endpoints) { Endpoint *p=e->endpoints; e->endpoints=p->next; endpoint_free(p,e->hub); }
    if(e->enumerator) {
        IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(e->enumerator,&e->notice->iface);
        IMMNotificationClient_Release(&e->notice->iface); IMMDeviceEnumerator_Release(e->enumerator);
    }
    while(InterlockedCompareExchange(&e->hub->producers,0,0)) WaitForSingleObject(e->hub->event,INFINITE);
    drain_packets(e,FALSE); config_free(&e->current); CoUninitialize(); return 0;
}
static Engine *engine_start_impl(const Config *c,HWND notify,const wchar_t *held) {
    Engine *e=mg_alloc(sizeof(*e)); Hub *h=mg_alloc(sizeof(*h));
    if(!e || !h) { mg_free(e); mg_free(h); return NULL; }
    h->refs=1; h->accepting=1; InitializeSListHead(&h->queue); h->event=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!h->event || FAILED(CoCreateGuid(&h->context))) { if(h->event) CloseHandle(h->event); mg_free(h); mg_free(e); return NULL; }
    e->hub=h; e->notify=notify; e->status=E_PENDING; InitializeSRWLock(&e->lock); config_init(&e->current);
#ifdef MG_TESTING
    if(held) { StringCchCopyW(e->held_endpoint,ARRAYSIZE(e->held_endpoint),held); e->held=1; }
#else
    (void)held;
#endif
    if(!engine_update(e,c)) { hub_release(h); mg_free(e); return NULL; }
    e->thread=CreateThread(NULL,0,audio_thread,e,0,NULL);
    if(!e->thread) { config_free(e->pending); mg_free(e->pending); hub_release(h); mg_free(e); return NULL; }
    return e;
}
Engine *engine_start(const Config *c,HWND notify) { return engine_start_impl(c,notify,NULL); }
#ifdef MG_TESTING
Engine *engine_test_start(const Config *c,const wchar_t *held) { return engine_start_impl(c,NULL,held); }
BOOL engine_test_reveal(Engine *e) {
    if(!e->notice) return FALSE;
    InterlockedExchange(&e->held,0); dn_added(&e->notice->iface,e->held_endpoint); return TRUE;
}
#endif
BOOL engine_update(Engine *e,const Config *c) {
    Config *copy=mg_alloc(sizeof(*copy)),*old;
    if(!copy) return FALSE; config_init(copy);
    if(!config_copy(copy,c)) { mg_free(copy); return FALSE; }
    AcquireSRWLockExclusive(&e->lock); old=e->pending; e->pending=copy; ReleaseSRWLockExclusive(&e->lock);
    if(old) { config_free(old); mg_free(old); } wake(e->hub,WAKE_CONFIG); return TRUE;
}
HRESULT engine_status(Engine *e) { return (HRESULT)InterlockedCompareExchange(&e->status,0,0); }
void engine_stop(Engine *e) {
    if(!e) return;
    wake(e->hub,WAKE_STOP); WaitForSingleObject(e->thread,INFINITE); CloseHandle(e->thread);
    if(e->pending) { config_free(e->pending); mg_free(e->pending); }
    hub_release(e->hub); mg_free(e);
}
