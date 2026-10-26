#include "muteguard.h"
#include <commctrl.h>
#include <stdio.h>
static BOOL mixed_outputs,mixed_refreshed;
static HRESULT test_outputs_enumerate(OutputList *list);
/* Exercise presentation and the actual row handlers without changing the engine. */
#define outputs_enumerate test_outputs_enumerate
#include "../src/ui.c"
#undef outputs_enumerate

static HRESULT test_outputs_enumerate(OutputList *list) {
    /* Intentionally Disabled, Available, Disabled, Available, Available.
       Substitute only UI enumeration; engine enumeration remains untouched. */
    OutputInfo source[]={
        {L"mixed-d",L"Beta",DEVICE_STATE_DISABLED},
        {L"mixed-b",mixed_refreshed?L"Zulu":L"Alpha",DEVICE_STATE_ACTIVE},
        {L"mixed-e",L"Alpha",DEVICE_STATE_DISABLED},
        {L"mixed-a",L"Alpha",DEVICE_STATE_ACTIVE},
        {L"mixed-c",mixed_refreshed?L"Beta":L"Zulu",DEVICE_STATE_ACTIVE}
    };
    size_t i;
    if(!mixed_outputs) return outputs_enumerate(list);
    ZeroMemory(list,sizeof(*list)); list->items=mg_alloc(sizeof(source));
    if(!list->items) return E_OUTOFMEMORY;
    for(i=0;i<ARRAYSIZE(source);++i) {
        OutputInfo *out=&list->items[list->count++];
        out->id=mg_string(source[i].id); out->name=mg_string(source[i].name); out->state=source[i].state;
        if(!out->id || !out->name) { outputs_free(list); return E_OUTOFMEMORY; }
    }
    return S_OK;
}

HINSTANCE g_instance;
HWND g_owner;
Config g_config;
Engine *g_engine;
wchar_t g_config_path[MAX_PATH];
BOOL app_commit(const Config *c) { return engine_update(g_engine,c) && config_copy(&g_config,c); }

