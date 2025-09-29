#include "rpc_core.h"
#include "vr_rpc_interfaces.h"

#include <openvr.hpp>
#include <windows.h>
#include <combaseapi.h>
#include <string>
#include <iostream>
#include <mutex>
#include <map>
#include <vector>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <iomanip>

#include <winusb.h>
#include <libusb.h>
#include <MinHook.h>

// Forward declarations
void* (*pfnHmdDriverFactory)(const char* pInterfaceName, int* pReturnCode) = nullptr;

#define PSVR2_VID 0x054c
#define PSVR2_PID 0x0cde

// === Encapsulated USB Interface State ===
class LibusbInterface {
public:
    libusb_device_handle* libusb_handle = NULL;
    int interface_number = -1;
};

// === Global State Management ===
std::mutex g_usb_mutex;
std::map<WINUSB_INTERFACE_HANDLE, LibusbInterface*> g_interface_map;

libusb_device_handle* g_handle = NULL; // The single libusb device handle for the PSVR2

// This will hold the base handle for the most recently initialized device.
// It's used to open associated interfaces.
static WINUSB_INTERFACE_HANDLE g_base_winusb_handle = NULL;

// === Typedefs for the functions we are hooking ===
typedef int(WINAPI* FUN_180122ac0_t)(long long* param_1);
typedef BOOL(WINAPI* WinUsb_WritePipe_t)(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID, PUCHAR Buffer, ULONG BufferLength, PULONG LengthTransferred, LPOVERLAPPED Overlapped);
typedef BOOL(WINAPI* WinUsb_ReadPipe_t)(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID, PUCHAR Buffer, ULONG BufferLength, PULONG LengthTransferred, LPOVERLAPPED Overlapped);
typedef BOOL(WINAPI* WinUsb_Free_t)(WINUSB_INTERFACE_HANDLE InterfaceHandle);
typedef BOOL(WINAPI* WinUsb_ControlTransfer_t)(WINUSB_INTERFACE_HANDLE InterfaceHandle, WINUSB_SETUP_PACKET SetupPacket, PUCHAR Buffer, ULONG BufferLength, PULONG LengthTransferred, LPOVERLAPPED Overlapped);
typedef BOOL(WINAPI* WinUsb_GetPipePolicy_t)(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID, ULONG PolicyType, PULONG ValueLength, PVOID Value);
typedef BOOL(WINAPI* WinUsb_SetPipePolicy_t)(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID, ULONG PolicyType, ULONG ValueLength, PVOID Value);
typedef BOOL(WINAPI* WinUsb_SetCurrentAlternateSetting_t)(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR SettingNumber);
typedef BOOL(WINAPI* WinUsb_GetDescriptor_t)(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR DescriptorType, UCHAR Index, USHORT LanguageID, PUCHAR Buffer, ULONG BufferLength, PULONG LengthTransferred);
typedef BOOL(WINAPI* WinUsb_GetCurrentAlternateSetting_t)(WINUSB_INTERFACE_HANDLE InterfaceHandle, PUCHAR SettingNumber);
typedef BOOL(WINAPI* WinUsb_AbortPipe_t)(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID);

// Pointers to the original, unhooked functions
FUN_180122ac0_t o_FUN_180122ac0 = NULL;
WinUsb_WritePipe_t o_WinUsb_WritePipe = NULL;
WinUsb_ReadPipe_t o_WinUsb_ReadPipe = NULL;
WinUsb_Free_t o_WinUsb_Free = NULL;
WinUsb_ControlTransfer_t o_WinUsb_ControlTransfer = NULL;
WinUsb_GetPipePolicy_t o_WinUsb_GetPipePolicy = NULL;
WinUsb_SetPipePolicy_t o_WinUsb_SetPipePolicy = NULL;
WinUsb_SetCurrentAlternateSetting_t o_WinUsb_SetCurrentAlternateSetting = NULL;
WinUsb_GetDescriptor_t o_WinUsb_GetDescriptor = NULL;
WinUsb_GetCurrentAlternateSetting_t o_WinUsb_GetCurrentAlternateSetting = NULL;
WinUsb_AbortPipe_t o_WinUsb_AbortPipe = NULL;


