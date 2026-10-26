#include "muteguard.h"
#include <commctrl.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <strsafe.h>
#include <stdlib.h>

enum { ID_ENABLE=200,ID_APPS,ID_ENTRY,ID_ADD,ID_REMOVE,ID_OUTPUTS,ID_PROTECT,ID_REFRESH,ID_APPLY,ID_CLOSE,ID_STATUS,ID_ALL };
typedef struct Settings {
    HWND window, controls[12]; HFONT font; UINT dpi; BOOL filling;
    Config draft; OutputList outputs;
} Settings;
static Settings *ui;
static int scaled(int n) { return MulDiv(n,(int)ui->dpi,96); }
static HWND control(int id) { return ui->controls[id-ID_ENABLE]; }
static void message_error(const wchar_t *text) { MessageBoxW(ui->window,text,L"MuteGuard",MB_OK|MB_ICONWARNING); }
static void apps_fill(void) {
    size_t i; SendMessageW(control(ID_APPS),LB_RESETCONTENT,0,0);
    for(i=0;i<ui->draft.app_count;++i) SendMessageW(control(ID_APPS),LB_ADDSTRING,0,(LPARAM)ui->draft.apps[i]);
}
static void running_fill(void) {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0); PROCESSENTRY32W process={sizeof(process)};
    SendMessageW(control(ID_ENTRY),CB_RESETCONTENT,0,0);
    if(snapshot!=INVALID_HANDLE_VALUE) {
        if(Process32FirstW(snapshot,&process)) do {
            if(config_valid_app(process.szExeFile) && SendMessageW(control(ID_ENTRY),CB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)process.szExeFile)==CB_ERR)
                SendMessageW(control(ID_ENTRY),CB_ADDSTRING,0,(LPARAM)process.szExeFile);
        } while(Process32NextW(snapshot,&process));
        CloseHandle(snapshot);
    }
    SetWindowTextW(control(ID_ENTRY),L"");
}
static void status_fill(void) {
    HRESULT hr=engine_status(g_engine); size_t i; BOOL selected=g_config.all_outputs;
    for(i=0;i<g_config.output_count;++i) if(config_output_mode(&g_config,g_config.outputs[i].id)==OUTPUT_MUTE) selected=TRUE;
    SetWindowTextW(control(ID_STATUS),!g_config.enabled?L"Automatic muting is disabled.":
        FAILED(hr)?L"Audio monitoring is unavailable. Recovery will retry.":
        !g_config.app_count || !selected?L"Choose applications and outputs, then Apply.":
        hr==S_FALSE?L"Monitoring available outputs. Missing outputs will retry.":L"Audio monitoring is active.");
}
static void rule_text(int index) {
    OutputMode mode=config_output_mode(&ui->draft,ui->outputs.items[index].id);
    ListView_SetItemText(control(ID_OUTPUTS),index,2,(LPWSTR)(mode==OUTPUT_PROTECT?L"Protected":mode==OUTPUT_MUTE?L"Mute":L"Unaffected"));
}
static const wchar_t *output_name(const OutputInfo *out) {
    return out->name && *out->name?out->name:out->id?out->id:L"";
}
static int output_compare(const void *left,const void *right) {
    const OutputInfo *a=left,*b=right; int order;
    BOOL active_a=a->state==DEVICE_STATE_ACTIVE,active_b=b->state==DEVICE_STATE_ACTIVE;
    if(active_a!=active_b) return active_a?-1:1;
    order=CompareStringOrdinal(output_name(a),-1,output_name(b),-1,TRUE);
    if(order!=CSTR_EQUAL) return order==CSTR_LESS_THAN?-1:1;
    return wcscmp(a->id?a->id:L"",b->id?b->id:L"");
}
static void outputs_present(OutputList *list) {
    HWND view=control(ID_OUTPUTS); OutputList old=ui->outputs; size_t i;
    int selected=ListView_GetNextItem(view,-1,LVNI_SELECTED),focused=ListView_GetNextItem(view,-1,LVNI_FOCUSED);
    int top=ListView_GetTopIndex(view),new_top=-1,new_selected=-1,horizontal=GetScrollPos(view,SB_HORZ); RECT before={0};
    BOOL manual=(ListView_GetExtendedListViewStyle(view)&LVS_EX_CHECKBOXES)!=0;
    BOOL same=manual==!ui->draft.all_outputs && list->count==old.count;
    if(list->count>1) qsort(list->items,list->count,sizeof(*list->items),output_compare);
    for(i=0;same && i<list->count;++i)
        same=list->items[i].state==old.items[i].state && !wcscmp(list->items[i].id,old.items[i].id) &&
            !wcscmp(output_name(&list->items[i]),output_name(&old.items[i]));
    if(same) { outputs_free(list); status_fill(); return; }
    if(top>=0 && (size_t)top<old.count) ListView_GetItemRect(view,top,&before,LVIR_BOUNDS);
    ui->filling=TRUE; SendMessageW(view,WM_SETREDRAW,FALSE,0);
    ListView_SetExtendedListViewStyleEx(view,LVS_EX_CHECKBOXES,ui->draft.all_outputs?0:LVS_EX_CHECKBOXES);
    SetWindowTextW(GetDlgItem(ui->window,301),ui->draft.all_outputs?L"Audio outputs - select your listening output and click Protect":L"Audio outputs - check the outputs to mute");
    SetWindowTextW(GetDlgItem(ui->window,302),ui->draft.all_outputs?
        L"Unprotected outputs are muted automatically, including newly connected outputs.\nSelect an output and click Protect to exclude it. Click Apply to save.":
        L"Unchecked outputs stay unaffected. Protected outputs cannot be checked.\nRules take effect when you click Apply. Closing keeps MuteGuard running.");
    ListView_DeleteAllItems(view); ui->outputs=*list; ZeroMemory(list,sizeof(*list));
    for(i=0;i<ui->outputs.count;++i) {
        LVITEMW item={0}; OutputInfo *out=&ui->outputs.items[i]; UINT state=0;
        item.mask=LVIF_TEXT; item.iItem=(int)i; item.pszText=(LPWSTR)output_name(out); ListView_InsertItem(view,&item);
        ListView_SetItemText(view,(int)i,1,out->state==DEVICE_STATE_ACTIVE?L"Available":out->state==DEVICE_STATE_DISABLED?L"Disabled":L"Disconnected");
        if(!ui->draft.all_outputs) ListView_SetCheckState(view,(int)i,config_output_mode(&ui->draft,out->id)==OUTPUT_MUTE);
        rule_text((int)i);
        /* Retain UI anchors by endpoint identity, never by their former row. */
        if(selected>=0 && (size_t)selected<old.count && !wcscmp(out->id,old.items[selected].id)) { state|=LVIS_SELECTED; new_selected=(int)i; }
        if(focused>=0 && (size_t)focused<old.count && !wcscmp(out->id,old.items[focused].id)) state|=LVIS_FOCUSED;
        if(state) ListView_SetItemState(view,(int)i,state,LVIS_SELECTED|LVIS_FOCUSED);
        if(top>=0 && (size_t)top<old.count && !wcscmp(out->id,old.items[top].id)) new_top=(int)i;
    }
    ListView_SetSelectionMark(view,new_selected);
    if(new_top<0 && ui->outputs.count) new_top=top<(int)ui->outputs.count?top:(int)ui->outputs.count-1;
    if(new_top>=0) {
        RECT after;
        if(ListView_GetItemRect(view,new_top,&after,LVIR_BOUNDS)) ListView_Scroll(view,0,after.top-before.top);
    }
    ListView_Scroll(view,horizontal-GetScrollPos(view,SB_HORZ),0);
    outputs_free(&old); ui->filling=FALSE; SendMessageW(view,WM_SETREDRAW,TRUE,0);
    RedrawWindow(view,NULL,NULL,RDW_INVALIDATE|RDW_ALLCHILDREN); status_fill();
}
static void outputs_fill(void) {
    OutputList list={0},detected={0}; size_t i,j,capacity; HRESULT hr=outputs_enumerate(&detected);
    if(FAILED(hr)) {
        /* Saved rules remain editable even if Windows Audio is unavailable. */
        ZeroMemory(&detected,sizeof(detected));
    }
    /* Bound UI work and make the combined allocation's arithmetic explicit. */
    if(detected.count>4096 || ui->draft.output_count>MG_LIMIT) { outputs_free(&detected); return; }
    capacity=detected.count+ui->draft.output_count;
    if(capacity) {
        list.items=mg_alloc(capacity*sizeof(*list.items));
        if(!list.items) { outputs_free(&detected); return; }
        if(detected.count) CopyMemory(list.items,detected.items,detected.count*sizeof(*list.items));
    }
    list.count=detected.count; mg_free(detected.items); /* String ownership moves to list. */
    for(i=0;i<ui->draft.output_count && list.count<capacity;++i) {
        OutputRule *r=&ui->draft.outputs[i]; OutputInfo extra={0};
        for(j=0;j<list.count && j<capacity;++j) if(!wcscmp(list.items[j].id,r->id)) break;
        if(j<list.count) continue;
        extra.id=mg_string(r->id); extra.name=mg_string(r->name);
        if(!extra.id || !extra.name) { mg_free(extra.id); mg_free(extra.name); continue; }
        list.items[list.count]=extra; ++list.count;
    }
    outputs_present(&list);
}
static void layout(void) {
    static const int boxes[12][4]={{20,16,680,26},{20,80,680,96},{20,184,490,240},{520,184,80,28},{610,184,90,28},
        {20,282,680,162},{20,452,185,28},{540,452,160,28},{520,538,80,30},{610,538,90,30},{20,530,475,42},{20,226,680,26}};
    static const int labels[3][4]={{20,52,680,24},{20,258,680,24},{20,490,680,34}};
    NONCLIENTMETRICSW metrics={sizeof(metrics)}; HFONT old=ui->font; size_t i;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,ui->dpi);
    ui->font=CreateFontIndirectW(&metrics.lfMessageFont);
    for(i=0;i<12;++i) {
        MoveWindow(ui->controls[i],scaled(boxes[i][0]),scaled(boxes[i][1]),scaled(boxes[i][2]),scaled(boxes[i][3]),TRUE);
        SendMessageW(ui->controls[i],WM_SETFONT,(WPARAM)ui->font,TRUE);
    }
    for(i=0;i<3;++i) {
        HWND label=GetDlgItem(ui->window,300+(int)i);
        MoveWindow(label,scaled(labels[i][0]),scaled(labels[i][1]),scaled(labels[i][2]),scaled(labels[i][3]),TRUE);
        SendMessageW(label,WM_SETFONT,(WPARAM)ui->font,TRUE);
    }
    ListView_SetColumnWidth(control(ID_OUTPUTS),0,scaled(370)); ListView_SetColumnWidth(control(ID_OUTPUTS),1,scaled(112)); ListView_SetColumnWidth(control(ID_OUTPUTS),2,scaled(160));
    if(old) DeleteObject(old);
}
static BOOL ui_create(HWND hwnd) {
    static const wchar_t *classes[12]={L"BUTTON",L"LISTBOX",L"COMBOBOX",L"BUTTON",L"BUTTON",WC_LISTVIEWW,L"BUTTON",L"BUTTON",L"BUTTON",L"BUTTON",L"STATIC",L"BUTTON"};
    static const wchar_t *texts[12]={L"&Enable automatic muting",L"",L"",L"&Add",L"&Remove",L"",L"Protect / unprotect selected",L"Refresh outputs",L"&Apply",L"&Close",L"",L"Mute on all outputs except protected devices"};
    static const DWORD styles[12]={BS_AUTOCHECKBOX,LBS_NOTIFY|WS_BORDER|WS_VSCROLL,CBS_DROPDOWN|CBS_SORT|CBS_AUTOHSCROLL|WS_VSCROLL,
        BS_PUSHBUTTON,BS_PUSHBUTTON,LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_BORDER,BS_PUSHBUTTON,BS_PUSHBUTTON,BS_DEFPUSHBUTTON,BS_PUSHBUTTON,SS_LEFT,BS_AUTOCHECKBOX};
    static const wchar_t *labels[3]={L"Applications (executable names)",L"Audio outputs - check the outputs to mute",L"Unchecked outputs stay unaffected. Protected outputs cannot be checked.\nRules take effect when you click Apply. Closing keeps MuteGuard running."};
    size_t i;
    ui->window=hwnd; ui->dpi=GetDpiForWindow(hwnd);
    for(i=0;i<12;++i) {
        ui->controls[i]=CreateWindowExW(0,classes[i],texts[i],WS_CHILD|WS_VISIBLE|styles[i]|(i==10?0:WS_TABSTOP),0,0,0,0,hwnd,(HMENU)(INT_PTR)(ID_ENABLE+i),g_instance,NULL);
        if(!ui->controls[i]) { message_error(L"Windows could not create a settings control. Please close MuteGuard and try again."); return FALSE; }
    }
    for(i=0;i<3;++i) if(!CreateWindowExW(0,L"STATIC",labels[i],WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd,(HMENU)(INT_PTR)(300+i),g_instance,NULL)) return FALSE;
    SendMessageW(control(ID_ENTRY),CB_LIMITTEXT,MG_NAME-1,0);
    SendMessageW(control(ID_ENTRY),CB_SETCUEBANNER,0,(LPARAM)L"Type app.exe or choose a running application");
    ListView_SetExtendedListViewStyle(control(ID_OUTPUTS),LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
    for(i=0;i<3;++i) {
        LVCOLUMNW column={0}; const wchar_t *names[]={L"Playback output",L"Status",L"Rule"};
        column.mask=LVCF_TEXT|LVCF_WIDTH; column.pszText=(LPWSTR)names[i]; column.cx=150; ListView_InsertColumn(control(ID_OUTPUTS),(int)i,&column);
    }
    /* Keep the new mode checkbox in the logical tab order before the device list. */
    SetWindowPos(control(ID_ALL),control(ID_REMOVE),0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    layout(); apps_fill(); running_fill(); outputs_fill(); CheckDlgButton(hwnd,ID_ENABLE,ui->draft.enabled?BST_CHECKED:BST_UNCHECKED);
    CheckDlgButton(hwnd,ID_ALL,ui->draft.all_outputs?BST_CHECKED:BST_UNCHECKED);
    return TRUE;
}
static LRESULT CALLBACK settings_proc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
    case WM_CREATE: return ui_create(hwnd)?0:-1;
    case WM_COMMAND:
        switch(LOWORD(w)) {
        case ID_ADD: {
            wchar_t name[MG_NAME]; GetWindowTextW(control(ID_ENTRY),name,ARRAYSIZE(name));
            if(!config_valid_app(name)) message_error(L"Enter an executable name such as Discord.exe, without a path.");
            else if(!config_add_app(&ui->draft,name)) message_error(L"Could not add this application (maximum 128).");
            else { apps_fill(); SetWindowTextW(control(ID_ENTRY),L""); }
            return 0;
        }
        case ID_REMOVE: {
            LRESULT index=SendMessageW(control(ID_APPS),LB_GETCURSEL,0,0);
            if(index!=LB_ERR) { config_remove_app(&ui->draft,(size_t)index); apps_fill(); } return 0;
        }
        case ID_PROTECT: {
            int index=ListView_GetNextItem(control(ID_OUTPUTS),-1,LVNI_SELECTED);
            if(index>=0) {
                OutputInfo *out=&ui->outputs.items[index]; OutputMode mode=config_output_mode(&ui->draft,out->id)==OUTPUT_PROTECT?OUTPUT_IGNORE:OUTPUT_PROTECT;
                if(config_set_output(&ui->draft,out->id,output_name(out),mode)) {
                    ui->filling=TRUE; if(!ui->draft.all_outputs) ListView_SetCheckState(control(ID_OUTPUTS),index,FALSE); rule_text(index); ui->filling=FALSE;
                } else message_error(L"Could not change this output rule (maximum 128).");
            } else message_error(L"Select an output row first."); return 0;
        }
        case ID_REFRESH: outputs_fill(); return 0;
        case ID_ALL:
            ui->draft.all_outputs=IsDlgButtonChecked(hwnd,ID_ALL)==BST_CHECKED;
            outputs_fill(); return 0;
        case IDOK: case ID_APPLY:
            ui->draft.enabled=IsDlgButtonChecked(hwnd,ID_ENABLE)==BST_CHECKED;
            if(app_commit(&ui->draft)) status_fill(); return 0;
        case ID_CLOSE: case IDCANCEL: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *hdr=(NMHDR*)l;
        if(!ui) return 0;
        if(hdr->idFrom==ID_OUTPUTS && !ui->filling && (hdr->code==LVN_ITEMCHANGING || hdr->code==LVN_ITEMCHANGED)) {
            NMLISTVIEW *v=(NMLISTVIEW*)l;
            if(v->iItem>=0 && (size_t)v->iItem<ui->outputs.count && (v->uChanged&LVIF_STATE) &&
               ((v->uNewState^v->uOldState)&LVIS_STATEIMAGEMASK)) {
                OutputInfo *out=&ui->outputs.items[v->iItem]; BOOL checked=((v->uNewState&LVIS_STATEIMAGEMASK)>>12)==2;
                if(ui->draft.all_outputs) return hdr->code==LVN_ITEMCHANGING;
                if(hdr->code==LVN_ITEMCHANGING) return checked && config_output_mode(&ui->draft,out->id)==OUTPUT_PROTECT;
                if(!config_set_output(&ui->draft,out->id,output_name(out),checked?OUTPUT_MUTE:OUTPUT_IGNORE)) {
                    message_error(L"Could not change this output rule (maximum 128)."); outputs_fill();
                } else { ui->filling=TRUE; rule_text(v->iItem); ui->filling=FALSE; }
            }
        }
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *r=(RECT*)l; ui->dpi=HIWORD(w);
        SetWindowPos(hwnd,NULL,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE); layout(); return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: {
        Settings *old=ui; ui=NULL;
        if(old->font) DeleteObject(old->font); config_free(&old->draft); outputs_free(&old->outputs); mg_free(old); return 0;
    }
    }
    return DefWindowProcW(hwnd,message,w,l);
}
void settings_open(void) {
    static BOOL registered=FALSE; WNDCLASSEXW wc={sizeof(wc)}; RECT r={0,0,720,584}; HWND window; UINT dpi;
    if(ui) { ShowWindow(ui->window,SW_RESTORE); SetForegroundWindow(ui->window); return; }
    if(!registered) {
        wc.lpfnWndProc=settings_proc; wc.hInstance=g_instance; wc.lpszClassName=L"MuteGuard.Settings";
        wc.hCursor=LoadCursorW(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); wc.hIcon=LoadIconW(g_instance,MAKEINTRESOURCEW(1));
        if(!RegisterClassExW(&wc)) return; registered=TRUE;
    }
    ui=mg_alloc(sizeof(*ui)); if(!ui) return; config_init(&ui->draft);
    if(!config_copy(&ui->draft,&g_config)) { mg_free(ui); ui=NULL; return; }
    dpi=GetDpiForSystem(); r.right=MulDiv(r.right,(int)dpi,96); r.bottom=MulDiv(r.bottom,(int)dpi,96);
    AdjustWindowRectExForDpi(&r,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,WS_EX_CONTROLPARENT,dpi);
    window=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName?wc.lpszClassName:L"MuteGuard.Settings",L"MuteGuard Settings",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,NULL,NULL,g_instance,NULL);
    if(!window) {
        DWORD error=GetLastError(); wchar_t text[180];
        if(ui) { config_free(&ui->draft); mg_free(ui); ui=NULL; }
        StringCchPrintfW(text,ARRAYSIZE(text),L"Windows could not open Settings (error %lu). Audio monitoring continues in the tray.",error);
        MessageBoxW(g_owner,text,L"MuteGuard",MB_OK|MB_ICONERROR); return;
    }
    /* Settings is an independent taskbar window, not owned by the hidden tray
       window. Explicit user opens must also override a launcher's SW_HIDE. */
    SetWindowPos(window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW); SetForegroundWindow(window);
}
void settings_changed(BOOL devices,BOOL enabled) {
    if(!ui) return;
    if(enabled) { ui->draft.enabled=g_config.enabled; CheckDlgButton(ui->window,ID_ENABLE,g_config.enabled?BST_CHECKED:BST_UNCHECKED); }
    if(devices) outputs_fill(); else status_fill();
}
void settings_close(void) { if(ui) DestroyWindow(ui->window); }
BOOL settings_message(MSG *m) { return ui && IsDialogMessageW(ui->window,m); }
