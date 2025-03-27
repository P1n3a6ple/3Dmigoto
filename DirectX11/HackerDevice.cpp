// Wrapper for the ID3D11Device.
// This gives us access to every D3D11 call for a device, and override the pieces needed.

// Object          OS               D3D11 version   Feature level
// ID3D11Device    Win7             11.0            11.0
// ID3D11Device1   Platform update  11.1            11.1
// ID3D11Device2   Win8.1           11.2
// ID3D11Device3   Win10            11.3
// ID3D11Device4                    11.4

// Include before util.h (or any header that includes util.h) to get pretty
// version of LOCK_RESOURCE_CREATION_MODE:
#include "HackerDevice.hpp"

#include "Assembler.h"
#include "CommandList.hpp"
#include "D3D11Wrapper.h"
#include "DecompileHLSL.h"
#include "FrameAnalysis.hpp"
#include "Globals.h"
#include "HackerContext.hpp"
#include "HackerDXGI.hpp"
#include "HookedDevice.h"
#include "Hunting.hpp"
#include "iid.h"
#include "IniHandler.h"
#include "Lock.h"
#include "log.h"
#include "nvstereo.h"
#include "Overlay.hpp"
#include "Profiling.hpp"
#include "ResourceHash.hpp"
#include "shader.h"
#include "ShaderRegex.hpp"
#include "util.h"

#include <codecvt>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <dxgi1_2.h>
#include <locale>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

// We include this specifically after d3d11.h so that it can define
// the __d3d11_h__ preprocessor and pick up extra calls.
#include "nvapi.h"

using std::string;
using std::unordered_map;
using std::vector;
using std::wstring;

using overlay::log;
using overlay::log_overlay;

// -----------------------------------------------------------------------------

// A map to look up the HackerDevice from an IUnknown. The reason for using an
// IUnknown as the key is that an ID3D11Device and IDXGIDevice are actually two
// different interfaces to the same object, which means that QueryInterface()
// can be used to traverse between them. They do not however inherit from each
// other and using C style casting between them will not work. We need to be
// able to find our HackerDevice from either interface, including hooked
// versions, so we need to find a common handle to use as a key between them.
//
// We could probably get away with calling QueryInterface(IID_ID3D11Device),
// however COM does not guarantee that pointers returned to the same interface
// will be identical (they can be "tear-off" interfaces independently
// refcounted from the main object and potentially from each other, or they
// could just be implemented in the main object with shared refcounting - we
// shouldn't assume which is in use for a given interface, because it's an
// implementation detail that could change).
//
// COM does however offer a guarantee that calling QueryInterface(IID_IUnknown)
// will return a consistent pointer for all interfaces to the same object, so
// we can safely use that. Note that it is important we use QueryInterface() to
// get this pointer, not C/C++ style casting, as using the later on pointers is
// really just a noop, and will return the same pointer we pass into them.
//
// In practice we see the consequences of ID3D11Device and IDXGIDevice being
// the same object in UE4 games (in all versions since the source was
// released), that call ID3D11Device::QueryInterface(IID_IDXGIDevice), and pass
// the returned pointer to CreateSwapChain. Since we no longer wrap the
// IDXGIDevice interface we can't directly get back to our HackerDevice, and so
// we use this map to look it up instead.
//
// Note that there is a real possibility that a game could then call
// QueryInterface on the IDXGIDevice to get back to the ID3D11Device, but since
// we aren't intercepting that call it would get the real ID3D11Device and
// could effectively unhook us. If that becomes a problem in practice, we will
// have to rethink this - either bringing back our IDXGIDevice wrapper (or a
// simplified version of it, that respects the relationship to ID3D11Device),
// hooking the QueryInterface on the returned object (but beware that DX itself
// could potentially then call into us), or denying the game from recieving the
// IDXGIDevice in the first place and hoping that it has a fallback path (it
// won't).
typedef unordered_map<IUnknown*, HackerDevice*> DeviceMap;
static DeviceMap                                device_map;

// This will look up a HackerDevice corresponding to some unknown device object
// (ID3D11Device*, IDXGIDevice*, etc). It will bump the refcount on the
// returned interface.
HackerDevice* lookup_hacker_device(
    IUnknown* unknown)
{
    HackerDevice*       ret          = nullptr;
    IUnknown*           real_unknown = nullptr;
    IDXGIObject*        dxgi_obj     = nullptr;
    DeviceMap::iterator i;

    // First, check if this is already a HackerDevice. This is a fast path,
    // but is also kind of important in case we ever make
    // HackerDevice::QueryInterface(IID_IUnknown) return the HackerDevice
    // (which is conceivable we might need to do some day if we find a game
    // that uses that to get back to the real DX interfaces), since doing
    // so would break the COM guarantee we rely on below.
    //
    // HookedDevices will also follow this path, since they hook
    // QueryInterface and will return the corresponding HackerDevice here,
    // but even if they didn't they would still be looked up in the map, so
    // either way we no longer need to call lookup_hooked_device.
    if (SUCCEEDED(unknown->QueryInterface(IID_HackerDevice, reinterpret_cast<void**>(&ret))))
    {
        LOG_INFO("lookup_hacker_device(%p): Supports HackerDevice\n", unknown);
        return ret;
    }

    // We've been passed an IUnknown, but it may not have been gained via
    // QueryInterface (and for convenience it's probably just been cast
    // with C style casting), but we need the real IUnknown pointer with
    // the COM guarantee that it will match for all interfaces of the same
    // object, so we call QueryInterface on it again to get this:
    if (FAILED(unknown->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&real_unknown))))
    {
        // ... ehh, what? Shouldn't happen. Fatal.
        LOG_INFO("lookup_hacker_device: QueryInterface(IID_Unknown) failed\n");
        double_beep_exit();
    }

    ENTER_CRITICAL_SECTION(&G->mCriticalSection);
    {
        i = device_map.find(real_unknown);
        if (i != device_map.end())
        {
            ret = i->second;
            ret->AddRef();
        }
    }
    LEAVE_CRITICAL_SECTION(&G->mCriticalSection);

    real_unknown->Release();

    if (!ret)
    {
        // Either not a d3d11 device, or something has handed us an
        // unwrapped device *and also* violated the COM identity rule.
        // This is known to happen with ReShade in certain games (e.g.
        // Resident Evil 2), though it appears that DirectX itself
        // violates the COM identity rule in some cases (Device4/5 +
        // Multithread interfaces)
        //
        // We have a few more tricks up our sleeve to try to find our
        // HackerDevice - the first would be to look up the
        // ID3D11Device interface and use it as a key to look up our
        // device_map. That would work for the ReShade case as it
        // stands today, but is not foolproof since e.g. that device
        // may itself be wrapped. We could try other interfaces that
        // may not be wrapped or use them to find the DirectX COM
        // identity and look up the map by that, but again if there is
        // a third party tool messing with us than all bets are off.
        //
        // Instead, let's try to do this in a fool proof manner that
        // will hopefully be impervious to anything that a third party
        // tool may do. When we created the device we stored a pointer
        // to our HackerDevice in the device's private data that we
        // should be able to retrieve. We can access that from either
        // the D3D11Device interface, or the DXGIObject interface. For
        // the sake of a possible future DX12 port (or DX10 backport)
        // I'm using the DXGI interface that's supposed to be version
        // agnostic. XXX: It might be worthwhile considering dropping
        // the above device_map lookup which relies on the COM identity
        // rule in favour of this, since we expect this to always work:
        if (SUCCEEDED(unknown->QueryInterface(IID_IDXGIObject, reinterpret_cast<void**>(&dxgi_obj))))
        {
            UINT size;
            if (SUCCEEDED(dxgi_obj->GetPrivateData(IID_HackerDevice, &size, &ret)))
            {
                LOG_INFO("Notice: Unwrapped device and COM Identity violation, Found HackerDevice via GetPrivateData strategy\n");
                ret->AddRef();
            }
            dxgi_obj->Release();
        }
    }

    LOG_INFO("lookup_hacker_device(%p) IUnknown: %p HackerDevice: %p\n", unknown, real_unknown, ret);

    return ret;
}

static IUnknown* register_hacker_device(
    HackerDevice* hacker_device)
{
    IUnknown* real_unknown = nullptr;

    // As above, our key is the real IUnknown gained through QueryInterface
    if (FAILED(hacker_device->GetPassThroughOrigDevice1()->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&real_unknown))))
    {
        LOG_INFO("register_hacker_device: QueryInterface(IID_Unknown) failed\n");
        double_beep_exit();
    }

    LOG_INFO("register_hacker_device: Registering IUnknown: %p -> HackerDevice: %p\n", real_unknown, hacker_device);

    ENTER_CRITICAL_SECTION(&G->mCriticalSection);
    {
        device_map[real_unknown] = hacker_device;
    }
    LEAVE_CRITICAL_SECTION(&G->mCriticalSection);

    real_unknown->Release();

    // We return the IUnknown for convenience, since the HackerDevice needs
    // to store it so it can later unregister it after the real Device has
    // been Released and we will no longer be able to find it through
    // QueryInterface. We have dropped the refcount on this - dangerous I
    // know, but otherwise it will never be released.
    return real_unknown;
}

static void unregister_hacker_device(
    HackerDevice* hacker_device)
{
    IUnknown*           real_unknown;
    DeviceMap::iterator i;

    // We can't do a QueryInterface() here to get the real IUnknown,
    // because the device has already been released. Instead, we use the
    // real IUnknown pointer saved in the HackerDevice.
    real_unknown = hacker_device->GetIUnknown();

    // I have some concerns about our HackerDevice refcounting, and suspect
    // there are cases where our HackerDevice wrapper won't be released
    // along with the wrapped object (because COM refcounting is
    // complicated, and there are several different models it could be
    // using, and our wrapper relies on the ID3D11Device::Release as being
    // the final Release, and not say, IDXGIDevice::Release), and there is
    // a small chance that the handle could have already been reused.
    //
    // Now there is an obvious race here that this critical section should
    // really be held around the original Release() call as well in case it
    // gets reused by another thread before we get here, but I think we
    // have bigger issues than just that, and it doesn't really matter
    // anyway if it does hit, so I'd rather not expand that lock if we
    // don't need to. Just detect if the handle has been reused and print
    // out a message - we know that the HackerDevice won't have been reused
    // yet, so this is safe.
    ENTER_CRITICAL_SECTION(&G->mCriticalSection);
    {
        i = device_map.find(real_unknown);
        if (i != device_map.end())
        {
            if (i->second == hacker_device)
            {
                LOG_INFO("unregister_hacker_device: Unregistering IUnknown %p -> HackerDevice %p\n", real_unknown, hacker_device);
                device_map.erase(i);
            }
            else
            {
                LOG_INFO("BUG: Removing HackerDevice from device_map     IUnknown %p expected to map to %p, actually %p\n", real_unknown, hacker_device, i->second);
            }
        }
    }
    LEAVE_CRITICAL_SECTION(&G->mCriticalSection);
}

// -----------------------------------------------------------------------------------------------

HackerDevice::HackerDevice(
    ID3D11Device1*        device1,
    ID3D11DeviceContext1* context1) :
    stereoHandle(nullptr), stereoTexture(nullptr), stereoResourceView(nullptr), zBufferResourceView(nullptr), iniTexture(nullptr), iniResourceView(nullptr)
{
    origDevice1     = device1;
    realOrigDevice1 = device1;
    origContext1    = context1;
    // Must be done after origDevice1 is set:
    unknown = register_hacker_device(this);
}

HRESULT HackerDevice::CreateStereoParamResources()
{
    HRESULT      hr;
    NvAPI_Status nvret;

    // We use the original device here. Functionally it should not matter
    // if we use the HackerDevice, but it does result in a lot of noise in
    // the frame analysis log as every call into nvapi using the
    // stereoHandle calls Begin() and End() on the immediate context.

    // Todo: This call will fail if stereo is disabled. Proper notification?
    nvret = NvAPI_Stereo_CreateHandleFromIUnknown(origDevice1, &stereoHandle);
    if (nvret != NVAPI_OK)
    {
        stereoHandle = nullptr;
        LOG_INFO("HackerDevice::CreateStereoParamResources NvAPI_Stereo_CreateHandleFromIUnknown failed: %d\n", nvret);
        return nvret;
    }
    paramTextureManager.mStereoHandle = stereoHandle;
    LOG_INFO("  created NVAPI stereo handle. Handle = %p\n", stereoHandle);

    // Create stereo parameter texture.
    LOG_INFO("  creating stereo parameter texture.\n");

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width                = nv::stereo::ParamTextureManagerD3D11::Parms::StereoTexWidth;
    desc.Height               = nv::stereo::ParamTextureManagerD3D11::Parms::StereoTexHeight;
    desc.MipLevels            = 1;
    desc.ArraySize            = 1;
    desc.Format               = nv::stereo::ParamTextureManagerD3D11::Parms::StereoTexFormat;
    desc.SampleDesc.Count     = 1;
    desc.SampleDesc.Quality   = 0;
    desc.Usage                = D3D11_USAGE_DEFAULT;
    desc.BindFlags            = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags       = 0;
    desc.MiscFlags            = 0;
    hr                        = origDevice1->CreateTexture2D(&desc, nullptr, &stereoTexture);
    if (FAILED(hr))
    {
        LOG_INFO("    call failed with result = %x.\n", hr);
        return hr;
    }
    LOG_INFO("    stereo texture created, handle = %p\n", stereoTexture);

    // Since we need to bind the texture to a shader input, we also need a resource view.
    LOG_INFO("  creating stereo parameter resource view.\n");

    D3D11_SHADER_RESOURCE_VIEW_DESC desc_rv = {};
    desc_rv.Format                          = desc.Format;
    desc_rv.ViewDimension                   = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc_rv.Texture2D.MostDetailedMip       = 0;
    desc_rv.Texture2D.MipLevels             = -1;
    hr                                      = origDevice1->CreateShaderResourceView(stereoTexture, &desc_rv, &stereoResourceView);
    if (FAILED(hr))
    {
        LOG_INFO("    call failed with result = %x.\n", hr);
        return hr;
    }

    LOG_INFO("    stereo texture resource view created, handle = %p.\n", stereoResourceView);
    return S_OK;
}