// === Our Hooked (Detour) Functions ===

int WINAPI Detour_FUN_180122ac0(long long* param_1) {
    std::lock_guard<std::mutex> lock(g_usb_mutex);

    byte(*get_interface_func)(long long*) = *(byte(**)(long long*))(*param_1 + 0x20);
    byte interface_number = get_interface_func(param_1);
    printf("[Hook] Driver is requesting to open interface number: %d\n", interface_number);

    // Create our custom interface object to wrap the real handle
    LibusbInterface* new_interface = new LibusbInterface();
    new_interface->interface_number = interface_number;

    // Find the correct vid, pid, and then the correct interface
    static libusb_context* ctx = NULL;
    if (ctx == NULL) {
        if (libusb_init(&ctx) < 0) {
            fprintf(stderr, "[libusb hook] Failed to initialize libusb context.\n");
            delete new_interface;
            return 1; // Failure
        }
    }

    if (g_handle == NULL) {
        libusb_device** device_list = NULL;
        ssize_t cnt = libusb_get_device_list(ctx, &device_list);
        if (cnt < 0) {
            fprintf(stderr, "[libusb hook] Failed to get device list.\n");
            libusb_exit(ctx);
            delete new_interface;
            return 1; // Failure
        }

        for (ssize_t i = 0; i < cnt; i++) {
            libusb_device* device = device_list[i];
            struct libusb_device_descriptor desc;
            if (libusb_get_device_descriptor(device, &desc) < 0) {
                continue;
            }

            // Print all connected USB devices for debugging
            printf("[libusb hook] Found device: VID=0x%04x, PID=0x%04x\n", desc.idVendor, desc.idProduct);
            // Print all interfaces for this device
            libusb_config_descriptor* config;
            if (libusb_get_config_descriptor(device, 0, &config) < 0) {
                continue;
            }
            for (int j = 0; j < config->bNumInterfaces; j++) {
                for (int k = 0; k < config->interface[j].num_altsetting; k++) {
                    const libusb_interface_descriptor& iface = config->interface[j].altsetting[k];
                    printf("  Interface %d, AltSetting %d, Class 0x%02x, SubClass 0x%02x, Protocol 0x%02x\n",
                        iface.bInterfaceNumber, iface.bAlternateSetting, iface.bInterfaceClass, iface.bInterfaceSubClass, iface.bInterfaceProtocol);
                }
            }
            libusb_free_config_descriptor(config);
        }
        libusb_free_device_list(device_list, 1);

        // Open the PSVR2 device if not already opened
        g_handle = libusb_open_device_with_vid_pid(ctx, PSVR2_VID, PSVR2_PID);
        if (g_handle == NULL) {
            fprintf(stderr, "[libusb hook] Could not find/open PSVR2 device (VID=0x%04x, PID=0x%04x).\n", PSVR2_VID, PSVR2_PID);
            delete new_interface;
            return 1; // Failure
        } else {
            printf("[libusb hook] Successfully opened PSVR2 device.\n");
        }
    }

    // Claim the requested interface
    if (libusb_claim_interface(g_handle, interface_number) < 0) {
        fprintf(stderr, "[libusb hook] Failed to claim interface %d.\n", interface_number);
        libusb_close(g_handle);
        g_handle = NULL;
        delete new_interface;
        return 1; // Failure
    } else {
        printf("[libusb hook] Successfully claimed interface %d.\n", interface_number);
    }

    new_interface->libusb_handle = g_handle; // Reuse the single device handle

    // The "fake" handle is a pointer to our struct, which abstracts away the real handle.
    WINUSB_INTERFACE_HANDLE fake_handle = (WINUSB_INTERFACE_HANDLE)new_interface;
    g_interface_map[fake_handle] = new_interface;

    // Populate the original driver structure with our fake handle
    *(int*)((char*)param_1 + 0x40) = 1;
    *(WINUSB_INTERFACE_HANDLE*)((char*)param_1 + 0x48) = fake_handle;
    *(HANDLE*)((char*)param_1 + 0x50) = INVALID_HANDLE_VALUE; // Doesn't matter to the driver

    printf("[Hook] Initialization complete. Fake handle %p created on interface %d.\n", fake_handle, interface_number);
    return 0; // Success
}