static int output_checks,output_failures;
#define CHECK_OUTPUT(x) do { ++output_checks; if(!(x)) { ++output_failures; printf("FAIL output line %d: %s\n",__LINE__,#x); } } while(0)
static int output_index(const wchar_t *id) {
    size_t i; for(i=0;i<ui->outputs.count;++i) if(!wcscmp(ui->outputs.items[i].id,id)) return (int)i;
    return -1;
}
static void check_mixed_rows(void) {
    const wchar_t *ids_before[]={L"mixed-a",L"mixed-b",L"mixed-c",L"mixed-e",L"mixed-d"};
    const wchar_t *ids_after[]={L"mixed-a",L"mixed-c",L"mixed-b",L"mixed-e",L"mixed-d"};
    const wchar_t *names_before[]={L"Alpha",L"Alpha",L"Zulu",L"Alpha",L"Beta"};
    const wchar_t *names_after[]={L"Alpha",L"Beta",L"Zulu",L"Alpha",L"Beta"};
    HWND view=GetDlgItem(ui->window,ID_OUTPUTS); wchar_t text[80]; size_t i;
    CHECK_OUTPUT(ListView_GetItemCount(view)==5 && ui->outputs.count==5);
    if(ui->outputs.count!=5) return;
    for(i=0;i<5;++i) {
        const wchar_t *id=mixed_refreshed?ids_after[i]:ids_before[i];
        const wchar_t *name=mixed_refreshed?names_after[i]:names_before[i];
        const wchar_t *rule=!wcscmp(id,L"mixed-b")?L"Protected":!wcscmp(id,L"mixed-a")?L"Mute":L"Unaffected";
        /* Read all three columns from the real ListView, not the comparator. */
        ListView_GetItemText(view,(int)i,0,text,ARRAYSIZE(text)); CHECK_OUTPUT(!wcscmp(text,name));
        ListView_GetItemText(view,(int)i,1,text,ARRAYSIZE(text)); CHECK_OUTPUT(!wcscmp(text,i<3?L"Available":L"Disabled"));
        ListView_GetItemText(view,(int)i,2,text,ARRAYSIZE(text)); CHECK_OUTPUT(!wcscmp(text,rule));
        CHECK_OUTPUT(!wcscmp(ui->outputs.items[i].id,id));
        CHECK_OUTPUT((ListView_GetCheckState(view,(int)i)!=0)==!wcscmp(id,L"mixed-a"));
    }
}
static void test_mixed_listview(void) {
    HWND view; int selected; Config saved; wchar_t path[MAX_PATH];
    config_init(&saved); mixed_outputs=TRUE; mixed_refreshed=FALSE; settings_open();
    CHECK_OUTPUT(ui!=NULL); if(!ui) { mixed_outputs=FALSE; return; }
    view=GetDlgItem(ui->window,ID_OUTPUTS); ui->draft.all_outputs=FALSE;
    CHECK_OUTPUT(config_set_output(&ui->draft,L"mixed-b",L"Alpha",OUTPUT_PROTECT));
    CHECK_OUTPUT(config_set_output(&ui->draft,L"mixed-a",L"Alpha",OUTPUT_MUTE));
    outputs_fill(); check_mixed_rows();
    selected=output_index(L"mixed-b"); CHECK_OUTPUT(selected==1);
    ListView_SetItemState(view,selected,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    /* Unchanged refresh, followed by a rename that forces a rebuild/reorder. */
    SendMessageW(ui->window,WM_COMMAND,ID_REFRESH,0); check_mixed_rows();
    CHECK_OUTPUT(ListView_GetNextItem(view,-1,LVNI_SELECTED)==1);
    mixed_refreshed=TRUE; settings_changed(TRUE,FALSE); check_mixed_rows();
    CHECK_OUTPUT(ListView_GetNextItem(view,-1,LVNI_SELECTED)==2);
    CHECK_OUTPUT(ListView_GetNextItem(view,-1,LVNI_FOCUSED)==2);
    ListView_SetCheckState(view,2,TRUE); CHECK_OUTPUT(!ListView_GetCheckState(view,2));
    CHECK_OUTPUT(config_saved_output_mode(&ui->draft,L"mixed-b")==OUTPUT_PROTECT);
    /* Checkbox notifications after reordering must still resolve the right ID. */
    ListView_SetCheckState(view,1,TRUE);
    CHECK_OUTPUT(config_saved_output_mode(&ui->draft,L"mixed-c")==OUTPUT_MUTE);
    CHECK_OUTPUT(config_saved_output_mode(&ui->draft,L"mixed-a")==OUTPUT_MUTE);
    CHECK_OUTPUT(config_saved_output_mode(&ui->draft,L"mixed-b")==OUTPUT_PROTECT);
    ListView_SetCheckState(view,1,FALSE); check_mixed_rows();
    StringCchPrintfW(path,ARRAYSIZE(path),L"build\\ui-mixed-%lu.dat",GetCurrentProcessId());
    CHECK_OUTPUT(config_save(&ui->draft,path)==S_OK && config_load(&saved,path)==S_OK);
    CHECK_OUTPUT(!saved.all_outputs && saved.output_count==2);
    CHECK_OUTPUT(config_saved_output_mode(&saved,L"mixed-a")==OUTPUT_MUTE);
    CHECK_OUTPUT(config_saved_output_mode(&saved,L"mixed-b")==OUTPUT_PROTECT);
    CHECK_OUTPUT(config_saved_output_mode(&saved,L"mixed-c")==OUTPUT_IGNORE);
    DeleteFileW(path); config_free(&saved); settings_close(); mixed_outputs=FALSE;
    printf("Mixed-state ListView: population, unchanged refresh, reordered refresh, all columns/rules/IDs checked.\n");
}
static void test_output_sort(void) {
    OutputInfo items[]={
        {L"z",L"Zulu",DEVICE_STATE_ACTIVE},{L"b",L"alpha",DEVICE_STATE_ACTIVE},
        {L"a",L"Alpha",DEVICE_STATE_ACTIVE},{L"inactive",L"Active speakers",DEVICE_STATE_DISABLED},
        {L"unplugged",L"Beta",DEVICE_STATE_UNPLUGGED},{L"missing",L"Gamma",DEVICE_STATE_NOTPRESENT},
        {L"unknown",L"Delta",0},{L"Fallback",NULL,DEVICE_STATE_ACTIVE},{L"Empty",L"",DEVICE_STATE_ACTIVE}
    };
    const wchar_t *expected[]={L"a",L"b",L"Empty",L"Fallback",L"z",L"inactive",L"unplugged",L"unknown",L"missing"};
    OutputInfo absent={0}; size_t i;
    qsort(items,ARRAYSIZE(items),sizeof(*items),output_compare);
    for(i=0;i<ARRAYSIZE(items);++i) CHECK_OUTPUT(!wcscmp(items[i].id,expected[i]));
    CHECK_OUTPUT(!wcscmp(output_name(&absent),L""));
}
static OutputList output_fixture(BOOL changed) {
    OutputList list={0}; size_t i;
    list.items=mg_alloc(40*sizeof(*list.items)); if(!list.items) return list;
    for(i=0;i<40;++i) {
        wchar_t id[32],name[32]; OutputInfo *out=&list.items[list.count];
        StringCchPrintfW(id,ARRAYSIZE(id),L"output-%02zu",i); StringCchPrintfW(name,ARRAYSIZE(name),L"Device %02zu",i);
        out->id=mg_string(id); out->name=mg_string(name); out->state=i%2?DEVICE_STATE_UNPLUGGED:DEVICE_STATE_ACTIVE;
        if(!out->id || !out->name) { mg_free(out->id); mg_free(out->name); outputs_free(&list); return list; }
        if(changed && i==4) out->state=DEVICE_STATE_DISABLED;
        if(changed && i==5) out->state=DEVICE_STATE_ACTIVE;
        if(i==37) { mg_free(out->name); out->name=NULL; }
        ++list.count;
    }
    return list;
}
static void test_output_refresh(void) {
    OutputList list; OutputInfo *unchanged; HWND view; int selected,top,checked; RECT before,after;
    wchar_t anchor[MG_ID],label[80],path[MAX_PATH]; Config saved; size_t i; BOOL inactive=FALSE;
    config_init(&saved); settings_open(); CHECK_OUTPUT(ui!=NULL); if(!ui) return;
    view=control(ID_OUTPUTS); ui->draft.all_outputs=FALSE;
    CHECK_OUTPUT(config_set_output(&ui->draft,L"output-05",L"Protected output",OUTPUT_PROTECT));
    CHECK_OUTPUT(config_set_output(&ui->draft,L"output-04",L"Muted output",OUTPUT_MUTE));
    list=output_fixture(FALSE); CHECK_OUTPUT(list.count==40); outputs_present(&list);
    selected=output_index(L"output-05"); CHECK_OUTPUT(selected>=0); if(selected<0) goto done;
    ListView_SetItemState(view,selected,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    CHECK_OUTPUT(ListView_GetItemRect(view,0,&before,LVIR_BOUNDS));
    ListView_Scroll(view,0,10*(before.bottom-before.top));
    top=ListView_GetTopIndex(view); CHECK_OUTPUT(top>0);
    StringCchCopyW(anchor,ARRAYSIZE(anchor),ui->outputs.items[top].id);
    CHECK_OUTPUT(ListView_GetItemRect(view,top,&before,LVIR_BOUNDS));
    list=output_fixture(TRUE); CHECK_OUTPUT(list.count==40); outputs_present(&list);
    selected=ListView_GetNextItem(view,-1,LVNI_SELECTED);
    CHECK_OUTPUT(selected==output_index(L"output-05"));
    CHECK_OUTPUT(ListView_GetNextItem(view,-1,LVNI_FOCUSED)==selected);
    top=ListView_GetTopIndex(view); CHECK_OUTPUT(!wcscmp(ui->outputs.items[top].id,anchor));
    CHECK_OUTPUT(ListView_GetItemRect(view,top,&after,LVIR_BOUNDS) && after.top==before.top);
    CHECK_OUTPUT(config_output_mode(&ui->draft,L"output-05")==OUTPUT_PROTECT);
    ListView_GetItemText(view,selected,2,label,ARRAYSIZE(label)); CHECK_OUTPUT(!wcscmp(label,L"Protected"));
    CHECK_OUTPUT(!ListView_GetCheckState(view,selected));
    checked=output_index(L"output-04"); CHECK_OUTPUT(checked>=0 && ListView_GetCheckState(view,checked));
    checked=output_index(L"output-37"); CHECK_OUTPUT(checked>=0);
    ListView_GetItemText(view,checked,0,label,ARRAYSIZE(label)); CHECK_OUTPUT(!wcscmp(label,L"output-37"));
    for(i=0;i<ui->outputs.count;++i) {
        if(ui->outputs.items[i].state!=DEVICE_STATE_ACTIVE) inactive=TRUE;
        else CHECK_OUTPUT(!inactive);
        if(i) CHECK_OUTPUT(output_compare(&ui->outputs.items[i-1],&ui->outputs.items[i])<=0);
    }
    unchanged=ui->outputs.items; list=output_fixture(TRUE); outputs_present(&list);
    CHECK_OUTPUT(ui->outputs.items==unchanged); /* Identical notifications do not rebuild rows. */
    CHECK_OUTPUT(ListView_GetNextItem(view,-1,LVNI_SELECTED)==selected && ListView_GetTopIndex(view)==top);
    /* A row action after reordering must still address that endpoint's ID. */
    checked=output_index(L"output-37"); CHECK_OUTPUT(checked>=0);
    ListView_SetItemState(view,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    ListView_SetItemState(view,checked,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    SendMessageW(control(ID_PROTECT),BM_CLICK,0,0);
    CHECK_OUTPUT(config_saved_output_mode(&ui->draft,L"output-37")==OUTPUT_PROTECT);
    CHECK_OUTPUT(config_saved_output_mode(&ui->draft,L"output-05")==OUTPUT_PROTECT);
    CHECK_OUTPUT(config_saved_output_mode(&ui->draft,L"output-04")==OUTPUT_MUTE);
    StringCchPrintfW(path,ARRAYSIZE(path),L"build\\ui-sort-%lu.dat",GetCurrentProcessId());
    CHECK_OUTPUT(config_save(&ui->draft,path)==S_OK && config_load(&saved,path)==S_OK);
    CHECK_OUTPUT(config_saved_output_mode(&saved,L"output-05")==OUTPUT_PROTECT);
    CHECK_OUTPUT(config_saved_output_mode(&saved,L"output-04")==OUTPUT_MUTE); DeleteFileW(path);
    /* Disconnection/removal and reappearance use the same presentation path. */
    list=output_fixture(FALSE); outputs_present(&list);
    CHECK_OUTPUT(config_saved_output_mode(&ui->draft,L"output-05")==OUTPUT_PROTECT);
    list=(OutputList){0}; outputs_present(&list); CHECK_OUTPUT(ListView_GetItemCount(view)==0);
    list=output_fixture(FALSE); outputs_present(&list); CHECK_OUTPUT(ListView_GetItemCount(view)==40);
done:
    settings_close(); config_free(&saved);
    printf("Output sorting/refresh/endpoint-rule identity: %d checks, %d failures\n",output_checks,output_failures);
}

int wmain(void) {
    INITCOMMONCONTROLSEX common={sizeof(common),ICC_LISTVIEW_CLASSES}; unsigned i; int failures=0;
    DWORD start_user,end_user,start_gdi,end_gdi; HRESULT hr=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    if(FAILED(hr)) return 1; setvbuf(stdout,NULL,_IONBF,0);
    g_instance=GetModuleHandleW(NULL); config_init(&g_config); InitCommonControlsEx(&common);
    g_owner=CreateWindowExW(0,L"STATIC",L"MuteGuard UI test",0,0,0,0,0,NULL,NULL,g_instance,NULL);
    g_engine=engine_start(&g_config,NULL); if(!g_engine || !g_owner) return 1;
    /* Warm common-control/theme caches before checking per-window cleanup. */
    for(i=0;i<3;++i) {
        HWND settings; MSG message; settings_open(); settings=FindWindowW(L"MuteGuard.Settings",NULL);
        SendMessageW(GetDlgItem(settings,211),BM_CLICK,0,0); SendMessageW(GetDlgItem(settings,211),BM_CLICK,0,0);
        while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        settings_close();
    }
    test_output_sort(); test_mixed_listview(); test_output_refresh(); failures+=output_failures;
    start_user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS); start_gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(i=0;i<25;++i) {
        HWND settings; MSG message;
        settings_open(); settings=FindWindowW(L"MuteGuard.Settings",NULL);
        printf("Settings cycle %u: window %p, visible %d\n",i,(void*)settings,settings?IsWindowVisible(settings):0);
        if(!settings || !IsWindowVisible(settings)) ++failures;
        if(IsDlgButtonChecked(settings,211)!=BST_CHECKED || (ListView_GetExtendedListViewStyle(GetDlgItem(settings,205))&LVS_EX_CHECKBOXES)) ++failures;
        SendMessageW(GetDlgItem(settings,211),BM_CLICK,0,0);
        if(!(ListView_GetExtendedListViewStyle(GetDlgItem(settings,205))&LVS_EX_CHECKBOXES)) ++failures;
        SendMessageW(GetDlgItem(settings,211),BM_CLICK,0,0);
        if(ListView_GetExtendedListViewStyle(GetDlgItem(settings,205))&LVS_EX_CHECKBOXES) ++failures;
        while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        settings_close(); if(settings && IsWindow(settings)) ++failures;
    }
    end_user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS); end_gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    printf("UI resources before/after: USER %lu/%lu, GDI %lu/%lu\n",start_user,end_user,start_gdi,end_gdi);
    if(end_user>start_user+2 || end_gdi>start_gdi+2) ++failures;
    engine_stop(g_engine); DestroyWindow(g_owner); config_free(&g_config); CoUninitialize();
    printf("UI lifecycle: 25 cycles, %d failures\n",failures); return failures?1:0;
}
