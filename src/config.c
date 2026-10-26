#include "muteguard.h"
#include <shlobj.h>
#include <strsafe.h>
#include <stdint.h>

_Ret_writes_bytes_maybenull_(bytes) void *mg_alloc(size_t bytes) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes); }
void mg_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
wchar_t *mg_string(const wchar_t *s) {
    size_t bytes = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *p = mg_alloc(bytes);
    if (p) CopyMemory(p, s, bytes);
    return p;
}
void config_init(Config *c) { ZeroMemory(c, sizeof(*c)); c->enabled = TRUE; c->all_outputs=TRUE; }
void config_free(Config *c) {
    size_t i;
    for (i = 0; i < c->app_count; ++i) mg_free(c->apps[i]);
    for (i = 0; i < c->output_count; ++i) { mg_free(c->outputs[i].id); mg_free(c->outputs[i].name); }
    mg_free(c->apps); mg_free(c->outputs); config_init(c);
}
static BOOL valid_text(const wchar_t *s,size_t cap) {
    size_t i,n=wcslen(s); if(!n || n>=cap) return FALSE;
    for(i=0;i<n;++i) {
        if(s[i]<32) return FALSE;
        if(s[i]>=0xD800 && s[i]<=0xDBFF) {
            if(++i>=n || s[i]<0xDC00 || s[i]>0xDFFF) return FALSE;
        } else if(s[i]>=0xDC00 && s[i]<=0xDFFF) return FALSE;
    }
    return TRUE;
}
BOOL config_valid_app(const wchar_t *s) {
    size_t n = wcslen(s), i;
    if (!valid_text(s,MG_NAME) || n < 5 || _wcsicmp(s + n - 4, L".exe") || s[0] == L' ' || s[n-1] == L' ') return FALSE;
    for (i = 0; i < n; ++i) if (s[i] < 32 || wcschr(L"\\/:*?\"<>|", s[i])) return FALSE;
    return TRUE;
}
BOOL config_matches_app(const Config *c, const wchar_t *name) {
    size_t i;
    for (i = 0; i < c->app_count; ++i) if (CompareStringOrdinal(c->apps[i],-1,name,-1,TRUE)==CSTR_EQUAL) return TRUE;
    return FALSE;
}
BOOL config_add_app(Config *c, const wchar_t *name) {
    wchar_t **items, *copy; size_t count=c->app_count;
    if (!config_valid_app(name)) return FALSE;
    if (config_matches_app(c, name)) return TRUE;
    if (count >= MG_LIMIT) return FALSE;
    copy = mg_string(name);
    items = mg_alloc((count + 1) * sizeof(*items));
    if (!copy || !items) { mg_free(copy); mg_free(items); return FALSE; }
    if (count) CopyMemory(items, c->apps, count * sizeof(*items));
    items[count] = copy; c->app_count=count+1; mg_free(c->apps); c->apps = items; return TRUE;
}
void config_remove_app(Config *c, size_t i) {
    if (i >= c->app_count) return;
    mg_free(c->apps[i]);
    MoveMemory(c->apps + i, c->apps + i + 1, (--c->app_count - i) * sizeof(*c->apps));
}
OutputMode config_saved_output_mode(const Config *c, const wchar_t *id) {
    size_t i; OutputMode result = OUTPUT_IGNORE;
    /* Protection wins even if callers supply duplicate entries. No name fallback. */
    for (i = 0; i < c->output_count; ++i) if (!wcscmp(c->outputs[i].id, id)) {
        if (c->outputs[i].mode == OUTPUT_PROTECT) return OUTPUT_PROTECT;
        result = c->outputs[i].mode;
    }
    return result;
}
OutputMode config_output_mode(const Config *c,const wchar_t *id) {
    OutputMode saved;
    if(!id || !valid_text(id,MG_ID)) return OUTPUT_IGNORE;
    saved=config_saved_output_mode(c,id);
    return saved==OUTPUT_PROTECT?OUTPUT_PROTECT:c->all_outputs?OUTPUT_MUTE:saved;
}
BOOL config_set_output(Config *c, const wchar_t *id, const wchar_t *name, OutputMode mode) {
    size_t i; OutputRule *items; wchar_t *copy, *label;
    if (!valid_text(id,MG_ID) || !valid_text(name,MG_ID) || mode < OUTPUT_IGNORE || mode > OUTPUT_PROTECT) return FALSE;
    for (i = 0; i < c->output_count; ++i) if (!wcscmp(c->outputs[i].id, id)) break;
    if (mode == OUTPUT_IGNORE) {
        if (i < c->output_count) {
            mg_free(c->outputs[i].id); mg_free(c->outputs[i].name);
            MoveMemory(c->outputs+i, c->outputs+i+1, (--c->output_count-i)*sizeof(*c->outputs));
        }
        return TRUE;
    }
    label = mg_string(name);
    if (!label) return FALSE;
    if (i < c->output_count) {
        mg_free(c->outputs[i].name); c->outputs[i].name = label; c->outputs[i].mode = mode; return TRUE;
    }
    if (i >= MG_LIMIT) { mg_free(label); return FALSE; }
    copy = mg_string(id); items = mg_alloc((i+1)*sizeof(*items));
    if (!copy || !items) { mg_free(copy); mg_free(items); mg_free(label); return FALSE; }
    if (i) CopyMemory(items, c->outputs, i*sizeof(*items));
    items[i].id = copy; items[i].name = label; items[i].mode = mode;
    mg_free(c->outputs); c->outputs = items; ++c->output_count; return TRUE;
}
BOOL config_copy(Config *dst, const Config *src) {
    size_t i; Config copy; config_init(&copy); copy.enabled = src->enabled; copy.all_outputs=src->all_outputs;
    for (i=0; i<src->app_count; ++i) if (!config_add_app(&copy, src->apps[i])) goto fail;
    for (i=0; i<src->output_count; ++i)
        if (!config_set_output(&copy, src->outputs[i].id, src->outputs[i].name, config_saved_output_mode(src,src->outputs[i].id))) goto fail;
    config_free(dst); *dst = copy; return TRUE;
fail: config_free(&copy); return FALSE;
}
BOOL config_path(wchar_t *path, size_t count) {
    PWSTR base = NULL; HRESULT hr = SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &base);
    if (FAILED(hr)) return FALSE;
    hr = StringCchPrintfW(path, count, L"%s\\MuteGuard", base); CoTaskMemFree(base);
    if (FAILED(hr)) return FALSE;
    if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    return SUCCEEDED(StringCchCatW(path, count, L"\\settings.dat"));
}