HRESULT HackerDevice::CreateIniParamResources()
{
    // No longer making this conditional. We are pretty well dependent on
    // the ini params these days and not creating this view might cause
    // issues with config reload.

    HRESULT                ret;
    D3D11_SUBRESOURCE_DATA initial_data {};
    D3D11_TEXTURE1D_DESC   desc = {};

    // If we are resizing IniParams we must release the old versions:
    if (iniResourceView)
    {
        long refcount   = iniResourceView->Release();
        iniResourceView = nullptr;
        LOG_INFO("  releasing ini parameters resource view, refcount = %d\n", refcount);
    }
    if (iniTexture)
    {
        long refcount = iniTexture->Release();
        iniTexture    = nullptr;
        LOG_INFO("  releasing iniparams texture, refcount = %d\n", refcount);
    }

    if (G->iniParamsReserved > ini_params_size_warning)
    {
        log_overlay(log::notice, "NOTICE: %d requested IniParams exceeds the recommended %d\n", G->iniParamsReserved, ini_params_size_warning);
    }

    G->iniParams.resize(G->iniParamsReserved);
    if (G->iniParams.empty())
    {
        LOG_INFO("  No IniParams used, skipping texture creation.\n");
        return S_OK;
    }

    LOG_INFO("  creating .ini constant parameter texture.\n");

    // Stuff the constants read from the .ini file into the subresource data structure, so
    // we can init the texture with them.
    initial_data.pSysMem     = G->iniParams.data();
    initial_data.SysMemPitch = sizeof(DirectX::XMFLOAT4) * G->iniParams.size();  // Ignored for Texture1D, but still recommended for debugging

    desc.Width          = G->iniParams.size();  // n texels, .rgba as a float4
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = DXGI_FORMAT_R32G32B32A32_FLOAT;  // float4
    desc.Usage          = D3D11_USAGE_DYNAMIC;             // Read/Write access from GPU and CPU
    desc.BindFlags      = D3D11_BIND_SHADER_RESOURCE;      // As resource view, access via t120
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;          // allow CPU access for hotkeys
    desc.MiscFlags      = 0;
    ret                 = origDevice1->CreateTexture1D(&desc, &initial_data, &iniTexture);
    if (FAILED(ret))
    {
        LOG_INFO("    CreateTexture1D call failed with result = %x.\n", ret);
        return ret;
    }
    LOG_INFO("    IniParam texture created, handle = %p\n", iniTexture);

    // Since we need to bind the texture to a shader input, we also need a resource view.
    // The pDesc is set to NULL so that it will simply use the desc format above.
    LOG_INFO("  creating IniParam resource view.\n");

    ret = origDevice1->CreateShaderResourceView(iniTexture, nullptr, &iniResourceView);
    if (FAILED(ret))
    {
        LOG_INFO("   CreateShaderResourceView call failed with result = %x.\n", ret);
        return ret;
    }

    LOG_INFO("    Iniparams resource view created, handle = %p.\n", iniResourceView);
    return S_OK;
}

void HackerDevice::CreatePinkHuntingResources()
{
    // Only create special pink mode PixelShader when requested.
    if (hunting_enabled() && (G->marking_mode == MarkingMode::PINK || G->config_reloadable))
    {
        char* hlsl =
            "float4 pshader() : SV_Target0"
            "{"
            "    return float4(1,0,1,1);"
            "}";

        ID3D10Blob* blob = nullptr;
        HRESULT     hr   = D3DCompile(hlsl, strlen(hlsl), "JustPink", nullptr, nullptr, "pshader", "ps_4_0", 0, 0, &blob, nullptr);
        LOG_INFO("  Created pink mode pixel shader: %d\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = origDevice1->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &G->mPinkingShader);
            cleanup_shader_maps(G->mPinkingShader);
            if (FAILED(hr))
                LOG_INFO("  Failed to create pinking pixel shader: %d\n", hr);
            blob->Release();
        }
    }
}

HRESULT HackerDevice::SetGlobalNVSurfaceCreationMode()
{
    HRESULT hr;

    // Override custom settings.
    if (stereoHandle && G->gSurfaceCreateMode >= 0)
    {
        nvapi_override();
        LOG_INFO("  setting custom surface creation mode.\n");

        hr = profiling::NvAPI_Stereo_SetSurfaceCreationMode(stereoHandle, static_cast<NVAPI_STEREO_SURFACECREATEMODE>(G->gSurfaceCreateMode));
        if (hr != NVAPI_OK)
        {
            LOG_INFO("    custom surface creation call failed: %d.\n", hr);
            return hr;
        }
    }

    return S_OK;
}

// With the addition of full DXGI support, this init sequence is too dangerous
// to do at object creation time.  The NV CreateHandleFromIUnknown calls back
// into this device, so we need to have it set up and ready.

void HackerDevice::Create3DMigotoResources()
{
    LOG_INFO("HackerDevice::Create3DMigotoResources(%s@%p) called.\n", type_name(this), this);

    // XXX: Ignoring the return values for now because so do our callers.
    // If we want to change this, keep in mind that failures in
    // CreateStereoParamResources and SetGlobalNVSurfaceCreationMode should
    // be considdered non-fatal, as stereo could be disabled in the control
    // panel, or we could be on an AMD or Intel card.

    LOCK_RESOURCE_CREATION_MODE();
    {
        CreateStereoParamResources();
        CreateIniParamResources();
        CreatePinkHuntingResources();
        SetGlobalNVSurfaceCreationMode();
    }
    UNLOCK_RESOURCE_CREATION_MODE();

    optimise_command_lists(this);
}

// Save reference to corresponding HackerContext during CreateDevice, needed for GetImmediateContext.

void HackerDevice::SetHackerContext(
    HackerContext* hacker_context)
{
    hackerContext = hacker_context;
}

HackerContext* HackerDevice::GetHackerContext()
{
    LOG_INFO("HackerDevice::GetHackerContext returns %p\n", hackerContext);
    return hackerContext;
}

void HackerDevice::SetHackerSwapChain(
    HackerSwapChain* hacker_swap_chain)
{
    hackerSwapChain = hacker_swap_chain;
}

HackerSwapChain* HackerDevice::GetHackerSwapChain()
{
    return hackerSwapChain;
}

// Returns the "real" DirectX object. Note that if hooking is enabled calls
// through this object will go back into 3DMigoto, which would then subject
// them to extra logging and any processing 3DMigoto applies, which may be
// undesirable in some cases. This used to cause a crash if a command list
// issued a draw call, since that would then trigger the command list and
// recurse until the stack ran out:
ID3D11Device1* HackerDevice::GetPossiblyHookedOrigDevice1()
{
    return realOrigDevice1;
}

// Use this one when you specifically don't want calls through this object to
// ever go back into 3DMigoto. If hooking is disabled this is identical to the
// above, but when hooking this will be the trampoline object instead:
ID3D11Device1* HackerDevice::GetPassThroughOrigDevice1()
{
    return origDevice1;
}

ID3D11DeviceContext1* HackerDevice::GetPossiblyHookedOrigContext1()
{
    return origContext1;
}

ID3D11DeviceContext1* HackerDevice::GetPassThroughOrigContext1()
{
    if (hackerContext)
        return hackerContext->GetPassThroughOrigContext1();

    return origContext1;
}

IUnknown* HackerDevice::GetIUnknown()
{
    return unknown;
}

void HackerDevice::HookDevice()
{
    // This will install hooks in the original device (if they have not
    // already been installed from a prior device) which will call the
    // equivalent function in this HackerDevice. It returns a trampoline
    // interface which we use in place of origDevice1 to call the real
    // original device, thereby side stepping the problem that calling the
    // old origDevice1 would be hooked and call back into us endlessly:
    origDevice1 = hook_device(origDevice1, this);
}

// -----------------------------------------------------------------------------------------------
// ToDo: I'd really rather not have these standalone utilities here, this file should
// ideally be only HackerDevice and it's methods.  Because of our spaghetti Globals+Utils,
// it gets too involved to move these out right now.

// For any given vertex or pixel shader from the ShaderFixes folder, we need to track them at load time so
// that we can associate a given active shader with an override file.  This allows us to reload the shaders
// dynamically, and do on-the-fly fix testing.
// ShaderModel is usually something like "vs_5_0", but "bin" is a valid ShaderModel string, and tells the
// reloader to disassemble the .bin file to determine the shader model.

// Currently, critical lock must be taken BEFORE this is called.

static void register_for_reload(
    ID3D11DeviceChild*  shader,
    UINT64              hash,
    wstring             shader_type,
    string              shader_model,
    ID3D11ClassLinkage* class_linkage,
    ID3DBlob*           byte_code,
    FILETIME            time_stamp,
    wstring             text,
    bool                deferred_replacement_candidate)
{
    LOG_INFO("    shader registered for possible reloading: %016llx_%ls as %s - %ls\n", hash, shader_type.c_str(), shader_model.c_str(), text.c_str());

    // Pretty sure we had a bug before since we would save a pointer to the
    // class linkage object without bumping its refcount, but I don't know
    // of any game that uses this to test it.
    if (class_linkage)
        class_linkage->AddRef();

    G->mReloadedShaders[shader].hash                           = hash;
    G->mReloadedShaders[shader].shaderType                     = shader_type;
    G->mReloadedShaders[shader].shaderModel                    = shader_model;
    G->mReloadedShaders[shader].linkage                        = class_linkage;
    G->mReloadedShaders[shader].byteCode                       = byte_code;
    G->mReloadedShaders[shader].timeStamp                      = time_stamp;
    G->mReloadedShaders[shader].replacement                    = nullptr;
    G->mReloadedShaders[shader].infoText                       = text;
    G->mReloadedShaders[shader].deferred_replacement_candidate = deferred_replacement_candidate;
    G->mReloadedShaders[shader].deferred_replacement_processed = false;
}

// Helper routines for ReplaceShader, as a way to factor out some of the inline code, in
// order to make it more clear, and as a first step toward full refactoring.

// This routine exports the original binary shader from the game, the cso.  It is a hidden
// feature in the d3dx.ini.  Seems like it might be nice to have them named *_orig.bin, to
// make them more clear.

