/* Reuse the existing silent WASAPI fixtures and observation helpers. */
#define wmain manual_regression_main
#include "test_audio.c"
#undef wmain

int wmain(int argc,wchar_t **argv) {
    Config c,reloaded; OutputList outputs={0}; Fixture *sessions=NULL,late={0},premuted={0};
    Engine *engine=NULL; size_t i,primary=SIZE_MAX,guard=SIZE_MAX,held=SIZE_MAX,active=0;
    wchar_t path[MAX_PATH]; HRESULT hr=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    setvbuf(stdout,NULL,_IONBF,0); if(FAILED(hr)) return 1;
    config_init(&c); config_init(&reloaded); CHECK(c.all_outputs);
    CHECK(SUCCEEDED(outputs_enumerate(&outputs)));
    for(i=0;i<outputs.count;++i) if(outputs.items[i].state==DEVICE_STATE_ACTIVE) {
        if(primary==SIZE_MAX) primary=i; else if(guard==SIZE_MAX) guard=i; else if(held==SIZE_MAX) held=i;
        ++active;
    }
    if(argc==3 && (!wcscmp(argv[1],L"--write-release-config") || !wcscmp(argv[1],L"--write-release-manual-config"))) {
        BOOL ok=guard!=SIZE_MAX && config_add_app(&c,L"test_audio.exe") &&
            config_set_output(&c,outputs.items[guard].id,L"Protected test output",OUTPUT_PROTECT);
        if(!wcscmp(argv[1],L"--write-release-manual-config")) {
            c.all_outputs=FALSE; ok=ok && primary!=SIZE_MAX && config_set_output(&c,outputs.items[primary].id,L"Selected test output",OUTPUT_MUTE);
        }
        ok=ok && SUCCEEDED(config_save(&c,argv[2]));
        outputs_free(&outputs); config_free(&c); config_free(&reloaded); CoUninitialize(); return ok?0:1;
    }
    if(active<3) { printf("SKIP: automatic discovery test requires three active render endpoints\n"); ++skips; goto done; }
    CHECK(config_add_app(&c,L"test_automatic.exe"));
    CHECK(config_set_output(&c,outputs.items[primary].id,L"Stored manual selection",OUTPUT_MUTE));
    CHECK(config_set_output(&c,outputs.items[guard].id,L"Listening output",OUTPUT_PROTECT));
    CHECK(config_set_output(&c,L"MuteGuard-missing-protected-id",L"Disconnected protected output",OUTPUT_PROTECT));
    StringCchPrintfW(path,ARRAYSIZE(path),L"build\\automatic-%lu.dat",GetCurrentProcessId());
    CHECK(SUCCEEDED(config_save(&c,path))); CHECK(config_load(&reloaded,path)==S_OK && reloaded.all_outputs);
    CHECK(config_output_mode(&reloaded,outputs.items[guard].id)==OUTPUT_PROTECT);
    CHECK(config_output_mode(&reloaded,L"MuteGuard-missing-protected-id")==OUTPUT_PROTECT); DeleteFileW(path);
    sessions=mg_alloc(outputs.count*sizeof(*sessions)); CHECK(sessions!=NULL); if(!sessions) goto done;
    for(i=0;i<outputs.count;++i) if(outputs.items[i].state==DEVICE_STATE_ACTIVE) {
        hr=fixture_create(outputs.items[i].id,&sessions[i]);
        if(FAILED(hr)) printf("Fixture creation failed on render index %zu: 0x%08lX\n",i,hr);
        CHECK(SUCCEEDED(hr));
    }
    CHECK(SUCCEEDED(fixture_create(outputs.items[primary].id,&premuted)));
    if(premuted.volume) CHECK(SUCCEEDED(ISimpleAudioVolume_SetMute(premuted.volume,TRUE,&external_context)));
    /* The test-only engine starts with this real endpoint withheld from its
       discovery results. Release builds contain no hook or hidden-endpoint state. */
    engine=engine_test_start(&reloaded,outputs.items[held].id); CHECK(engine!=NULL); if(!engine) goto done;
    for(i=0;i<outputs.count;++i) if(sessions[i].volume && i!=held)
        CHECK(wait_mute(sessions[i].volume,i!=guard));
    CHECK(muted(sessions[held].volume,FALSE)); CHECK(muted(sessions[guard].volume,FALSE));
    CHECK(engine_test_reveal(engine));
    CHECK(wait_mute(sessions[held].volume,TRUE)); CHECK(muted(sessions[guard].volume,FALSE));
    printf("Simulated device-added discovery: actual session on a previously withheld real render endpoint was muted. No physical reconnection was performed.\n");
    CHECK(SUCCEEDED(fixture_create(outputs.items[held].id,&late))); CHECK(wait_mute(late.volume,TRUE));
    c.all_outputs=FALSE; CHECK(engine_update(engine,&c));
    for(i=0;i<outputs.count;++i) if(sessions[i].volume) CHECK(wait_mute(sessions[i].volume,i==primary));
    CHECK(wait_mute(late.volume,FALSE)); CHECK(muted(premuted.volume,TRUE));
    c.all_outputs=TRUE; CHECK(engine_update(engine,&c));
    for(i=0;i<outputs.count;++i) if(sessions[i].volume) CHECK(wait_mute(sessions[i].volume,i!=guard));
    CHECK(wait_mute(late.volume,TRUE));
    c.enabled=FALSE; CHECK(engine_update(engine,&c));
    for(i=0;i<outputs.count;++i) if(sessions[i].volume) CHECK(wait_mute(sessions[i].volume,FALSE));
    CHECK(wait_mute(late.volume,FALSE)); CHECK(muted(premuted.volume,TRUE));
    c.enabled=TRUE; CHECK(engine_update(engine,&c));
    for(i=0;i<outputs.count;++i) if(sessions[i].volume) CHECK(wait_mute(sessions[i].volume,i!=guard));
    engine_stop(engine); engine=NULL;
    for(i=0;i<outputs.count;++i) if(sessions[i].volume) CHECK(muted(sessions[i].volume,FALSE));
    CHECK(muted(late.volume,FALSE)); CHECK(muted(premuted.volume,TRUE));
    printf("Automatic-mode live coverage: %zu active render endpoints, one protected endpoint; manual selections retained across mode changes.\n",active);
done:
    if(engine) engine_stop(engine);
    if(sessions) for(i=0;i<outputs.count;++i) fixture_free(&sessions[i]);
    mg_free(sessions); fixture_free(&late); fixture_free(&premuted); outputs_free(&outputs);
    config_free(&c); config_free(&reloaded); CoUninitialize();
    printf("Automatic-mode audio integration: %d checks, %d failures, %d skips\n",checks,failures,skips); return failures?1:0;
}