/* Bounded, versioned UTF-16 records: magic, enabled, counts, length-prefixed strings.
   Parse into a temporary config, and reject the whole file on any invalid record. */
#define FILE_MAGIC_V1 0x31474D55u
#define FILE_MAGIC 0x32474D55u
#define FILE_LIMIT (1024u * 1024u)
typedef struct Cursor { BYTE *p, *end; } Cursor;
static BOOL read_u32(Cursor *r, DWORD *v) {
    if ((size_t)(r->end-r->p)<4) return FALSE;
    CopyMemory(v,r->p,4); r->p+=4; return TRUE;
}
static BOOL read_string(Cursor *r, wchar_t *s, size_t cap) {
    DWORD n; size_t i;
    if (!read_u32(r,&n) || !n || n >= cap || (size_t)(r->end-r->p) < n*2u) return FALSE;
    CopyMemory(s,r->p,n*2u); r->p+=n*2u; s[n]=0;
    for(i=0;i<n;++i) {
        if (s[i]<32) return FALSE;
        if (s[i]>=0xD800 && s[i]<=0xDBFF) {
            if (++i>=n || s[i]<0xDC00 || s[i]>0xDFFF) return FALSE;
        } else if (s[i]>=0xDC00 && s[i]<=0xDFFF) return FALSE;
    }
    return TRUE;
}
HRESULT config_load(Config *c, const wchar_t *path) {
    HANDLE f; LARGE_INTEGER size; BYTE *data; DWORD got, magic, enabled, automatic=0, apps, outputs, mode, i;
    Cursor r; Config tmp; wchar_t a[MG_ID], b[MG_ID]; HRESULT hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) { DWORD e=GetLastError(); return e==ERROR_FILE_NOT_FOUND ? S_FALSE : HRESULT_FROM_WIN32(e); }
    if(!GetFileSizeEx(f,&size) || size.QuadPart<16 || size.QuadPart>FILE_LIMIT) { CloseHandle(f); return hr; }
    data=mg_alloc((size_t)size.QuadPart);
    if(!data) { CloseHandle(f); return E_OUTOFMEMORY; }
    if(!ReadFile(f,data,(DWORD)size.QuadPart,&got,NULL) || got!=(DWORD)size.QuadPart) { mg_free(data); CloseHandle(f); return hr; }
    CloseHandle(f); r.p=data; r.end=data+got; config_init(&tmp);
    if(!read_u32(&r,&magic) || (magic!=FILE_MAGIC && magic!=FILE_MAGIC_V1) || !read_u32(&r,&enabled) || enabled>1) goto done;
    /* All existing V1 files retain manual behavior, even when no outputs were
       selected. Only a new config or an explicit user change enables V2 mode. */
    if(magic==FILE_MAGIC && (!read_u32(&r,&automatic) || automatic>1)) goto done;
    if(!read_u32(&r,&apps) || apps>MG_LIMIT || !read_u32(&r,&outputs) || outputs>MG_LIMIT) goto done;
    tmp.enabled=(BOOL)enabled; tmp.all_outputs=(BOOL)automatic;
    for(i=0;i<apps;++i) if(!read_string(&r,a,MG_NAME) || config_matches_app(&tmp,a) || !config_add_app(&tmp,a)) goto done;
    for(i=0;i<outputs;++i) {
        if(!read_u32(&r,&mode) || (mode!=OUTPUT_MUTE && mode!=OUTPUT_PROTECT) || !read_string(&r,a,MG_ID) ||
           !read_string(&r,b,MG_ID) || config_saved_output_mode(&tmp,a)!=OUTPUT_IGNORE || !config_set_output(&tmp,a,b,(OutputMode)mode)) goto done;
    }
    if(r.p!=r.end) goto done;
    config_free(c); *c=tmp; config_init(&tmp); hr=S_OK;