static void export_orig_binary(
    UINT64         hash,
    const wchar_t* shader_type,
    const void*    shader_bytecode,
    SIZE_T         bytecode_length)
{
    wchar_t path[MAX_PATH];
    HANDLE  f;
    bool    exists = false;

    swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls.bin", G->SHADER_CACHE_PATH, hash, shader_type);
    f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        int cnt = 0;
        while (f != INVALID_HANDLE_VALUE)
        {
            // Check if same file.
            DWORD data_size = GetFileSize(f, nullptr);
            char* buf       = new char[data_size];
            DWORD read_size;
            if (!ReadFile(f, buf, data_size, &read_size, nullptr) || data_size != read_size)
                LOG_INFO("  Error reading file.\n");
            CloseHandle(f);
            if (data_size == bytecode_length && !memcmp(shader_bytecode, buf, data_size))
                exists = true;
            delete[] buf;
            if (exists)
                break;
            swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_%d.bin", G->SHADER_CACHE_PATH, hash, shader_type, ++cnt);
            f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (!exists)
    {
        FILE* fw;
        wfopen_ensuring_access(&fw, path, L"wb");
        if (fw)
        {
            LOG_INFO_W(L"    storing original binary shader to %s\n", path);
            fwrite(shader_bytecode, 1, bytecode_length, fw);
            fclose(fw);
        }
        else
        {
            LOG_INFO_W(L"    error storing original binary shader to %s\n", path);
        }
    }
}

static bool get_file_last_write_time(
    wchar_t*  path,
    FILETIME* ft_write)
{
    HANDLE f;

    f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;

    GetFileTime(f, nullptr, nullptr, ft_write);
    CloseHandle(f);
    return true;
}

static bool check_cache_timestamp(
    HANDLE    bin_handle,
    wchar_t*  bin_path,
    FILETIME& time_stamp)
{
    FILETIME txt_time, bin_time;
    wchar_t  txt_path[MAX_PATH], *end = nullptr;

    wcscpy_s(txt_path, MAX_PATH, bin_path);
    end = wcsstr(txt_path, L".bin");
    wcscpy_s(end, sizeof(L".bin"), L".txt");
    if (get_file_last_write_time(txt_path, &txt_time) && GetFileTime(bin_handle, nullptr, nullptr, &bin_time))
    {
        // We need to compare the timestamp on the .bin and .txt files.
        // This needs to be an exact match to ensure that the .bin file
        // corresponds to this .txt file (and we need to explicitly set
        // this timestamp when creating the .bin file). Just checking
        // for newer modification time is not enough, since the .txt
        // files in the zip files that fixes are distributed in contain
        // a timestamp that may be older than .bin files generated on
        // an end-users system.
        if (CompareFileTime(&bin_time, &txt_time))
            return false;

        // It no longer matters which timestamp we save for later
        // comparison, since they need to match, but we save the .txt
        // file's timestamp since that is the one we are comparing
        // against later.
        time_stamp = txt_time;
        return true;
    }

    // If we couldn't get the timestamps it probably means the
    // corresponding .txt file no longer exists. This is actually a bit of
    // an odd (but not impossible) situation to be in - if a user used
    // uninstall.bat when updating a fix they should have removed any stale
    // .bin files as well, and if they didn't use uninstall.bat then they
    // should only be adding new files... so how did a shader that used to
    // be present disappear but leave the cache next to it?
    //
    // A shaderhacker might hit this if they removed the .txt file but not
    // the .bin file, but we could consider that to be user error, so it's
    // not clear any policy here would be correct. Alternatively, a fix
    // might have been shipped with only .bin files - historically we have
    // allowed (but discouraged) that scenario, so for now we issue a
    // warning but allow it.
    LOG_INFO("    WARNING: Unable to validate timestamp of %S - no corresponding .txt file?\n", bin_path);
    return true;
}

static bool load_cached_shader(
    wchar_t*       bin_path,
    const wchar_t* shader_type,
    char*&         code,
    SIZE_T&        code_size,
    string&        shader_model,
    FILETIME&      time_stamp)
{
    HANDLE f;
    DWORD  file_size, read_size;

    f = CreateFile(bin_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;

    if (!check_cache_timestamp(f, bin_path, time_stamp))
    {
        LOG_INFO_W(L"    Discarding stale cached shader: %s\n", bin_path);
        goto bail_close_handle;
    }

    LOG_INFO_W(L"    Replacement binary shader found: %s\n", bin_path);
    warn_if_conflicting_shader_exists(bin_path, end_user_conflicting_shader_msg);

    file_size = GetFileSize(f, nullptr);
    code      = new char[file_size];
    if (!ReadFile(f, code, file_size, &read_size, nullptr) || file_size != read_size)
    {
        LOG_INFO("    Error reading binary file.\n");
        goto err_free_code;
    }

    code_size = file_size;
    LOG_INFO("    Bytecode loaded. Size = %Iu\n", code_size);
    CloseHandle(f);

    shader_model = "bin";  // tag it as reload candidate, but needing disassemble

    return true;

err_free_code:
    delete[] code;
    code = nullptr;
bail_close_handle:
    CloseHandle(f);
    return false;
}

// Load .bin shaders from the ShaderFixes folder as cached shaders.
// This will load either *_replace.bin, or *.bin variants.

static bool load_binary_shaders(
    UINT64         hash,
    const wchar_t* shader_type,
    char*&         code,
    SIZE_T&        code_size,
    string&        shader_model,
    FILETIME&      time_stamp)
{
    wchar_t path[MAX_PATH];

    swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_replace.bin", G->SHADER_PATH, hash, shader_type);
    if (load_cached_shader(path, shader_type, code, code_size, shader_model, time_stamp))
        return true;

    // If we can't find an HLSL compiled version, look for ASM assembled one.
    swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls.bin", G->SHADER_PATH, hash, shader_type);
    return load_cached_shader(path, shader_type, code, code_size, shader_model, time_stamp);
}

// Load an HLSL text file as the replacement shader.  Recompile it using D3DCompile.
// If caching is enabled, save a .bin replacement for this new shader.

static bool replace_hlsl_shader(
    UINT64         hash,
    const wchar_t* shader_type,
    const void*    shader_bytecode,
    SIZE_T         bytecode_length,
    const char*    override_shader_model,
    char*&         code,
    SIZE_T&        code_size,
    string&        shader_model,
    FILETIME&      time_stamp,
    wstring&       header_line)
{
    wchar_t path[MAX_PATH];
    HANDLE  f;
    string  asm_shader_model;

    swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_replace.txt", G->SHADER_PATH, hash, shader_type);
    f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        LOG_INFO("    Replacement shader found. Loading replacement HLSL code.\n");
        warn_if_conflicting_shader_exists(path, end_user_conflicting_shader_msg);

        DWORD    src_data_size = GetFileSize(f, nullptr);
        char*    src_data      = new char[src_data_size];
        DWORD    read_size;
        FILETIME ft_write;
        if (!ReadFile(f, src_data, src_data_size, &read_size, nullptr) || !GetFileTime(f, nullptr, nullptr, &ft_write) || src_data_size != read_size)
            LOG_INFO("    Error reading file.\n");
        CloseHandle(f);
        LOG_INFO("    Source code loaded. Size = %d\n", src_data_size);

        // Disassemble old shader to get shader model.
        asm_shader_model = get_shader_model(shader_bytecode, bytecode_length);
        if (asm_shader_model.empty())
        {
            LOG_INFO("    disassembly of original shader failed.\n");

            delete[] src_data;
        }
        else
        {
            // Any HLSL compiled shaders are reloading candidates, if moved to ShaderFixes
            shader_model = asm_shader_model;
            time_stamp   = ft_write;
            std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> utf8_to_utf16;
            header_line = utf8_to_utf16.from_bytes(src_data, strchr(src_data, '\n'));

            // Way too many obscure interractions in this function, using another
            // temporary variable to not modify anything already here and reduce
            // the risk of breaking it in some subtle way:
            const char* tmp_shader_model;
            char        apath[MAX_PATH];

            if (override_shader_model)
                tmp_shader_model = override_shader_model;
            else
                tmp_shader_model = asm_shader_model.c_str();

            // Compile replacement.
            LOG_INFO("    compiling replacement HLSL code with shader model %s\n", tmp_shader_model);

            // TODO: Add #defines for StereoParams and IniParams

            ID3DBlob* error_msgs;  // FIXME: This can leak
            ID3DBlob* compiled_output = nullptr;
            // Pass the real filename and use the standard include handler so that
            // #include will work with a relative path from the shader itself.
            // Later we could add a custom include handler to track dependencies so
            // that we can make reloading work better when using includes:
            wcstombs(apath, path, MAX_PATH);
            MigotoIncludeHandler include_handler(apath);
            HRESULT              ret = D3DCompile(src_data, src_data_size, apath, nullptr, G->recursive_include == -1 ? D3D_COMPILE_STANDARD_FILE_INCLUDE : &include_handler, "main", tmp_shader_model, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &compiled_output, &error_msgs);
            delete[] src_data;
            src_data = nullptr;
            if (compiled_output)
            {
                code_size = compiled_output->GetBufferSize();
                code      = new char[code_size];
                memcpy(code, compiled_output->GetBufferPointer(), code_size);
                compiled_output->Release();
                compiled_output = nullptr;
            }

            LOG_INFO("    compile result of replacement HLSL shader: %x\n", ret);

            if (LogFile && error_msgs)
            {
                LPVOID err_msg  = error_msgs->GetBufferPointer();
                SIZE_T err_size = error_msgs->GetBufferSize();
                LOG_INFO("--------------------------------------------- BEGIN ---------------------------------------------\n");
                fwrite(err_msg, 1, err_size - 1, LogFile);
                LOG_INFO("---------------------------------------------- END ----------------------------------------------\n");
                error_msgs->Release();
            }

            // Cache binary replacement.
            if (G->CACHE_SHADERS && code)
            {
                swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_replace.bin", G->SHADER_PATH, hash, shader_type);
                FILE* fw;
                wfopen_ensuring_access(&fw, path, L"wb");
                if (fw)
                {
                    LOG_INFO("    storing compiled shader to %S\n", path);
                    fwrite(code, 1, code_size, fw);
                    fclose(fw);

                    // Set the last modified timestamp on the cached shader to match the
                    // .txt file it is created from, so we can later check its validity:
                    set_file_last_write_time(path, &ft_write);
                }
                else
                {
                    LOG_INFO("    error writing compiled shader to %S\n", path);
                }
            }
        }
    }
    return !!code;
}

// If a matching file exists, load an ASM text shader as a replacement for a shader.
// Reassemble it, and return the binary.
//
// Changing the output of this routine to be simply .bin files. We had some old test
// code for assembler validation, but that just causes confusion.  Retiring the *_reasm.txt
// files as redundant.
// Files are like:
//  cc79d4a79b16b59c-vs.txt  as ASM text
//  cc79d4a79b16b59c-vs.bin  as reassembled binary shader code
//
// Using this naming convention because we already have multiple fixes that use the *-vs.txt format
// to mean ASM text files, and changing all of those seems unnecessary.  This will parallel the use
// of HLSL files like:
//  cc79d4a79b16b59c-vs_replace.txt   as HLSL text
//  cc79d4a79b16b59c-vs_replace.bin   as recompiled binary shader code
//
// So it should be clear by name, what type of file they are.

static bool replace_asm_shader(
    UINT64         hash,
    const wchar_t* shader_type,
    const void*    shader_bytecode,
    SIZE_T         bytecode_length,
    char*&         code,
    SIZE_T&        code_size,
    string&        shader_model,
    FILETIME&      time_stamp,
    wstring&       header_line)
{
    wchar_t path[MAX_PATH];
    HANDLE  f;
    string  asm_shader_model;

    swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls.txt", G->SHADER_PATH, hash, shader_type);
    f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        LOG_INFO("    Replacement ASM shader found. Assembling replacement ASM code.\n");
        warn_if_conflicting_shader_exists(path, end_user_conflicting_shader_msg);

        DWORD        src_data_size = GetFileSize(f, nullptr);
        vector<char> asm_text_bytes(src_data_size);
        DWORD        read_size;
        FILETIME     ft_write;
        if (!ReadFile(f, asm_text_bytes.data(), src_data_size, &read_size, nullptr) || !GetFileTime(f, nullptr, nullptr, &ft_write) || src_data_size != read_size)
            LOG_INFO("    Error reading file.\n");
        CloseHandle(f);
        LOG_INFO("    Asm source code loaded. Size = %d\n", src_data_size);

        // Disassemble old shader to get shader model.
        asm_shader_model = get_shader_model(shader_bytecode, bytecode_length);
        if (asm_shader_model.empty())
        {
            LOG_INFO("    disassembly of original shader failed.\n");
        }
        else
        {
            // Any ASM shaders are reloading candidates, if moved to ShaderFixes
            shader_model = asm_shader_model;
            time_stamp   = ft_write;
            std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> utf8_to_utf16;
            header_line = utf8_to_utf16.from_bytes(asm_text_bytes.data(), strchr(asm_text_bytes.data(), '\n'));

            vector<byte> byte_code(bytecode_length);
            memcpy(byte_code.data(), shader_bytecode, bytecode_length);

            // Re-assemble the ASM text back to binary
            try
            {
                vector<AssemblerParseError> parse_errors;
                byte_code = assemble_flugan_with_optional_signature_parsing(&asm_text_bytes, G->assemble_signature_comments, &byte_code, &parse_errors);

                // Assuming the re-assembly worked, let's make it the active shader code.
                code_size = byte_code.size();
                code      = new char[code_size];
                memcpy(code, byte_code.data(), code_size);

                // Cache binary replacement.
                if (parse_errors.empty())
                {
                    if (G->CACHE_SHADERS && code && parse_errors.empty())
                    {
                        // Write reassembled binary output as a cached shader.
                        FILE* fw;
                        swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls.bin", G->SHADER_PATH, hash, shader_type);
                        wfopen_ensuring_access(&fw, path, L"wb");
                        if (fw)
                        {
                            LOG_INFO_W(L"    storing reassembled binary to %s\n", path);
                            fwrite(byte_code.data(), 1, byte_code.size(), fw);
                            fclose(fw);

                            // Set the last modified timestamp on the cached shader to match the
                            // .txt file it is created from, so we can later check its validity:
                            set_file_last_write_time(path, &ft_write);
                        }
                        else
                        {
                            LOG_INFO_W(L"    error storing reassembled binary to %s\n", path);
                        }
                    }
                }
                else
                {
                    // Parse errors are currently being treated as non-fatal on
                    // creation time replacement and ShaderRegex for backwards
                    // compatibility (live shader reload is fatal).
                    for (auto& parse_error : parse_errors)
                        log_overlay(log::notice, "%S: %s\n", path, parse_error.what());

                    // Do not record the timestamp so that F10 will reload the
                    // shader even if not touched in the meantime allowing the
                    // shaderhackers to see their bugs. For much the same
                    // reason we disable caching these shaders above (though
                    // that is not retrospective if a cache already existed).
                    time_stamp = {};
                }
            }
            catch (const std::exception& e)
            {
                log_overlay(log::warning, "Error assembling %S: %s\n", path, e.what());
            }
        }
    }

    return !!code;
}