BOOL WINAPI Detour_WinUsb_Free(WINUSB_INTERFACE_HANDLE InterfaceHandle) {
    std::lock_guard<std::mutex> lock(g_usb_mutex);
    auto it = g_interface_map.find(InterfaceHandle);
    if (it == g_interface_map.end()) {
        return o_WinUsb_Free(InterfaceHandle); // Not our handle, pass it through
    }

    LibusbInterface* interface_obj = it->second;
    printf("[Hook WinUsb_Free] Intercepted call for fake handle %p.\n", InterfaceHandle);
    
    delete interface_obj;
    g_interface_map.erase(it);

    return TRUE;
}

BOOL WINAPI Detour_WinUsb_WritePipe(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID, PUCHAR Buffer, ULONG BufferLength, PULONG LengthTransferred, LPOVERLAPPED Overlapped) {
    LibusbInterface* interface_obj;
    {
        std::lock_guard<std::mutex> lock(g_usb_mutex);
        auto it = g_interface_map.find(InterfaceHandle);
        if (it == g_interface_map.end()) {
            return o_WinUsb_WritePipe(InterfaceHandle, PipeID, Buffer, BufferLength, LengthTransferred, Overlapped);
        }
        interface_obj = it->second;
    }
    
    int transferred = 0;
    int res = libusb_bulk_transfer(interface_obj->libusb_handle, PipeID, Buffer, BufferLength, &transferred, 0);
    if (res < 0)
    {
        fprintf(stderr, "[libusb hook] WritePipe failed on interface %d, PipeID 0x%02x: %s\n", interface_obj->interface_number, PipeID, libusb_error_name(res));
        if (LengthTransferred) *LengthTransferred = 0;
        return FALSE;
    }
    if (LengthTransferred) *LengthTransferred = transferred;
    return TRUE;
}

BOOL WINAPI Detour_WinUsb_ReadPipe(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID, PUCHAR Buffer, ULONG BufferLength, PULONG LengthTransferred, LPOVERLAPPED Overlapped) {
    LibusbInterface* interface_obj;
    {
        std::lock_guard<std::mutex> lock(g_usb_mutex);
        auto it = g_interface_map.find(InterfaceHandle);
        if (it == g_interface_map.end()) {
            return o_WinUsb_ReadPipe(InterfaceHandle, PipeID, Buffer, BufferLength, LengthTransferred, Overlapped);
        }
        interface_obj = it->second;
    }

    int transferred = 0;
    int res = libusb_bulk_transfer(interface_obj->libusb_handle, PipeID, Buffer, BufferLength, &transferred, 0);
    if (res < 0)
    {
        fprintf(stderr, "[libusb hook] ReadPipe failed on interface %d, PipeID 0x%02x: %s\n", interface_obj->interface_number, PipeID, libusb_error_name(res));
        if (LengthTransferred) *LengthTransferred = 0;
        return FALSE;
    }
    if (LengthTransferred) *LengthTransferred = transferred;
    return TRUE;
}

BOOL WINAPI Detour_WinUsb_ControlTransfer(WINUSB_INTERFACE_HANDLE InterfaceHandle, WINUSB_SETUP_PACKET SetupPacket, PUCHAR Buffer, ULONG BufferLength, PULONG LengthTransferred, LPOVERLAPPED Overlapped) {
    LibusbInterface* interface_obj;
    {
        std::lock_guard<std::mutex> lock(g_usb_mutex);
        auto it = g_interface_map.find(InterfaceHandle);
        if (it == g_interface_map.end()) {
            return o_WinUsb_ControlTransfer(InterfaceHandle, SetupPacket, Buffer, BufferLength, LengthTransferred, Overlapped);
        }
        interface_obj = it->second;
    }
    
    int transferred = 0;
    int res = libusb_control_transfer(interface_obj->libusb_handle,
        SetupPacket.RequestType,
        SetupPacket.Request,
        SetupPacket.Value,
        SetupPacket.Index,
        Buffer,
        BufferLength,
        5000);

    if (res < 0)
    {
        fprintf(stderr, "[libusb hook] ControlTransfer failed on interface %d: %s\n", interface_obj->interface_number, libusb_error_name(res));
        if (LengthTransferred) *LengthTransferred = 0;
        return FALSE;
    }

    return TRUE;
}

