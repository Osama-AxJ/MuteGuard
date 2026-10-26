#include "muteguard.h"
#include <stdio.h>
#include <strsafe.h>

static int checks,failures;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)
static void write_bytes(const wchar_t *path,const BYTE *bytes,DWORD n) {
    HANDLE file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL); DWORD wrote=0;
    CHECK(file!=INVALID_HANDLE_VALUE); if(file==INVALID_HANDLE_VALUE) return;
    CHECK(WriteFile(file,bytes,n,&wrote,NULL) && wrote==n); CloseHandle(file);
}
int wmain(void) {
    Config c,d,copy; wchar_t path[MAX_PATH],name[MG_NAME]; DWORD i,n=0; BYTE bytes[4096]; HANDLE file;
    config_init(&c); config_init(&d); config_init(&copy);
    StringCchPrintfW(path,ARRAYSIZE(path),L"build\\config-test-%lu.dat",GetCurrentProcessId());
    CHECK(config_load(&c,path)==S_FALSE); CHECK(c.enabled && c.all_outputs && !c.app_count && !c.output_count);
    CHECK(config_output_mode(&c,L"new-render-id")==OUTPUT_MUTE);
    CHECK(config_output_mode(&c,L"")==OUTPUT_IGNORE); CHECK(config_output_mode(&c,L"bad\nidentity")==OUTPUT_IGNORE);
    c.all_outputs=FALSE; /* Existing manual-mode regression tests follow. */
    CHECK(config_add_app(&c,L"Discord.exe")); CHECK(config_add_app(&c,L"DISCORD.EXE")); CHECK(c.app_count==1);
    CHECK(!config_add_app(&c,L"C:\\Discord.exe")); CHECK(!config_add_app(&c,L"*.exe")); CHECK(!config_add_app(&c,L".exe"));
    CHECK(!config_add_app(&c,L" foo.exe")); CHECK(!config_add_app(&c,L"foo.exe\n"));
    CHECK(!config_add_app(&c,L"\xD800.exe")); CHECK(!config_set_output(&c,L"bad",L"",OUTPUT_MUTE));
    { wchar_t *names[]={L"Äudio.exe"}; Config unicode={TRUE,1,0,names,NULL,FALSE}; CHECK(config_matches_app(&unicode,L"äUDIO.EXE")); }
    CHECK(config_add_app(&c,L"音声.exe")); CHECK(config_matches_app(&c,L"discord.EXE")); CHECK(!config_matches_app(&c,L"Discord.exe.evil.exe"));
    CHECK(config_set_output(&c,L"endpoint-a",L"Same name",OUTPUT_MUTE));
    CHECK(config_set_output(&c,L"endpoint-b",L"Same name",OUTPUT_PROTECT));
    CHECK(config_output_mode(&c,L"endpoint-a")==OUTPUT_MUTE); CHECK(config_output_mode(&c,L"endpoint-b")==OUTPUT_PROTECT);
    CHECK(config_output_mode(&c,L"endpoint-c")==OUTPUT_IGNORE);
    CHECK(config_copy(&copy,&c)); CHECK(copy.apps[0]!=c.apps[0]);
    c.enabled=FALSE; CHECK(SUCCEEDED(config_save(&c,path))); CHECK(config_load(&d,path)==S_OK);
    CHECK(!d.enabled && d.app_count==2 && d.output_count==2); CHECK(!wcscmp(d.apps[1],L"音声.exe"));
    CHECK(config_output_mode(&d,L"endpoint-b")==OUTPUT_PROTECT);
    CHECK(config_set_output(&d,L"endpoint-a",L"Renamed",OUTPUT_PROTECT)); CHECK(config_output_mode(&d,L"endpoint-a")==OUTPUT_PROTECT);
    CHECK(config_set_output(&d,L"endpoint-a",L"Renamed",OUTPUT_IGNORE)); CHECK(d.output_count==1);
    config_remove_app(&d,0); CHECK(d.app_count==1 && !wcscmp(d.apps[0],L"音声.exe"));
    file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(file!=INVALID_HANDLE_VALUE); CHECK(ReadFile(file,bytes,sizeof(bytes),&n,NULL)); CloseHandle(file);
    {
        BYTE legacy[4096]; Config migrated; config_init(&migrated);
        /* Produce an exact V1 record: old magic and no automatic-mode word. */
        CopyMemory(legacy,bytes,8); legacy[3]=0x31; CopyMemory(legacy+8,bytes+12,n-12);
        write_bytes(path,legacy,n-4); CHECK(config_load(&migrated,path)==S_OK);
        CHECK(!migrated.all_outputs && !migrated.enabled && migrated.app_count==2 && migrated.output_count==2);
        CHECK(config_output_mode(&migrated,L"endpoint-c")==OUTPUT_IGNORE);
        CHECK(config_output_mode(&migrated,L"endpoint-a")==OUTPUT_MUTE);
        CHECK(config_output_mode(&migrated,L"endpoint-b")==OUTPUT_PROTECT);
        migrated.all_outputs=TRUE; CHECK(config_copy(&d,&migrated)); CHECK(d.all_outputs && d.output_count==2);
        CHECK(config_output_mode(&d,L"newly-connected-id")==OUTPUT_MUTE);
        CHECK(config_saved_output_mode(&d,L"newly-connected-id")==OUTPUT_IGNORE);
        CHECK(config_output_mode(&d,L"endpoint-b")==OUTPUT_PROTECT);
        CHECK(SUCCEEDED(config_save(&d,path))); config_free(&d); CHECK(config_load(&d,path)==S_OK && d.all_outputs);
        CHECK(config_output_mode(&d,L"endpoint-b")==OUTPUT_PROTECT);
        d.all_outputs=FALSE; CHECK(config_output_mode(&d,L"newly-connected-id")==OUTPUT_IGNORE);
        CHECK(config_output_mode(&d,L"endpoint-a")==OUTPUT_MUTE);
        config_free(&migrated);
        /* Empty V1 files must also remain manual after migration. */
        ZeroMemory(legacy,16); legacy[0]=0x55; legacy[1]=0x4d; legacy[2]=0x47; legacy[3]=0x31; legacy[4]=1;
        write_bytes(path,legacy,16); CHECK(config_load(&migrated,path)==S_OK && !migrated.all_outputs && !migrated.app_count);
        config_free(&migrated);
        bytes[8]=2; write_bytes(path,bytes,n); CHECK(FAILED(config_load(&d,path))); bytes[8]=0;
    }
    /* Every truncated prefix must fail without partially installing rules. */
    for(i=0;i<n;++i) {
        write_bytes(path,bytes,i); CHECK(FAILED(config_load(&copy,path))); CHECK(copy.enabled && copy.app_count==2 && copy.output_count==2);
    }
    bytes[n]=42; write_bytes(path,bytes,n+1); CHECK(FAILED(config_load(&copy,path)));
    bytes[0]^=0xff; write_bytes(path,bytes,n); CHECK(FAILED(config_load(&copy,path))); bytes[0]^=0xff;
    bytes[4]=2; write_bytes(path,bytes,n); CHECK(FAILED(config_load(&copy,path))); bytes[4]=0;
    bytes[16]=0xff; bytes[17]=0xff; write_bytes(path,bytes,n); CHECK(FAILED(config_load(&copy,path)));
    /* Bounded random input exercises malformed length/count records. */
    for(i=0;i<256;++i) {
        DWORD j,state=i+1; for(j=0;j<256;++j) { state=state*1664525u+1013904223u; bytes[j]=(BYTE)(state>>24); }
        write_bytes(path,bytes,i); CHECK(FAILED(config_load(&copy,path)));
    }
    config_free(&c); config_init(&c);
    for(i=0;i<MG_LIMIT;++i) { StringCchPrintfW(name,ARRAYSIZE(name),L"app%lu.exe",i); CHECK(config_add_app(&c,name)); }
    CHECK(!config_add_app(&c,L"overflow.exe")); CHECK(SUCCEEDED(config_save(&c,path))); CHECK(config_load(&d,path)==S_OK); CHECK(d.app_count==MG_LIMIT);
    /* Duplicate protection takes precedence even for an untrusted in-memory list. */
    { OutputRule rules[2]={{L"same",L"x",OUTPUT_PROTECT},{L"same",L"x",OUTPUT_MUTE}}; Config raw={TRUE,0,2,NULL,rules,TRUE}; CHECK(config_output_mode(&raw,L"same")==OUTPUT_PROTECT); CHECK(config_copy(&copy,&raw)); CHECK(copy.all_outputs && config_output_mode(&copy,L"same")==OUTPUT_PROTECT); }
    CHECK(FAILED(config_save(&c,L"build\\missing-parent\\settings.dat")));
    DeleteFileW(path); config_free(&c); config_free(&d); config_free(&copy);
    printf("Configuration/policy: %d checks, %d failures\n",checks,failures); return failures?1:0;
}
