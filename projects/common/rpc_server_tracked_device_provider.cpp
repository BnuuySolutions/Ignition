#include "vr_rpc_interfaces.h"
#include <iostream>
#include <vector>

#if _WIN32
#include <windows.h>
#endif

bool hasStubbed = false;

// --- RpcServerTrackedDeviceProvider ---

RpcServerTrackedDeviceProvider::RpcServerTrackedDeviceProvider(vr::IServerTrackedDeviceProvider* real) : RpcObject(), real_provider_(real) {
    if (!IsProxy()) {
        std::string prefix = std::to_string(GetId()) + ".";

        RpcSystem::RegisterFunction(prefix + "Init", [this](const auto& args) {
            // The server receives the proxy to the client's context manager
            ClientContextManager* context_proxy = static_cast<ClientContextManager*>(args[0].asObject());
            
            auto result = this->Init(context_proxy);
            
            return RpcValue((int)result);
        });
        
        RpcSystem::RegisterFunction(prefix + "ShouldBlockStandbyMode", [this](const auto& args) {
            return RpcValue((int)this->ShouldBlockStandbyMode());
        });
        
        RpcSystem::RegisterFunction(prefix + "Cleanup", [this](const auto& args) {
            this->Cleanup();
            return RpcValue();
        });

        RpcSystem::RegisterFunction(prefix + "RunFrame", [this](const auto& args) {
            this->RunFrame();
            return RpcValue();
        });

        RpcSystem::RegisterFunction(prefix + "EnterStandby", [this](const auto& args) {
            this->EnterStandby();
            return RpcValue();
        });

        RpcSystem::RegisterFunction(prefix + "LeaveStandby", [this](const auto& args) {
            this->LeaveStandby();
            return RpcValue();
        });

        RpcSystem::RegisterFunction(prefix + "GetInterfaceVersions", [this](const auto& args) {
            const char *const *versions = this->GetInterfaceVersions();
            if (!versions) {
                return RpcValue();
            }

            std::string concatenated_versions;
            for (int i = 0; versions[i] != nullptr; ++i) {
                concatenated_versions.append(versions[i]);
                concatenated_versions.push_back('\0'); // Use null char as separator
            }
            return RpcValue(concatenated_versions);
        });
    }
}

RpcServerTrackedDeviceProvider::RpcServerTrackedDeviceProvider(RpcObjectId id) : RpcObject(id), real_provider_(nullptr) {}

RpcServerTrackedDeviceProvider::~RpcServerTrackedDeviceProvider() {}

const std::string& RpcServerTrackedDeviceProvider::GetRpcClassName() const {
    static const std::string name = "IServerTrackedDeviceProvider";
    return name;
}

vr::EVRInitError RpcServerTrackedDeviceProvider::Init(vr::IVRDriverContext *pDriverContext) {
    if (IsProxy()) {
        // Client side. The provided context is from SteamVR and is real.
        VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
        
        // Create a manager for this context so the server can get interfaces from it.
        auto* context_manager = new ClientContextManager(pDriverContext);

        // Call the server's Init, passing a proxy to our context manager
        RpcValue result = RpcSystem::CallMethod(GetId(), "Init", RpcValue(context_manager));
        return (vr::EVRInitError)result.asInt();
    }
    else {
        return real_provider_->Init(pDriverContext);
    }
}

void RpcServerTrackedDeviceProvider::Cleanup() {
    if (IsProxy()) {
        RpcSystem::CallMethod(GetId(), "Cleanup");
    }
    else {
        real_provider_->Cleanup();
    }
}

const char *const *RpcServerTrackedDeviceProvider::GetInterfaceVersions() {
    if (IsProxy()) {
        // If we've already fetched them, return the cached version.
        if (!client_versions_ptrs_.empty()) {
            return client_versions_ptrs_.data();
        }

        RpcValue result = RpcSystem::CallMethod(GetId(), "GetInterfaceVersions");
        if (result.isString()) {
            const std::string& concatenated = result.asString();
            size_t start = 0;
            while (start < concatenated.length()) {
                size_t end = concatenated.find('\0', start);
                if (end == std::string::npos) {
                    break; // Should not happen if server implementation is correct
                }
                client_versions_storage_.push_back(concatenated.substr(start, end - start));
                start = end + 1;
            }

            // Build the array of C-string pointers
            for (const auto& str : client_versions_storage_) {
                client_versions_ptrs_.push_back(str.c_str());
            }
            client_versions_ptrs_.push_back(nullptr); // Null-terminate the array
            return client_versions_ptrs_.data();
        }
        return nullptr;
    } else {
        return real_provider_->GetInterfaceVersions();
    }
}

void RpcServerTrackedDeviceProvider::RunFrame() {
    if (IsProxy()) {
        RpcSystem::CallMethod(GetId(), "RunFrame");
    }
    else {
        real_provider_->RunFrame();

        if (!hasStubbed) {
#if _WIN32
            // Stub out code at 0x11d0c0 with a ret in driver_playstation_vr2_orig.dll to avoid crash.
            HMODULE hModuleo = GetModuleHandleW(L"driver_playstation_vr2_orig.dll");

            void* function = reinterpret_cast<char*>(hModuleo) + 0x11d0c0;
            DWORD oldProtect;
            if (!VirtualProtect(function, 1, PAGE_EXECUTE_READWRITE, &oldProtect
            )) {
                std::cout << "VirtualProtect failed: " << GetLastError() << std::endl;
            }
            unsigned char retInstruction = 0xC3; // x86 RET instruction
            SIZE_T bytesWritten;
            if (!WriteProcessMemory(GetCurrentProcess(), function, &retInstruction, 1, 
                &bytesWritten) || bytesWritten != 1) {
                std::cout << "WriteProcessMemory failed: " << GetLastError() << std::endl;
            }
            if (!VirtualProtect(function, 1, oldProtect, &oldProtect)) {
                std::cout << "VirtualProtect restore failed: " << GetLastError() << std::endl;
            }

            // Flush instruction cache to ensure modified code is used.
            if (!FlushInstructionCache(GetCurrentProcess(), function, 1)) {
                std::cout << "FlushInstructionCache failed: " << GetLastError() << std::endl;
            }

            hasStubbed = true;
#endif
        }
    }
}

bool RpcServerTrackedDeviceProvider::ShouldBlockStandbyMode() {
    if (IsProxy()) {
        return RpcSystem::CallMethod(GetId(), "ShouldBlockStandbyMode").asInt();
    }
    else {
        return real_provider_->ShouldBlockStandbyMode();
    }
}

void RpcServerTrackedDeviceProvider::EnterStandby() {
    if (IsProxy()) {
        RpcSystem::CallMethod(GetId(), "EnterStandby");
    }
    else {
        real_provider_->EnterStandby();
    }
}

void RpcServerTrackedDeviceProvider::LeaveStandby() {
    if (IsProxy()) {
        RpcSystem::CallMethod(GetId(), "LeaveStandby");
    }
    else {
        real_provider_->LeaveStandby();
    }
}