BOOL WINAPI Detour_WinUsb_SetPipePolicy(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID, ULONG PolicyType, ULONG ValueLength, PVOID Value) {
    std::lock_guard<std::mutex> lock(g_usb_mutex);
    auto it = g_interface_map.find(InterfaceHandle);
    if (it == g_interface_map.end()) {
        return o_WinUsb_SetPipePolicy(InterfaceHandle, PipeID, PolicyType, ValueLength, Value);
    }
    
    // Assert stub
    throw std::runtime_error("Detour_WinUsb_SetPipePolicy is not implemented.");
}

BOOL WINAPI Detour_WinUsb_GetPipePolicy(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID, ULONG PolicyType, PULONG ValueLength, PVOID Value) {
    std::lock_guard<std::mutex> lock(g_usb_mutex);
    auto it = g_interface_map.find(InterfaceHandle);
    if (it == g_interface_map.end()) {
        return o_WinUsb_GetPipePolicy(InterfaceHandle, PipeID, PolicyType, ValueLength, Value);
    }
    
    if (PolicyType == MAXIMUM_TRANSFER_SIZE) {
        if (ValueLength && *ValueLength >= sizeof(ULONG) && Value) {
            *(ULONG*)Value = 420;
            return TRUE;
        } else {
            if (ValueLength) *ValueLength = sizeof(ULONG);
            return FALSE;
        }
    }

    // Assert stub
    throw std::runtime_error("Detour_WinUsb_GetPipePolicy is not implemented for this PolicyType.");
}

BOOL WINAPI Detour_WinUsb_SetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR SettingNumber) {
    LibusbInterface* interface_obj;
    {
        std::lock_guard<std::mutex> lock(g_usb_mutex);
        auto it = g_interface_map.find(InterfaceHandle);
        if (it == g_interface_map.end()) {
            return o_WinUsb_SetCurrentAlternateSetting(InterfaceHandle, SettingNumber);
        }
        interface_obj = it->second;
    }
    
    int res = libusb_set_interface_alt_setting(interface_obj->libusb_handle, interface_obj->interface_number, SettingNumber);
    if (res < 0)
    {
        fprintf(stderr, "[libusb hook] SetCurrentAlternateSetting failed on interface %d: %s\n", interface_obj->interface_number, libusb_error_name(res));
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI Detour_WinUsb_GetDescriptor(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR DescriptorType, UCHAR Index, USHORT LanguageID, PUCHAR Buffer, ULONG BufferLength, PULONG LengthTransferred) {
    LibusbInterface* interface_obj;
    {
        std::lock_guard<std::mutex> lock(g_usb_mutex);
        auto it = g_interface_map.find(InterfaceHandle);
        if (it == g_interface_map.end()) {
            return o_WinUsb_GetDescriptor(InterfaceHandle, DescriptorType, Index, LanguageID, Buffer, BufferLength, LengthTransferred);
        }
        interface_obj = it->second;
    }
    
    int res = libusb_get_descriptor(interface_obj->libusb_handle, DescriptorType, Index, Buffer, BufferLength);
    if (res < 0)
    {
        fprintf(stderr, "[libusb hook] GetDescriptor failed on interface %d: %s\n", interface_obj->interface_number, libusb_error_name(res));
        if (LengthTransferred) *LengthTransferred = 0;
        return FALSE;
    }
    if (LengthTransferred) *LengthTransferred = res;
    return TRUE;
}

BOOL WINAPI Detour_WinUsb_GetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE InterfaceHandle, PUCHAR SettingNumber) {
    LibusbInterface* interface_obj;
    {
        std::lock_guard<std::mutex> lock(g_usb_mutex);
        auto it = g_interface_map.find(InterfaceHandle);
        if (it == g_interface_map.end()) {
            return o_WinUsb_GetCurrentAlternateSetting(InterfaceHandle, SettingNumber);
        }
        interface_obj = it->second;
    }
    
    libusb_device* dev = libusb_get_device(interface_obj->libusb_handle);
    libusb_config_descriptor* config;
    if (libusb_get_active_config_descriptor(dev, &config) < 0) {
        fprintf(stderr, "[libusb hook] GetCurrentAlternateSetting failed to get config descriptor on interface %d.\n", interface_obj->interface_number);
        return FALSE;
    }
    *SettingNumber = config->interface[0].altsetting[0].bAlternateSetting;
    libusb_free_config_descriptor(config);
    return TRUE;
}

BOOL WINAPI Detour_WinUsb_AbortPipe(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID) {
    std::lock_guard<std::mutex> lock(g_usb_mutex);
    auto it = g_interface_map.find(InterfaceHandle);
    if (it == g_interface_map.end()) {
        return o_WinUsb_AbortPipe(InterfaceHandle, PipeID);
    }
    
    return TRUE; // No-op for now
}


// === Hooking Infrastructure ===

void CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID* ppOriginal, LPCSTR procName) {
    if (pTarget) {
        if (MH_CreateHook(pTarget, pDetour, ppOriginal) != MH_OK) {
            fprintf(stderr, "Failed to create hook for %s\n", procName);
        }
    } else {
        fprintf(stderr, "Could not find %s\n", procName);
    }
}