static bool decompile_and_possibly_patch_shader(
    UINT64         hash,
    const void*    shader_bytecode,
    SIZE_T         bytecode_length,
    char*&         code,
    SIZE_T&        code_size,
    string&        shader_model,
    wstring&       header_line,
    const wchar_t* shader_type,
    string&        found_shader_model,
    FILETIME&      time_stamp,
    const char*    override_shader_model)
{
    wchar_t val[MAX_PATH];
    string  asm_text;
    FILE*   fw               = nullptr;
    string  asm_shader_model = "";
    bool    patched          = false;
    bool    error_occurred   = false;
    HRESULT hr;

    if (!G->EXPORT_HLSL && !G->decompiler_settings.fixSvPosition && !G->decompiler_settings.recompileVs)
        return NULL;

    // Skip?
    swprintf_s(val, MAX_PATH, L"%ls\\%016llx-%ls_bad.txt", G->SHADER_PATH, hash, shader_type);
    if (GetFileAttributes(val) != INVALID_FILE_ATTRIBUTES)
    {
        LOG_INFO("    skipping shader marked bad. %S\n", val);
        return NULL;
    }

    // Store HLSL export files in ShaderCache, auto-Fixed shaders in ShaderFixes
    if (G->EXPORT_HLSL >= 1)
        swprintf_s(val, MAX_PATH, L"%ls\\%016llx-%ls_replace.txt", G->SHADER_CACHE_PATH, hash, shader_type);
    else
        swprintf_s(val, MAX_PATH, L"%ls\\%016llx-%ls_replace.txt", G->SHADER_PATH, hash, shader_type);

    // If we can open the file already, it exists, and thus we should skip doing this slow operation again.
    if (GetFileAttributes(val) != INVALID_FILE_ATTRIBUTES)
        return NULL;

    // Disassemble old shader for fixing.
    asm_text = binary_to_asm_text(shader_bytecode, bytecode_length, false);
    if (asm_text.empty())
    {
        LOG_INFO("    disassembly of original shader failed.\n");
        return NULL;
    }

    // Decompile code.
    LOG_INFO("    creating HLSL representation.\n");

    ParseParameters p {};
    p.bytecode                   = shader_bytecode;
    p.decompiled                 = asm_text.c_str();
    p.decompiledSize             = asm_text.size();
    p.ZeroOutput                 = false;
    p.G                          = &G->decompiler_settings;
    const string decompiled_code = DecompileBinaryHLSL(p, patched, asm_shader_model, error_occurred);
    if (!decompiled_code.size() || error_occurred)
    {
        LOG_INFO("    error while decompiling.\n");
        return NULL;
    }

    if ((G->EXPORT_HLSL >= 1) || (G->EXPORT_FIXED && patched))
    {
        errno_t err = wfopen_ensuring_access(&fw, val, L"wb");
        if (err != 0 || !fw)
        {
            LOG_INFO("    !!! Fail to open replace.txt file: 0x%x\n", err);
            return NULL;
        }

        LOG_INFO("    storing patched shader to %S\n", val);
        // Save decompiled HLSL code to that new file.
        fwrite(decompiled_code.c_str(), 1, decompiled_code.size(), fw);

        // Now also write the ASM text to the shader file as a set of comments at the bottom.
        // That will make the ASM code the master reference for fixing shaders, and should be more
        // convenient, especially in light of the numerous decompiler bugs we see.
        if (G->EXPORT_HLSL >= 2)
        {
            fprintf_s(fw, "\n\n/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Original ASM ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
            fwrite(asm_text.c_str(), 1, asm_text.size(), fw);
            fprintf_s(fw, "\n//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/\n");
        }
    }

    // Let's re-compile every time we create a new one, regardless.  Previously this would only re-compile
    // after auto-fixing shaders. This makes shader Decompiler errors more obvious.

    // Way too many obscure interractions in this function, using another
    // temporary variable to not modify anything already here and reduce
    // the risk of breaking it in some subtle way:
    const char* tmp_shader_model;
    char        apath[MAX_PATH];

    if (override_shader_model)
        tmp_shader_model = override_shader_model;
    else
        tmp_shader_model = asm_shader_model.c_str();

    LOG_INFO("    compiling fixed HLSL code with shader model %s, size = %Iu\n", tmp_shader_model, decompiled_code.size());

    // TODO: Add #defines for StereoParams and IniParams

    ID3DBlob* error_msgs      = nullptr;
    ID3DBlob* compiled_output = nullptr;
    // Probably unecessary here since this shader is one we freshly decompiled,
    // but for consistency pass the path here as well so that the standard
    // include handler can correctly handle includes with paths relative to the
    // shader itself:
    wcstombs(apath, val, MAX_PATH);
    hr = D3DCompile(decompiled_code.c_str(), decompiled_code.size(), apath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", tmp_shader_model, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &compiled_output, &error_msgs);
    LOG_INFO("    compile result of fixed HLSL shader: %x\n", hr);

    if (LogFile && error_msgs)
    {
        LPVOID err_msg  = error_msgs->GetBufferPointer();
        SIZE_T err_size = error_msgs->GetBufferSize();
        LOG_INFO("--------------------------------------------- BEGIN ---------------------------------------------\n");
        fwrite(err_msg, 1, err_size - 1, LogFile);
        LOG_INFO("------------------------------------------- HLSL code -------------------------------------------\n");
        fwrite(decompiled_code.c_str(), 1, decompiled_code.size(), LogFile);
        LOG_INFO("\n---------------------------------------------- END ----------------------------------------------\n");

        // And write the errors to the HLSL file as comments too, as a more convenient spot to see them.
        fprintf_s(fw, "\n\n/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~ HLSL errors ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
        fwrite(err_msg, 1, err_size - 1, fw);
        fprintf_s(fw, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/\n");
    }
    if (error_msgs)
        error_msgs->Release();

    // If requested by .ini, also write the newly re-compiled assembly code to the file.  This gives a direct
    // comparison between original ASM, and recompiled ASM.
    if ((G->EXPORT_HLSL >= 3) && compiled_output)
    {
        asm_text = binary_to_asm_text(compiled_output->GetBufferPointer(), compiled_output->GetBufferSize(), G->patch_cb_offsets);
        if (asm_text.empty())
        {
            LOG_INFO("    disassembly of recompiled shader failed.\n");
        }
        else
        {
            fprintf_s(fw, "\n\n/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~ Recompiled ASM ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
            fwrite(asm_text.c_str(), 1, asm_text.size(), fw);
            fprintf_s(fw, "\n//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/\n");
        }
    }

    if (compiled_output)
    {
        // If the shader has been auto-fixed, return it as the live shader.
        // For just caching shaders, we return zero so it won't affect game visuals.
        if (patched)
        {
            code_size = compiled_output->GetBufferSize();
            code      = new char[code_size];
            memcpy(code, compiled_output->GetBufferPointer(), code_size);
        }
        compiled_output->Release();
        compiled_output = nullptr;
    }

    if (fw)
    {
        // Any HLSL compiled shaders are reloading candidates, if moved to ShaderFixes
        FILETIME ft_write;
        GetFileTime(fw, nullptr, nullptr, &ft_write);
        found_shader_model = asm_shader_model;
        time_stamp         = ft_write;

        fclose(fw);
    }

    return !!code;
}

// Fairly bold new strategy here for ReplaceShader.
// This is called at launch to replace any shaders that we might want patched to fix problems.
// It would previously use both ShaderCache, and ShaderFixes both to fix shaders, but this is
// problematic in that broken shaders dumped as part of universal cache could be buggy, and generated
// visual anomolies.  Moreover, we don't really want every file to patched, just the ones we need.

// I'm moving to a model where only stuff in ShaderFixes is active, and stuff in ShaderCache is for reference.
// This will allow us to dump and use the ShaderCache for offline fixes, looking for similar fix patterns, and
// also make them live by moving them to ShaderFixes.
// For auto-fixed shaders- rather than leave them in ShaderCache, when they are fixed, we'll move them into
// ShaderFixes as being live.

// Only used in CreateXXXShader (Vertex, Pixel, Compute, Geometry, Hull, Domain)

// This whole function is in need of major refactoring. At a quick glance I can
// see several code paths that will leak objects, and in general it is far,
// too long and complex - the human brain has between 5 an 9 (typically 7)
// general purpose registers, but this function requires far more than that to
// understand. I've added comments to a few objects that can leak, but there's
// little value in fixing one or two problems without tackling the whole thing,
// but I need to understand it better before I'm willing to start refactoring
// it. -DarkStarSword
//
// Chapter 6 of the Linux coding style guidelines is worth a read:
//   https://www.kernel.org/doc/Documentation/CodingStyle
//
// In general I (bo3b) agree, but would hesitate to apply a C style guide to kinda/sorta
// C++ code.  A sort of mix of the linux guide and C++ is Google's Style Guide:
//   https://google.github.io/styleguide/cppguide.html
// Apparently serious C++ programmers hate it, so that must mean it makes things simpler
// and clearer. We could stand to even have just a style template in VS that would
// make everything consistent at a minimum.  I'd say refactoring this sucker is
// higher value though.
//
// I hate to make a bad thing worse, but I need to return yet another parameter here,
// the string read from the first line of the HLSL file.  This the logical place for
// it because the file is already open and read into memory.

char* HackerDevice::_ReplaceShaderFromShaderFixes(
    UINT64         hash,
    const wchar_t* shader_type,
    const void*    shader_bytecode,
    SIZE_T         bytecode_length,
    SIZE_T&        code_size,
    string&        found_shader_model,
    FILETIME&      time_stamp,
    wstring&       header_line,
    const char*    override_shader_model)
{
    found_shader_model = "";
    time_stamp         = {};

    char* code = nullptr;

    if (!G->SHADER_PATH[0] || !G->SHADER_CACHE_PATH[0])
        return nullptr;

    // Export every original game shader as a .bin file.
    if (G->EXPORT_BINARY)
        export_orig_binary(hash, shader_type, shader_bytecode, bytecode_length);

    // Export every shader seen as an ASM text file.
    if (G->EXPORT_SHADERS)
        create_asm_text_file(G->SHADER_CACHE_PATH, hash, shader_type, shader_bytecode, bytecode_length, G->patch_cb_offsets);

    // Read the binary compiled shaders, as previously cached shaders.  This is how
    // fixes normally ship, so that we just load previously compiled/assembled shaders.
    if (load_binary_shaders(hash, shader_type, code, code_size, found_shader_model, time_stamp))
        return code;

    // Load previously created HLSL shaders, but only from ShaderFixes.
    if (replace_hlsl_shader(hash, shader_type, shader_bytecode, bytecode_length, override_shader_model, code, code_size, found_shader_model, time_stamp, header_line))
    {
        return code;
    }

    // If still not found, look for replacement ASM text shaders.
    if (replace_asm_shader(hash, shader_type, shader_bytecode, bytecode_length, code, code_size, found_shader_model, time_stamp, header_line))
    {
        return code;
    }

    if (decompile_and_possibly_patch_shader(hash, shader_bytecode, bytecode_length, code, code_size, found_shader_model, header_line, shader_type, found_shader_model, time_stamp, override_shader_model))
    {
        return code;
    }

    return nullptr;
}

// This function handles shaders replaced from ShaderFixes at load time with or
// without hunting.
//
// When hunting is disabled we don't save off the original shader unless we
// determine that we need it for depth or partner filtering.  These shaders are
// not candidates for the auto patch engine.
//
// When hunting is enabled we always save off the original shader because the
// answer to "do we need the original?" is "...maybe?"
template <
    class ID3D11Shader,
    HRESULT (__stdcall ID3D11Device::*OrigCreateShader)(
        const void*         pShaderBytecode,
        SIZE_T              BytecodeLength,
        ID3D11ClassLinkage* pClassLinkage,
        ID3D11Shader**      ppShader)>
HRESULT HackerDevice::ReplaceShaderFromShaderFixes(
    UINT64              hash,
    const void*         shader_bytecode,
    SIZE_T              bytecode_length,
    ID3D11ClassLinkage* class_linkage,
    ID3D11Shader**      shader,
    wchar_t*            shader_type)
{
    ShaderOverrideMap::iterator override;
    const char*                 override_shader_model = nullptr;
    SIZE_T                      replace_shader_size;
    string                      shader_model;
    wstring                     header_line;
    FILETIME                    ft_write;
    HRESULT                     hr = E_FAIL;

    // Check if the user has overridden the shader model:
    override = lookup_shaderoverride(hash);
    if (override != G->mShaderOverrideMap.end())
    {
        if (override->second.model[0])
            override_shader_model = override->second.model;
    }

    char* replace_shader = _ReplaceShaderFromShaderFixes(hash, shader_type, shader_bytecode, bytecode_length, replace_shader_size, shader_model, ft_write, header_line, override_shader_model);
    if (!replace_shader)
        return E_FAIL;

    // Create the new shader.
    LOG_DEBUG("    HackerDevice::Create%lsShader.  Device: %p\n", shader_type, this);

    *shader = NULL;  // Appease the static analysis gods
    hr      = (origDevice1->*OrigCreateShader)(replace_shader, replace_shader_size, class_linkage, shader);
    if (FAILED(hr))
    {
        LOG_INFO("    error replacing shader.\n");
        goto out_delete;
    }

    cleanup_shader_maps(*shader);

    LOG_INFO("    shader successfully replaced.\n");

    if (hunting_enabled())
    {
        // Hunting mode:  keep byte_code around for possible replacement or marking
        ID3DBlob* blob;
        hr = D3DCreateBlob(bytecode_length, &blob);
        if (SUCCEEDED(hr))
        {
            // We save the *original* shader bytecode, not the replaced shader,
            // because we will use this in CopyToFixes and ShaderRegex in the
            // event that the shader is deleted.
            memcpy(blob->GetBufferPointer(), shader_bytecode, blob->GetBufferSize());
            ENTER_CRITICAL_SECTION(&G->mCriticalSection);
            {
                register_for_reload(*shader, hash, shader_type, shader_model, class_linkage, blob, ft_write, header_line, false);
            }
            LEAVE_CRITICAL_SECTION(&G->mCriticalSection);
        }
    }

    // FIXME: We have some very similar data structures that we should merge together:
    // mReloadedShaders and originalShader.
    KeepOriginalShader<ID3D11Shader, OrigCreateShader>(hash, shader_type, *shader, shader_bytecode, bytecode_length, class_linkage);

out_delete:
    delete replace_shader;
    return hr;
}

// This function handles shaders that were *NOT* replaced from ShaderFixes
//
// When hunting is disabled we don't save off the original shader unless we
// determine that we need it for for deferred analysis in the auto patch
// engine. These are not candidates for depth or partner filtering since that
// would require a ShaderOverride and a manually patched shader (ok,
// technically we could with an auto patched shader, but those are deprecated
// features - don't encourage them!)
//
// When hunting is enabled we always save off the original shader because the
// answer to "do we need the original?" is "...maybe?"
template <
    class ID3D11Shader,
    HRESULT (__stdcall ID3D11Device::*OrigCreateShader)(
        const void*         pShaderBytecode,
        SIZE_T              BytecodeLength,
        ID3D11ClassLinkage* pClassLinkage,
        ID3D11Shader**      ppShader)>
HRESULT HackerDevice::ProcessShaderNotFoundInShaderFixes(
    UINT64              hash,
    const void*         shader_bytecode,
    SIZE_T              bytecode_length,
    ID3D11ClassLinkage* class_linkage,
    ID3D11Shader**      shader,
    wchar_t*            shader_type)
{
    HRESULT hr;

    *shader = NULL;  // Appease the static analysis gods
    hr      = (origDevice1->*OrigCreateShader)(shader_bytecode, bytecode_length, class_linkage, shader);
    if (FAILED(hr))
        return hr;

    cleanup_shader_maps(*shader);

    // When in hunting mode, make a copy of the original binary, regardless.  This can be replaced, but we'll at least
    // have a copy for every shader seen. If we are performing any sort of deferred shader replacement, such as pipline
    // state analysis we always need to keep a copy of the original bytecode for later analysis. For now the shader
    // regex engine counts as deferred, though that may change with optimisations in the future.
    if (hunting_enabled() || !shader_regex_groups.empty())
    {
        ENTER_CRITICAL_SECTION(&G->mCriticalSection);
        {
            ID3DBlob* blob;
            hr = D3DCreateBlob(bytecode_length, &blob);
            if (SUCCEEDED(hr))
            {
                memcpy(blob->GetBufferPointer(), shader_bytecode, blob->GetBufferSize());
                register_for_reload(*shader, hash, shader_type, "bin", class_linkage, blob, {}, L"", true);

                // Also add the original shader to the original shaders
                // map so that if it is later replaced marking_mode =
                // original and depth buffer filtering will work:
                if (lookup_original_shader(*shader) == end(G->mOriginalShaders))
                {
                    // Since we are both returning *and* storing this we need to
                    // bump the refcount to 2, otherwise it could get freed and we
                    // may get a crash later in RevertMissingShaders, especially
                    // easy to expose with the auto shader patching engine
                    // and reverting shaders:
                    (*shader)->AddRef();
                    G->mOriginalShaders[*shader] = *shader;
                }
            }
        }
        LEAVE_CRITICAL_SECTION(&G->mCriticalSection);
    }

    return hr;
}

bool HackerDevice::NeedOriginalShader(
    UINT64 hash)
{
    shader_override*            shader_override;
    ShaderOverrideMap::iterator i;

    if (hunting_enabled() && (G->marking_mode == MarkingMode::ORIGINAL || G->config_reloadable || G->show_original_enabled))
        return true;

    i = lookup_shaderoverride(hash);
    if (i == G->mShaderOverrideMap.end())
        return false;
    shader_override = &i->second;

    if ((shader_override->depth_filter == DepthBufferFilter::DEPTH_ACTIVE) ||
        (shader_override->depth_filter == DepthBufferFilter::DEPTH_INACTIVE))
    {
        return true;
    }

    if (shader_override->partner_hash)
        return true;

    return false;
}

// This function ensures that a shader handle is expunged from all our shader
// maps. Ideally we would call this when the shader is released, but since we
// don't wrap or hook that call we can't do that. Instead, we call it just
// after any CreateXXXShader call - at that time we know the handle was
// previously invalid and is now valid, but we haven't used it yet.
//
// This is a big hammer, and we could probably cut this down, but I want to
// make very certain that we don't have any other unusual sequences that could
// lead to us using stale entries (e.g. suppose an application called
// XXGetShader() and retrieved a shader 3DMigoto had set, then later called
// XXSetShader() - we would look up that handle, and if that handle had been
// reused we might end up trying to replace it). This fixes an issue where we
// could sometimes mistakingly revert one shader to an unrelated shader on F10:
//
//   https://github.com/bo3b/3Dmigoto/issues/86
//
void cleanup_shader_maps(
    ID3D11DeviceChild* handle)
{
    if (!handle)
        return;

    ENTER_CRITICAL_SECTION(&G->mCriticalSection);
    {
        {
            ShaderMap::iterator i = lookup_shader_hash(handle);
            if (i != G->mShaders.end())
            {
                LOG_INFO("Shader handle %p reused, previous hash was: %016llx\n", handle, i->second);
                G->mShaders.erase(i);
            }
        }

        {
            ShaderReloadMap::iterator i = lookup_reloaded_shader(handle);
            if (i != G->mReloadedShaders.end())
            {
                LOG_INFO("Shader handle %p reused, found in mReloadedShaders\n", handle);
                if (i->second.replacement)
                    i->second.replacement->Release();
                if (i->second.byteCode)
                    i->second.byteCode->Release();
                if (i->second.linkage)
                    i->second.linkage->Release();
                G->mReloadedShaders.erase(i);
            }
        }

        {
            ShaderReplacementMap::iterator i = lookup_original_shader(handle);
            if (i != G->mOriginalShaders.end())
            {
                LOG_INFO("Shader handle %p reused, releasing previous original shader\n", handle);
                i->second->Release();
                G->mOriginalShaders.erase(i);
            }
        }
    }
    LEAVE_CRITICAL_SECTION(&G->mCriticalSection);
}

// Keep the original shader around if it may be needed by a filter in a
// [ShaderOverride] section, or if hunting is enabled and either the
// marking_mode=original, or reload_config support is enabled
template <
    class ID3D11Shader,
    HRESULT (__stdcall ID3D11Device::*OrigCreateShader)(
        const void*         pShaderBytecode,
        SIZE_T              BytecodeLength,
        ID3D11ClassLinkage* pClassLinkage,
        ID3D11Shader**      ppShader)>
void HackerDevice::KeepOriginalShader(
    UINT64              hash,
    wchar_t*            shader_type,
    ID3D11Shader*       shader,
    const void*         shader_bytecode,
    SIZE_T              bytecode_length,
    ID3D11ClassLinkage* class_linkage)
{
    ID3D11Shader* original_shader = nullptr;
    HRESULT       hr;

    if (!NeedOriginalShader(hash))
        return;

    LOG_INFO_W(L"    keeping original shader for filtering: %016llx-%ls\n", hash, shader_type);

    ENTER_CRITICAL_SECTION(&G->mCriticalSection);
    {
        hr = (origDevice1->*OrigCreateShader)(shader_bytecode, bytecode_length, class_linkage, &original_shader);
        cleanup_shader_maps(original_shader);
        if (SUCCEEDED(hr))
            G->mOriginalShaders[shader] = original_shader;

        // Unlike the *other* code path in CreateShader that can also
        // fill out this structure, we do *not* bump the refcount on
        // the originalShader here since we are *only* storing it, not
        // also returning it to the game.
    }
    LEAVE_CRITICAL_SECTION(&G->mCriticalSection);
}

// -----------------------------------------------------------------------------------------------

/*** IUnknown methods ***/

ULONG STDMETHODCALLTYPE HackerDevice::AddRef()
{
    return origDevice1->AddRef();
}

ULONG STDMETHODCALLTYPE HackerDevice::Release()
{
    ULONG ul_ref = origDevice1->Release();
    LOG_DEBUG("HackerDevice::Release counter=%d, this=%p\n", ul_ref, this);

    if (ul_ref <= 0)
    {
        if (!gLogDebug)
            LOG_INFO("HackerDevice::Release counter=%d, this=%p\n", ul_ref, this);
        LOG_INFO("  deleting self\n");

        unregister_hacker_device(this);

        if (stereoHandle)
        {
            int result   = NvAPI_Stereo_DestroyHandle(stereoHandle);
            stereoHandle = nullptr;
            LOG_INFO("  releasing NVAPI stereo handle, result = %d\n", result);
        }
        if (stereoResourceView)
        {
            long result        = stereoResourceView->Release();
            stereoResourceView = nullptr;
            LOG_INFO("  releasing stereo parameters resource view, result = %d\n", result);
        }
        if (stereoTexture)
        {
            long result   = stereoTexture->Release();
            stereoTexture = nullptr;
            LOG_INFO("  releasing stereo texture, result = %d\n", result);
        }
        if (iniResourceView)
        {
            long result     = iniResourceView->Release();
            iniResourceView = nullptr;
            LOG_INFO("  releasing ini parameters resource view, result = %d\n", result);
        }
        if (iniTexture)
        {
            long result = iniTexture->Release();
            iniTexture  = nullptr;
            LOG_INFO("  releasing iniparams texture, result = %d\n", result);
        }
        delete this;
        return 0L;
    }
    return ul_ref;
}

// If called with IDXGIDevice, that's the game trying to access the original DXGIFactory to
// get access to the swap chain.  We need to return a HackerDXGIDevice so that we can get
// access to that swap chain.
//
// This is the 'secret' path to getting the DXGIFactory and thus the swap chain, without
// having to go direct to DXGI calls. As described:
// https://msdn.microsoft.com/en-us/library/windows/desktop/bb174535(v=vs.85).aspx
//
// This technique is used in Mordor for sure, and very likely others.
//
// Next up, it seems that we also need to handle a QueryInterface(IDXGIDevice1), as
// WatchDogs uses that call.  Another oddity: this device is called to return the
// same device. ID3D11Device->QueryInterface(ID3D11Device).  No idea why, but we
// need to return our wrapped version.
//
// 1-4-18: No longer using this technique, we have a direct hook on CreateSwapChain,
// which will catch all variants. But leaving documentation for awhile.

// New addition, we need to also look for QueryInterface casts to different types.
// In Dragon Age, it seems clear that they are upcasting their ID3D11Device to an
// ID3D11Device1, and if we don't wrap that, we have an object leak where they can bypass us.
//
// Initial call needs to be LOG_DEBUG, because this is otherwise far to chatty in the
// log.  That can be kind of misleading, so careful with missing log info. To
// keep it consistent, all normal cases will be LOG_DEBUG, error states are LOG_INFO.

HRESULT STDMETHODCALLTYPE HackerDevice::QueryInterface(
    REFIID riid,
    void** ppvObject)
{
    LOG_DEBUG("HackerDevice::QueryInterface(%s@%p) called with IID: %s\n", type_name(this), this, name_from_IID(riid).c_str());

    if (ppvObject && IsEqualIID(riid, IID_HackerDevice))
    {
        // This is a special case - only 3DMigoto itself should know
        // this IID, so this is us checking if it has a HackerDevice.
        // There's no need to call through to DX for this one.
        AddRef();
        *ppvObject = this;
        return S_OK;
    }

    HRESULT hr = origDevice1->QueryInterface(riid, ppvObject);
    if (FAILED(hr))
    {
        LOG_INFO("  failed result = %x for %p\n", hr, ppvObject);
        return hr;
    }

    // No need for further checks of null ppvObject, as it could not have successfully
    // called the original in that case.

    if (riid == __uuidof(ID3D11Device))
    {
        if (!(G->enable_hooks & EnableHooks::DEVICE))
        {
            // If we are hooking we don't return the wrapped device
            *ppvObject = this;
        }
        LOG_DEBUG("  return HackerDevice(%s@%p) wrapper of %p\n", type_name(this), this, realOrigDevice1);
    }
    else if (riid == __uuidof(ID3D11Device1))
    {
        // Well, bizarrely, this approach to upcasting to a ID3D11Device1 is supported on Win7,
        // but only if you have the 'evil update', the platform update installed.  Since that
        // is an optional update, that certainly means that numerous people do not have it
        // installed. Ergo, a game developer cannot in good faith just assume that it's there,
        // and it's very unlikely they would require it. No performance advantage on Win8.
        // So, that means that a game developer must support a fallback path, even if they
        // actually want Device1 for some reason.
        //
        // Sooo... Current plan is to return an error here, and pretend that the platform
        // update is not installed, or missing feature on Win8.1.  This will force the game
        // to use a more compatible path and make our job easier.
        // This worked in DragonAge, to avoid a crash. Wrapping Device1 also progressed but
        // adds a ton of undesirable complexity, so let's keep it simpler since we don't
        // seem to lose anything. Not features, not performance.
        //
        // Dishonored 2 is the first known game that lacks a fallback
        // and requires the platform update.

        if (!G->enable_platform_update)
        {
            LOG_INFO("  returns E_NOINTERFACE as error for ID3D11Device1 (try allow_platform_update=1 if the game refuses to run).\n");
            *ppvObject = nullptr;
            return E_NOINTERFACE;
        }

        if (!(G->enable_hooks & EnableHooks::DEVICE))
        {
            // If we are hooking we don't return the wrapped device
            *ppvObject = this;
        }
        LOG_DEBUG("  return HackerDevice(%s@%p) wrapper of %p\n", type_name(this), this, realOrigDevice1);
    }

    LOG_DEBUG("  returns result = %x for %p\n", hr, *ppvObject);
    return hr;
}

// -----------------------------------------------------------------------------------------------

/*** ID3D11Device methods ***/

// These are the boilerplate routines that are necessary to pass through any calls to these
// to Direct3D.  Since Direct3D does not have proper objects, we can't rely on super class calls.

HRESULT STDMETHODCALLTYPE HackerDevice::CreateUnorderedAccessView(
    ID3D11Resource*                         pResource,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc,
    ID3D11UnorderedAccessView**             ppUAView)
{
    return origDevice1->CreateUnorderedAccessView(pResource, pDesc, ppUAView);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateRenderTargetView(
    ID3D11Resource*                      pResource,
    const D3D11_RENDER_TARGET_VIEW_DESC* pDesc,
    ID3D11RenderTargetView**             ppRTView)
{
    LOG_DEBUG("HackerDevice::CreateRenderTargetView(%s@%p)\n", type_name(this), this);
    return origDevice1->CreateRenderTargetView(pResource, pDesc, ppRTView);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateDepthStencilView(
    ID3D11Resource*                      pResource,
    const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc,
    ID3D11DepthStencilView**             ppDepthStencilView)
{
    LOG_DEBUG("HackerDevice::CreateDepthStencilView(%s@%p)\n", type_name(this), this);
    return origDevice1->CreateDepthStencilView(pResource, pDesc, ppDepthStencilView);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateInputLayout(
    const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs,
    UINT                            NumElements,
    const void*                     pShaderBytecodeWithInputSignature,
    SIZE_T                          BytecodeLength,
    ID3D11InputLayout**             ppInputLayout)
{
    HRESULT   ret;
    ID3DBlob* blob;

    ret = origDevice1->CreateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout);

    if (hunting_enabled() && SUCCEEDED(ret) && ppInputLayout && *ppInputLayout)
    {
        // When dumping vertex buffers to text file in frame analysis
        // we want to use the input layout to decode the buffer, but
        // DirectX provides no API to query this. So, we store a copy
        // of the input layout in a blob inside the private data of the
        // input layout object. The private data is slow to access, so
        // we should not use this in a hot path, but for frame analysis
        // it doesn't matter. We use a blob to manage releasing the
        // backing memory, since the anonymous void* version of this
        // API does not appear to free the private data on release.

        if (SUCCEEDED(D3DCreateBlob(sizeof(D3D11_INPUT_ELEMENT_DESC) * NumElements, &blob)))
        {
            memcpy(blob->GetBufferPointer(), pInputElementDescs, blob->GetBufferSize());
            (*ppInputLayout)->SetPrivateDataInterface(InputLayoutDescGuid, blob);
            blob->Release();
        }
    }

    return ret;
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateClassLinkage(
    ID3D11ClassLinkage** ppLinkage)
{
    return origDevice1->CreateClassLinkage(ppLinkage);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateBlendState(
    const D3D11_BLEND_DESC* pBlendStateDesc,
    ID3D11BlendState**      ppBlendState)
{
    return origDevice1->CreateBlendState(pBlendStateDesc, ppBlendState);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateDepthStencilState(
    const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc,
    ID3D11DepthStencilState**       ppDepthStencilState)
{
    return origDevice1->CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateSamplerState(
    const D3D11_SAMPLER_DESC* pSamplerDesc,
    ID3D11SamplerState**      ppSamplerState)
{
    return origDevice1->CreateSamplerState(pSamplerDesc, ppSamplerState);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateQuery(
    const D3D11_QUERY_DESC* pQueryDesc,
    ID3D11Query**           ppQuery)
{
    HRESULT hr = origDevice1->CreateQuery(pQueryDesc, ppQuery);
    if (hunting_enabled() && SUCCEEDED(hr) && ppQuery && *ppQuery)
        G->mQueryTypes[*ppQuery] = AsyncQueryType::QUERY;
    return hr;
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreatePredicate(
    const D3D11_QUERY_DESC* pPredicateDesc,
    ID3D11Predicate**       ppPredicate)
{
    HRESULT hr = origDevice1->CreatePredicate(pPredicateDesc, ppPredicate);
    if (hunting_enabled() && SUCCEEDED(hr) && ppPredicate && *ppPredicate)
        G->mQueryTypes[*ppPredicate] = AsyncQueryType::PREDICATE;
    return hr;
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateCounter(
    const D3D11_COUNTER_DESC* pCounterDesc,
    ID3D11Counter**           ppCounter)
{
    HRESULT hr = origDevice1->CreateCounter(pCounterDesc, ppCounter);
    if (hunting_enabled() && SUCCEEDED(hr) && ppCounter && *ppCounter)
        G->mQueryTypes[*ppCounter] = AsyncQueryType::COUNTER;
    return hr;
}

HRESULT STDMETHODCALLTYPE HackerDevice::OpenSharedResource(
    HANDLE hResource,
    REFIID ReturnedInterface,
    void** ppResource)
{
    return origDevice1->OpenSharedResource(hResource, ReturnedInterface, ppResource);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CheckFormatSupport(
    DXGI_FORMAT Format,
    UINT*       pFormatSupport)
{
    return origDevice1->CheckFormatSupport(Format, pFormatSupport);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CheckMultisampleQualityLevels(
    DXGI_FORMAT Format,
    UINT        SampleCount,
    UINT*       pNumQualityLevels)
{
    return origDevice1->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels);
}

void STDMETHODCALLTYPE HackerDevice::CheckCounterInfo(
    D3D11_COUNTER_INFO* pCounterInfo)
{
    return origDevice1->CheckCounterInfo(pCounterInfo);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CheckCounter(
    const D3D11_COUNTER_DESC* pDesc,
    D3D11_COUNTER_TYPE*       pType,
    UINT*                     pActiveCounters,
    LPSTR                     szName,
    UINT*                     pNameLength,
    LPSTR                     szUnits,
    UINT*                     pUnitsLength,
    LPSTR                     szDescription,
    UINT*                     pDescriptionLength)
{
    return origDevice1->CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength, szUnits, pUnitsLength, szDescription, pDescriptionLength);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CheckFeatureSupport(
    D3D11_FEATURE Feature,
    void*         pFeatureSupportData,
    UINT          FeatureSupportDataSize)
{
    return origDevice1->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
}

HRESULT STDMETHODCALLTYPE HackerDevice::GetPrivateData(
    REFGUID guid,
    UINT*   pDataSize,
    void*   pData)
{
    return origDevice1->GetPrivateData(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE HackerDevice::SetPrivateData(
    REFGUID     guid,
    UINT        DataSize,
    const void* pData)
{
    return origDevice1->SetPrivateData(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE HackerDevice::SetPrivateDataInterface(
    REFGUID         guid,
    const IUnknown* pData)
{
    LOG_INFO("HackerDevice::SetPrivateDataInterface(%s@%p) called with IID: %s\n", type_name(this), this, name_from_IID(guid).c_str());

    return origDevice1->SetPrivateDataInterface(guid, pData);
}

// Doesn't seem like any games use this, but might be something we need to
// return only DX11.

D3D_FEATURE_LEVEL STDMETHODCALLTYPE HackerDevice::GetFeatureLevel()
{
    D3D_FEATURE_LEVEL feature_level = origDevice1->GetFeatureLevel();

    LOG_DEBUG("HackerDevice::GetFeatureLevel(%s@%p) returns FeatureLevel:%x\n", type_name(this), this, feature_level);
    return feature_level;
}

UINT STDMETHODCALLTYPE HackerDevice::GetCreationFlags()
{
    return origDevice1->GetCreationFlags();
}

HRESULT STDMETHODCALLTYPE HackerDevice::GetDeviceRemovedReason()
{
    return origDevice1->GetDeviceRemovedReason();
}

HRESULT STDMETHODCALLTYPE HackerDevice::SetExceptionMode(
    UINT RaiseFlags)
{
    return origDevice1->SetExceptionMode(RaiseFlags);
}

UINT STDMETHODCALLTYPE HackerDevice::GetExceptionMode()
{
    return origDevice1->GetExceptionMode();
}

// -----------------------------------------------------------------------------------------------

static bool check_texture_override_iteration(
    texture_override* texture_override)
{
    if (texture_override->iterations.empty())
        return true;

    vector<int>::iterator k                 = texture_override->iterations.begin();
    int                   current_iteration = texture_override->iterations[0] = texture_override->iterations[0] + 1;
    LOG_INFO("  current iteration = %d\n", current_iteration);

    while (++k != texture_override->iterations.end())
    {
        if (current_iteration == *k)
            return true;
    }

    LOG_INFO("  override skipped\n");
    return false;
}

// Only Texture2D surfaces can be square. Use template specialisation to skip
// the check on other resource types:
template <
    typename DescType>
static bool is_square_surface(
    DescType* desc)
{
    return false;
}
static bool is_square_surface(
    D3D11_TEXTURE2D_DESC* desc)
{
    return (desc && G->gSurfaceSquareCreateMode >= 0 && desc->Width == desc->Height && (desc->Usage & D3D11_USAGE_IMMUTABLE) == 0);
}

// Template specialisations to override resource descriptions.
// TODO: Refactor this to use common code with CustomResource.
// TODO: Add overrides for BindFlags since they can affect the stereo mode.
// Maybe MiscFlags as well in case we need to do something like forcing a
// buffer to be unstructured to allow it to be steroised when
// StereoFlagsDX10=0x000C000.

template <
    typename DescType>
static void override_resource_desc_common_2d_3d(
    DescType*         desc,
    texture_override* texture_override)
{
    if (texture_override->format != -1)
    {
        LOG_INFO("  setting custom format to %d\n", texture_override->format);
        desc->Format = static_cast<DXGI_FORMAT>(texture_override->format);
    }

    if (texture_override->width != -1)
    {
        LOG_INFO("  setting custom width to %d\n", texture_override->width);
        desc->Width = texture_override->width;
    }

    if (texture_override->width_multiply != 1.0f)
    {
        desc->Width = static_cast<UINT>(desc->Width * texture_override->width_multiply);
        LOG_INFO("  multiplying custom width by %f to %d\n", texture_override->width_multiply, desc->Width);
    }

    if (texture_override->height != -1)
    {
        LOG_INFO("  setting custom height to %d\n", texture_override->height);
        desc->Height = texture_override->height;
    }

    if (texture_override->height_multiply != 1.0f)
    {
        desc->Height = static_cast<UINT>(desc->Height * texture_override->height_multiply);
        LOG_INFO("  multiplying custom height by %f to %d\n", texture_override->height_multiply, desc->Height);
    }
}

static void override_resource_desc(
    D3D11_BUFFER_DESC* desc,
    texture_override*  texture_override) {}
static void override_resource_desc(
    D3D11_TEXTURE1D_DESC* desc,
    texture_override*     texture_override) {}
static void override_resource_desc(
    D3D11_TEXTURE2D_DESC* desc,
    texture_override*     texture_override)
{
    override_resource_desc_common_2d_3d(desc, texture_override);
}
static void override_resource_desc(
    D3D11_TEXTURE3D_DESC* desc,
    texture_override*     texture_override)
{
    override_resource_desc_common_2d_3d(desc, texture_override);
}

template <
    typename DescType>
static const DescType* process_texture_override(
    uint32_t                        hash,
    StereoHandle                    stereo_handle,
    const DescType*                 orig_desc,
    DescType*                       new_desc,
    NVAPI_STEREO_SURFACECREATEMODE* old_mode)
{
    NVAPI_STEREO_SURFACECREATEMODE new_mode = static_cast<NVAPI_STEREO_SURFACECREATEMODE>(-1);
    TextureOverrideMatches         matches;
    texture_override*              texture_override = nullptr;
    const DescType*                ret              = orig_desc;
    unsigned                       i;

    *old_mode = static_cast<NVAPI_STEREO_SURFACECREATEMODE>(-1);

    // Check for square surfaces. We used to do this after processing the
    // StereoMode in TextureOverrides, but realistically we always want the
    // TextureOverrides to be able to override this since they are more
    // specific, so now we do this first.
    if (is_square_surface(orig_desc))
        new_mode = static_cast<NVAPI_STEREO_SURFACECREATEMODE>(G->gSurfaceSquareCreateMode);

    find_texture_overrides(hash, orig_desc, &matches, NULL);

    if (orig_desc && !matches.empty())
    {
        // There is at least one matching texture override, which means
        // we may possibly be altering the resource description. Make a
        // copy of it and adjust the return pointer to the copy:
        *new_desc = *orig_desc;
        ret       = new_desc;

        // We go through each matching texture override applying any
        // resource description and stereo mode overrides. The texture
        // overrides with higher priorities come later in the list, so
        // if there are any conflicts they will override the earlier
        // lower priority ones.
        for (i = 0; i < matches.size(); i++)
        {
            texture_override = matches[i];

            if (LogFile)
            {
                char buf[256] {};
                StrResourceDesc(buf, 256, orig_desc);
                LOG_INFO("  %S matched resource with hash=%08x %s\n", texture_override->ini_section.c_str(), hash, buf);
            }

            if (!check_texture_override_iteration(texture_override))
                continue;

            if (texture_override->stereoMode != -1)
                new_mode = static_cast<NVAPI_STEREO_SURFACECREATEMODE>(texture_override->stereoMode);

            override_resource_desc(new_desc, texture_override);
        }
    }

    LOCK_RESOURCE_CREATION_MODE();
    {
        if (new_mode != static_cast<NVAPI_STEREO_SURFACECREATEMODE>(-1))
        {
            profiling::NvAPI_Stereo_GetSurfaceCreationMode(stereo_handle, old_mode);
            nvapi_override();
            LOG_INFO("    setting custom surface creation mode %d\n", new_mode);

            if (NVAPI_OK != profiling::NvAPI_Stereo_SetSurfaceCreationMode(stereo_handle, new_mode))
                LOG_INFO("      call failed.\n");
        }
        return ret;
    }
}

static void restore_old_surface_create_mode(
    NVAPI_STEREO_SURFACECREATEMODE old_mode,
    StereoHandle                   stereo_handle)
{
    {
        if (old_mode != static_cast<NVAPI_STEREO_SURFACECREATEMODE>(-1))
        {
            if (NVAPI_OK != profiling::NvAPI_Stereo_SetSurfaceCreationMode(stereo_handle, old_mode))
                LOG_INFO("    restore call failed.\n");
        }
    }
    UNLOCK_RESOURCE_CREATION_MODE();
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateBuffer(
    const D3D11_BUFFER_DESC*      pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Buffer**                ppBuffer)
{
    D3D11_BUFFER_DESC              new_desc;
    const D3D11_BUFFER_DESC*       new_desc_ptr = nullptr;
    NVAPI_STEREO_SURFACECREATEMODE old_mode;

    LOG_DEBUG("HackerDevice::CreateBuffer called\n");
    if (pDesc)
        LogDebugResourceDesc(pDesc);

    // Create hash from the raw buffer data if available, but also include
    // the pDesc data as a unique fingerprint for a buffer.
    uint32_t data_hash = 0, hash = 0;
    if (pInitialData && pInitialData->pSysMem && pDesc)
        hash = data_hash = crc32c_hw(hash, pInitialData->pSysMem, pDesc->ByteWidth);
    if (pDesc)
        hash = crc32c_hw(hash, pDesc, sizeof(D3D11_BUFFER_DESC));

    // Override custom settings?
    new_desc_ptr = process_texture_override(hash, stereoHandle, pDesc, &new_desc, &old_mode);

    HRESULT hr = origDevice1->CreateBuffer(new_desc_ptr, pInitialData, ppBuffer);
    restore_old_surface_create_mode(old_mode, stereoHandle);
    if (hr == S_OK && ppBuffer && *ppBuffer)
    {
        ENTER_CRITICAL_SECTION(&G->mResourcesLock);
        {
            ResourceHandleInfo* handle_info = &G->mResources[*ppBuffer];
            new ResourceReleaseTracker(*ppBuffer);
            handle_info->type      = D3D11_RESOURCE_DIMENSION_BUFFER;
            handle_info->hash      = hash;
            handle_info->orig_hash = hash;
            handle_info->data_hash = data_hash;

            // XXX: This is only used for hash tracking, which we
            // don't enable for buffers for performance reasons:
            // if (pDesc)
            //    memcpy(&handle_info->descBuf, pDesc, sizeof(D3D11_BUFFER_DESC));
        }
        LEAVE_CRITICAL_SECTION(&G->mResourcesLock);
        ENTER_CRITICAL_SECTION(&G->mCriticalSection);
        {
            // For stat collection and hash contamination tracking:
            if (hunting_enabled() && pDesc)
            {
                G->mResourceInfo[hash]                           = *pDesc;
                G->mResourceInfo[hash].initial_data_used_in_hash = !!data_hash;
            }
        }
        LEAVE_CRITICAL_SECTION(&G->mCriticalSection);
    }
    return hr;
}

// -----------------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE HackerDevice::CreateTexture1D(
    const D3D11_TEXTURE1D_DESC*   pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture1D**             ppTexture1D)
{
    D3D11_TEXTURE1D_DESC           new_desc;
    const D3D11_TEXTURE1D_DESC*    new_desc_ptr = nullptr;
    NVAPI_STEREO_SURFACECREATEMODE old_mode;
    uint32_t                       data_hash, hash;

    LOG_DEBUG("HackerDevice::CreateTexture1D called\n");
    if (pDesc)
        LogDebugResourceDesc(pDesc);

    hash = data_hash = CalcTexture1DDataHash(pDesc, pInitialData);
    if (pDesc)
        hash = crc32c_hw(hash, pDesc, sizeof(D3D11_TEXTURE1D_DESC));
    LOG_DEBUG("  InitialData = %p, hash = %08lx\n", pInitialData, hash);

    // Override custom settings?
    new_desc_ptr = process_texture_override(hash, stereoHandle, pDesc, &new_desc, &old_mode);

    HRESULT hr = origDevice1->CreateTexture1D(new_desc_ptr, pInitialData, ppTexture1D);

    restore_old_surface_create_mode(old_mode, stereoHandle);

    if (hr == S_OK && ppTexture1D && *ppTexture1D)
    {
        ENTER_CRITICAL_SECTION(&G->mResourcesLock);
        {
            ResourceHandleInfo* handle_info = &G->mResources[*ppTexture1D];
            new ResourceReleaseTracker(*ppTexture1D);
            handle_info->type      = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
            handle_info->hash      = hash;
            handle_info->orig_hash = hash;
            handle_info->data_hash = data_hash;

            // TODO: For hash tracking if we ever need it for Texture1Ds:
            // if (pDesc)
            //     memcpy(&handle_info->desc1D, pDesc, sizeof(D3D11_TEXTURE1D_DESC));
        }
        LEAVE_CRITICAL_SECTION(&G->mResourcesLock);
        ENTER_CRITICAL_SECTION(&G->mCriticalSection);
        {
            // For stat collection and hash contamination tracking:
            if (hunting_enabled() && pDesc)
            {
                G->mResourceInfo[hash]                           = *pDesc;
                G->mResourceInfo[hash].initial_data_used_in_hash = !!data_hash;
            }
        }
        LEAVE_CRITICAL_SECTION(&G->mCriticalSection);
    }
    return hr;
}

static bool heuristic_could_be_possible_resolution(
    unsigned width,
    unsigned height)
{
    // Exclude very small resolutions:
    if (width < 640 || height < 480)
        return false;

    // Assume square textures are not a resolution, like 3D Vision:
    if (width == height)
        return false;

    // Special case for WATCH_DOGS2 1.09.154 update, which creates 16384 x 4096
    // shadow maps on ultra that are mistaken for the resolution. I don't
    // think that 4 is ever a valid aspect radio, so exclude it:
    if (width == height * 4)
        return false;

    return true;
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateTexture2D(
    const D3D11_TEXTURE2D_DESC*   pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture2D**             ppTexture2D)
{
    D3D11_TEXTURE2D_DESC           new_desc;
    const D3D11_TEXTURE2D_DESC*    new_desc_ptr = nullptr;
    NVAPI_STEREO_SURFACECREATEMODE old_mode;

    LOG_DEBUG("HackerDevice::CreateTexture2D called with parameters\n");
    if (pDesc)
        LogDebugResourceDesc(pDesc);
    if (pInitialData && pInitialData->pSysMem)
    {
        LOG_DEBUG_NO_NL("  pInitialData = %p->%p, SysMemPitch: %u, SysMemSlicePitch: %u ", pInitialData, pInitialData->pSysMem, pInitialData->SysMemPitch, pInitialData->SysMemSlicePitch);
        const uint8_t* hex = static_cast<const uint8_t*>(pInitialData->pSysMem);
        for (size_t i = 0; i < 16; i++)
            LOG_DEBUG_NO_NL(" %02hX", hex[i]);
        LOG_DEBUG("\n");
    }

    // Rectangular depth stencil textures of at least 640x480 may indicate
    // the game's resolution, for games that upscale to their swap chains:
    if (pDesc &&
        (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) &&
        G->mResolutionInfo.from == GetResolutionFrom::DEPTH_STENCIL &&
        heuristic_could_be_possible_resolution(pDesc->Width, pDesc->Height))
    {
        G->mResolutionInfo.width  = pDesc->Width;
        G->mResolutionInfo.height = pDesc->Height;
        LOG_INFO("Got resolution from depth/stencil buffer: %ix%i\n", G->mResolutionInfo.width, G->mResolutionInfo.height);
    }

    // Hash based on raw texture data
    // TODO: Wrap these texture objects and return them to the game.
    //  That would avoid the hash lookup later.

    // We are using both pDesc and pInitialData if it exists.  Even in the
    // pInitialData=0 case, we still need to make a hash, as these are often
    // hashes that are created on the fly, filled in later. So, even though all
    // we have to go on is the easily duplicated pDesc, we'll still use it and
    // accept that we might get collisions.

    // Also, we see duplicate hashes happen, sort-of collisions.  These don't
    // happen because of hash miscalculation, they are literally exactly the
    // same data. Like a fully black texture screen size, shows up multiple times
    // and calculates to same hash.
    // We also see the handle itself get reused. That suggests that maybe we ought
    // to be tracking Release operations as well, and removing them from the map.

    uint32_t data_hash, hash;
    hash = data_hash = CalcTexture2DDataHash(pDesc, pInitialData);
    if (pDesc)
        hash = CalcTexture2DDescHash(hash, pDesc);
    LOG_DEBUG("  InitialData = %p, hash = %08lx\n", pInitialData, hash);

    // Override custom settings?
    new_desc_ptr = process_texture_override(hash, stereoHandle, pDesc, &new_desc, &old_mode);

    // Actual creation:
    HRESULT hr = origDevice1->CreateTexture2D(new_desc_ptr, pInitialData, ppTexture2D);
    restore_old_surface_create_mode(old_mode, stereoHandle);
    if (ppTexture2D)
        LOG_DEBUG("  returns result = %x, handle = %p\n", hr, *ppTexture2D);

    // Register texture. Every one seen.
    if (hr == S_OK && ppTexture2D)
    {
        ENTER_CRITICAL_SECTION(&G->mResourcesLock);
        {
            ResourceHandleInfo* handle_info = &G->mResources[*ppTexture2D];
            new ResourceReleaseTracker(*ppTexture2D);
            handle_info->type      = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
            handle_info->hash      = hash;
            handle_info->orig_hash = hash;
            handle_info->data_hash = data_hash;
            if (pDesc)
                memcpy(&handle_info->desc2D, pDesc, sizeof(D3D11_TEXTURE2D_DESC));
        }
        LEAVE_CRITICAL_SECTION(&G->mResourcesLock);
        ENTER_CRITICAL_SECTION(&G->mCriticalSection);
        {
            if (hunting_enabled() && pDesc)
            {
                G->mResourceInfo[hash]                           = *pDesc;
                G->mResourceInfo[hash].initial_data_used_in_hash = !!data_hash;
            }
        }
        LEAVE_CRITICAL_SECTION(&G->mCriticalSection);
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateTexture3D(
    const D3D11_TEXTURE3D_DESC*   pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture3D**             ppTexture3D)
{
    D3D11_TEXTURE3D_DESC           new_desc;
    const D3D11_TEXTURE3D_DESC*    new_desc_ptr = nullptr;
    NVAPI_STEREO_SURFACECREATEMODE old_mode;

    LOG_INFO("HackerDevice::CreateTexture3D called with parameters\n");
    if (pDesc)
        LogDebugResourceDesc(pDesc);
    if (pInitialData && pInitialData->pSysMem)
    {
        LOG_INFO("  pInitialData = %p->%p, SysMemPitch: %u, SysMemSlicePitch: %u\n", pInitialData, pInitialData->pSysMem, pInitialData->SysMemPitch, pInitialData->SysMemSlicePitch);
    }

    // Rectangular depth stencil textures of at least 640x480 may indicate
    // the game's resolution, for games that upscale to their swap chains:
    if (pDesc && (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) &&
        G->mResolutionInfo.from == GetResolutionFrom::DEPTH_STENCIL &&
        heuristic_could_be_possible_resolution(pDesc->Width, pDesc->Height))
    {
        G->mResolutionInfo.width  = pDesc->Width;
        G->mResolutionInfo.height = pDesc->Height;
        LOG_INFO("Got resolution from depth/stencil buffer: %ix%i\n", G->mResolutionInfo.width, G->mResolutionInfo.height);
    }

    // Create hash code from raw texture data and description.
    // Initial data is optional, so we might have zero data to add to the hash there.
    uint32_t data_hash, hash;
    hash = data_hash = CalcTexture3DDataHash(pDesc, pInitialData);
    if (pDesc)
        hash = CalcTexture3DDescHash(hash, pDesc);
    LOG_INFO("  InitialData = %p, hash = %08lx\n", pInitialData, hash);

    // Override custom settings?
    new_desc_ptr = process_texture_override(hash, stereoHandle, pDesc, &new_desc, &old_mode);

    HRESULT hr = origDevice1->CreateTexture3D(new_desc_ptr, pInitialData, ppTexture3D);

    restore_old_surface_create_mode(old_mode, stereoHandle);

    // Register texture.
    if (hr == S_OK && ppTexture3D)
    {
        ENTER_CRITICAL_SECTION(&G->mResourcesLock);
        {
            ResourceHandleInfo* handle_info = &G->mResources[*ppTexture3D];
            new ResourceReleaseTracker(*ppTexture3D);
            handle_info->type      = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
            handle_info->hash      = hash;
            handle_info->orig_hash = hash;
            handle_info->data_hash = data_hash;
            if (pDesc)
                memcpy(&handle_info->desc3D, pDesc, sizeof(D3D11_TEXTURE3D_DESC));
        }
        LEAVE_CRITICAL_SECTION(&G->mResourcesLock);
        ENTER_CRITICAL_SECTION(&G->mCriticalSection);
        {
            if (hunting_enabled() && pDesc)
            {
                G->mResourceInfo[hash]                           = *pDesc;
                G->mResourceInfo[hash].initial_data_used_in_hash = !!data_hash;
            }
        }
        LEAVE_CRITICAL_SECTION(&G->mCriticalSection);
    }

    LOG_INFO("  returns result = %x\n", hr);

    return hr;
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateShaderResourceView(
    ID3D11Resource*                        pResource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc,
    ID3D11ShaderResourceView**             ppSRView)
{
    LOG_DEBUG("HackerDevice::CreateShaderResourceView called\n");

    HRESULT hr = origDevice1->CreateShaderResourceView(pResource, pDesc, ppSRView);

    // Check for depth buffer view.
    if (hr == S_OK && G->ZBufferHashToInject && ppSRView)
    {
        ENTER_CRITICAL_SECTION(&G->mResourcesLock);
        {
            unordered_map<ID3D11Resource*, ResourceHandleInfo>::iterator i = lookup_resource_handle_info(pResource);
            if (i != G->mResources.end() && i->second.hash == G->ZBufferHashToInject)
            {
                LOG_INFO("  resource view of z buffer found: handle = %p, hash = %08lx\n", *ppSRView, i->second.hash);

                zBufferResourceView = *ppSRView;
            }
        }
        LEAVE_CRITICAL_SECTION(&G->mResourcesLock);
    }

    LOG_DEBUG("  returns result = %x\n", hr);

    return hr;
}

// Whitelist bytecode sections for the bytecode hash. This should include any
// section that clearly makes the shader different from another near identical
// shader such that they are not compatible with one another, such as the
// bytecode itself as well as the input/output/patch constant signatures.
//
// It should not include metadata that might change for a reason other than the
// shader being changed. In particular, it should not include the compiler
// version (in the RDEF section), which may change if the developer upgrades
// their build environment, or debug information that includes the directory on
// the developer's machine that the shader was compiled from (in the SDBG
// section). The STAT section is also intentionally not included because it
// contains nothing useful.
//
// The RDEF section may arguably be useful to compromise between this and a
// hash of the entire shader - it includes the compiler version, which makes it
// a bad idea to hash, BUT it also includes the reflection information such as
// variable names which arguably might be useful to distinguish between
// otherwise identical shaders. However I don't think there is much advantage
// of that over just hashing the full shader, and in some cases we might like
// to ignore variable name changes, so it seems best to skip it.
static char* hash_whitelisted_sections[] = {
    "SHDR", "SHEX",          // Bytecode
    "ISGN", /*   */ "ISG1",  // Input signature
    "PCSG", /*   */ "PSG1",  // Patch constant signature
    "OSGN", "OSG5", "OSG1",  // Output signature
};

static uint32_t hash_shader_bytecode(
    struct dxbc_header* header,
    SIZE_T              bytecode_length)
{
    uint32_t*              offsets = reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(header) + sizeof(struct dxbc_header));
    struct section_header* section;
    unsigned               i, j;
    uint32_t               hash = 0;

    if (bytecode_length < sizeof(struct dxbc_header) + header->num_sections * sizeof(uint32_t))
        return 0;

    for (i = 0; i < header->num_sections; i++)
    {
        section = reinterpret_cast<struct section_header*>(reinterpret_cast<char*>(header) + offsets[i]);
        if (bytecode_length < reinterpret_cast<char*>(section) - reinterpret_cast<char*>(header) + sizeof(struct section_header) + section->size)
            return 0;

        for (j = 0; j < ARRAYSIZE(hash_whitelisted_sections); j++)
        {
            if (!strncmp(section->signature, hash_whitelisted_sections[j], 4))
                hash = crc32c_hw(hash, reinterpret_cast<char*>(section) + sizeof(struct section_header), section->size);
        }
    }

    return hash;
}

static UINT64 hash_shader(
    const void* shader_bytecode,
    SIZE_T      bytecode_length)
{
    UINT64              hash   = 0;
    struct dxbc_header* header = static_cast<struct dxbc_header*>(const_cast<void*>(shader_bytecode));

    if (bytecode_length < sizeof(struct dxbc_header))
        goto fnv;

    switch (G->shader_hash_type)
    {
        case ShaderHashType::FNV:
fnv:
            hash = fnv_64_buf(shader_bytecode, bytecode_length);
            LOG_INFO("       FNV hash = %016I64x\n", hash);
            break;

        case ShaderHashType::EMBEDDED:
            // Confirmed with dx11shaderanalyse that the hash
            // embedded in the file is as md5sum would have printed
            // it (that is - if md5sum used the same obfuscated
            // message size padding), so read it as big-endian so
            // that we print it the same way for consistency.
            //
            // Endian bug: _byteswap_uint64 is unconditional, but I
            // don't want to pull in all of winsock just for ntohl,
            // and since we are only targetting x86... meh.
            hash = _byteswap_uint64(header->hash[0] | static_cast<UINT64>(header->hash[1]) << 32);
            LOG_INFO("  Embedded hash = %016I64x\n", hash);
            break;

        case ShaderHashType::BYTECODE:
            hash = hash_shader_bytecode(header, bytecode_length);
            if (!hash)
                goto fnv;
            LOG_INFO("  Bytecode hash = %016I64x\n", hash);
            break;
    }

    return hash;
}

// C++ function template of common code shared by all CreateXXXShader functions:
template <
    class ID3D11Shader,
    HRESULT (__stdcall ID3D11Device::*OrigCreateShader)(
        const void*         pShaderBytecode,
        SIZE_T              BytecodeLength,
        ID3D11ClassLinkage* pClassLinkage,
        ID3D11Shader**      ppShader)>
HRESULT STDMETHODCALLTYPE HackerDevice::CreateShader(
    const void*         shader_bytecode,
    SIZE_T              bytecode_length,
    ID3D11ClassLinkage* class_linkage,
    ID3D11Shader**      shader,
    wchar_t*            shader_type)
{
    HRESULT hr;
    UINT64  hash;

    if (!shader || !shader_bytecode)
    {
        // Let DX worry about the error code
        return (origDevice1->*OrigCreateShader)(shader_bytecode, bytecode_length, class_linkage, shader);
    }

    // Calculate hash
    hash = hash_shader(shader_bytecode, bytecode_length);

    hr = ReplaceShaderFromShaderFixes<ID3D11Shader, OrigCreateShader>(hash, shader_bytecode, bytecode_length, class_linkage, shader, shader_type);

    if (hr != S_OK)
    {
        hr = ProcessShaderNotFoundInShaderFixes<ID3D11Shader, OrigCreateShader>(hash, shader_bytecode, bytecode_length, class_linkage, shader, shader_type);
    }

    if (hr == S_OK)
    {
        ENTER_CRITICAL_SECTION(&G->mCriticalSection);
        {
            G->mShaders[*shader] = hash;
            LOG_DEBUG_W(L"    %ls: handle = %p, hash = %016I64x\n", shader_type, *shader, hash);
        }
        LEAVE_CRITICAL_SECTION(&G->mCriticalSection);
    }

    LOG_INFO("  returns result = %x, handle = %p\n", hr, *shader);

    return hr;
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateVertexShader(
    const void*          pShaderBytecode,
    SIZE_T               BytecodeLength,
    ID3D11ClassLinkage*  pClassLinkage,
    ID3D11VertexShader** ppVertexShader)
{
    LOG_INFO("HackerDevice::CreateVertexShader called with bytecode_length = %Iu, handle = %p, ClassLinkage = %p\n", BytecodeLength, pShaderBytecode, pClassLinkage);

    return CreateShader<ID3D11VertexShader, &ID3D11Device::CreateVertexShader>(pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader, L"vs");
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateGeometryShader(
    const void*            pShaderBytecode,
    SIZE_T                 BytecodeLength,
    ID3D11ClassLinkage*    pClassLinkage,
    ID3D11GeometryShader** ppGeometryShader)
{
    LOG_INFO("HackerDevice::CreateGeometryShader called with bytecode_length = %Iu, handle = %p\n", BytecodeLength, pShaderBytecode);

    return CreateShader<ID3D11GeometryShader, &ID3D11Device::CreateGeometryShader>(pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader, L"gs");
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateGeometryShaderWithStreamOutput(
    const void*                       pShaderBytecode,
    SIZE_T                            BytecodeLength,
    const D3D11_SO_DECLARATION_ENTRY* pSODeclaration,
    UINT                              NumEntries,
    const UINT*                       pBufferStrides,
    UINT                              NumStrides,
    UINT                              RasterizedStream,
    ID3D11ClassLinkage*               pClassLinkage,
    ID3D11GeometryShader**            ppGeometryShader)
{
    LOG_INFO("HackerDevice::CreateGeometryShaderWithStreamOutput called.\n");

    // TODO: This is another call that can create geometry and/or vertex
    // shaders - hook them up and allow them to be overridden as well.

    HRESULT hr = origDevice1->CreateGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries, pBufferStrides, NumStrides, RasterizedStream, pClassLinkage, ppGeometryShader);
    LOG_INFO("  returns result = %x, handle = %p\n", hr, (ppGeometryShader ? *ppGeometryShader : NULL));

    return hr;
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreatePixelShader(
    const void*         pShaderBytecode,
    SIZE_T              BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    ID3D11PixelShader** ppPixelShader)
{
    LOG_INFO("HackerDevice::CreatePixelShader called with bytecode_length = %Iu, handle = %p, ClassLinkage = %p\n", BytecodeLength, pShaderBytecode, pClassLinkage);

    return CreateShader<ID3D11PixelShader, &ID3D11Device::CreatePixelShader>(pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader, L"ps");
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateHullShader(
    const void*         pShaderBytecode,
    SIZE_T              BytecodeLength,
    ID3D11ClassLinkage* pClassLinkage,
    ID3D11HullShader**  ppHullShader)
{
    LOG_INFO("HackerDevice::CreateHullShader called with bytecode_length = %Iu, handle = %p\n", BytecodeLength, pShaderBytecode);

    return CreateShader<ID3D11HullShader, &ID3D11Device::CreateHullShader>(pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader, L"hs");
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateDomainShader(
    const void*          pShaderBytecode,
    SIZE_T               BytecodeLength,
    ID3D11ClassLinkage*  pClassLinkage,
    ID3D11DomainShader** ppDomainShader)
{
    LOG_INFO("HackerDevice::CreateDomainShader called with bytecode_length = %Iu, handle = %p\n", BytecodeLength, pShaderBytecode);

    return CreateShader<ID3D11DomainShader, &ID3D11Device::CreateDomainShader>(pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader, L"ds");
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateComputeShader(
    const void*           pShaderBytecode,
    SIZE_T                BytecodeLength,
    ID3D11ClassLinkage*   pClassLinkage,
    ID3D11ComputeShader** ppComputeShader)
{
    LOG_INFO("HackerDevice::CreateComputeShader called with bytecode_length = %Iu, handle = %p\n", BytecodeLength, pShaderBytecode);

    return CreateShader<ID3D11ComputeShader, &ID3D11Device::CreateComputeShader>(pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader, L"cs");
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateRasterizerState(
    const D3D11_RASTERIZER_DESC* pRasterizerDesc,
    ID3D11RasterizerState**      ppRasterizerState)
{
    HRESULT hr;

    if (pRasterizerDesc)
        LOG_DEBUG(
            "HackerDevice::CreateRasterizerState called with\n"
            "  FillMode = %d, CullMode = %d, DepthBias = %d, DepthBiasClamp = %f, SlopeScaledDepthBias = %f,\n"
            "  DepthClipEnable = %d, ScissorEnable = %d, MultisampleEnable = %d, AntialiasedLineEnable = %d\n",
            pRasterizerDesc->FillMode, pRasterizerDesc->CullMode, pRasterizerDesc->DepthBias, pRasterizerDesc->DepthBiasClamp,
            pRasterizerDesc->SlopeScaledDepthBias, pRasterizerDesc->DepthClipEnable, pRasterizerDesc->ScissorEnable,
            pRasterizerDesc->MultisampleEnable, pRasterizerDesc->AntialiasedLineEnable);

    if (G->SCISSOR_DISABLE && pRasterizerDesc && pRasterizerDesc->ScissorEnable)
    {
        LOG_DEBUG("  disabling scissor mode.\n");
        const_cast<D3D11_RASTERIZER_DESC*>(pRasterizerDesc)->ScissorEnable = FALSE;
    }

    hr = origDevice1->CreateRasterizerState(pRasterizerDesc, ppRasterizerState);

    LOG_DEBUG("  returns result = %x\n", hr);
    return hr;
}

// This method creates a Context, and we want to return a wrapped/hacker
// version as the result. The method signature requires an
// ID3D11DeviceContext, but we return our HackerContext.

// A deferred context is for multithreading part of the drawing.

HRESULT STDMETHODCALLTYPE HackerDevice::CreateDeferredContext(
    UINT                  ContextFlags,
    ID3D11DeviceContext** ppDeferredContext)
{
    LOG_INFO("HackerDevice::CreateDeferredContext(%s@%p) called with flags = %#x, ptr:%p\n", type_name(this), this, ContextFlags, ppDeferredContext);

    HRESULT hr = origDevice1->CreateDeferredContext(ContextFlags, ppDeferredContext);
    if (FAILED(hr))
    {
        LOG_INFO("  failed result = %x for %p\n", hr, ppDeferredContext);
        return hr;
    }

    if (ppDeferredContext)
    {
        analyse_iunknown(*ppDeferredContext);
        ID3D11DeviceContext1* orig_context1;
        HRESULT               res = (*ppDeferredContext)->QueryInterface(IID_PPV_ARGS(&orig_context1));
        if (FAILED(res))
            orig_context1 = dynamic_cast<ID3D11DeviceContext1*>(*ppDeferredContext);
        HackerContext* hacker_context = HackerContextFactory(realOrigDevice1, orig_context1);
        hacker_context->SetHackerDevice(this);
        hacker_context->Bind3DMigotoResources();

        if (G->enable_hooks & EnableHooks::DEFERRED_CONTEXTS)
            hacker_context->HookContext();
        else
            *ppDeferredContext = hacker_context;

        LOG_INFO("  created HackerContext(%s@%p) wrapper of %p\n", type_name(hacker_context), hacker_context, orig_context1);
    }

    LOG_INFO("  returns result = %x for %p\n", hr, *ppDeferredContext);
    return hr;
}

// A variant where we want to return a HackerContext instead of the
// real one.  Creating a new HackerContext is not correct here, because we
// need to provide the one created originally with the device.

// This is a main way to get the context when you only have the device.
// There is only one immediate context per device, so if they are requesting
// it, we need to return them the HackerContext.
//
// It is apparently possible for poorly written games to call this function
// with null as the ppImmediateContext. This not an optional parameter, and
// that call makes no sense, but apparently happens if they pass null to
// CreateDeviceAndSwapChain for ImmediateContext.  A bug in an older SDK.
// WatchDogs seems to do this.
//
// Also worth noting here is that by not calling through to GetImmediateContext
// we did not properly account for references.
// "The GetImmediateContext method increments the reference count of the immediate context by one. "
//
// Fairly common to see this called all the time, so switching to LOG_DEBUG for
// this as a way to trim down normal log.

void STDMETHODCALLTYPE HackerDevice::GetImmediateContext(
    ID3D11DeviceContext** ppImmediateContext)
{
    LOG_DEBUG("HackerDevice::GetImmediateContext(%s@%p) called with:%p\n", type_name(this), this, ppImmediateContext);

    if (ppImmediateContext == nullptr)
    {
        LOG_INFO("  *** no return possible, nullptr input.\n");
        return;
    }

    // XXX: We might need to add locking here if one thread can call
    // GetImmediateContext() while another calls Release on the same
    // immediate context. Thought this might have been necessary to
    // eliminate a race in Far Cry 4, but that turned out to be due to the
    // HackerContext not having a link back to the HackerDevice, and the
    // same device was not being accessed from multiple threads so there
    // was no race.

    // We still need to call the original function to make sure the reference counts are correct:
    origDevice1->GetImmediateContext(ppImmediateContext);

    // we can arrive here with no hackerContext created if one was not
    // requested from CreateDevice/CreateDeviceFromSwapChain. In that case
    // we need to wrap the immediate context now:
    if (hackerContext == nullptr)
    {
        LOG_INFO("*** HackerContext missing at HackerDevice::GetImmediateContext\n");

        analyse_iunknown(*ppImmediateContext);

        ID3D11DeviceContext1* orig_context1;
        HRESULT               res = (*ppImmediateContext)->QueryInterface(IID_PPV_ARGS(&orig_context1));
        if (FAILED(res))
            orig_context1 = dynamic_cast<ID3D11DeviceContext1*>(*ppImmediateContext);
        hackerContext = HackerContextFactory(realOrigDevice1, orig_context1);
        hackerContext->SetHackerDevice(this);
        hackerContext->Bind3DMigotoResources();
        if (!G->constants_run)
            hackerContext->InitIniParams();
        if (G->enable_hooks & EnableHooks::IMMEDIATE_CONTEXT)
            hackerContext->HookContext();
        LOG_INFO("  HackerContext %p created to wrap %p\n", hackerContext, *ppImmediateContext);
    }
    else if (hackerContext->GetPossiblyHookedOrigContext1() != *ppImmediateContext)
    {
        LOG_INFO("WARNING: hackerContext %p found to be wrapping %p instead of %p at HackerDevice::GetImmediateContext!\n", hackerContext, hackerContext->GetPossiblyHookedOrigContext1(), *ppImmediateContext);
    }

    if (!(G->enable_hooks & EnableHooks::IMMEDIATE_CONTEXT))
        *ppImmediateContext = hackerContext;
    LOG_DEBUG("  returns handle = %p\n", *ppImmediateContext);
}

// -----------------------------------------------------------------------------

/*** ID3D11Device1 methods ***/

// -----------------------------------------------------------------------------
// HackerDevice1 methods.  Requires Win7 Platform Update
//
// This object requires implementation of every single method in the object
// hierarchy ID3D11Device1->ID3D11Device->IUnknown
//
// Everything outside of the methods directly related to the ID3D11Device1
// will call through to the HackerDevice object using the local reference
// as composition, instead of inheritance.  We cannot use inheritance, because
// the vtable needs to remain exactly as defined by COM.

// Follow the lead for GetImmediateContext and return the wrapped version.

void STDMETHODCALLTYPE HackerDevice::GetImmediateContext1(
    ID3D11DeviceContext1** ppImmediateContext)
{
    LOG_INFO("HackerDevice::GetImmediateContext1(%s@%p) called with:%p\n", type_name(this), this, ppImmediateContext);

    if (ppImmediateContext == nullptr)
    {
        LOG_INFO("  *** no return possible, nullptr input.\n");
        return;
    }

    // We still need to call the original function to make sure the reference counts are correct:
    origDevice1->GetImmediateContext1(ppImmediateContext);

    // we can arrive here with no hackerContext created if one was not
    // requested from CreateDevice/CreateDeviceFromSwapChain. In that case
    // we need to wrap the immediate context now:
    if (hackerContext == nullptr)
    {
        LOG_INFO("*** HackerContext1 missing at HackerDevice::GetImmediateContext1\n");

        analyse_iunknown(*ppImmediateContext);

        hackerContext = HackerContextFactory(origDevice1, *ppImmediateContext);
        hackerContext->SetHackerDevice(this);
        LOG_INFO("  hackerContext %p created to wrap %p\n", hackerContext, *ppImmediateContext);
    }
    else if (hackerContext->GetPossiblyHookedOrigContext1() != *ppImmediateContext)
    {
        LOG_INFO("WARNING: hackerContext %p found to be wrapping %p instead of %p at HackerDevice::GetImmediateContext1!\n", hackerContext, hackerContext->GetPossiblyHookedOrigContext1(), *ppImmediateContext);
    }

    *ppImmediateContext = reinterpret_cast<ID3D11DeviceContext1*>(hackerContext);
    LOG_INFO("  returns handle = %p\n", *ppImmediateContext);
}

// Now used for platform_update games.  Dishonored2 uses this.
// Updated to follow the lead of CreateDeferredContext.

HRESULT STDMETHODCALLTYPE HackerDevice::CreateDeferredContext1(
    UINT                   ContextFlags,
    ID3D11DeviceContext1** ppDeferredContext)
{
    LOG_INFO("HackerDevice::CreateDeferredContext1(%s@%p) called with flags = %#x, ptr:%p\n", type_name(this), this, ContextFlags, ppDeferredContext);

    HRESULT hr = origDevice1->CreateDeferredContext1(ContextFlags, ppDeferredContext);
    if (FAILED(hr))
    {
        LOG_INFO("  failed result = %x for %p\n", hr, ppDeferredContext);
        return hr;
    }

    if (ppDeferredContext)
    {
        analyse_iunknown(*ppDeferredContext);
        HackerContext* hacker_context = HackerContextFactory(realOrigDevice1, *ppDeferredContext);
        hacker_context->SetHackerDevice(this);
        hacker_context->Bind3DMigotoResources();

        if (G->enable_hooks & EnableHooks::DEFERRED_CONTEXTS)
            hacker_context->HookContext();
        else
            *ppDeferredContext = hacker_context;

        LOG_INFO("  created HackerContext(%s@%p) wrapper of %p\n", type_name(hacker_context), hacker_context, *ppDeferredContext);
    }

    LOG_INFO("  returns result = %x for %p\n", hr, *ppDeferredContext);
    return hr;
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateBlendState1(
    const D3D11_BLEND_DESC1* pBlendStateDesc,
    ID3D11BlendState1**      ppBlendState)
{
    return origDevice1->CreateBlendState1(pBlendStateDesc, ppBlendState);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateRasterizerState1(
    const D3D11_RASTERIZER_DESC1* pRasterizerDesc,
    ID3D11RasterizerState1**      ppRasterizerState)
{
    return origDevice1->CreateRasterizerState1(pRasterizerDesc, ppRasterizerState);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateDeviceContextState(
    UINT                     Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT                     FeatureLevels,
    UINT                     SDKVersion,
    REFIID                   EmulatedInterface,
    D3D_FEATURE_LEVEL*       pChosenFeatureLevel,
    ID3DDeviceContextState** ppContextState)
{
    return origDevice1->CreateDeviceContextState(Flags, pFeatureLevels, FeatureLevels, SDKVersion, EmulatedInterface, pChosenFeatureLevel, ppContextState);
}

HRESULT STDMETHODCALLTYPE HackerDevice::OpenSharedResource1(
    HANDLE hResource,
    REFIID returnedInterface,
    void** ppResource)
{
    return origDevice1->OpenSharedResource1(hResource, returnedInterface, ppResource);
}

HRESULT STDMETHODCALLTYPE HackerDevice::OpenSharedResourceByName(
    LPCWSTR lpName,
    DWORD   dwDesiredAccess,
    REFIID  returnedInterface,
    void**  ppResource)
{
    return origDevice1->OpenSharedResourceByName(lpName, dwDesiredAccess, returnedInterface, ppResource);
}
