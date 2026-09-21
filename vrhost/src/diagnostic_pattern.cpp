#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr const char* BuildId = "R16-openxr-direct-diagnostic-20260915";
    constexpr ULONGLONG PhaseDurationMs = 5000;
    std::atomic<bool> KeepRunning{true};
    std::ofstream LogFile;

    void Log(const std::string& text)
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::ostringstream line;
        line << std::setfill('0')
             << "[" << std::setw(2) << st.wHour << ":"
             << std::setw(2) << st.wMinute << ":"
             << std::setw(2) << st.wSecond << "."
             << std::setw(3) << st.wMilliseconds << "] "
             << text;
        std::cout << line.str() << std::endl;
        if (LogFile)
        {
            LogFile << line.str() << std::endl;
            LogFile.flush();
        }
    }

    std::string XrCode(XrResult result)
    {
        return std::to_string(static_cast<long long>(result));
    }

    void CheckXr(XrResult result, const char* what)
    {
        if (XR_FAILED(result))
            throw std::runtime_error(std::string(what) + " failed XrResult=" + XrCode(result));
    }

    void CheckHr(HRESULT result, const char* what)
    {
        if (FAILED(result))
        {
            std::ostringstream ss;
            ss << what << " failed HRESULT=0x" << std::hex
               << static_cast<std::uint32_t>(result);
            throw std::runtime_error(ss.str());
        }
    }

    BOOL WINAPI ConsoleHandler(DWORD type)
    {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
            type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT)
        {
            KeepRunning.store(false);
            return TRUE;
        }
        return FALSE;
    }

    template <typename T>
    void ReleaseCom(T*& value)
    {
        if (value)
        {
            value->Release();
            value = nullptr;
        }
    }

    bool SameLuid(const LUID& a, const LUID& b)
    {
        return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
    }

    struct D3DState
    {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;

        D3DState() = default;
        D3DState(const D3DState&) = delete;
        D3DState& operator=(const D3DState&) = delete;

        D3DState(D3DState&& other) noexcept
            : device(other.device), context(other.context)
        {
            other.device = nullptr;
            other.context = nullptr;
        }

        D3DState& operator=(D3DState&& other) noexcept
        {
            if (this != &other)
            {
                ReleaseCom(context);
                ReleaseCom(device);
                device = other.device;
                context = other.context;
                other.device = nullptr;
                other.context = nullptr;
            }
            return *this;
        }

        ~D3DState()
        {
            ReleaseCom(context);
            ReleaseCom(device);
        }
    };

    D3DState CreateD3D(const XrGraphicsRequirementsD3D11KHR& req)
    {
        IDXGIFactory1* factory = nullptr;
        CheckHr(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
            reinterpret_cast<void**>(&factory)), "CreateDXGIFactory1");

        IDXGIAdapter1* match = nullptr;
        DXGI_ADAPTER_DESC1 matchDesc{};
        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (SameLuid(desc.AdapterLuid, req.adapterLuid))
            {
                match = adapter;
                matchDesc = desc;
                break;
            }
            adapter->Release();
        }
        factory->Release();

        if (!match)
            throw std::runtime_error("OpenXR-required D3D11 adapter not found");

        std::wostringstream adapterName;
        adapterName << L"[diag] OpenXR adapter=" << matchDesc.Description;
        std::wcout << adapterName.str() << std::endl;
        if (LogFile)
        {
            std::string narrow(matchDesc.Description,
                matchDesc.Description + wcslen(matchDesc.Description));
            Log("[diag] OpenXR adapter=" + narrow);
        }

        const std::array<D3D_FEATURE_LEVEL, 7> all{
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3
        };
        std::vector<D3D_FEATURE_LEVEL> levels;
        for (auto level : all)
            if (level >= req.minFeatureLevel)
                levels.push_back(level);
        if (levels.empty())
            levels.push_back(req.minFeatureLevel);

        D3DState out;
        D3D_FEATURE_LEVEL selected{};
        const HRESULT hr = D3D11CreateDevice(
            match, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels.data(), static_cast<UINT>(levels.size()),
            D3D11_SDK_VERSION, &out.device, &selected, &out.context);
        match->Release();
        CheckHr(hr, "D3D11CreateDevice");

        Log("[diag] D3D11 device created featureLevel=0x" +
            [&] {
                std::ostringstream ss;
                ss << std::hex << static_cast<unsigned>(selected);
                return ss.str();
            }());
        return out;
    }

    struct Swapchain
    {
        std::string name;
        XrSwapchain handle = XR_NULL_HANDLE;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t arraySize = 1;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::vector<XrSwapchainImageD3D11KHR> images;
        std::vector<std::array<ID3D11RenderTargetView*, 2>> rtvs;

        void Destroy()
        {
            for (auto& pair : rtvs)
            {
                ReleaseCom(pair[0]);
                ReleaseCom(pair[1]);
            }
            rtvs.clear();
            images.clear();
            if (handle != XR_NULL_HANDLE)
            {
                xrDestroySwapchain(handle);
                handle = XR_NULL_HANDLE;
            }
        }

        ~Swapchain()
        {
            Destroy();
        }
    };

    bool ContainsFormat(const std::vector<std::int64_t>& formats, DXGI_FORMAT format)
    {
        return std::find(formats.begin(), formats.end(),
            static_cast<std::int64_t>(format)) != formats.end();
    }

    DXGI_FORMAT ChooseUnormFormat(const std::vector<std::int64_t>& formats)
    {
        for (DXGI_FORMAT f : {
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM })
        {
            if (ContainsFormat(formats, f))
                return f;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    DXGI_FORMAT ChooseSrgbFormat(const std::vector<std::int64_t>& formats)
    {
        for (DXGI_FORMAT f : {
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB })
        {
            if (ContainsFormat(formats, f))
                return f;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    bool CreateSwapchain(XrSession session, ID3D11Device* device,
        Swapchain& out, const char* name, std::uint32_t width,
        std::uint32_t height, std::uint32_t arraySize, DXGI_FORMAT format)
    {
        out.name = name;
        out.width = width;
        out.height = height;
        out.arraySize = arraySize;
        out.format = format;

        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = static_cast<std::int64_t>(format);
        ci.sampleCount = 1;
        ci.width = width;
        ci.height = height;
        ci.faceCount = 1;
        ci.arraySize = arraySize;
        ci.mipCount = 1;

        XrResult xr = xrCreateSwapchain(session, &ci, &out.handle);
        if (XR_FAILED(xr))
        {
            Log("[diag] " + out.name + " xrCreateSwapchain FAILED result=" + XrCode(xr));
            out.handle = XR_NULL_HANDLE;
            return false;
        }

        std::uint32_t count = 0;
        xr = xrEnumerateSwapchainImages(out.handle, 0, &count, nullptr);
        if (XR_FAILED(xr) || count == 0)
        {
            Log("[diag] " + out.name + " enumerate image count FAILED result=" + XrCode(xr));
            out.Destroy();
            return false;
        }

        out.images.resize(count);
        for (auto& image : out.images)
            image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        xr = xrEnumerateSwapchainImages(
            out.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(out.images.data()));
        if (XR_FAILED(xr))
        {
            Log("[diag] " + out.name + " enumerate images FAILED result=" + XrCode(xr));
            out.Destroy();
            return false;
        }

        out.rtvs.resize(count);
        for (std::uint32_t image = 0; image < count; ++image)
        {
            for (std::uint32_t slice = 0; slice < arraySize; ++slice)
            {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format = format;
                if (arraySize > 1)
                {
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rd.Texture2DArray.MipSlice = 0;
                    rd.Texture2DArray.FirstArraySlice = slice;
                    rd.Texture2DArray.ArraySize = 1;
                }
                else
                {
                    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                    rd.Texture2D.MipSlice = 0;
                }

                HRESULT hr = device->CreateRenderTargetView(
                    out.images[image].texture, &rd, &out.rtvs[image][slice]);
                if (FAILED(hr) || !out.rtvs[image][slice])
                {
                    std::ostringstream ss;
                    ss << "[diag] " << out.name
                       << " CreateRenderTargetView FAILED image=" << image
                       << " slice=" << slice
                       << " hr=0x" << std::hex << static_cast<std::uint32_t>(hr);
                    Log(ss.str());
                    out.Destroy();
                    return false;
                }
            }
        }

        Log("[diag] " + out.name + " ready " +
            std::to_string(width) + "x" + std::to_string(height) +
            " array=" + std::to_string(arraySize) +
            " format=" + std::to_string(static_cast<int>(format)) +
            " images=" + std::to_string(count));
        return true;
    }

    struct RenderResult
    {
        bool ok = false;
        XrResult xr = XR_SUCCESS;
        std::uint32_t imageIndex = 0;
        const char* stage = "none";
    };

    RenderResult ClearSwapchain(Swapchain& swapchain,
        ID3D11DeviceContext* context,
        const std::array<std::array<float, 4>, 2>& colors)
    {
        RenderResult result{};
        if (swapchain.handle == XR_NULL_HANDLE || swapchain.images.empty())
        {
            result.stage = "not-ready";
            return result;
        }

        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        result.stage = "acquire";
        result.xr = xrAcquireSwapchainImage(
            swapchain.handle, &acquire, &result.imageIndex);
        if (XR_FAILED(result.xr))
            return result;

        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        result.stage = "wait";
        result.xr = xrWaitSwapchainImage(swapchain.handle, &wait);
        if (XR_FAILED(result.xr))
        {
            // The image is acquired but was never successfully waited. Do not
            // fabricate a release; stop the diagnostic so the first failure is
            // preserved instead of poisoning all later frames.
            KeepRunning.store(false);
            return result;
        }

        if (result.imageIndex >= swapchain.rtvs.size())
        {
            result.stage = "index";
            result.xr = XR_ERROR_RUNTIME_FAILURE;
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(swapchain.handle, &release);
            KeepRunning.store(false);
            return result;
        }

        const std::uint32_t slices = std::min<std::uint32_t>(swapchain.arraySize, 2);
        for (std::uint32_t slice = 0; slice < slices; ++slice)
            context->ClearRenderTargetView(
                swapchain.rtvs[result.imageIndex][slice], colors[slice].data());
        context->Flush();

        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        result.stage = "release";
        result.xr = xrReleaseSwapchainImage(swapchain.handle, &release);
        if (XR_FAILED(result.xr))
            return result;

        result.ok = true;
        result.stage = "complete";
        return result;
    }

    void LogRenderFailure(const Swapchain& swapchain, const RenderResult& result)
    {
        Log("[diag] " + swapchain.name +
            " render FAILED stage=" + result.stage +
            " result=" + XrCode(result.xr) +
            " image=" + std::to_string(result.imageIndex));
    }

    XrEnvironmentBlendMode ChooseBlendMode(
        XrInstance instance, XrSystemId system)
    {
        std::uint32_t count = 0;
        CheckXr(xrEnumerateEnvironmentBlendModes(
            instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &count, nullptr), "xrEnumerateEnvironmentBlendModes(count)");
        std::vector<XrEnvironmentBlendMode> modes(count);
        CheckXr(xrEnumerateEnvironmentBlendModes(
            instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            count, &count, modes.data()), "xrEnumerateEnvironmentBlendModes(list)");
        for (auto mode : modes)
            if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
                return mode;
        if (modes.empty())
            throw std::runtime_error("runtime reported no environment blend modes");
        return modes.front();
    }

    const char* SessionStateName(XrSessionState state)
    {
        switch (state)
        {
        case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "OTHER";
        }
    }

    const char* PhaseName(int phase)
    {
        switch (phase)
        {
        case 0: return "ARRAY-UNORM projection: LEFT RED / RIGHT GREEN";
        case 1: return "SPLIT-UNORM projection: LEFT BLUE / RIGHT YELLOW";
        case 2: return "VIEW-QUAD-UNORM: MAGENTA";
        case 3: return "ARRAY-SRGB projection: LEFT CYAN / RIGHT ORANGE";
        default: return "unknown";
        }
    }
}

int main()
{
    LogFile.open("outrun-vr-diagnostic.log", std::ios::out | std::ios::trunc);
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    Log(std::string("[diag] build=") + BuildId);
    Log("[diag] No game is required. Put on Quest 3, keep Virtual Desktop connected, then run this EXE.");
    Log("[diag] Four phases repeat every 5 seconds. Press ESC or Ctrl+C to exit.");

    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;

    try
    {
        std::uint32_t extCount = 0;
        CheckXr(xrEnumerateInstanceExtensionProperties(
            nullptr, 0, &extCount, nullptr),
            "xrEnumerateInstanceExtensionProperties(count)");
        std::vector<XrExtensionProperties> extensions(extCount);
        for (auto& ext : extensions)
            ext = {XR_TYPE_EXTENSION_PROPERTIES};
        CheckXr(xrEnumerateInstanceExtensionProperties(
            nullptr, extCount, &extCount, extensions.data()),
            "xrEnumerateInstanceExtensionProperties(list)");

        bool haveD3D11 = false;
        for (const auto& ext : extensions)
            if (std::string(ext.extensionName) ==
                XR_KHR_D3D11_ENABLE_EXTENSION_NAME)
                haveD3D11 = true;
        if (!haveD3D11)
            throw std::runtime_error("runtime lacks XR_KHR_D3D11_enable");

        const char* enabledExtensions[]{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        XrInstanceCreateInfo ii{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ii.applicationInfo.applicationName,
            "OutRun VR Direct Diagnostic");
        ii.applicationInfo.applicationVersion = 16;
        strcpy_s(ii.applicationInfo.engineName, "OutRun2006Tweaks");
        ii.applicationInfo.engineVersion = 16;
        ii.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        ii.enabledExtensionCount = 1;
        ii.enabledExtensionNames = enabledExtensions;
        CheckXr(xrCreateInstance(&ii, &instance), "xrCreateInstance");

        XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
        CheckXr(xrGetInstanceProperties(instance, &props),
            "xrGetInstanceProperties");
        Log("[diag] runtime=" + std::string(props.runtimeName) +
            " version=" +
            std::to_string(XR_VERSION_MAJOR(props.runtimeVersion)) + "." +
            std::to_string(XR_VERSION_MINOR(props.runtimeVersion)) + "." +
            std::to_string(XR_VERSION_PATCH(props.runtimeVersion)));

        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        CheckXr(xrGetSystem(instance, &sgi, &system), "xrGetSystem");

        PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
        CheckXr(xrGetInstanceProcAddr(
            instance, "xrGetD3D11GraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&getReq)),
            "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");
        XrGraphicsRequirementsD3D11KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CheckXr(getReq(instance, system, &requirements),
            "xrGetD3D11GraphicsRequirementsKHR");

        D3DState d3d = CreateD3D(requirements);

        XrGraphicsBindingD3D11KHR binding{
            XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = d3d.device;
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &binding;
        sci.systemId = system;
        CheckXr(xrCreateSession(instance, &sci, &session),
            "xrCreateSession");
        Log("[diag] xrCreateSession OK");

        XrPosef identity{};
        identity.orientation.w = 1.0f;

        XrReferenceSpaceCreateInfo localInfo{
            XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        localInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        localInfo.poseInReferenceSpace = identity;
        CheckXr(xrCreateReferenceSpace(session, &localInfo, &localSpace),
            "xrCreateReferenceSpace(LOCAL)");

        XrReferenceSpaceCreateInfo viewInfo{
            XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        viewInfo.poseInReferenceSpace = identity;
        CheckXr(xrCreateReferenceSpace(session, &viewInfo, &viewSpace),
            "xrCreateReferenceSpace(VIEW)");

        std::uint32_t viewCount = 0;
        CheckXr(xrEnumerateViewConfigurationViews(
            instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &viewCount, nullptr),
            "xrEnumerateViewConfigurationViews(count)");
        if (viewCount < 2)
            throw std::runtime_error("runtime reported fewer than two stereo views");

        std::vector<XrViewConfigurationView> configs(viewCount);
        for (auto& config : configs)
            config = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
        CheckXr(xrEnumerateViewConfigurationViews(
            instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            viewCount, &viewCount, configs.data()),
            "xrEnumerateViewConfigurationViews(list)");
        Log("[diag] recommended per-eye=" +
            std::to_string(configs[0].recommendedImageRectWidth) + "x" +
            std::to_string(configs[0].recommendedImageRectHeight) +
            " / " +
            std::to_string(configs[1].recommendedImageRectWidth) + "x" +
            std::to_string(configs[1].recommendedImageRectHeight));

        std::uint32_t formatCount = 0;
        CheckXr(xrEnumerateSwapchainFormats(
            session, 0, &formatCount, nullptr),
            "xrEnumerateSwapchainFormats(count)");
        std::vector<std::int64_t> formats(formatCount);
        CheckXr(xrEnumerateSwapchainFormats(
            session, formatCount, &formatCount, formats.data()),
            "xrEnumerateSwapchainFormats(list)");

        {
            std::ostringstream ss;
            ss << "[diag] swapchain formats:";
            for (auto format : formats)
                ss << " " << format;
            Log(ss.str());
        }

        DXGI_FORMAT unorm = ChooseUnormFormat(formats);
        DXGI_FORMAT srgb = ChooseSrgbFormat(formats);
        if (unorm == DXGI_FORMAT_UNKNOWN)
            throw std::runtime_error("runtime exposes no R8/B8 8-bit UNORM swapchain");
        Log("[diag] selected UNORM format=" +
            std::to_string(static_cast<int>(unorm)));
        if (srgb != DXGI_FORMAT_UNKNOWN)
            Log("[diag] selected SRGB format=" +
                std::to_string(static_cast<int>(srgb)));
        else
            Log("[diag] SRGB 8-bit format unavailable; phase 4 will reuse UNORM");

        const std::uint32_t eyeW = std::min<std::uint32_t>(
            1024, std::max(configs[0].recommendedImageRectWidth,
                           configs[1].recommendedImageRectWidth));
        const std::uint32_t eyeH = std::min<std::uint32_t>(
            1024, std::max(configs[0].recommendedImageRectHeight,
                           configs[1].recommendedImageRectHeight));

        Swapchain arrayUnorm, splitLeft, splitRight, quadUnorm, arraySrgb;
        if (!CreateSwapchain(session, d3d.device, arrayUnorm,
                "array-unorm", eyeW, eyeH, 2, unorm))
            throw std::runtime_error("array UNORM swapchain creation failed");
        if (!CreateSwapchain(session, d3d.device, splitLeft,
                "split-left-unorm", eyeW, eyeH, 1, unorm) ||
            !CreateSwapchain(session, d3d.device, splitRight,
                "split-right-unorm", eyeW, eyeH, 1, unorm))
            throw std::runtime_error("split UNORM swapchain creation failed");
        if (!CreateSwapchain(session, d3d.device, quadUnorm,
                "quad-unorm", 1024, 512, 1, unorm))
            throw std::runtime_error("quad UNORM swapchain creation failed");
        const DXGI_FORMAT phaseSrgb =
            srgb != DXGI_FORMAT_UNKNOWN ? srgb : unorm;
        if (!CreateSwapchain(session, d3d.device, arraySrgb,
                "array-srgb", eyeW, eyeH, 2, phaseSrgb))
            throw std::runtime_error("array SRGB/fallback swapchain creation failed");

        const XrEnvironmentBlendMode blend =
            ChooseBlendMode(instance, system);

        bool sessionRunning = false;
        bool quit = false;
        XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
        ULONGLONG phaseOrigin = 0;
        int lastPhase = -1;
        std::uint64_t frameNumber = 0;
        std::uint64_t submitSuccess[4]{};
        std::uint64_t renderFailures[4]{};

        while (KeepRunning.load() && !quit)
        {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
                KeepRunning.store(false);

            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            while (xrPollEvent(instance, &event) == XR_SUCCESS)
            {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                {
                    const auto* changed =
                        reinterpret_cast<const XrEventDataSessionStateChanged*>(
                            &event);
                    sessionState = changed->state;
                    Log(std::string("[diag] session state=") +
                        SessionStateName(sessionState));

                    if (sessionState == XR_SESSION_STATE_READY &&
                        !sessionRunning)
                    {
                        XrSessionBeginInfo begin{
                            XR_TYPE_SESSION_BEGIN_INFO};
                        begin.primaryViewConfigurationType =
                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        CheckXr(xrBeginSession(session, &begin),
                            "xrBeginSession");
                        sessionRunning = true;
                        phaseOrigin = GetTickCount64();
                        Log("[diag] xrBeginSession OK");
                    }
                    else if (sessionState == XR_SESSION_STATE_STOPPING &&
                             sessionRunning)
                    {
                        CheckXr(xrEndSession(session), "xrEndSession");
                        sessionRunning = false;
                    }
                    else if (sessionState == XR_SESSION_STATE_EXITING ||
                             sessionState == XR_SESSION_STATE_LOSS_PENDING)
                    {
                        quit = true;
                    }
                }
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }

            if (!sessionRunning)
            {
                Sleep(10);
                continue;
            }

            XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState frameState{XR_TYPE_FRAME_STATE};
            CheckXr(xrWaitFrame(session, &waitInfo, &frameState),
                "xrWaitFrame");

            XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
            CheckXr(xrBeginFrame(session, &beginInfo), "xrBeginFrame");

            const ULONGLONG elapsed =
                GetTickCount64() - phaseOrigin;
            const int phase = static_cast<int>(
                (elapsed / PhaseDurationMs) % 4);
            if (phase != lastPhase)
            {
                lastPhase = phase;
                Log(std::string("[diag] PHASE ") +
                    std::to_string(phase + 1) + "/4 " +
                    PhaseName(phase));
            }

            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = frameState.predictedDisplayTime;
            end.environmentBlendMode = blend;

            const XrCompositionLayerBaseHeader* layer = nullptr;
            XrCompositionLayerProjection projection{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            std::array<XrCompositionLayerProjectionView, 2> projectionViews{};
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};

            bool ready = false;
            if (frameState.shouldRender == XR_TRUE)
            {
                if (phase == 0 || phase == 1 || phase == 3)
                {
                    std::array<XrView, 2> views{};
                    for (auto& view : views)
                        view = {XR_TYPE_VIEW};
                    XrViewState viewState{XR_TYPE_VIEW_STATE};
                    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
                    locate.viewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    locate.displayTime = frameState.predictedDisplayTime;
                    locate.space = localSpace;
                    std::uint32_t located = 0;
                    const XrResult locateResult = xrLocateViews(
                        session, &locate, &viewState, 2, &located, views.data());
                    if (XR_FAILED(locateResult) || located < 2)
                    {
                        Log("[diag] xrLocateViews FAILED result=" +
                            XrCode(locateResult) +
                            " count=" + std::to_string(located));
                        ++renderFailures[phase];
                    }
                    else
                    {
                        RenderResult leftOrArray{};
                        RenderResult right{};
                        if (phase == 0)
                        {
                            leftOrArray = ClearSwapchain(
                                arrayUnorm, d3d.context,
                                {{{1.f, 0.f, 0.f, 1.f},
                                  {0.f, 1.f, 0.f, 1.f}}});
                            if (!leftOrArray.ok)
                                LogRenderFailure(arrayUnorm, leftOrArray);
                            ready = leftOrArray.ok;
                        }
                        else if (phase == 1)
                        {
                            leftOrArray = ClearSwapchain(
                                splitLeft, d3d.context,
                                {{{0.f, 0.15f, 1.f, 1.f},
                                  {0.f, 0.15f, 1.f, 1.f}}});
                            right = ClearSwapchain(
                                splitRight, d3d.context,
                                {{{1.f, 0.9f, 0.f, 1.f},
                                  {1.f, 0.9f, 0.f, 1.f}}});
                            if (!leftOrArray.ok)
                                LogRenderFailure(splitLeft, leftOrArray);
                            if (!right.ok)
                                LogRenderFailure(splitRight, right);
                            ready = leftOrArray.ok && right.ok;
                        }
                        else
                        {
                            leftOrArray = ClearSwapchain(
                                arraySrgb, d3d.context,
                                {{{0.f, 1.f, 1.f, 1.f},
                                  {1.f, 0.25f, 0.f, 1.f}}});
                            if (!leftOrArray.ok)
                                LogRenderFailure(arraySrgb, leftOrArray);
                            ready = leftOrArray.ok;
                        }

                        if (ready)
                        {
                            for (int eye = 0; eye < 2; ++eye)
                            {
                                projectionViews[eye] = {
                                    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                                projectionViews[eye].pose = views[eye].pose;
                                projectionViews[eye].fov = views[eye].fov;
                                if (phase == 1)
                                {
                                    projectionViews[eye].subImage.swapchain =
                                        eye == 0 ? splitLeft.handle :
                                                   splitRight.handle;
                                    projectionViews[eye].subImage.imageArrayIndex = 0;
                                }
                                else
                                {
                                    Swapchain& source =
                                        phase == 0 ? arrayUnorm : arraySrgb;
                                    projectionViews[eye].subImage.swapchain =
                                        source.handle;
                                    projectionViews[eye].subImage.imageArrayIndex =
                                        static_cast<std::uint32_t>(eye);
                                }
                                projectionViews[eye].subImage.imageRect.offset =
                                    {0, 0};
                                projectionViews[eye].subImage.imageRect.extent =
                                    {static_cast<std::int32_t>(eyeW),
                                     static_cast<std::int32_t>(eyeH)};
                            }
                            projection.space = localSpace;
                            projection.viewCount = 2;
                            projection.views = projectionViews.data();
                            layer =
                                reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                                    &projection);
                        }
                        else
                        {
                            ++renderFailures[phase];
                        }
                    }
                }
                else
                {
                    const RenderResult quadResult = ClearSwapchain(
                        quadUnorm, d3d.context,
                        {{{1.f, 0.f, 1.f, 1.f},
                          {1.f, 0.f, 1.f, 1.f}}});
                    if (!quadResult.ok)
                    {
                        LogRenderFailure(quadUnorm, quadResult);
                        ++renderFailures[phase];
                    }
                    else
                    {
                        quad.space = viewSpace;
                        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        quad.pose.orientation = {0.f, 0.f, 0.f, 1.f};
                        quad.pose.position = {0.f, 0.f, -1.5f};
                        quad.size = {1.6f, 0.8f};
                        quad.subImage.swapchain = quadUnorm.handle;
                        quad.subImage.imageRect.offset = {0, 0};
                        quad.subImage.imageRect.extent = {1024, 512};
                        quad.subImage.imageArrayIndex = 0;
                        layer =
                            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                                &quad);
                        ready = true;
                    }
                }
            }

            end.layerCount = ready ? 1u : 0u;
            end.layers = ready ? &layer : nullptr;
            const XrResult endResult = xrEndFrame(session, &end);
            if (XR_FAILED(endResult))
            {
                Log("[diag] xrEndFrame FAILED phase=" +
                    std::to_string(phase + 1) +
                    " result=" + XrCode(endResult) +
                    " layerCount=" + std::to_string(end.layerCount));
                ++renderFailures[phase];
            }
            else if (ready)
            {
                ++submitSuccess[phase];
            }

            ++frameNumber;
            if (frameNumber % 300 == 0)
            {
                Log("[diag] heartbeat frame=" +
                    std::to_string(frameNumber) +
                    " state=" + SessionStateName(sessionState) +
                    " shouldRender=" +
                    std::to_string(frameState.shouldRender == XR_TRUE ? 1 : 0) +
                    " phase=" + std::to_string(phase + 1) +
                    " submitOk=[" +
                    std::to_string(submitSuccess[0]) + "," +
                    std::to_string(submitSuccess[1]) + "," +
                    std::to_string(submitSuccess[2]) + "," +
                    std::to_string(submitSuccess[3]) + "] fail=[" +
                    std::to_string(renderFailures[0]) + "," +
                    std::to_string(renderFailures[1]) + "," +
                    std::to_string(renderFailures[2]) + "," +
                    std::to_string(renderFailures[3]) + "]");
            }
        }

        Log("[diag] stopping. submitOk=[" +
            std::to_string(submitSuccess[0]) + "," +
            std::to_string(submitSuccess[1]) + "," +
            std::to_string(submitSuccess[2]) + "," +
            std::to_string(submitSuccess[3]) + "] fail=[" +
            std::to_string(renderFailures[0]) + "," +
            std::to_string(renderFailures[1]) + "," +
            std::to_string(renderFailures[2]) + "," +
            std::to_string(renderFailures[3]) + "]");

        if (sessionRunning)
        {
            xrRequestExitSession(session);
            for (int i = 0; i < 200 && sessionRunning; ++i)
            {
                XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
                while (xrPollEvent(instance, &event) == XR_SUCCESS)
                {
                    if (event.type ==
                        XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                    {
                        const auto* changed =
                            reinterpret_cast<const XrEventDataSessionStateChanged*>(
                                &event);
                        if (changed->state == XR_SESSION_STATE_STOPPING)
                        {
                            xrEndSession(session);
                            sessionRunning = false;
                            break;
                        }
                    }
                    event = {XR_TYPE_EVENT_DATA_BUFFER};
                }
                Sleep(10);
            }
        }

        arraySrgb.Destroy();
        quadUnorm.Destroy();
        splitRight.Destroy();
        splitLeft.Destroy();
        arrayUnorm.Destroy();

        if (viewSpace != XR_NULL_HANDLE)
            xrDestroySpace(viewSpace);
        if (localSpace != XR_NULL_HANDLE)
            xrDestroySpace(localSpace);
        if (session != XR_NULL_HANDLE)
            xrDestroySession(session);
        if (instance != XR_NULL_HANDLE)
            xrDestroyInstance(instance);

        Log("[diag] clean exit");
        return 0;
    }
    catch (const std::exception& e)
    {
        Log(std::string("[diag] FATAL: ") + e.what());
        if (viewSpace != XR_NULL_HANDLE)
            xrDestroySpace(viewSpace);
        if (localSpace != XR_NULL_HANDLE)
            xrDestroySpace(localSpace);
        if (session != XR_NULL_HANDLE)
            xrDestroySession(session);
        if (instance != XR_NULL_HANDLE)
            xrDestroyInstance(instance);
        return 1;
    }
}