void CreateHook(LPCWSTR moduleName, LPCSTR procName, LPVOID pDetour, LPVOID* ppOriginal) {
    LPVOID pTarget = (LPVOID)GetProcAddress(GetModuleHandleW(moduleName), procName);
    CreateHook(pTarget, pDetour, ppOriginal, procName);
}

void PlaceDriverHooks(HMODULE hModule) {
    if (o_FUN_180122ac0 != NULL) return;

    LPVOID target_init_func = (LPVOID)((char*)hModule + 0x122ac0);
    CreateHook(target_init_func, &Detour_FUN_180122ac0, (LPVOID*)&o_FUN_180122ac0, "FUN_180122ac0");
}

RpcServerTrackedDeviceProvider* g_pRpcProvider = nullptr;

void RegisterRPCClasses() {
    RpcSystem::RegisterRPCClass<RpcServerTrackedDeviceProvider>();
    RpcSystem::RegisterRPCClass<ClientContextManager>();
    RpcSystem::RegisterRPCClass<RpcDriverHost>();
    RpcSystem::RegisterRPCClass<RpcDriverLog>();
    RpcSystem::RegisterRPCClass<RpcSettings>();
    RpcSystem::RegisterRPCClass<RpcTrackedDeviceServerDriver>();
    RpcSystem::RegisterRPCClass<RpcDriverInput>();
    RpcSystem::RegisterRPCClass<RpcDriverManager>();
    RpcSystem::RegisterRPCClass<RpcProperties>();
    RpcSystem::RegisterRPCClass<RpcResources>();
    RpcSystem::RegisterRPCClass<RpcDisplayComponent>();
    RpcSystem::RegisterRPCClass<RpcCameraComponent>();
}

int main(int argc, char *argv[])
{
    std::cout << "Ignition server starting..." << std::endl;

    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        printf("Failed to initialize COM.\n");
        return -1;
    }

    RpcSystem::Initialize("ignition_pipe");
    RegisterRPCClasses();

    HMODULE hModule;