done: config_free(&tmp); mg_free(data); return hr;
}
static void write_u32(BYTE **p, DWORD n) { CopyMemory(*p,&n,4); *p+=4; }
static void write_string(BYTE **p,const wchar_t *s) {
    DWORD n=(DWORD)wcslen(s); write_u32(p,n); CopyMemory(*p,s,n*2u); *p+=n*2u;
}
HRESULT config_save(const Config *c, const wchar_t *path) {
    size_t bytes=20,i; BYTE *data,*p; wchar_t temp[MAX_PATH]; HANDLE f; DWORD wrote,error=0; HRESULT hr;
    for(i=0;i<c->app_count;++i) bytes+=4+wcslen(c->apps[i])*2;
    for(i=0;i<c->output_count;++i) bytes+=12+(wcslen(c->outputs[i].id)+wcslen(c->outputs[i].name))*2;
    if(bytes>FILE_LIMIT) return E_INVALIDARG;
    data=mg_alloc(bytes); if(!data) return E_OUTOFMEMORY; p=data;
    write_u32(&p,FILE_MAGIC); write_u32(&p,c->enabled?1:0); write_u32(&p,c->all_outputs?1:0); write_u32(&p,(DWORD)c->app_count); write_u32(&p,(DWORD)c->output_count);
    for(i=0;i<c->app_count;++i) write_string(&p,c->apps[i]);
    for(i=0;i<c->output_count;++i) { write_u32(&p,c->outputs[i].mode); write_string(&p,c->outputs[i].id); write_string(&p,c->outputs[i].name); }
    hr=StringCchPrintfW(temp,ARRAYSIZE(temp),L"%s.tmp",path);
    if(FAILED(hr)) { mg_free(data); return hr; }
    f=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) { mg_free(data); return HRESULT_FROM_WIN32(GetLastError()); }
    if(!WriteFile(f,data,(DWORD)bytes,&wrote,NULL) || wrote!=(DWORD)bytes || !FlushFileBuffers(f)) error=GetLastError()?GetLastError():ERROR_WRITE_FAULT;
    CloseHandle(f); mg_free(data);
    if(!error && !MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) error=GetLastError();
    if(error) DeleteFileW(temp);
    return error?HRESULT_FROM_WIN32(error):S_OK;
}
