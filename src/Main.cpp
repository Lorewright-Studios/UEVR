// dllmain
#include <windows.h>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <utility/Thread.hpp>

#include "Framework.hpp"
#include "mods/vr/MetaAsw.hpp"

void startup_thread(HMODULE poc_module) {
    g_framework = std::make_unique<Framework>(poc_module);
}

BOOL APIENTRY DllMain(HANDLE handle, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)startup_thread, handle, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        // Process is exiting: if UEVR forced Meta ASW, set it back to automatic.
        uevr::meta_asw::Controller::get().on_process_exit();
    }

    return TRUE;
}