#ifdef HARDCODED_DRIVER_PATH
    hModule = LoadLibraryW(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\PlayStation VR2 App\\SteamVR_Plug-In\\bin\\win64\\driver_playstation_vr2.dll");
#else
    if (argc < 2)
    {
        std::cout << "Usage: ignition_server <path to driver DLL>" << std::endl;
        return 1;
    }
    hModule = LoadLibraryA(argv[1]);
#endif

    if (!hModule)
    {
        std::cout << "Failed to load driver DLL. Error: " << GetLastError() << std::endl;
        return -1;
    }

    printf("Initializing MinHook...\n");
    if (MH_Initialize() != MH_OK) {
        fprintf(stderr, "MinHook initialization failed.\n");
        return 1;
    }

    HMODULE hWinUsb = GetModuleHandleA("winusb.dll");
    if (hWinUsb == NULL) {
        hWinUsb = LoadLibraryA("winusb.dll");
    }
    if (hWinUsb == NULL) {
        fprintf(stderr, "Failed to load winusb.dll\n");
        return 1;
    }
    
    // Hook all the standard WinUSB functions
    CreateHook(L"winusb.dll", "WinUsb_Free", &Detour_WinUsb_Free, (LPVOID*)&o_WinUsb_Free);
    CreateHook(L"winusb.dll", "WinUsb_WritePipe", &Detour_WinUsb_WritePipe, (LPVOID*)&o_WinUsb_WritePipe);
    CreateHook(L"winusb.dll", "WinUsb_ReadPipe", &Detour_WinUsb_ReadPipe, (LPVOID*)&o_WinUsb_ReadPipe);
    CreateHook(L"winusb.dll", "WinUsb_ControlTransfer", &Detour_WinUsb_ControlTransfer, (LPVOID*)&o_WinUsb_ControlTransfer);
    CreateHook(L"winusb.dll", "WinUsb_SetPipePolicy", &Detour_WinUsb_SetPipePolicy, (LPVOID*)&o_WinUsb_SetPipePolicy);
    CreateHook(L"winusb.dll", "WinUsb_GetPipePolicy", &Detour_WinUsb_GetPipePolicy, (LPVOID*)&o_WinUsb_GetPipePolicy);
    CreateHook(L"winusb.dll", "WinUsb_SetCurrentAlternateSetting", &Detour_WinUsb_SetCurrentAlternateSetting, (LPVOID*)&o_WinUsb_SetCurrentAlternateSetting);
    
    // Add hooks for the new functions
    CreateHook(L"winusb.dll", "WinUsb_GetDescriptor", &Detour_WinUsb_GetDescriptor, (LPVOID*)&o_WinUsb_GetDescriptor);
    CreateHook(L"winusb.dll", "WinUsb_GetCurrentAlternateSetting", &Detour_WinUsb_GetCurrentAlternateSetting, (LPVOID*)&o_WinUsb_GetCurrentAlternateSetting);
    CreateHook(L"winusb.dll", "WinUsb_AbortPipe", &Detour_WinUsb_AbortPipe, (LPVOID*)&o_WinUsb_AbortPipe);

    HMODULE hModuleo = LoadLibraryW(L"driver_playstation_vr2_orig.dll");
    if(hModuleo) {
        PlaceDriverHooks(hModuleo);
    } else {
        fprintf(stderr, "Failed to load original driver for hooking internal function.\n");
    }


    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        fprintf(stderr, "Failed to enable hooks.\n");
        MH_Uninitialize();
        return 1;
    }
    printf("All initial hooks enabled successfully.\n");

    pfnHmdDriverFactory = decltype(pfnHmdDriverFactory)(GetProcAddress(hModule, "HmdDriverFactory"));
    if (!pfnHmdDriverFactory)
    {
        std::cout << "Failed to get HmdDriverFactory address. Error: " << GetLastError() << std::endl;
        return -1;
    }

    int returnCode = vr::VRInitError_None;
    auto* pRealDeviceProvider =
        static_cast<vr::IServerTrackedDeviceProvider*>(pfnHmdDriverFactory(vr::IServerTrackedDeviceProvider_Version, &returnCode));

    if (returnCode != vr::VRInitError_None || !pRealDeviceProvider)
    {
        std::cout << "HmdDriverFactory failed to get IServerTrackedDeviceProvider. Error: " << returnCode << std::endl;
        return -1;
    }

    RpcSystem::StartServer();

    g_pRpcProvider = new RpcServerTrackedDeviceProvider(pRealDeviceProvider);

    RpcSystem::RegisterFunction(vr::IServerTrackedDeviceProvider_Version, [](const auto& args) {
        auto val = RpcValue(g_pRpcProvider);
        return val;
        });

    std::cout << "Ignition server running." << std::endl;
    
    while (RpcSystem::IsConnected())
    {
        Sleep(1000);
    }

    std::cout << "Client disconnected, shutting down." << std::endl;

    delete g_pRpcProvider;
    RpcSystem::Shutdown();

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    printf("Hooks disabled and resources cleaned up.\n");

    CoUninitialize();

    return 0;
}
