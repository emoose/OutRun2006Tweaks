// R23 host entry point.
//
// Reuse the validated host implementation types, but replace the frame loop so
// a candidate capture/direct source is never made presentation-authoritative
// until the surrounding Frame.v2 metadata has been re-read and proven stable.
// Pixel diagnostics use reusable non-blocking staging resources and independently
// probe source, left/right projection output, and theater output.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <Windows.h>
#include <TlHelp32.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>
#include "vr_shared.hpp"
#include "stereo_shader.hpp"
#include "runtime/r23_verified_bundle.hpp"
#include "vr/ipc/cadence_v1.hpp"

#ifndef OUTRUN_VR_BUILD_SHA
#define OUTRUN_VR_BUILD_SHA "unknown"
#endif

// main.cpp keeps compositor implementation details private. R23 is an overlay
// TU over that exact implementation and needs access only to stage/commit
// candidate resources without duplicating the whole compositor. All headers are
// already included above, so this visibility override is confined to the class
// definitions in main.cpp and does not leak into STL/Windows headers.
#define private public
#define protected public
#define main OutRunVrLegacyMainR23
#include "main.cpp"
#undef main
#undef protected
#undef private

// sbs_capture_override.hpp used a legacy ReleaseFrame macro while preprocessing
// main.cpp. R23 performs the production publication explicitly below.
#ifdef ReleaseFrame
#undef ReleaseFrame
#endif

namespace
{
    constexpr ULONGLONG R23PixelIntervalMs = 5000;
    constexpr UINT R23SourceSamples = 8;
    constexpr DWORD R23TheaterRefreshWaitMs = 2;
    constexpr ULONGLONG R23TheaterRefreshLogIntervalMs = 5000;
    // OpenXR/VDXR commonly runs at 90 Hz while OutRun produces stereo at
    // roughly 60 Hz. Re-rendering the same Desktop Duplication source into the
    // projection swapchain on every HMD tick wastes GPU work, while falling
    // back to the theater quad exposes the raw SBS image. Keep the last
    // successfully released projection alive for intermediate HMD frames and
    // for short game-state/capture gaps.
    constexpr ULONGLONG R23CachedProjectionHoldMs = 1000;
    constexpr ULONGLONG R23PresentationDebounceMs = 750;
    constexpr ULONGLONG R23CachedProjectionLogIntervalMs = 5000;

    bool R23UsableGameplayBootstrapFrame(
        const OutRunVR::SharedRenderFrameState& frame) noexcept
    {
        constexpr std::uint32_t required =
            OutRunVR::RenderFrameStereoComplete |
            OutRunVR::RenderFrameWorldStereo |
            OutRunVR::RenderFrameDrawDuplicated |
            OutRunVR::RenderFrameEffectivePoseValid;

        if (!frame.frameId || !frame.sourcePoseSequence ||
            frame.presentationMode != OutRunVR::PresentationGameplay ||
            frame.state != OutRunVR::StereoSbsActive ||
            frame.failureReason != OutRunVR::StereoFailureNone ||
            frame.presentQpc <= 0 ||
            !frame.backbufferWidth || !frame.backbufferHeight ||
            (frame.flags & OutRunVR::RenderFramePresentInFlight) != 0 ||
            (frame.flags & required) != required)
            return false;

        if ((frame.flags & OutRunVR::RenderFrameDirectGpuTransport) == 0)
            return true;

        const std::uint32_t slot =
            frame.reserved[OutRunVR::RenderFrameDirectSlotIndex];
        const std::uint32_t generation =
            frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex];
        const std::uint32_t width =
            frame.reserved[OutRunVR::RenderFrameDirectWidthIndex];
        const std::uint32_t height =
            frame.reserved[OutRunVR::RenderFrameDirectHeightIndex];
        const std::uint32_t leftHandle =
            frame.reserved[OutRunVR::RenderFrameDirectLeftHandleIndex];
        const std::uint32_t rightHandle =
            frame.reserved[OutRunVR::RenderFrameDirectRightHandleIndex];

        return slot < OutRunVR::RenderFrameRingSize &&
            generation != 0 &&
            leftHandle != 0 && rightHandle != 0 &&
            width != 0 && height != 0 &&
            width == frame.backbufferWidth &&
            height == frame.backbufferHeight;
    }

    std::uint64_t R23CachedProjectionSubmits = 0;
    std::uint64_t R42SameFrameProjectionReuses = 0;
    std::uint64_t R42ProjectionRefreshes = 0;
    ULONGLONG R23LastCachedProjectionLogMs = 0;
    bool R42FirstProjectionReuseLogged = false;
    std::uint64_t R23PresentationGraceFrames = 0;
    std::uint64_t R23TheaterRefreshAttempts = 0;
    std::uint64_t R23TheaterRefreshFresh = 0;
    ULONGLONG R23LastTheaterRefreshLogMs = 0;
    std::uint64_t R23GameplayTheaterFallbacks = 0;
    ULONGLONG R23LastGameplayFallbackLogMs = 0;


    struct R23FinalCounters
    {
        std::uint64_t r32FastDirect = 0;
        std::uint64_t exact = 0;
        std::uint64_t softGrace = 0;
        std::uint64_t direct = 0;
        std::uint64_t liveTheater = 0;
        std::uint64_t directFlat = 0;
        std::uint64_t cached = 0;
        std::uint64_t emergency = 0;
        std::uint64_t empty = 0;
        std::uint64_t mixed = 0;
    };

    R23FinalCounters R23ReadFinalCounters() noexcept
    {
        using namespace OutRunVrR24BlackScreenGuard;
        return {
            OutRunVrR32DirectSubmit::FastDirectSubmits,
            ExactProjectionSubmits,
            SoftGraceProjectionSubmits,
            DirectSafeProjectionSubmits,
            LiveTheaterFallbacks,
            DirectFlatFallbacks,
            CachedLayerFallbacks,
            EmergencyLayerFallbacks,
            EmptyFrameFallbacks,
            MixedValidatedSubmits
        };
    }

    const char* R23ActualFinalKind(
        const R23FinalCounters& before,
        const R23FinalCounters& after,
        const char* requested) noexcept
    {
        if (after.r32FastDirect != before.r32FastDirect)
            return "projection-direct-r32-fast";
        if (after.exact != before.exact) return "projection-exact";
        if (after.softGrace != before.softGrace) return "projection-soft-grace";
        if (after.direct != before.direct) return "projection-direct-safe";
        if (after.cached != before.cached) return "fallback-cached-image";
        if (after.directFlat != before.directFlat) return "fallback-direct-flat";
        if (after.liveTheater != before.liveTheater) return "fallback-live-theater";
        if (after.emergency != before.emergency) return "fallback-emergency";
        if (after.empty != before.empty) return "no-layer";
        if (after.mixed != before.mixed) return "mixed-validated";
        return requested ? requested : "unclassified";
    }

    struct R23MetricWindow
    {
        std::vector<double> values;
        double sum = 0.0;
        double maximum = 0.0;

        void Add(double value)
        {
            if (!std::isfinite(value) || value < 0.0)
                return;
            values.push_back(value);
            sum += value;
            maximum = std::max(maximum, value);
        }

        double Average() const noexcept
        {
            return values.empty() ? 0.0 :
                sum / static_cast<double>(values.size());
        }

        double P95() const
        {
            if (values.empty())
                return 0.0;
            std::vector<double> sorted = values;
            std::sort(sorted.begin(), sorted.end());
            const std::size_t index =
                std::min<std::size_t>(
                    sorted.size() - 1,
                    static_cast<std::size_t>(
                        std::ceil(sorted.size() * 0.95)) - 1);
            return sorted[index];
        }

        void Reset()
        {
            values.clear();
            sum = 0.0;
            maximum = 0.0;
        }
    };

    struct R23PipelineWindow
    {
        std::unordered_map<std::string, std::uint64_t> rejects;
        std::unordered_map<std::string, std::uint64_t> finals;
        R23MetricWindow capture;
        R23MetricWindow commit;
        R23MetricWindow render;
        R23MetricWindow endFrame;
        std::uint64_t frames = 0;

        void Note(const char* rejectReason, const char* finalKind,
            double captureMs, double commitMs,
            double renderMs, double endMs)
        {
            ++frames;
            if (rejectReason && *rejectReason &&
                std::strcmp(rejectReason, "not-evaluated") != 0 &&
                std::strcmp(rejectReason, "already-processed") != 0 &&
                std::strcmp(rejectReason, "committed-pending-render") != 0 &&
                std::strcmp(rejectReason, "displayed-fresh") != 0)
                ++rejects[rejectReason];
            if (finalKind && *finalKind)
                ++finals[finalKind];
            capture.Add(captureMs);
            commit.Add(commitMs);
            render.Add(renderMs);
            endFrame.Add(endMs);
        }

        static void AppendCounts(
            std::ostringstream& out,
            const std::unordered_map<std::string, std::uint64_t>& counts)
        {
            bool first = true;
            for (const auto& item : counts)
            {
                if (!first) out << ",";
                first = false;
                out << item.first << ":" << item.second;
            }
            if (first) out << "none";
        }

        void AppendAndReset(std::ostringstream& out)
        {
            out << " intervalFrames=" << frames
                << " actualSubmits={";
            AppendCounts(out, finals);
            out << "} rejectCounts={";
            AppendCounts(out, rejects);
            out << "}"
                << " captureAvgMaxP95="
                << capture.Average() << "/"
                << capture.maximum << "/" << capture.P95()
                << " commitAvgMaxP95="
                << commit.Average() << "/"
                << commit.maximum << "/" << commit.P95()
                << " renderAvgMaxP95="
                << render.Average() << "/"
                << render.maximum << "/" << render.P95()
                << " endAvgMaxP95="
                << endFrame.Average() << "/"
                << endFrame.maximum << "/" << endFrame.P95();
            frames = 0;
            rejects.clear();
            finals.clear();
            capture.Reset();
            commit.Reset();
            render.Reset();
            endFrame.Reset();
        }
    };

    struct R23DirectHoldState
    {
        ID3D11Texture2D* eye[2]{};
        ID3D11ShaderResourceView* srv[2]{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT width = 0;
        UINT height = 0;
        UINT mipLevels = 0;
        UINT arraySize = 0;
        std::uint32_t frameId = 0;
        std::uint32_t generation = 0;
        bool valid = false;

        ~R23DirectHoldState()
        {
            for (int eyeIndex = 0; eyeIndex < 2; ++eyeIndex)
            {
                if (srv[eyeIndex]) srv[eyeIndex]->Release();
                if (eye[eyeIndex]) eye[eyeIndex]->Release();
            }
        }
    };
    R23DirectHoldState R23DirectHold{};
    bool R23FirstDirectHoldLogged = false;
    bool R36FirstDirectBootstrapLogged = false;
    std::array<std::uint32_t, OutRunVR::RenderFrameRingSize>
        R37BootstrapSubmittedFrame{};
    std::array<std::uint32_t, OutRunVR::RenderFrameRingSize>
        R37BootstrapSubmittedGeneration{};

    bool R37FrameIdBefore(
        std::uint32_t candidate, std::uint32_t reference) noexcept
    {
        return candidate != reference &&
            static_cast<std::int32_t>(candidate - reference) < 0;
    }

    void R23ReleaseDirectHoldResources() noexcept
    {
        for (int eye = 0; eye < 2; ++eye)
        {
            ReleaseCom(R23DirectHold.srv[eye]);
            ReleaseCom(R23DirectHold.eye[eye]);
        }
        R23DirectHold.format = DXGI_FORMAT_UNKNOWN;
        R23DirectHold.width = 0;
        R23DirectHold.height = 0;
        R23DirectHold.mipLevels = 0;
        R23DirectHold.arraySize = 0;
        R23DirectHold.frameId = 0;
        R23DirectHold.generation = 0;
        R23DirectHold.valid = false;
    }

    void R23InvalidateDirectHold() noexcept
    {
        R23DirectHold.frameId = 0;
        R23DirectHold.generation = 0;
        R23DirectHold.valid = false;
    }

    bool R23StageDirectHold(StereoCompositor& c,
        const OutRunVR::SharedRenderFrameState& frame)
    {
        const std::uint32_t slot =
            frame.reserved[OutRunVR::RenderFrameDirectSlotIndex];
        const std::uint32_t generation =
            frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex];
        if (!c.context_ || !c.device_ || slot >= OutRunVR::RenderFrameRingSize ||
            !frame.frameId || !generation || !c.directLeft_[slot] ||
            !c.directRight_[slot])
            return false;

        D3D11_TEXTURE2D_DESC left{};
        D3D11_TEXTURE2D_DESC right{};
        c.directLeft_[slot]->GetDesc(&left);
        c.directRight_[slot]->GetDesc(&right);
        if (!left.Width || !left.Height || left.Width != right.Width ||
            left.Height != right.Height || left.MipLevels != right.MipLevels ||
            left.ArraySize != right.ArraySize || left.Format != right.Format ||
            left.SampleDesc.Count != 1 || right.SampleDesc.Count != 1 ||
            left.Width != frame.backbufferWidth ||
            left.Height != frame.backbufferHeight)
            return false;

        const bool recreate =
            !R23DirectHold.eye[0] || !R23DirectHold.eye[1] ||
            !R23DirectHold.srv[0] || !R23DirectHold.srv[1] ||
            R23DirectHold.width != left.Width ||
            R23DirectHold.height != left.Height ||
            R23DirectHold.mipLevels != left.MipLevels ||
            R23DirectHold.arraySize != left.ArraySize ||
            R23DirectHold.format != left.Format;
        if (recreate)
        {
            R23ReleaseDirectHoldResources();
            D3D11_TEXTURE2D_DESC hold = left;
            hold.Usage = D3D11_USAGE_DEFAULT;
            hold.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hold.CPUAccessFlags = 0;
            hold.MiscFlags = 0;
            for (int eye = 0; eye < 2; ++eye)
            {
                if (FAILED(c.device_->CreateTexture2D(
                        &hold, nullptr, &R23DirectHold.eye[eye])) ||
                    !R23DirectHold.eye[eye] ||
                    FAILED(c.device_->CreateShaderResourceView(
                        R23DirectHold.eye[eye], nullptr,
                        &R23DirectHold.srv[eye])) ||
                    !R23DirectHold.srv[eye])
                {
                    R23ReleaseDirectHoldResources();
                    return false;
                }
            }
            R23DirectHold.width = left.Width;
            R23DirectHold.height = left.Height;
            R23DirectHold.mipLevels = left.MipLevels;
            R23DirectHold.arraySize = left.ArraySize;
            R23DirectHold.format = left.Format;
        }

        // Immediate-context ordering guarantees that both copies execute before
        // the following projection draw samples this host-owned pair. The R32
        // EVENT fence then covers the copy + projection work before producer ACK.
        c.context_->CopyResource(R23DirectHold.eye[0], c.directLeft_[slot]);
        c.context_->CopyResource(R23DirectHold.eye[1], c.directRight_[slot]);
        R23DirectHold.frameId = frame.frameId;
        R23DirectHold.generation = generation;
        R23DirectHold.valid = true;
        if (!R23FirstDirectHoldLogged)
        {
            R23FirstDirectHoldLogged = true;
            std::cout
                << "DirectGPU single-copy production path active; legacy private snapshot/fence bypassed and grace projection samples only host-owned hold textures.\n";
        }
        return true;
    }

    const char* R23SourceKindName(OutRunVrR23VerifiedBundle::SourceKind kind)
    {
        using OutRunVrR23VerifiedBundle::SourceKind;
        switch (kind)
        {
        case SourceKind::ClassicSbs: return "classic-sbs";
        case SourceKind::DirectGpu: return "direct-gpu";
        default: return "none";
        }
    }

    std::string R23GameBuildTag(
        const OutRunVR::SharedRenderFrameState& frame)
    {
        char tag[13]{};
        std::memcpy(tag,
            &frame.reserved[
                OutRunVR::RenderFrameGameBuildTag0Index],
            12);
        tag[12] = '\0';
        std::size_t length = 0;
        while (length < 12 && tag[length] != '\0')
            ++length;
        return std::string(tag, length);
    }

    float R23HalfToFloat(std::uint16_t value)
    {
        const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000u) << 16;
        std::uint32_t exponent = (value >> 10) & 0x1Fu;
        std::uint32_t mantissa = value & 0x03FFu;
        std::uint32_t bits = 0;
        if (exponent == 0)
        {
            if (mantissa == 0) bits = sign;
            else
            {
                int unbiased = -14;
                while ((mantissa & 0x0400u) == 0) { mantissa <<= 1; --unbiased; }
                mantissa &= 0x03FFu;
                bits = sign | (static_cast<std::uint32_t>(unbiased + 127) << 23) |
                    (mantissa << 13);
            }
        }
        else if (exponent == 0x1Fu)
            bits = sign | 0x7F800000u | (mantissa << 13);
        else
            bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
        float out = 0.0f;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    bool R23DecodeRgb(const std::uint8_t* p, DXGI_FORMAT format, float rgb[3])
    {
        if (!p || !rgb) return false;
        if (format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        {
            rgb[0] = p[2] / 255.0f; rgb[1] = p[1] / 255.0f; rgb[2] = p[0] / 255.0f;
            return true;
        }
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        {
            rgb[0] = p[0] / 255.0f; rgb[1] = p[1] / 255.0f; rgb[2] = p[2] / 255.0f;
            return true;
        }
        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            const auto* h = reinterpret_cast<const std::uint16_t*>(p);
            rgb[0] = R23HalfToFloat(h[0]); rgb[1] = R23HalfToFloat(h[1]); rgb[2] = R23HalfToFloat(h[2]);
            return true;
        }
        return false;
    }

    UINT R23BytesPerPixel(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
    }

    struct R23AsyncPixelProbe
    {
        ID3D11Texture2D* sourceStage = nullptr;
        ID3D11Texture2D* projectionStage = nullptr;
        ID3D11Texture2D* theaterStage = nullptr;
        DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT projectionFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT theaterFormat = DXGI_FORMAT_UNKNOWN;
        bool sourcePending = false;
        bool projectionPending = false;
        bool theaterPending = false;
        ULONGLONG lastSourceScheduleMs = 0;
        ULONGLONG lastProjectionScheduleMs = 0;
        ULONGLONG lastTheaterScheduleMs = 0;
        std::uint64_t sourceMapDefers = 0;
        std::uint64_t projectionMapDefers = 0;
        std::uint64_t theaterMapDefers = 0;
        std::uint32_t projectionFrameId = 0;
        OutRunVrR23VerifiedBundle::SourceKind projectionKind =
            OutRunVrR23VerifiedBundle::SourceKind::None;
        bool projectionFinalKnown = false;
        bool projectionFinalSubmitted = false;
        bool theaterFinalKnown = false;
        bool theaterFinalSubmitted = false;

        ~R23AsyncPixelProbe()
        {
            ReleaseCom(sourceStage);
            ReleaseCom(projectionStage);
            ReleaseCom(theaterStage);
        }

        bool EnsureStage(ID3D11Device* device, ID3D11Texture2D*& stage,
            DXGI_FORMAT& cachedFormat, DXGI_FORMAT format, UINT width)
        {
            if (stage && cachedFormat == format)
                return true;
            ReleaseCom(stage);
            cachedFormat = DXGI_FORMAT_UNKNOWN;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = width; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = format; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            const HRESULT hr = device->CreateTexture2D(&d, nullptr, &stage);
            if (FAILED(hr) || !stage)
            {
                std::cerr << "[R23 pixel] CreateTexture2D staging failed hr=0x"
                          << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
                return false;
            }
            cachedFormat = format;
            return true;
        }

        void ConsumeSource(ID3D11DeviceContext* context)
        {
            if (!sourcePending || !sourceStage || !context) return;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = context->Map(sourceStage, 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { ++sourceMapDefers; return; }
            if (FAILED(hr) || !mapped.pData)
            {
                std::cerr << "[R23 pixel] source-game-uv async Map failed hr=0x"
                          << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
                sourcePending = false;
                return;
            }

            float minRgb[3]{ 1.0e30f, 1.0e30f, 1.0e30f };
            float maxRgb[3]{ -1.0e30f, -1.0e30f, -1.0e30f };
            double sumRgb[3]{};
            std::uint32_t finitePixels = 0, nonFinitePixels = 0;
            const UINT bpp = R23BytesPerPixel(sourceFormat);
            const auto* row = static_cast<const std::uint8_t*>(mapped.pData);
            for (UINT i = 0; i < R23SourceSamples; ++i)
            {
                float rgb[3]{};
                if (!R23DecodeRgb(row + static_cast<std::size_t>(i) * bpp, sourceFormat, rgb))
                    continue;
                const bool finite = std::isfinite(rgb[0]) && std::isfinite(rgb[1]) && std::isfinite(rgb[2]);
                if (!finite) { ++nonFinitePixels; continue; }
                ++finitePixels;
                for (int c = 0; c < 3; ++c)
                {
                    minRgb[c] = std::min(minRgb[c], rgb[c]);
                    maxRgb[c] = std::max(maxRgb[c], rgb[c]);
                    sumRgb[c] += rgb[c];
                }
            }
            context->Unmap(sourceStage, 0);
            sourcePending = false;
            const double denom = finitePixels ? static_cast<double>(finitePixels) : 1.0;
            std::cout << "[R23 pixel] source-game-uv samples=" << R23SourceSamples
                      << " finite=" << finitePixels << " nonFinitePixels=" << nonFinitePixels
                      << " rgbMin=" << minRgb[0] << "," << minRgb[1] << "," << minRgb[2]
                      << " rgbMax=" << maxRgb[0] << "," << maxRgb[1] << "," << maxRgb[2]
                      << " rgbMean=" << sumRgb[0] / denom << "," << sumRgb[1] / denom << "," << sumRgb[2] / denom
                      << " mapDefers=" << sourceMapDefers << "\n";
        }

        void ConsumeProjection(ID3D11DeviceContext* context)
        {
            if (!projectionPending || !projectionStage || !context) return;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = context->Map(projectionStage, 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { ++projectionMapDefers; return; }
            if (FAILED(hr) || !mapped.pData)
            {
                std::cerr << "[R23 pixel] projection async Map failed hr=0x"
                          << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
                projectionPending = false;
                return;
            }

            const UINT bpp = R23BytesPerPixel(projectionFormat);
            const auto* row = static_cast<const std::uint8_t*>(mapped.pData);
            float left[3]{}, right[3]{};
            const bool leftDecoded = R23DecodeRgb(row, projectionFormat, left);
            const bool rightDecoded = R23DecodeRgb(row + bpp, projectionFormat, right);
            const bool leftFinite = leftDecoded && std::isfinite(left[0]) &&
                std::isfinite(left[1]) && std::isfinite(left[2]);
            const bool rightFinite = rightDecoded && std::isfinite(right[0]) &&
                std::isfinite(right[1]) && std::isfinite(right[2]);
            context->Unmap(projectionStage, 0);
            projectionPending = false;

            std::cout << "[R23 pixel] projection frame=" << projectionFrameId
                      << " source=" << R23SourceKindName(projectionKind)
                      << " finalSubmissionKnown=" << (projectionFinalKnown ? 1 : 0)
                      << " finalSubmitted=" << (projectionFinalSubmitted ? 1 : 0)
                      << " leftFinite=" << (leftFinite ? 1 : 0)
                      << " leftRgb=" << left[0] << "," << left[1] << "," << left[2]
                      << " rightFinite=" << (rightFinite ? 1 : 0)
                      << " rightRgb=" << right[0] << "," << right[1] << "," << right[2]
                      << " mapDefers=" << projectionMapDefers << "\n";
        }

        void ConsumeTheater(ID3D11DeviceContext* context)
        {
            if (!theaterPending || !theaterStage || !context) return;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = context->Map(theaterStage, 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { ++theaterMapDefers; return; }
            if (FAILED(hr) || !mapped.pData)
            {
                std::cerr << "[R23 pixel] theater async Map failed hr=0x"
                          << std::hex << static_cast<unsigned long>(hr) << std::dec << "\n";
                theaterPending = false;
                return;
            }
            float rgb[3]{};
            const bool decoded = R23DecodeRgb(
                static_cast<const std::uint8_t*>(mapped.pData), theaterFormat, rgb);
            const bool finite = decoded && std::isfinite(rgb[0]) &&
                std::isfinite(rgb[1]) && std::isfinite(rgb[2]);
            context->Unmap(theaterStage, 0);
            theaterPending = false;
            std::cout << "[R23 pixel] theater finalSubmissionKnown="
                      << (theaterFinalKnown ? 1 : 0)
                      << " finalSubmitted=" << (theaterFinalSubmitted ? 1 : 0)
                      << " finite=" << (finite ? 1 : 0)
                      << " rgb=" << rgb[0] << "," << rgb[1] << "," << rgb[2]
                      << " mapDefers=" << theaterMapDefers << "\n";
        }

        void TryConsume(ID3D11DeviceContext* context)
        {
            ConsumeSource(context);
            ConsumeProjection(context);
            ConsumeTheater(context);
        }

        void NoteFinalSubmission()
        {
            const bool submitted =
                OutRunVrR23RuntimeHardening::LastSubmittedLayer.load(std::memory_order_acquire);
            const std::uint32_t frameId =
                OutRunVrR23RuntimeHardening::LastSubmittedFrameId.load(std::memory_order_acquire);
            const auto kind = static_cast<OutRunVrR23VerifiedBundle::SourceKind>(
                OutRunVrR23RuntimeHardening::LastSubmittedKind.load(std::memory_order_acquire));

            if (projectionPending && !projectionFinalKnown &&
                frameId == projectionFrameId && kind == projectionKind)
            {
                projectionFinalKnown = true;
                projectionFinalSubmitted = submitted;
            }
            if (theaterPending && !theaterFinalKnown &&
                kind == OutRunVrR23VerifiedBundle::SourceKind::None)
            {
                theaterFinalKnown = true;
                theaterFinalSubmitted = submitted;
            }
        }

        void ScheduleSource(StereoCompositor& c)
        {
            const ULONGLONG now = GetTickCount64();
            if (sourcePending || now - lastSourceScheduleMs < R23PixelIntervalMs ||
                !c.source_ || !c.device_ || !c.context_) return;
            UvRect uv{};
            if (!c.GetGameUv(uv)) return;
            if (!EnsureStage(c.device_, sourceStage, sourceFormat,
                    c.sourceFormat_, R23SourceSamples)) return;

            const float xs[4]{ 0.125f, 0.375f, 0.625f, 0.875f };
            const float ys[2]{ 0.35f, 0.65f };
            UINT index = 0;
            for (float yFrac : ys)
            {
                for (float xFrac : xs)
                {
                    const UINT sx = std::min(c.sourceWidth_ - 1,
                        static_cast<UINT>((uv.x + uv.w * xFrac) * c.sourceWidth_));
                    const UINT sy = std::min(c.sourceHeight_ - 1,
                        static_cast<UINT>((uv.y + uv.h * yFrac) * c.sourceHeight_));
                    D3D11_BOX box{ sx, sy, 0, sx + 1, sy + 1, 1 };
                    c.context_->CopySubresourceRegion(sourceStage, 0, index++, 0, 0,
                        c.source_, 0, &box);
                }
            }
            sourcePending = true;
            lastSourceScheduleMs = now;
            std::cout << "[R23 pixel] scheduled game-UV distributed source probe uv=["
                      << uv.x << "," << uv.y << "," << uv.w << "," << uv.h << "]\n";
        }

        void ScheduleProjection(StereoCompositor& c, std::uint32_t image)
        {
            const ULONGLONG now = GetTickCount64();
            if (projectionPending || now - lastProjectionScheduleMs < R23PixelIntervalMs ||
                image >= c.projection_.images.size() || !c.device_ || !c.context_) return;
            if (!EnsureStage(c.device_, projectionStage, projectionFormat,
                    c.projection_.format, 2)) return;
            ID3D11Texture2D* texture = c.projection_.images[image].texture;
            if (!texture) return;
            for (UINT eye = 0; eye < 2; ++eye)
            {
                const UINT sx = c.projection_.width / 2;
                const UINT sy = c.projection_.height / 2;
                D3D11_BOX box{ sx, sy, 0, sx + 1, sy + 1, 1 };
                c.context_->CopySubresourceRegion(projectionStage, 0, eye, 0, 0,
                    texture, D3D11CalcSubresource(0, eye, 1), &box);
            }
            OutRunVrR23VerifiedBundle::Snapshot verified{};
            if (OutRunVrR23VerifiedBundle::ReadFresh(verified))
            {
                projectionFrameId = verified.frameId;
                projectionKind = verified.kind;
            }
            else
            {
                projectionFrameId = 0;
                projectionKind = OutRunVrR23VerifiedBundle::SourceKind::None;
            }
            projectionFinalKnown = false;
            projectionFinalSubmitted = false;
            projectionPending = true;
            lastProjectionScheduleMs = now;
        }

        void ScheduleTheater(StereoCompositor& c, std::uint32_t image)
        {
            const ULONGLONG now = GetTickCount64();
            if (theaterPending || now - lastTheaterScheduleMs < R23PixelIntervalMs ||
                image >= c.theater_.images.size() || !c.device_ || !c.context_) return;
            if (!EnsureStage(c.device_, theaterStage, theaterFormat,
                    c.theater_.format, 1)) return;
            ID3D11Texture2D* texture = c.theater_.images[image].texture;
            if (!texture) return;
            const UINT sx = c.theater_.width / 2;
            const UINT sy = c.theater_.height / 2;
            D3D11_BOX box{ sx, sy, 0, sx + 1, sy + 1, 1 };
            c.context_->CopySubresourceRegion(theaterStage, 0, 0, 0, 0,
                texture, 0, &box);
            theaterFinalKnown = false;
            theaterFinalSubmitted = false;
            theaterPending = true;
            lastTheaterScheduleMs = now;
        }
    };

    R23AsyncPixelProbe R23Pixels{};

    CaptureStatus R23Capture(StereoCompositor& c, DWORD timeoutMs = 0,
        bool allowInitialWarmupWait = true)
    {
        // DirectGpuOnly is a gameplay source policy. Menus still need their
        // mono theater source; only explicit diagnostic isolation disables
        // Desktop Duplication globally.
        if (c.disableDesktopDuplication_)
            return {};
        R23Pixels.TryConsume(c.context_);
        if (!IsWindow(c.hwnd_))
            if (HWND replacement = FindGameWindow(c.gamePid_))
                c.hwnd_ = replacement;

        const HMONITOR monitorNow = IsWindow(c.hwnd_)
            ? MonitorFromWindow(c.hwnd_, MONITOR_DEFAULTTONEAREST)
            : nullptr;
        if (!c.duplication_ ||
            (monitorNow && monitorNow != c.targetMonitor_))
            c.BindCaptureOutput(false);

        CaptureStatus status{};
        status.available = c.haveFrame_;
        status.lastPresentQpc = c.lastCapturePresentQpc_;
        status.lastPresentQpcLow = c.lastCapturePresentQpcLow_;
        RECT gameRect{};
        status.fullGameClientVisible =
            c.GetGameClientOutputRect(gameRect);

        if (!c.duplication_ && !c.RecreateDuplication(false))
            return status;

        DXGI_OUTDUPL_FRAME_INFO fi{};
        IDXGIResource* resource = nullptr;
        if (!c.duplication_)
            return status;

        const DWORD waitMs =
            (c.haveFrame_ || !allowInitialWarmupWait)
            ? timeoutMs : std::max<DWORD>(timeoutMs, 1000);
        const HRESULT hr =
            c.duplication_->AcquireNextFrame(waitMs, &fi, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return status;
        if (FAILED(hr) || !resource)
        {
            if (hr == DXGI_ERROR_ACCESS_LOST)
            {
                c.haveFrame_ = false;
                c.stereoSourceValid_ = false;
                c.BindCaptureOutput(false);
            }
            status.available = c.haveFrame_;
            return status;
        }

        bool gameRegionChanged = false;
        auto intersectsGame = [&](const RECT& rect) noexcept
        {
            RECT intersection{};
            return status.fullGameClientVisible &&
                IntersectRect(&intersection, &rect, &gameRect) != FALSE &&
                intersection.right > intersection.left &&
                intersection.bottom > intersection.top;
        };

        UINT dirtyBytes = 0;
        HRESULT dirtyHr =
            c.duplication_->GetFrameDirtyRects(
                0, nullptr, &dirtyBytes);
        if ((dirtyHr == DXGI_ERROR_MORE_DATA ||
             SUCCEEDED(dirtyHr)) &&
            dirtyBytes >= sizeof(RECT))
        {
            try
            {
                std::vector<RECT> dirty(
                    (dirtyBytes + sizeof(RECT) - 1) /
                    sizeof(RECT));
                UINT actual = dirtyBytes;
                if (SUCCEEDED(c.duplication_->GetFrameDirtyRects(
                        static_cast<UINT>(
                            dirty.size() * sizeof(RECT)),
                        dirty.data(), &actual)))
                {
                    const UINT count = std::min<UINT>(
                        static_cast<UINT>(dirty.size()),
                        actual / sizeof(RECT));
                    for (UINT i = 0; i < count; ++i)
                    {
                        if (intersectsGame(dirty[i]))
                        {
                            gameRegionChanged = true;
                            break;
                        }
                    }
                }
            }
            catch (...) {}
        }

        if (!gameRegionChanged)
        {
            UINT moveBytes = 0;
            HRESULT moveHr =
                c.duplication_->GetFrameMoveRects(
                    0, nullptr, &moveBytes);
            if ((moveHr == DXGI_ERROR_MORE_DATA ||
                 SUCCEEDED(moveHr)) &&
                moveBytes >= sizeof(DXGI_OUTDUPL_MOVE_RECT))
            {
                try
                {
                    std::vector<DXGI_OUTDUPL_MOVE_RECT> moves(
                        (moveBytes +
                         sizeof(DXGI_OUTDUPL_MOVE_RECT) - 1) /
                        sizeof(DXGI_OUTDUPL_MOVE_RECT));
                    UINT actual = moveBytes;
                    if (SUCCEEDED(
                            c.duplication_->GetFrameMoveRects(
                                static_cast<UINT>(
                                    moves.size() *
                                    sizeof(DXGI_OUTDUPL_MOVE_RECT)),
                                moves.data(), &actual)))
                    {
                        const UINT count = std::min<UINT>(
                            static_cast<UINT>(moves.size()),
                            actual /
                            sizeof(DXGI_OUTDUPL_MOVE_RECT));
                        for (UINT i = 0; i < count; ++i)
                        {
                            if (intersectsGame(
                                    moves[i].DestinationRect))
                            {
                                gameRegionChanged = true;
                                break;
                            }
                        }
                    }
                }
                catch (...) {}
            }
        }

        if (!gameRegionChanged &&
            status.fullGameClientVisible &&
            fi.AccumulatedFrames > 1)
            gameRegionChanged = true;

        ID3D11Texture2D* texture = nullptr;
        const HRESULT qi = resource->QueryInterface(
            __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&texture));
        resource->Release();

        bool copied = false;
        if (SUCCEEDED(qi) && texture)
        {
            D3D11_TEXTURE2D_DESC d{};
            texture->GetDesc(&d);
            if ((d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                 d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                c.EnsureSource(d))
            {
                c.context_->CopyResource(c.source_, texture);
                copied = true;
            }
            texture->Release();
        }
        c.duplication_->ReleaseFrame();

        if (copied)
        {
            c.haveFrame_ = true;
            if (fi.LastPresentTime.QuadPart != 0)
            {
                c.lastCapturePresentQpc_ =
                    fi.LastPresentTime.QuadPart;
                c.lastCapturePresentQpcLow_ =
                    static_cast<std::uint32_t>(
                        fi.LastPresentTime.QuadPart);
            }
            status.available = true;
            status.fresh = fi.AccumulatedFrames > 0;
            status.lastPresentQpc =
                c.lastCapturePresentQpc_;
            status.lastPresentQpcLow =
                c.lastCapturePresentQpcLow_;
            status.gameRegionChanged = gameRegionChanged;
            status.accumulatedFrames = fi.AccumulatedFrames;
            OutRunVrSbsCaptureOverride::PublishProductionCapture(
                c.source_, c.outputDesktop_, c.targetMonitor_,
                c.sdrWhiteScale_,
                fi.LastPresentTime.QuadPart);
            R23Pixels.ScheduleSource(c);
        }
        return status;
    }

    CaptureStatus R23RefreshTheaterFallbackCapture(StereoCompositor& c)
    {
        ++R23TheaterRefreshAttempts;
        const CaptureStatus status = R23Capture(
            c, R23TheaterRefreshWaitMs, false);
        if (status.fresh)
            ++R23TheaterRefreshFresh;

        const ULONGLONG now = GetTickCount64();
        if (R23LastTheaterRefreshLogMs == 0 ||
            now - R23LastTheaterRefreshLogMs >= R23TheaterRefreshLogIntervalMs)
        {
            R23LastTheaterRefreshLogMs = now;
            const ULONGLONG publishedAt =
                OutRunVrSbsCaptureOverride::LastProductionPublishMs;
            const long long publishAgeMs = publishedAt && now >= publishedAt
                ? static_cast<long long>(now - publishedAt) : -1;
            std::cout
                << "[R23 mono] gameplay stereo pending: bounded Desktop Duplication refresh for theater-only fallback"
                << " attempts=" << R23TheaterRefreshAttempts
                << " fresh=" << R23TheaterRefreshFresh
                << " available=" << (status.available ? 1 : 0)
                << " captureQpc=" << status.lastPresentQpc
                << " publishAgeMs=" << publishAgeMs
                << " waitMs=" << R23TheaterRefreshWaitMs << "\n";
        }
        return status;
    }

    bool R23FrameUnchanged(RenderFrameReader& reader,
        const OutRunVR::SharedRenderFrameState& before)
    {
        OutRunVR::SharedRenderFrameState after{};
        return reader.ReadFrame(before.frameId, after) &&
            (after.flags & OutRunVR::RenderFramePresentInFlight) == 0 &&
            after.sequence == before.sequence &&
            after.clientPid == before.clientPid &&
            after.state == before.state && after.frameId == before.frameId &&
            after.sourcePoseSequence == before.sourcePoseSequence &&
            after.presentationMode == before.presentationMode &&
            after.presentQpc == before.presentQpc && after.flags == before.flags &&
            after.failureReason == before.failureReason &&
            after.backbufferWidth == before.backbufferWidth &&
            after.backbufferHeight == before.backbufferHeight &&
            std::memcmp(after.eye, before.eye, sizeof(before.eye)) == 0 &&
            std::memcmp(after.reserved, before.reserved, sizeof(before.reserved)) == 0;
    }


    bool R23SelectClassicFrameForCapture(
        RenderFrameReader& reader, const CaptureStatus& capture,
        std::uint32_t lastProcessedFrame,
        std::uint32_t requiredFlags,
        OutRunVR::SharedRenderFrameState& selected,
        std::int64_t maxSkewTicks)
    {
        selected = {};
        if (!capture.available || !capture.fresh ||
            !capture.fullGameClientVisible ||
            capture.lastPresentQpc <= 0)
            return false;

        // R35.3: DXGI Desktop Duplication dirty/move metadata is advisory.
        // Borderless D3D9Ex presents can advance LastPresentTime while the dirty
        // rectangles do not intersect the OutRun client. The frame ring gives
        // us a stronger correlation key: accept the capture only when a stable
        // completed SBS frame exists at/before this output Present and inside
        // maxSkewTicks. This prevents healthy stereo from collapsing into the
        // giant recovery theater solely because dirty metadata was incomplete.

        std::array<OutRunVR::SharedRenderFrameState,
            OutRunVR::RenderFrameRingSize> history{};
        std::size_t count = 0;
        if (!reader.ReadHistory(history, count))
            return false;

        bool found = false;
        std::int64_t bestPresentQpc = 0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& frame = history[i];
            if ((frame.flags & OutRunVR::RenderFramePresentInFlight) != 0 ||
                (frame.flags & OutRunVR::RenderFrameDirectGpuTransport) != 0 ||
                frame.state != OutRunVR::StereoSbsActive ||
                !frame.frameId ||
                frame.frameId == lastProcessedFrame ||
                !frame.sourcePoseSequence ||
                (frame.flags & requiredFlags) != requiredFlags ||
                frame.presentQpc <= 0 ||
                frame.presentQpc > capture.lastPresentQpc)
                continue;

            const std::int64_t skew =
                capture.lastPresentQpc - frame.presentQpc;
            if (maxSkewTicks > 0 && skew > maxSkewTicks)
                continue;

            if (!found || frame.presentQpc > bestPresentQpc ||
                (frame.presentQpc == bestPresentQpc &&
                 frame.frameId > selected.frameId))
            {
                selected = frame;
                bestPresentQpc = frame.presentQpc;
                found = true;
            }
        }

        // The ring may advance while Desktop Duplication copies the output.
        // That is fine as long as the exact frameId chosen above is still
        // present and byte-for-byte stable. We no longer reject merely because
        // latestSlot moved to the next game frame.
        return found && R23FrameUnchanged(reader, selected);
    }

    bool R23ValidateDirectResourceSize(StereoCompositor& c,
        const OutRunVR::SharedRenderFrameState& frame)
    {
        const std::uint32_t slot = frame.reserved[OutRunVR::RenderFrameDirectSlotIndex];
        const std::uint32_t width = frame.reserved[OutRunVR::RenderFrameDirectWidthIndex];
        const std::uint32_t height = frame.reserved[OutRunVR::RenderFrameDirectHeightIndex];
        if (slot >= OutRunVR::RenderFrameRingSize || !width || !height ||
            width != frame.backbufferWidth || height != frame.backbufferHeight ||
            !c.directLeft_[slot] || !c.directRight_[slot]) return false;
        D3D11_TEXTURE2D_DESC l{}, r{};
        c.directLeft_[slot]->GetDesc(&l); c.directRight_[slot]->GetDesc(&r);
        return l.Width == width && l.Height == height && r.Width == width && r.Height == height &&
            l.Format == r.Format && l.SampleDesc.Count == 1 && r.SampleDesc.Count == 1;
    }

    void R23InvalidateDirect(StereoCompositor& c)
    {
        c.directFrameValid_ = false;
        c.directTransportReady_ = false;
        R23InvalidateDirectHold();
    }

    bool R23CommitDirectAfterValidation(StereoCompositor& c,
        const OutRunVR::SharedRenderFrameState& frame)
    {
        if (!c.PrepareDirectStereoSource(frame) ||
            !R23ValidateDirectResourceSize(c, frame) ||
            !R23StageDirectHold(c, frame))
        {
            R23InvalidateDirect(c);
            return false;
        }
        c.directTransportReady_ = true;
        c.directFrameValid_ = true;
        c.stereoSourceValid_ = false;
        return true;
    }

    bool R23CommitClassicAfterValidation(StereoCompositor& c)
    {
        c.directFrameValid_ = false;
        if (!c.CommitStereoSource()) return false;
        c.stereoSourceValid_ = true;
        return true;
    }

    bool R23RenderProjection(StereoCompositor& c,
        const std::array<XrView, 2>& views,
        std::array<XrCompositionLayerProjectionView, 2>& pv)
    {
        R23Pixels.TryConsume(c.context_);
        if (!c.HasStereoSource()) return false;
        UvRect eyes[2]{};
        ID3D11ShaderResourceView* srv[2]{};
        DXGI_FORMAT fmt[2]{ DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
        if (c.directFrameValid_ && R23DirectHold.valid &&
            R23DirectHold.srv[0] && R23DirectHold.srv[1])
        {
            eyes[0] = eyes[1] = { 0.f, 0.f, 1.f, 1.f };
            srv[0] = R23DirectHold.srv[0];
            srv[1] = R23DirectHold.srv[1];
            fmt[0] = fmt[1] = R23DirectHold.format;
        }
        else
        {
            UvRect whole{};
            if (!c.GetGameUv(whole)) return false;
            eyes[0] = { whole.x, whole.y, whole.w * 0.5f, whole.h };
            eyes[1] = { whole.x + whole.w * 0.5f, whole.y, whole.w * 0.5f, whole.h };
            srv[0] = srv[1] = c.stereoSourceSrv_;
            fmt[0] = fmt[1] = c.stereoSourceFormat_;
        }

        std::uint32_t image = 0;
        c.Acquire(c.projection_, image);
        bool ok = c.RenderTo(c.projection_.rtvs[image][0], c.projection_.width,
            c.projection_.height, eyes[0], srv[0], fmt[0]);
        ok = c.RenderTo(c.projection_.rtvs[image][1], c.projection_.width,
            c.projection_.height, eyes[1], srv[1], fmt[1]) && ok;
        if (ok) R23Pixels.ScheduleProjection(c, image);
        c.Release(c.projection_);
        if (!ok) return false;

        for (int eye = 0; eye < 2; ++eye)
        {
            pv[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            pv[eye].pose = views[eye].pose;
            pv[eye].fov = views[eye].fov;
            pv[eye].subImage.swapchain = c.projection_.handle;
            pv[eye].subImage.imageRect.offset = { 0, 0 };
            pv[eye].subImage.imageRect.extent = {
                static_cast<int32_t>(c.projection_.width),
                static_cast<int32_t>(c.projection_.height) };
            pv[eye].subImage.imageArrayIndex = eye;
        }
        return true;
    }

    bool R23RenderWorldLockedMenuProjection(
        StereoCompositor& c, const std::array<XrView, 2>& views,
        const XrPosef& menuAnchor,
        std::array<XrCompositionLayerProjectionView, 2>& pv)
    {
        R23Pixels.TryConsume(c.context_);
        if (!c.haveFrame_ || !c.sourceSrv_) return false;
        UvRect whole{};
        if (!c.GetGameUv(whole)) return false;

        XrFovf commonFov{};
        commonFov.angleLeft = 0.5f *
            (views[0].fov.angleLeft + views[1].fov.angleLeft);
        commonFov.angleRight = 0.5f *
            (views[0].fov.angleRight + views[1].fov.angleRight);
        commonFov.angleUp = 0.5f *
            (views[0].fov.angleUp + views[1].fov.angleUp);
        commonFov.angleDown = 0.5f *
            (views[0].fov.angleDown + views[1].fov.angleDown);

        const float tanLeft = std::tan(commonFov.angleLeft);
        const float tanRight = std::tan(commonFov.angleRight);
        const float tanDown = std::tan(commonFov.angleDown);
        const float tanUp = std::tan(commonFov.angleUp);
        const float spanX = tanRight - tanLeft;
        const float spanY = tanUp - tanDown;
        if (!std::isfinite(spanX) || !std::isfinite(spanY) ||
            spanX <= 0.05f || spanY <= 0.05f)
            return false;

        RECT cr{};
        GetClientRect(c.hwnd_, &cr);
        const float sourceAspect = (cr.bottom > cr.top)
            ? static_cast<float>(cr.right - cr.left) /
                static_cast<float>(cr.bottom - cr.top)
            : 16.0f / 9.0f;
        const float fovAspect = spanX / spanY;
        if (!std::isfinite(sourceAspect) || !std::isfinite(fovAspect) ||
            sourceAspect <= 0.1f || fovAspect <= 0.1f)
            return false;

        // R41: preserve the desktop/menu aspect inside the HMD projection.
        // This is a pure 2D clip-space rectangle shared by both eyes; unlike
        // R39 there is no world plane, head pose or per-eye disparity to shear.
        float halfWidth = 1.0f;
        float halfHeight = 1.0f;
        if (sourceAspect >= fovAspect)
            halfHeight = std::clamp(fovAspect / sourceAspect, 0.05f, 1.0f);
        else
            halfWidth = std::clamp(sourceAspect / fovAspect, 0.05f, 1.0f);
        const float sx[4]{ -halfWidth, halfWidth, -halfWidth, halfWidth };
        const float sy[4]{ halfHeight, halfHeight, -halfHeight, -halfHeight };
        float clip[4][4]{};
        for (int corner = 0; corner < 4; ++corner)
        {
            clip[corner][0] = sx[corner];
            clip[corner][1] = sy[corner];
            clip[corner][2] = 0.5f;
            clip[corner][3] = 1.0f;
        }

        std::uint32_t image = 0;
        c.Acquire(c.projection_, image);
        bool ok = image < c.projection_.rtvs.size();
        for (int eye = 0; eye < 2 && ok; ++eye)
        {
            ok = c.RenderMenuPlaneTo(
                c.projection_.rtvs[image][eye],
                c.projection_.width, c.projection_.height,
                whole, c.sourceSrv_, c.sourceFormat_, clip);
        }
        if (ok) R23Pixels.ScheduleProjection(c, image);
        c.Release(c.projection_);
        if (!ok) return false;

        // R45: both eyes share the same LOCAL-space camera anchor (zero virtual
        // IPD), captured from the HMD center when entering a mono menu or when
        // a recenter/reference-space request is applied. R43 used identity LOCAL
        // pose, which was world-fixed but had nothing for F12 to update.
        for (int eye = 0; eye < 2; ++eye)
        {
            pv[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            pv[eye].pose = menuAnchor;
            pv[eye].fov = commonFov;
            pv[eye].subImage.swapchain = c.projection_.handle;
            pv[eye].subImage.imageRect.offset = { 0, 0 };
            pv[eye].subImage.imageRect.extent = {
                static_cast<int32_t>(c.projection_.width),
                static_cast<int32_t>(c.projection_.height) };
            pv[eye].subImage.imageArrayIndex = eye;
        }
        return true;
    }

    bool R23RenderTheater(StereoCompositor& c, XrSpace viewSpace,
        XrSpace localSpace, XrTime displayTime, XrCompositionLayerQuad& quad,
        bool sourceIsSbs)
    {
        R23Pixels.TryConsume(c.context_);
        if (!c.haveFrame_ || !c.sourceSrv_) return false;
        UvRect whole{};
        if (!c.GetGameUv(whole)) return false;
        UvRect theaterUv = whole;
        if (sourceIsSbs)
            theaterUv.w *= 0.5f;
        if (theaterUv.w <= 0.0f)
            return false;
        std::uint32_t image = 0;
        c.Acquire(c.theater_, image);
        const bool ok = c.RenderTo(c.theater_.rtvs[image][0], c.theater_.width,
            c.theater_.height, theaterUv, c.sourceSrv_, c.sourceFormat_);
        if (ok) R23Pixels.ScheduleTheater(c, image);
        c.Release(c.theater_);
        if (!ok || !c.EnsureTheaterAnchor(viewSpace, localSpace, displayTime)) return false;

        quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        quad.space = localSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.pose = c.theaterAnchor_;
        quad.subImage.swapchain = c.theater_.handle;
        quad.subImage.imageRect.offset = { 0, 0 };
        quad.subImage.imageRect.extent = {
            static_cast<int32_t>(c.theater_.width), static_cast<int32_t>(c.theater_.height) };
        RECT cr{};
        GetClientRect(c.hwnd_, &cr);
        const float aspect = (cr.bottom > cr.top)
            ? (static_cast<float>(cr.right - cr.left) /
                static_cast<float>(cr.bottom - cr.top)) *
                (sourceIsSbs ? 0.5f : 1.0f)
            : 16.f / 9.f;
        quad.size.width = 2.f;
        quad.size.height = 2.f / aspect;
        return true;
    }

    void R23CopyMatchedViews(const OutRunVR::SharedRenderFrameState& frame,
        std::array<XrView, 2>& matchedViews)
    {
        for (int eye = 0; eye < 2; ++eye)
        {
            matchedViews[eye] = { XR_TYPE_VIEW };
            matchedViews[eye].pose.orientation = {
                frame.eye[eye].orientation[0], frame.eye[eye].orientation[1],
                frame.eye[eye].orientation[2], frame.eye[eye].orientation[3] };
            matchedViews[eye].pose.position = {
                frame.eye[eye].position[0], frame.eye[eye].position[1], frame.eye[eye].position[2] };
            matchedViews[eye].fov = {
                frame.eye[eye].fov.angleLeft, frame.eye[eye].fov.angleRight,
                frame.eye[eye].fov.angleUp, frame.eye[eye].fov.angleDown };
        }
    }

    int R35ReadEnvInt(const char* name, int fallback) noexcept
    {
        const char* value = std::getenv(name);
        if (!value || !*value)
            return fallback;
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        return end && end != value ? static_cast<int>(parsed) : fallback;
    }

    float R35ReadEnvFloat(const char* name, float fallback) noexcept
    {
        const char* value = std::getenv(name);
        if (!value || !*value)
            return fallback;
        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        return end && end != value && std::isfinite(parsed)
            ? parsed : fallback;
    }

    class R35CadenceHost
    {
    public:
        struct Request
        {
            std::uint32_t id = 0;
            bool issued = false;
        };

        R35CadenceHost(int mode, float targetHz, float maxRenderHz,
            std::uint32_t timeoutMs, float requestedRefreshHz)
            : mode_(std::clamp(mode, 0, 2)),
              configuredTargetHz_(std::clamp(targetHz, 0.0f, 120.0f)),
              maxRenderHz_(std::clamp(maxRenderHz, 60.0f, 120.0f)),
              timeoutMs_(std::clamp<std::uint32_t>(timeoutMs, 5u, 100u)),
              requestedRefreshHz_(std::max(0.0f, requestedRefreshHz))
        {
            if (mode_ == 0)
                return;

            hostMapping_ = CreateFileMappingW(
                INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                static_cast<DWORD>(sizeof(OutRunVR::CadenceV1::HostState)),
                OutRunVR::CadenceV1::HostStateName);
            if (!hostMapping_)
                throw std::runtime_error("CreateFileMappingW Cadence.v1 host failed");
            host_ = static_cast<OutRunVR::CadenceV1::HostState*>(
                MapViewOfFile(hostMapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                    sizeof(OutRunVR::CadenceV1::HostState)));
            if (!host_)
                throw std::runtime_error("MapViewOfFile Cadence.v1 host failed");

            requestEvent_ = CreateEventW(
                nullptr, FALSE, FALSE, OutRunVR::CadenceV1::RequestEventName);
            presentedEvent_ = CreateEventW(
                nullptr, FALSE, FALSE, OutRunVR::CadenceV1::PresentedEventName);
            if (!requestEvent_ || !presentedEvent_)
                throw std::runtime_error("CreateEventW Cadence.v1 failed");

            std::memset(host_, 0, sizeof(*host_));
            host_->version = OutRunVR::CadenceV1::ProtocolVersion;
            host_->structSize = sizeof(*host_);
            host_->hostPid = GetCurrentProcessId();
            MemoryBarrier();
            host_->magic = OutRunVR::CadenceV1::HostMagic;
            Publish();
        }

        ~R35CadenceHost()
        {
            if (host_)
            {
                running_ = false;
                enabled_ = false;
                Publish();
                UnmapViewOfFile(host_);
            }
            if (client_)
                UnmapViewOfFile(client_);
            if (clientMapping_)
                CloseHandle(clientMapping_);
            if (requestEvent_)
                CloseHandle(requestEvent_);
            if (presentedEvent_)
                CloseHandle(presentedEvent_);
            if (hostMapping_)
                CloseHandle(hostMapping_);
        }

        bool Enabled() const noexcept { return mode_ != 0 && enabled_; }
        int Mode() const noexcept { return mode_; }
        float TargetHz() const noexcept { return activeRenderHz_; }
        std::uint32_t TimeoutMs() const noexcept { return timeoutMs_; }

        void SetRunning(bool running) noexcept
        {
            running_ = running;
            enabled_ = mode_ != 0;
            Publish();
        }

        static float QuantizeRuntimeHz(double hz) noexcept
        {
            constexpr float modes[]{ 60.0f, 72.0f, 80.0f, 90.0f, 120.0f };
            float best = modes[0];
            double bestError = std::fabs(hz - static_cast<double>(best));
            for (float mode : modes)
            {
                const double error =
                    std::fabs(hz - static_cast<double>(mode));
                if (error < bestError)
                {
                    best = mode;
                    bestError = error;
                }
            }
            return bestError <= 4.0 ? best :
                static_cast<float>(std::clamp(hz, 60.0, 120.0));
        }

        void UpdateActiveRenderHz() noexcept
        {
            float desired = configuredTargetHz_ > 0.0f
                ? configuredTargetHz_
                : QuantizeRuntimeHz(actualXrHz_);
            desired = std::min(desired, maxRenderHz_);
            if (desired <= 0.0f || !std::isfinite(desired))
                return;

            if (activeRenderHz_ <= 0.0f)
            {
                activeRenderHz_ = desired;
                nextRequestDisplayTime_ = 0;
                return;
            }

            if (std::fabs(activeRenderHz_ - desired) < 0.5f)
            {
                pendingRenderHz_ = 0.0f;
                pendingRenderHzSamples_ = 0;
                return;
            }

            if (std::fabs(pendingRenderHz_ - desired) < 0.5f)
                ++pendingRenderHzSamples_;
            else
            {
                pendingRenderHz_ = desired;
                pendingRenderHzSamples_ = 1;
            }

            // About a quarter second at common Quest rates. This filters
            // xrWaitFrame period noise but still follows a deliberate VD
            // 72/80/90/120 Hz mode switch quickly.
            if (pendingRenderHzSamples_ >= 20)
            {
                activeRenderHz_ = pendingRenderHz_;
                pendingRenderHz_ = 0.0f;
                pendingRenderHzSamples_ = 0;
                nextRequestDisplayTime_ = 0;
                std::cout
                    << "[R36 cadence] activeRenderHz=" << activeRenderHz_
                    << " actualXrHz=" << actualXrHz_
                    << " maxRenderHz=" << maxRenderHz_
                    << " policy="
                    << (configuredTargetHz_ > 0.0f ? "fixed" : "auto-native")
                    << "\n";
            }
        }

        Request IssueIfDue(const XrFrameState& frame,
            std::uint32_t poseSequence) noexcept
        {
            ++intervalXrFrames_;
            ObserveRuntimeRefresh(frame.predictedDisplayPeriod);
            UpdateActiveRenderHz();
            if (!Enabled() || !running_ ||
                frame.predictedDisplayPeriod <= 0 ||
                frame.predictedDisplayTime <= 0 ||
                activeRenderHz_ <= 0.0f)
                return { requestId_, false };

            // In Auto mode, if the cap permits the runtime's native mode,
            // release exactly one game render per xrWaitFrame. This avoids
            // 60->90 3:2 cadence and the accumulated rounding drift of an
            // independently generated target timeline.
            const float nativeHz = QuantizeRuntimeHz(actualXrHz_);
            const bool nativeOneToOne =
                configuredTargetHz_ <= 0.0f &&
                maxRenderHz_ + 0.5f >= nativeHz &&
                std::fabs(activeRenderHz_ - nativeHz) < 0.5f;

            if (!nativeOneToOne)
            {
                const XrTime targetPeriod = static_cast<XrTime>(
                    std::llround(1000000000.0 /
                        static_cast<double>(activeRenderHz_)));
                if (nextRequestDisplayTime_ == 0)
                    nextRequestDisplayTime_ = frame.predictedDisplayTime;

                if (frame.predictedDisplayTime < nextRequestDisplayTime_)
                    return { requestId_, false };

                do
                {
                    nextRequestDisplayTime_ += targetPeriod;
                } while (nextRequestDisplayTime_ <= frame.predictedDisplayTime);
            }

            if (++requestId_ == 0)
                ++requestId_;
            poseSequence_ = poseSequence;
            predictedDisplayTime_ = frame.predictedDisplayTime;
            predictedDisplayPeriod_ = frame.predictedDisplayPeriod;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            requestQpc_ = now.QuadPart;
            Publish();
            SetEvent(requestEvent_);
            ++intervalRequests_;
            return { requestId_, true };
        }

        double WaitForPresented(const Request& request,
            const XrFrameState& frame) noexcept
        {
            if (mode_ < 2 || !request.issued || !request.id)
                return 0.0;

            OutRunVR::CadenceV1::ClientState client{};
            if (ReadClient(client) &&
                client.presentedRequestId == request.id)
                return 0.0;

            const double periodMs = frame.predictedDisplayPeriod > 0
                ? static_cast<double>(frame.predictedDisplayPeriod) / 1000000.0
                : 0.0;
            const DWORD budgetMs = static_cast<DWORD>(std::clamp(
                periodMs > 0.0 ? periodMs * 0.65 : 2.0,
                1.0, static_cast<double>(timeoutMs_)));

            LARGE_INTEGER start{}, end{}, frequency{};
            QueryPerformanceCounter(&start);
            QueryPerformanceFrequency(&frequency);
            WaitForSingleObject(presentedEvent_, budgetMs);
            QueryPerformanceCounter(&end);
            const double waitedMs =
                frequency.QuadPart > 0 && end.QuadPart >= start.QuadPart
                ? static_cast<double>(end.QuadPart - start.QuadPart) *
                    1000.0 / static_cast<double>(frequency.QuadPart)
                : 0.0;

            ++intervalSerialWaitCount_;
            intervalSerialWaitMs_ += waitedMs;
            intervalSerialWaitMaxMs_ =
                std::max(intervalSerialWaitMaxMs_, waitedMs);

            if (!ReadClient(client) ||
                client.presentedRequestId != request.id)
                ++intervalSerialTimeouts_;
            else
                ++intervalSerializedMatches_;
            return waitedMs;
        }

        void NoteFrame(bool freshProjection, bool cachedProjection,
            bool layerReady, std::uint32_t candidateRequestId) noexcept
        {
            if (freshProjection)
                ++intervalFresh_;
            else if (cachedProjection)
                ++intervalCached_;
            else if (!layerReady)
                ++intervalNoLayer_;
            else
                ++intervalOtherLayer_;

            lastCandidateRequestId_ = candidateRequestId;
            if (candidateRequestId)
            {
                ++intervalTaggedCandidates_;
                if (candidateRequestId == requestId_)
                    ++intervalLatestRequestMatches_;
            }
        }

        void AppendAndReset(std::ostringstream& out)
        {
            OutRunVR::CadenceV1::ClientState client{};
            const bool haveClient = ReadClient(client);
            const double ratio = activeRenderHz_ > 0.0f
                ? actualXrHz_ / static_cast<double>(activeRenderHz_) : 0.0;
            const double nearestInteger = std::round(ratio);
            const bool integerCadence = ratio >= 1.0 &&
                nearestInteger >= 1.0 &&
                std::fabs(ratio - nearestInteger) <= 0.02;
            out << " cadence={mode:" << mode_
                << ",configuredTargetHz:" << configuredTargetHz_
                << ",maxRenderHz:" << maxRenderHz_
                << ",activeRenderHz:" << activeRenderHz_
                << ",requestedRefreshHz:" << requestedRefreshHz_
                << ",actualXrHz:" << actualXrHz_
                << ",xrPeriodMs:" << actualDisplayPeriodMs_
                << ",xrGameRatio:" << ratio
                << ",ratioKind:" << (integerCadence ? "integer" : "fractional")
                << ",xr:" << intervalXrFrames_
                << ",req:" << intervalRequests_
                << ",fresh:" << intervalFresh_
                << ",cached:" << intervalCached_
                << ",other:" << intervalOtherLayer_
                << ",noLayer:" << intervalNoLayer_
                << ",tagged:" << intervalTaggedCandidates_
                << ",latestMatch:" << intervalLatestRequestMatches_
                << ",latestReq:" << requestId_
                << ",candidateReq:" << lastCandidateRequestId_
                << ",clientAccepted:"
                << (haveClient ? client.acceptedRequestId : 0u)
                << ",clientPresented:"
                << (haveClient ? client.presentedRequestId : 0u)
                << ",clientTimeouts:"
                << (haveClient ? client.timeoutCount : 0u)
                << ",clientWaitUs:"
                << (haveClient ? client.lastWaitUs : 0u)
                << ",serialMatch:" << intervalSerializedMatches_
                << ",serialTimeout:" << intervalSerialTimeouts_
                << ",serialWaitAvgMax:";
            const double avg = intervalSerialWaitCount_
                ? intervalSerialWaitMs_ /
                    static_cast<double>(intervalSerialWaitCount_)
                : 0.0;
            out << avg << "/" << intervalSerialWaitMaxMs_ << "}";

            intervalXrFrames_ = 0;
            intervalRequests_ = 0;
            intervalFresh_ = 0;
            intervalCached_ = 0;
            intervalOtherLayer_ = 0;
            intervalNoLayer_ = 0;
            intervalTaggedCandidates_ = 0;
            intervalLatestRequestMatches_ = 0;
            intervalSerializedMatches_ = 0;
            intervalSerialTimeouts_ = 0;
            intervalSerialWaitCount_ = 0;
            intervalSerialWaitMs_ = 0.0;
            intervalSerialWaitMaxMs_ = 0.0;
        }

    private:
        void ObserveRuntimeRefresh(XrDuration predictedDisplayPeriod) noexcept
        {
            if (predictedDisplayPeriod <= 0)
                return;
            const double periodMs =
                static_cast<double>(predictedDisplayPeriod) / 1000000.0;
            const double hz =
                1000000000.0 / static_cast<double>(predictedDisplayPeriod);
            if (!std::isfinite(periodMs) || !std::isfinite(hz) ||
                hz < 30.0 || hz > 180.0)
                return;

            actualDisplayPeriodMs_ = periodMs;
            // Small EWMA filters occasional runtime period noise without hiding
            // a real 72/80/90/120 Hz mode switch.
            actualXrHz_ = actualXrHz_ > 0.0
                ? actualXrHz_ * 0.90 + hz * 0.10
                : hz;

            if (lastLoggedXrHz_ <= 0.0 ||
                std::fabs(actualXrHz_ - lastLoggedXrHz_) >= 0.75)
            {
                lastLoggedXrHz_ = actualXrHz_;
                const double ratio = activeRenderHz_ > 0.0f
                    ? actualXrHz_ / static_cast<double>(activeRenderHz_) : 0.0;
                const double nearestInteger = std::round(ratio);
                const bool integerCadence = ratio >= 1.0 &&
                    nearestInteger >= 1.0 &&
                    std::fabs(ratio - nearestInteger) <= 0.02;
                std::cout
                    << "[R35 refresh] actualXrHz=" << actualXrHz_
                    << " displayPeriodMs=" << actualDisplayPeriodMs_
                    << " requestedOverrideHz=" << requestedRefreshHz_
                    << " configuredTargetHz=" << configuredTargetHz_
                    << " maxRenderHz=" << maxRenderHz_
                    << " activeRenderHz=" << activeRenderHz_
                    << " ratio=" << ratio
                    << " ratioKind="
                    << (integerCadence ? "integer" : "fractional")
                    << "\n";
            }
        }

        void Publish() noexcept
        {
            if (!host_)
                return;
            LONG seq = InterlockedIncrement(
                reinterpret_cast<volatile LONG*>(&host_->sequence));
            if ((seq & 1) == 0)
                InterlockedIncrement(
                    reinterpret_cast<volatile LONG*>(&host_->sequence));
            MemoryBarrier();
            host_->magic = OutRunVR::CadenceV1::HostMagic;
            host_->version = OutRunVR::CadenceV1::ProtocolVersion;
            host_->structSize = sizeof(*host_);
            host_->hostPid = GetCurrentProcessId();
            host_->flags =
                (enabled_ ? OutRunVR::CadenceV1::HostEnabled : 0u) |
                (running_ ? OutRunVR::CadenceV1::HostRunning : 0u) |
                (mode_ >= 2
                    ? OutRunVR::CadenceV1::HostSerializedProbe : 0u);
            host_->requestId = requestId_;
            host_->poseSequence = poseSequence_;
            host_->targetHzMilli = static_cast<std::uint32_t>(
                std::lround(std::max(0.0f, activeRenderHz_) * 1000.0f));
            host_->timeoutMs = timeoutMs_;
            host_->requestQpc = requestQpc_;
            host_->predictedDisplayTime = predictedDisplayTime_;
            host_->predictedDisplayPeriod = predictedDisplayPeriod_;
            MemoryBarrier();
            seq = InterlockedIncrement(
                reinterpret_cast<volatile LONG*>(&host_->sequence));
            if (seq & 1)
                InterlockedIncrement(
                    reinterpret_cast<volatile LONG*>(&host_->sequence));
        }

        bool EnsureClient() noexcept
        {
            if (client_)
                return true;
            clientMapping_ = OpenFileMappingW(
                FILE_MAP_READ, FALSE, OutRunVR::CadenceV1::ClientStateName);
            if (!clientMapping_)
                return false;
            client_ = static_cast<const OutRunVR::CadenceV1::ClientState*>(
                MapViewOfFile(clientMapping_, FILE_MAP_READ, 0, 0,
                    sizeof(OutRunVR::CadenceV1::ClientState)));
            if (!client_)
            {
                CloseHandle(clientMapping_);
                clientMapping_ = nullptr;
                return false;
            }
            return true;
        }

        bool ReadClient(OutRunVR::CadenceV1::ClientState& out) noexcept
        {
            if (!EnsureClient())
                return false;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                const std::uint32_t before = client_->sequence;
                if (before & 1u)
                    continue;
                MemoryBarrier();
                std::memcpy(&out, client_, sizeof(out));
                MemoryBarrier();
                const std::uint32_t after = client_->sequence;
                if (before == after && !(after & 1u) &&
                    out.magic == OutRunVR::CadenceV1::ClientMagic &&
                    out.version == OutRunVR::CadenceV1::ProtocolVersion &&
                    out.structSize == sizeof(out))
                    return true;
            }
            return false;
        }

        int mode_ = 0;
        float configuredTargetHz_ = 0.0f;
        float maxRenderHz_ = 120.0f;
        float activeRenderHz_ = 0.0f;
        float pendingRenderHz_ = 0.0f;
        std::uint32_t pendingRenderHzSamples_ = 0;
        std::uint32_t timeoutMs_ = 35;
        float requestedRefreshHz_ = 0.0f;
        double actualXrHz_ = 0.0;
        double actualDisplayPeriodMs_ = 0.0;
        double lastLoggedXrHz_ = 0.0;
        bool enabled_ = false;
        bool running_ = false;
        HANDLE hostMapping_ = nullptr;
        OutRunVR::CadenceV1::HostState* host_ = nullptr;
        HANDLE clientMapping_ = nullptr;
        const OutRunVR::CadenceV1::ClientState* client_ = nullptr;
        HANDLE requestEvent_ = nullptr;
        HANDLE presentedEvent_ = nullptr;
        std::uint32_t requestId_ = 0;
        std::uint32_t poseSequence_ = 0;
        std::int64_t requestQpc_ = 0;
        XrTime predictedDisplayTime_ = 0;
        XrDuration predictedDisplayPeriod_ = 0;
        XrTime nextRequestDisplayTime_ = 0;

        std::uint64_t intervalXrFrames_ = 0;
        std::uint64_t intervalRequests_ = 0;
        std::uint64_t intervalFresh_ = 0;
        std::uint64_t intervalCached_ = 0;
        std::uint64_t intervalOtherLayer_ = 0;
        std::uint64_t intervalNoLayer_ = 0;
        std::uint64_t intervalTaggedCandidates_ = 0;
        std::uint64_t intervalLatestRequestMatches_ = 0;
        std::uint64_t intervalSerializedMatches_ = 0;
        std::uint64_t intervalSerialTimeouts_ = 0;
        std::uint64_t intervalSerialWaitCount_ = 0;
        double intervalSerialWaitMs_ = 0.0;
        double intervalSerialWaitMaxMs_ = 0.0;
        std::uint32_t lastCandidateRequestId_ = 0;
    };

}

int main(int argc, char** argv)
{
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;

    try
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ParseRuntimeOverride(argc, argv);
        const float renderScale = ReadRenderScale(argc, argv);
        const bool directTransportEnabled = DirectTransportEnabled();
        const bool directTransportOnly =
            directTransportEnabled && DirectTransportOnly();
        const bool disableDesktopDuplication = DisableDesktopDuplication();
        const float targetRefreshRateHz = RequestedRefreshRateHz();
        const int cadenceMode = std::clamp(
            R35ReadEnvInt("OUTRUN_VR_CADENCE_MODE", 1), 0, 2);
        const float cadenceTargetHz = std::clamp(
            R35ReadEnvFloat("OUTRUN_VR_CADENCE_TARGET_HZ", 0.0f),
            0.0f, 120.0f);
        const float cadenceMaxHz = std::clamp(
            R35ReadEnvFloat("OUTRUN_VR_CADENCE_MAX_HZ", 120.0f),
            60.0f, 120.0f);
        const std::uint32_t cadenceTimeoutMs =
            static_cast<std::uint32_t>(std::clamp(
                R35ReadEnvFloat("OUTRUN_VR_CADENCE_TIMEOUT_MS", 35.0f),
                5.0f, 100.0f));
        HWND gameWindow = WaitForGameWindow();
        DWORD gamePid = 0; GetWindowThreadProcessId(gameWindow, &gamePid);

        if (!HasExtension(XR_KHR_D3D11_ENABLE_EXTENSION_NAME))
            throw std::runtime_error("runtime lacks XR_KHR_D3D11_enable");

        std::vector<const char*> extensions{
            XR_KHR_D3D11_ENABLE_EXTENSION_NAME
        };
        bool displayRefreshExtensionEnabled = false;
#ifdef XR_FB_display_refresh_rate
        if (targetRefreshRateHz > 0.0f &&
            HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
        {
            extensions.push_back(
                XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
            displayRefreshExtensionEnabled = true;
        }
#endif

        XrInstanceCreateInfo ii{ XR_TYPE_INSTANCE_CREATE_INFO };
        strncpy_s(ii.applicationInfo.applicationName, sizeof(ii.applicationInfo.applicationName),
            "OutRun 2006 True Stereo VR", _TRUNCATE);
        ii.applicationInfo.applicationVersion = 1;
        strncpy_s(ii.applicationInfo.engineName, sizeof(ii.applicationInfo.engineName),
            "OutRun2006Tweaks", _TRUNCATE);
        ii.applicationInfo.engineVersion = 1;
        ii.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        ii.enabledExtensionCount =
            static_cast<std::uint32_t>(extensions.size());
        ii.enabledExtensionNames = extensions.data();
        CheckXr(xrCreateInstance(&ii, &instance), "xrCreateInstance");

        XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
        CheckXr(xrGetInstanceProperties(instance, &ip), "xrGetInstanceProperties");
        std::cout << "OpenXR runtime: " << ip.runtimeName << " "
                  << XR_VERSION_MAJOR(ip.runtimeVersion) << "."
                  << XR_VERSION_MINOR(ip.runtimeVersion) << "."
                  << XR_VERSION_PATCH(ip.runtimeVersion) << "\n";

        XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        for (;;)
        {
            const XrResult r = xrGetSystem(instance, &sgi, &system);
            if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) { Sleep(1000); continue; }
            CheckXr(r, "xrGetSystem"); break;
        }

        PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
        CheckXr(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&getReq)), "xrGetInstanceProcAddr");
        XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
        CheckXr(getReq(instance, system, &req), "xrGetD3D11GraphicsRequirementsKHR");
        D3DObjects d3d = CreateD3D11Device(req);

        XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
        binding.device = d3d.device;
        XrSessionCreateInfo si{ XR_TYPE_SESSION_CREATE_INFO };
        si.next = &binding; si.systemId = system;
        CheckXr(xrCreateSession(instance, &si, &session), "xrCreateSession");

        std::cout
            << "VR transport: directEnabled="
            << (directTransportEnabled ? 1 : 0)
            << " directOnly=" << (directTransportOnly ? 1 : 0)
            << " refreshOverrideHz=" << targetRefreshRateHz
            << " refreshPolicy="
            << (targetRefreshRateHz > 0.0f ? "explicit-request" : "runtime-owned")
            << " cadenceMode=" << cadenceMode
            << " cadenceTargetHz=" << cadenceTargetHz
            << " cadenceMaxHz=" << cadenceMaxHz
            << " cadenceTimeoutMs=" << cadenceTimeoutMs
            << "\n";
#ifdef XR_FB_display_refresh_rate
        if (displayRefreshExtensionEnabled)
            TryRequestDisplayRefreshRate(
                instance, session, targetRefreshRateHz);
        else if (targetRefreshRateHz > 0.0f)
            std::cout
                << "XR_FB_display_refresh_rate unavailable; runtime refresh rate unchanged.\n";
#else
        if (targetRefreshRateHz > 0.0f)
            std::cout
                << "OpenXR headers do not expose XR_FB_display_refresh_rate; runtime refresh rate unchanged.\n";
#endif

        XrPosef identity{}; identity.orientation.w = 1;
        XrReferenceSpaceCreateInfo li{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        li.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; li.poseInReferenceSpace = identity;
        CheckXr(xrCreateReferenceSpace(session, &li, &localSpace), "xrCreateReferenceSpace LOCAL");
        XrReferenceSpaceCreateInfo vi{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        vi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; vi.poseInReferenceSpace = identity;
        CheckXr(xrCreateReferenceSpace(session, &vi, &viewSpace), "xrCreateReferenceSpace VIEW");

        std::uint32_t cc = 0;
        CheckXr(xrEnumerateViewConfigurationViews(instance, system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &cc, nullptr),
            "xrEnumerateViewConfigurationViews count");
        if (cc < 2) throw std::runtime_error("runtime has fewer than two stereo views");
        std::vector<XrViewConfigurationView> cv(cc);
        for (auto& c : cv) c = { XR_TYPE_VIEW_CONFIGURATION_VIEW };
        CheckXr(xrEnumerateViewConfigurationViews(instance, system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, cc, &cc, cv.data()),
            "xrEnumerateViewConfigurationViews list");
        std::array<XrViewConfigurationView, 2> configs{ cv[0], cv[1] };

        SharedWriter shared(req.adapterLuid);
        R35CadenceHost cadence(cadenceMode, cadenceTargetHz,
            cadenceMaxHz, cadenceTimeoutMs, targetRefreshRateHz);
        RenderFrameReader renderFrames;
        StereoCompositor compositor(session, d3d.device, d3d.context, gameWindow,
            configs, directTransportEnabled, directTransportOnly,
            disableDesktopDuplication, renderScale);
        compositor.Initialize();
        ViewHistory viewHistory;
        HostTimings timings;
        std::ofstream pipelineLog("outrun-vr-host-pipeline.log",
            std::ios::out | std::ios::trunc);
        LARGE_INTEGER qpcFrequency{};
        QueryPerformanceFrequency(&qpcFrequency);
        ULONGLONG lastPipelineTelemetryMs = 0;
        R23PipelineWindow pipelineWindow;
        const XrEnvironmentBlendMode blend = ChooseBlendMode(instance, system);

        bool running = false, quit = false, exitRequested = false;
        ULONGLONG exitRequestMs = 0;
        XrSessionState state = XR_SESSION_STATE_UNKNOWN;
        LARGE_INTEGER lastXrWaitReturnQpc{};
        OutRunVR::ClientPresentationMode lastPresentation = OutRunVR::PresentationUnknown;
        std::array<XrView, 2> matchedViews{};
        bool matchedStereoValid = false;
        std::uint32_t lastProcessedStereoFrame = 0;
        ULONGLONG lastStereoMatchMs = 0;
        std::array<XrCompositionLayerProjectionView, 2>
            cachedProjectionViews{};
        bool cachedProjectionValid = false;
        ULONGLONG cachedProjectionRenderedMs = 0;
        std::array<XrCompositionLayerProjectionView, 2>
            cachedMenuProjectionViews{};
        bool cachedMenuProjectionValid = false;
        bool awaitingFirstGameplayStereo = false;
        XrPosef menuProjectionAnchor{};
        menuProjectionAnchor.orientation.w = 1.0f;
        bool menuProjectionAnchorValid = false;
        bool refreshMenuAnchorAfterLocate = true;
        bool pendingReferenceSpaceChange = false;
        XrTime pendingReferenceSpaceChangeTime = 0;

        while (!quit)
        {
            // A global legacy bridge can outlive a fast game restart. If a new
            // live x86 client takes ownership while this host is still bound to
            // an older game window, shut down cleanly so the game's auto-launch
            // helper can start a host attached to the new PID.
            const DWORD publishedClientPid = SharedClientPid();
            if (!exitRequested && publishedClientPid &&
                publishedClientPid != gamePid &&
                QueryProcessLiveness(publishedClientPid) == ProcessLiveness::Alive &&
                FindGameWindow(publishedClientPid))
            {
                exitRequested = true;
                exitRequestMs = GetTickCount64();
                OutRunVrR23VerifiedBundle::Invalidate();
                if (session != XR_NULL_HANDLE)
                    xrRequestExitSession(session);
                std::cout << "VR client changed from pid=" << gamePid
                          << " to pid=" << publishedClientPid
                          << "; restarting host binding.\n";
            }

            if (!compositor.GameAlive() && !exitRequested)
            {
                exitRequested = true; exitRequestMs = GetTickCount64();
                OutRunVrR23VerifiedBundle::Invalidate();
                if (session != XR_NULL_HANDLE) xrRequestExitSession(session);
                std::cout << "OutRun exited; requesting OpenXR shutdown.\n";
            }

            XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
            while (xrPollEvent(instance, &event) == XR_SUCCESS)
            {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                {
                    auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                    state = e->state;
                    if (state == XR_SESSION_STATE_READY && !running)
                    {
                        XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
                        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        CheckXr(xrBeginSession(session, &begin), "xrBeginSession");
                        running = true;
                        cadence.SetRunning(true);
                    }
                    else if (state == XR_SESSION_STATE_STOPPING && running)
                    {
                        CheckXr(xrEndSession(session), "xrEndSession");
                        running = false; cadence.SetRunning(false); viewHistory.Clear(); matchedStereoValid = false;
                        lastStereoMatchMs = 0; cachedProjectionValid = false;
                        cachedProjectionRenderedMs = 0;
                        cachedMenuProjectionValid = false;
                        awaitingFirstGameplayStereo = false;
                        menuProjectionAnchorValid = false;
                        refreshMenuAnchorAfterLocate = true;
                        compositor.ReferenceSpaceChanged();
                        OutRunVrR23VerifiedBundle::Invalidate();
                        OutRunVR::SharedRenderFrameState rf{};
                        if (renderFrames.Read(rf)) lastProcessedStereoFrame = rf.frameId;
                    }
                    else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING)
                        quit = true;
                }
                else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
                {
                    const auto* e = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
                    if (e->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
                    { pendingReferenceSpaceChange = true; pendingReferenceSpaceChangeTime = e->changeTime; }
                }
                event = { XR_TYPE_EVENT_DATA_BUFFER };
            }

            if (quit) break;
            if (exitRequested && GetTickCount64() - exitRequestMs > 2000) break;
            if (!running) { Sleep(10); continue; }

            XrFrameWaitInfo wi{ XR_TYPE_FRAME_WAIT_INFO };
            XrFrameState fs{ XR_TYPE_FRAME_STATE };
            LARGE_INTEGER ws{}, we{}; QueryPerformanceCounter(&ws);
            CheckXr(xrWaitFrame(session, &wi, &fs), "xrWaitFrame");
            QueryPerformanceCounter(&we);
            const double xrWaitFrameMs = timings.Ms(ws, we);
            timings.wait.Add(xrWaitFrameMs);
            const double xrFrameIntervalMs =
                lastXrWaitReturnQpc.QuadPart > 0 && qpcFrequency.QuadPart > 0 &&
                we.QuadPart >= lastXrWaitReturnQpc.QuadPart
                ? static_cast<double>(we.QuadPart - lastXrWaitReturnQpc.QuadPart) *
                    1000.0 / static_cast<double>(qpcFrequency.QuadPart)
                : 0.0;
            lastXrWaitReturnQpc = we;

            if (pendingReferenceSpaceChange &&
                (pendingReferenceSpaceChangeTime == 0 || fs.predictedDisplayTime >= pendingReferenceSpaceChangeTime))
            {
                shared.ReferenceSpaceChanged(); compositor.ReferenceSpaceChanged(); viewHistory.Clear();
                matchedStereoValid = false; cachedProjectionValid = false;
                cachedProjectionRenderedMs = 0;
                cachedMenuProjectionValid = false;
                awaitingFirstGameplayStereo = false;
                menuProjectionAnchorValid = false;
                refreshMenuAnchorAfterLocate = true;
                OutRunVrR23VerifiedBundle::Invalidate();
                OutRunVR::SharedRenderFrameState rf{};
                lastProcessedStereoFrame = renderFrames.Read(rf) ? rf.frameId : shared.ReadStereoMeta().frame;
                pendingReferenceSpaceChange = false; pendingReferenceSpaceChangeTime = 0;
            }

            XrFrameBeginInfo bi{ XR_TYPE_FRAME_BEGIN_INFO };
            CheckXr(xrBeginFrame(session, &bi), "xrBeginFrame");
            // F10/focus recenter is an application-space operation, not merely
            // cache invalidation. Apply it before every pose/view locate so the
            // current frame and all submitted layer spaces share one origin.
            OutRunVrR26RecenterHardening::ApplyPendingApplicationRecenter(
                session, viewSpace, localSpace, fs.predictedDisplayTime);
            XrSpaceLocation head{ XR_TYPE_SPACE_LOCATION };
            CheckXr(xrLocateSpace(viewSpace, localSpace, fs.predictedDisplayTime, &head), "xrLocateSpace");
            std::array<XrView, 2> views{}; for (auto& v : views) v = { XR_TYPE_VIEW };
            XrViewState vs{ XR_TYPE_VIEW_STATE };
            XrViewLocateInfo vl{ XR_TYPE_VIEW_LOCATE_INFO };
            vl.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vl.displayTime = fs.predictedDisplayTime; vl.space = localSpace;
            std::uint32_t vc = 0;
            CheckXr(xrLocateViews(session, &vl, &vs, 2, &vc, views.data()), "xrLocateViews");

            constexpr XrSpaceLocationFlags R45MenuAnchorFlags =
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if (refreshMenuAnchorAfterLocate &&
                (head.locationFlags & R45MenuAnchorFlags) ==
                    R45MenuAnchorFlags)
            {
                menuProjectionAnchor = head.pose;
                menuProjectionAnchorValid = true;
                refreshMenuAnchorAfterLocate = false;
                cachedMenuProjectionValid = false;
                std::cout
                    << "[R45 menu] LOCAL projection anchor captured from current HMD center\n";
            }

            const std::uint32_t hostSequence = shared.Write(head, views, vc, configs,
                state, vs.viewStateFlags, fs.shouldRender == XR_TRUE, directTransportEnabled,
                compositor.DirectTransportReady(), ip.runtimeName);
            if (directTransportEnabled) shared.ServiceInteropProbe(d3d.device, d3d.context);
            const XrViewStateFlags neededViews = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                XR_VIEW_STATE_POSITION_VALID_BIT;
            if (vc >= 2 && (vs.viewStateFlags & neededViews) == neededViews)
                viewHistory.Store(hostSequence, views);

            // R35: xrBeginFrame has already succeeded and the due pose has
            // been published. Only now release the next game frame.
            const auto cadenceRequest =
                cadence.IssueIfDue(fs, hostSequence);
            const double cadenceSerialWaitMs =
                cadence.WaitForPresented(cadenceRequest, fs);

            XrFrameEndInfo end{ XR_TYPE_FRAME_END_INFO };
            end.displayTime = fs.predictedDisplayTime; end.environmentBlendMode = blend;
            const XrCompositionLayerBaseHeader* layers[1]{};
            std::array<XrCompositionLayerProjectionView, 2> pv{};
            XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
            XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };

            const auto requestedPresentation = shared.Presentation();
            auto presentation = requestedPresentation;

            // T0/R46+P0: Gameplay can be announced before a releasable
            // stereo packet exists, and Frame.v2 may survive a fast game restart.
            // RenderFrameReader already rejects stale run identities; require the
            // same completed world/pose invariants used by production selection
            // before leaving the visible menu/loading projection.
            if (requestedPresentation == OutRunVR::PresentationGameplay &&
                lastPresentation != OutRunVR::PresentationGameplay)
            {
                OutRunVR::SharedRenderFrameState bootstrapFrame{};
                const bool haveBootstrapFrame =
                    renderFrames.Read(bootstrapFrame) &&
                    R23UsableGameplayBootstrapFrame(bootstrapFrame);
                if (!haveBootstrapFrame)
                    presentation = OutRunVR::PresentationTheater;
            }

            // R45: the game now publishes presentation from an explicit
            // GameState whitelist. Do not retain a stale stereo projection for
            // 750 ms after it says Theater; that hold was enough to transform
            // selector/result frames as gameplay and corrupt the preview car.

            if (presentation != lastPresentation)
            {
                compositor.ReferenceSpaceChanged(); matchedStereoValid = false;
                OutRunVrR23VerifiedBundle::Invalidate();
                OutRunVR::SharedRenderFrameState rf{};
                lastProcessedStereoFrame = renderFrames.Read(rf) ? rf.frameId : shared.ReadStereoMeta().frame;
                if (presentation != OutRunVR::PresentationGameplay)
                {
                    cachedProjectionValid = false;
                    cachedProjectionRenderedMs = 0;
                    cachedMenuProjectionValid = false;
                    awaitingFirstGameplayStereo = false;
                    if ((head.locationFlags & R45MenuAnchorFlags) ==
                        R45MenuAnchorFlags)
                    {
                        menuProjectionAnchor = head.pose;
                        menuProjectionAnchorValid = true;
                        refreshMenuAnchorAfterLocate = false;
                    }
                    else
                    {
                        menuProjectionAnchorValid = false;
                        refreshMenuAnchorAfterLocate = true;
                    }
                }
                else
                {
                    // Keep the last visible menu/loading projection only until
                    // the first actual gameplay stereo projection is released.
                    awaitingFirstGameplayStereo = true;
                }
                lastPresentation = presentation;
                std::cout << "VR presentation R45: "
                    << (presentation == OutRunVR::PresentationGameplay ? "true stereo projection" : "LOCAL-fixed mono 2D projection menu")
                    << ".\n";
            }

            bool layerReady = false;
            bool intentionalMonoProjection = false;
            const char* finalLayerKind = "none";
            const char* candidateRejectReason = "not-evaluated";
            std::uint32_t candidateGameFrameId = 0;
            std::uint32_t candidatePoseSequence = 0;
            std::int64_t candidateGamePresentQpc = 0;
            std::uint32_t candidateCadenceRequestId = 0;
            double candidateConsumeAgeMs = -1.0;
            bool frameFreshProjection = false;
            bool frameCachedProjection = false;
            std::int64_t candidateCaptureQpc = 0;
            double candidateCaptureAgeMs = -1.0;
            double candidateCaptureMs = 0.0;
            double candidateCommitCopyMs = 0.0;
            double frameRenderMs = 0.0;
            if (fs.shouldRender == XR_TRUE && vc >= 2)
            {
                if (presentation == OutRunVR::PresentationGameplay)
                {
                    bool productionCaptureAttempted = false;
                    bool newStereoCommitted = false;
                    bool pendingBundlePublish = false;
                    OutRunVR::SharedRenderFrameState pendingBundleFrame{};
                    OutRunVrR23VerifiedBundle::SourceKind pendingBundleSource =
                        OutRunVrR23VerifiedBundle::SourceKind::None;
                    std::int64_t pendingBundleCaptureQpc = 0;
                    OutRunVR::SharedRenderFrameState before{};
                    bool have = renderFrames.Read(before);

                    // R41 DirectGPU latest-frame-wins. R37 walked the four
                    // producer slots oldest-first, which made VDXR display a
                    // permanent ~4-frame / 40-50 ms history behind the newest
                    // game Present. Select the newest complete frame in the
                    // current transport generation. Frames skipped without ever
                    // being sampled by D3D11 are immediately per-slot ACKed so
                    // the producer can recycle them; the selected frame keeps
                    // R32's GPU EVENT completion ACK.
                    if (directTransportOnly)
                    {
                        std::array<OutRunVR::SharedRenderFrameState,
                            OutRunVR::RenderFrameRingSize> history{};
                        std::size_t historyCount = 0;
                        OutRunVR::SharedRenderFrameState selectedDirect{};
                        bool foundDirect = false;
                        if (renderFrames.ReadHistory(history, historyCount))
                        {
                            std::uint32_t currentGeneration = 0;
                            std::uint32_t newestDirectFrame = 0;
                            bool haveCurrentGeneration = false;
                            for (std::size_t i = 0; i < historyCount; ++i)
                            {
                                const auto& frame = history[i];
                                const std::uint32_t generation =
                                    frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex];
                                if (!frame.frameId || !generation ||
                                    frame.state != OutRunVR::StereoSbsActive ||
                                    (frame.flags & OutRunVR::RenderFramePresentInFlight) != 0 ||
                                    (frame.flags & OutRunVR::RenderFrameDirectGpuTransport) == 0)
                                    continue;
                                if (!haveCurrentGeneration ||
                                    R37FrameIdBefore(
                                        newestDirectFrame, frame.frameId))
                                {
                                    newestDirectFrame = frame.frameId;
                                    currentGeneration = generation;
                                    haveCurrentGeneration = true;
                                }
                            }

                            for (std::size_t i = 0; i < historyCount; ++i)
                            {
                                const auto& frame = history[i];
                                const std::uint32_t slot =
                                    frame.reserved[OutRunVR::RenderFrameDirectSlotIndex];
                                const std::uint32_t generation =
                                    frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex];
                                if (!frame.frameId ||
                                    (lastProcessedStereoFrame != 0 &&
                                     !R37FrameIdBefore(
                                         lastProcessedStereoFrame,
                                         frame.frameId)) ||
                                    frame.state != OutRunVR::StereoSbsActive ||
                                    (frame.flags & OutRunVR::RenderFramePresentInFlight) != 0 ||
                                    (frame.flags & OutRunVR::RenderFrameDirectGpuTransport) == 0 ||
                                    slot >= OutRunVR::RenderFrameRingSize ||
                                    generation == 0 ||
                                    !haveCurrentGeneration ||
                                    generation != currentGeneration)
                                    continue;

                                if (!foundDirect ||
                                    R37FrameIdBefore(
                                        selectedDirect.frameId, frame.frameId))
                                {
                                    selectedDirect = frame;
                                    foundDirect = true;
                                }
                            }

                            if (foundDirect)
                            {
                                for (std::size_t i = 0; i < historyCount; ++i)
                                {
                                    const auto& frame = history[i];
                                    const std::uint32_t slot =
                                        frame.reserved[OutRunVR::RenderFrameDirectSlotIndex];
                                    const std::uint32_t generation =
                                        frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex];
                                    if (!frame.frameId ||
                                        frame.frameId == selectedDirect.frameId ||
                                        (lastProcessedStereoFrame != 0 &&
                                         !R37FrameIdBefore(
                                             lastProcessedStereoFrame,
                                             frame.frameId)) ||
                                        frame.state != OutRunVR::StereoSbsActive ||
                                        (frame.flags & OutRunVR::RenderFramePresentInFlight) != 0 ||
                                        (frame.flags & OutRunVR::RenderFrameDirectGpuTransport) == 0 ||
                                        slot >= OutRunVR::RenderFrameRingSize ||
                                        generation != currentGeneration)
                                        continue;

                                    // No D3D11 draw/copy references this skipped
                                    // frame, so producer reuse is safe immediately.
                                    if (OutRunVrD3D9ExDirectPassthrough::
                                            PublishCompletedFrame(frame))
                                    {
                                        R37BootstrapSubmittedFrame[slot] =
                                            frame.frameId;
                                        R37BootstrapSubmittedGeneration[slot] =
                                            generation;
                                    }
                                }

                                before = selectedDirect;
                                have = true;
                                if (!R36FirstDirectBootstrapLogged)
                                {
                                    R36FirstDirectBootstrapLogged = true;
                                    std::cout
                                        << "[R41] DirectGPU latest-frame-wins active; selected frame="
                                        << before.frameId
                                        << " slot="
                                        << before.reserved[OutRunVR::RenderFrameDirectSlotIndex]
                                        << " generation="
                                        << before.reserved[OutRunVR::RenderFrameDirectGenerationIndex]
                                        << "; older unsampled ring frames are ACKed immediately.\n";
                                }
                            }
                            else
                            {
                                // R41: direct-only mode must never fall back to
                                // renderFrames.Read(before), because that snapshot
                                // can be an already displayed older ring entry.
                                have = false;
                            }
                        }
                        else
                        {
                            have = false;
                        }
                    }

                    constexpr std::uint32_t need = OutRunVR::RenderFrameStereoComplete |
                        OutRunVR::RenderFrameWorldStereo | OutRunVR::RenderFrameDrawDuplicated |
                        OutRunVR::RenderFrameEffectivePoseValid;
                    if (have)
                    {
                        candidateGameFrameId = before.frameId;
                        candidatePoseSequence = before.sourcePoseSequence;
                        candidateGamePresentQpc = before.presentQpc;
                        candidateCadenceRequestId =
                            before.reserved[OutRunVR::RenderFrameCadenceRequestIndex];
                    }
                    if (!have)
                        candidateRejectReason = "no-frame-state";
                    else if ((before.flags & OutRunVR::RenderFramePresentInFlight) != 0)
                        candidateRejectReason = "present-in-flight";
                    else if (before.state != OutRunVR::StereoSbsActive)
                        candidateRejectReason = "not-sbs-active";
                    else if (!before.frameId)
                        candidateRejectReason = "zero-frame-id";
                    else if (before.frameId == lastProcessedStereoFrame)
                        candidateRejectReason = "already-processed";
                    else if (!before.sourcePoseSequence)
                        candidateRejectReason = "zero-pose-sequence";
                    else if ((before.flags & need) != need)
                        candidateRejectReason = "incomplete-frame-flags";
                    else
                        candidateRejectReason = "candidate";

                    if (have &&
                        (before.flags & OutRunVR::RenderFramePresentInFlight) == 0 &&
                        before.state == OutRunVR::StereoSbsActive &&
                        before.frameId &&
                        before.frameId != lastProcessedStereoFrame &&
                        before.sourcePoseSequence &&
                        (before.flags & need) == need)
                    {
                        OutRunVR::SharedRenderFrameState candidate = before;
                        bool directFrame =
                            (candidate.flags &
                             OutRunVR::RenderFrameDirectGpuTransport) != 0;
                        bool candidateReady = directFrame;
                        CaptureStatus capture{};

                        LARGE_INTEGER cs{}, ce{};
                        QueryPerformanceCounter(&cs);
                        if (!directFrame && directTransportOnly)
                        {
                            candidateReady = false;
                            candidateRejectReason =
                                "direct-only-classic-rejected";
                        }
                        else if (!directFrame)
                        {
                            productionCaptureAttempted = true;
                            capture = R23Capture(compositor, 2);
                            candidateCaptureQpc = capture.lastPresentQpc;

                            // LastPresentTime is output-global. Require the
                            // complete OutRun client to be visible and require a
                            // dirty/move update that intersects that client.
                            // Then correlate against the four-entry Frame.v2
                            // history and choose the closest game Present at or
                            // before the captured output frame.
                            const std::int64_t maxSkewTicks =
                                qpcFrequency.QuadPart > 0
                                ? qpcFrequency.QuadPart / 10 // 100 ms
                                : 0;
                            candidateReady =
                                R23SelectClassicFrameForCapture(
                                    renderFrames, capture,
                                    lastProcessedStereoFrame,
                                    need, candidate, maxSkewTicks);
                            if (candidateReady)
                            {
                                candidateGameFrameId = candidate.frameId;
                                candidatePoseSequence =
                                    candidate.sourcePoseSequence;
                                candidateGamePresentQpc =
                                    candidate.presentQpc;
                            }
                        }
                        QueryPerformanceCounter(&ce);
                        candidateCaptureMs = timings.Ms(cs, ce);
                        timings.capture.Add(candidateCaptureMs);

                        if (!directFrame && candidateCaptureQpc > 0 &&
                            qpcFrequency.QuadPart > 0)
                        {
                            LARGE_INTEGER qpcNow{};
                            QueryPerformanceCounter(&qpcNow);
                            if (qpcNow.QuadPart >= candidateCaptureQpc)
                            {
                                candidateCaptureAgeMs =
                                    static_cast<double>(
                                        qpcNow.QuadPart -
                                        candidateCaptureQpc) *
                                    1000.0 /
                                    static_cast<double>(
                                        qpcFrequency.QuadPart);
                            }
                        }

                        if (!candidateReady)
                        {
                            if (!directFrame && directTransportOnly)
                            {
                                candidateRejectReason =
                                    "direct-only-classic-rejected";
                            }
                            else if (!directFrame)
                            {
                                if (!capture.available)
                                    candidateRejectReason =
                                        "capture-unavailable";
                                else if (!capture.fullGameClientVisible)
                                    candidateRejectReason =
                                        "sbs-client-clipped";
                                else if (!capture.gameRegionChanged)
                                    candidateRejectReason =
                                        "desktop-update-outside-game";
                                else
                                    candidateRejectReason =
                                        "capture-frame-history-miss";
                            }
                        }
                        else
                        {
                            std::array<XrView, 2> history{};
                            if (!viewHistory.Find(
                                    candidate.sourcePoseSequence,
                                    history))
                            {
                                candidateReady = false;
                                candidateRejectReason =
                                    "pose-history-miss";
                            }
                        }

                        // DirectGPU already carries image, frameId, pose and
                        // resource generation in one ring entry. Classic SBS
                        // was re-selected from stable frame history above.
                        const bool same =
                            candidateReady &&
                            R23FrameUnchanged(
                                renderFrames, candidate);
                        if (candidateReady && !same)
                            candidateRejectReason =
                                "selected-frame-mutated";

                        bool committed = false;
                        if (same)
                        {
                            LARGE_INTEGER cms{}, cme{};
                            QueryPerformanceCounter(&cms);
                            committed = directFrame
                                ? R23CommitDirectAfterValidation(
                                    compositor, candidate)
                                : R23CommitClassicAfterValidation(
                                    compositor);
                            QueryPerformanceCounter(&cme);
                            candidateCommitCopyMs =
                                timings.Ms(cms, cme);
                            if (!committed)
                                candidateRejectReason =
                                    "commit-copy-failed";
                        }

                        if (committed)
                        {
                            // Do not publish the legacy global consumed-frame
                            // ACK here. D3D11 has only queued the sampling/copy
                            // work at this point. R32 arms an EVENT after the
                            // actual projection commands and publishes the
                            // dedicated per-slot GPU-completion ACK only when
                            // GetData reports completion.
                            R23CopyMatchedViews(candidate, matchedViews);
                            matchedStereoValid = true;
                            newStereoCommitted = true;
                            candidateRejectReason = "committed-pending-render";
                            if (candidate.presentQpc > 0 && qpcFrequency.QuadPart > 0)
                            {
                                LARGE_INTEGER consumedNow{};
                                QueryPerformanceCounter(&consumedNow);
                                if (consumedNow.QuadPart >= candidate.presentQpc)
                                    candidateConsumeAgeMs =
                                        static_cast<double>(consumedNow.QuadPart - candidate.presentQpc) *
                                        1000.0 / static_cast<double>(qpcFrequency.QuadPart);
                            }

                            // Do not replace the verified/display authority yet.
                            // The source becomes authoritative only after a new
                            // projection swapchain image was rendered
                            // successfully. If rendering fails, the previously
                            // released cached projection and bundle stay paired.
                            pendingBundlePublish = true;
                            pendingBundleFrame = candidate;
                            pendingBundleSource = directFrame
                                ? OutRunVrR23VerifiedBundle::SourceKind::DirectGpu
                                : OutRunVrR23VerifiedBundle::SourceKind::ClassicSbs;
                            pendingBundleCaptureQpc =
                                directFrame ? 0 : candidateCaptureQpc;
                        }
                    }

                    const ULONGLONG projectionNow = GetTickCount64();
                    const bool grace =
                        newStereoCommitted ||
                        (matchedStereoValid &&
                         compositor.HasStereoSource() &&
                         projectionNow >= lastStereoMatchMs &&
                         projectionNow - lastStereoMatchMs <= StereoGraceMs);
                    // T0/R47: while Gameplay remains active, never age
                    // out the last successfully released stereo projection.
                    // Presentation/session/reference-space transitions already
                    // invalidate this cache, so a producer stall freezes the
                    // last good stereo image instead of submitting a solid or
                    // empty compositor frame.
                    const bool cachedHold = cachedProjectionValid;

                    LARGE_INTEGER rs{}, re{}; QueryPerformanceCounter(&rs);

                    // R42: projection pixels are refreshed only when a new game
                    // source was committed (or when no released projection
                    // exists yet). Intermediate xrWaitFrame ticks resubmit the
                    // same released projection image without another pair of
                    // full-eye blits. R32 keeps the exact producer slot protected
                    // by the original async EVENT, so same-frame resubmission no
                    // longer needs the expensive SafeEye recovery path.
                    const bool hadCachedProjection = cachedProjectionValid;
                    const bool projectionRefreshNeeded =
                        newStereoCommitted || !cachedProjectionValid;
                    if (grace && projectionRefreshNeeded &&
                        R23RenderProjection(compositor, matchedViews, pv))
                    {
                        ++R42ProjectionRefreshes;
                        projection.space = localSpace;
                        projection.viewCount = 2;
                        projection.views = pv.data();
                        layers[0] =
                            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                                &projection);
                        layerReady = true;
                        finalLayerKind = "projection-fresh";
                        frameFreshProjection = true;
                        cachedProjectionViews = pv;
                        cachedProjectionValid = true;
                        // Track source freshness, not repeated XR redraws. This
                        // lets gameplay->menu/loading debounce expire normally.
                        if (newStereoCommitted || !hadCachedProjection)
                            cachedProjectionRenderedMs = projectionNow;
                        if (pendingBundlePublish)
                        {
                            OutRunVrR23VerifiedBundle::Publish(
                                pendingBundleFrame,
                                pendingBundleSource,
                                pendingBundleCaptureQpc);
                            if (pendingBundleSource ==
                                OutRunVrR23VerifiedBundle::SourceKind::DirectGpu)
                            {
                                const std::uint32_t slot =
                                    pendingBundleFrame.reserved[
                                        OutRunVR::RenderFrameDirectSlotIndex];
                                const std::uint32_t generation =
                                    pendingBundleFrame.reserved[
                                        OutRunVR::RenderFrameDirectGenerationIndex];
                                if (slot < OutRunVR::RenderFrameRingSize &&
                                    generation != 0)
                                {
                                    R37BootstrapSubmittedFrame[slot] =
                                        pendingBundleFrame.frameId;
                                    R37BootstrapSubmittedGeneration[slot] =
                                        generation;
                                }
                            }
                            lastProcessedStereoFrame =
                                pendingBundleFrame.frameId;
                            lastStereoMatchMs = projectionNow;
                            candidateRejectReason = "displayed-fresh";
                            pendingBundlePublish = false;
                        }
                    }
                    else if (cachedProjectionValid && (grace || cachedHold))
                    {
                        projection.space = localSpace;
                        projection.viewCount = 2;
                        projection.views = cachedProjectionViews.data();
                        layers[0] =
                            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                                &projection);
                        layerReady = true;
                        finalLayerKind = "projection-cached";
                        frameCachedProjection = true;
                        ++R23CachedProjectionSubmits;
                        if (grace && !newStereoCommitted)
                        {
                            ++R42SameFrameProjectionReuses;
                            if (!R42FirstProjectionReuseLogged)
                            {
                                R42FirstProjectionReuseLogged = true;
                                std::cout
                                    << "[R42 projection-reuse] unchanged game frame reuses the released projection; duplicate per-XR-tick eye blits disabled.\n";
                            }
                        }

                        if (R23LastCachedProjectionLogMs == 0 ||
                            projectionNow - R23LastCachedProjectionLogMs >=
                                R23CachedProjectionLogIntervalMs)
                        {
                            R23LastCachedProjectionLogMs = projectionNow;
                            std::cout
                                << "[R23 projection-hold] reusing last released stereo projection instead of theater/SBS fallback"
                                << " count=" << R23CachedProjectionSubmits
                                << " newFrame=" << (newStereoCommitted ? 1 : 0)
                                << " grace=" << (grace ? 1 : 0)
                                << " presentationGraceFrames="
                                << R23PresentationGraceFrames
                                << " sameFrameReuse="
                                << R42SameFrameProjectionReuses
                                << " sourceRefresh="
                                << R42ProjectionRefreshes
                                << "\n";
                        }
                    }
                    QueryPerformanceCounter(&re);
                    frameRenderMs += timings.Ms(rs, re);
                    timings.render.Add(timings.Ms(rs, re));

                    if (frameFreshProjection)
                    {
                        awaitingFirstGameplayStereo = false;
                        cachedMenuProjectionValid = false;
                    }

                    // If Gameplay was announced before a releasable stereo
                    // projection, keep the last LOCAL-fixed menu/loading image
                    // visible in direct-only mode. This is strictly a bootstrap
                    // bridge; the first fresh gameplay projection disables it.
                    if (!layerReady && directTransportOnly &&
                        awaitingFirstGameplayStereo &&
                        cachedMenuProjectionValid)
                    {
                        pv = cachedMenuProjectionViews;
                        projection.space = localSpace;
                        projection.viewCount = 2;
                        projection.views = pv.data();
                        layers[0] =
                            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                                &projection);
                        layerReady = true;
                        intentionalMonoProjection = true;
                        finalLayerKind = "gameplay-bootstrap-menu-hold";
                    }

                    // Never submit a zero-layer frame just because Desktop
                    // Duplication missed the stereo grace window. VDXR can show a
                    // solid compositor colour / severe HMD stutter even while the
                    // desktop game keeps running normally.
                    if (!layerReady && directTransportOnly)
                    {
                        finalLayerKind = "direct-only-no-classic-fallback";
                    }
                    else if (!layerReady)
                    {
                        LARGE_INTEGER cs{}, ce{}; QueryPerformanceCounter(&cs);
                        const CaptureStatus fallbackCapture =
                            R23RefreshTheaterFallbackCapture(compositor);
                        QueryPerformanceCounter(&ce); timings.capture.Add(timings.Ms(cs, ce));

                        LARGE_INTEGER frs{}, fre{}; QueryPerformanceCounter(&frs);
                        // Only crop the left half when the shared frame state says
                        // the desktop source is a completed SBS frame. If stereo
                        // never became active (for example SAFE transport policy
                        // mismatch), the desktop contains a normal full-width mono
                        // frame; cropping it caused the observed half-screen zoom.
                        const bool fallbackSourceIsSbs = have &&
                            before.state == OutRunVR::StereoSbsActive &&
                            (before.flags & OutRunVR::RenderFrameStereoComplete) != 0;
                        if (fallbackCapture.available &&
                            R23RenderTheater(compositor, viewSpace, localSpace,
                                fs.predictedDisplayTime, quad, fallbackSourceIsSbs))
                        {
                            layers[0] =
                                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                            layerReady = true;
                            finalLayerKind = fallbackSourceIsSbs
                                ? "recovery-left-eye-theater"
                                : "recovery-full-mono-theater";
                            ++R23GameplayTheaterFallbacks;
                            const ULONGLONG now = GetTickCount64();
                            if (R23LastGameplayFallbackLogMs == 0 ||
                                now - R23LastGameplayFallbackLogMs >= 5000)
                            {
                                R23LastGameplayFallbackLogMs = now;
                                std::cout
                                    << "[R23 fallback] no reusable stereo projection remained; using "
                                    << (fallbackSourceIsSbs ? "left-eye SBS crop" : "full-width mono")
                                    << " LOCAL-fixed theater until fresh stereo returns"
                                    << " count=" << R23GameplayTheaterFallbacks
                                    << " productionAttempted=" << (productionCaptureAttempted ? 1 : 0)
                                    << "\n";
                            }
                        }
                        QueryPerformanceCounter(&fre);
                        frameRenderMs += timings.Ms(frs, fre);
                        timings.render.Add(timings.Ms(frs, fre));
                    }
                }
                else
                {
                    LARGE_INTEGER cs{}, ce{}; QueryPerformanceCounter(&cs);
                    const CaptureStatus capture = R23Capture(compositor);
                    QueryPerformanceCounter(&ce); timings.capture.Add(timings.Ms(cs, ce));
                    LARGE_INTEGER rs{}, re{}; QueryPerformanceCounter(&rs);

                    // R43: keep the efficient projection-layer menu, but anchor
                    // it in LOCAL space. R41 intentionally used VIEW space and
                    // therefore followed the headset exactly. Both eyes still
                    // receive identical desktop pixels and zero virtual IPD;
                    // only the reference space changes so the menu remains at
                    // the recentered world orientation while the head moves.
                    if (capture.available &&
                        menuProjectionAnchorValid &&
                        (!cachedMenuProjectionValid || capture.fresh))
                    {
                        std::array<XrCompositionLayerProjectionView, 2>
                            refreshed{};
                        if (R23RenderWorldLockedMenuProjection(
                                compositor, views, menuProjectionAnchor,
                                refreshed))
                        {
                            cachedMenuProjectionViews = refreshed;
                            cachedMenuProjectionValid = true;
                        }
                    }
                    if (cachedMenuProjectionValid)
                    {
                        pv = cachedMenuProjectionViews;
                        projection.space = localSpace;
                        projection.viewCount = 2;
                        projection.views = pv.data();
                        layers[0] =
                            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                                &projection);
                        layerReady = true;
                        intentionalMonoProjection = true;
                        finalLayerKind = capture.fresh
                            ? "menu-local-fixed-projection-fresh"
                            : "menu-local-fixed-projection-cached";
                    }
                    QueryPerformanceCounter(&re);
                    frameRenderMs += timings.Ms(rs, re);
                    timings.render.Add(timings.Ms(rs, re));
                }
            }

            end.layerCount = layerReady ? 1 : 0;
            end.layers = layerReady ? layers : nullptr;
            const R23FinalCounters finalBefore =
                R23ReadFinalCounters();
            OutRunVrR24BlackScreenGuard::IntentionalMonoProjection.store(
                intentionalMonoProjection, std::memory_order_release);
            LARGE_INTEGER es{}, ee{};
            QueryPerformanceCounter(&es);
            const XrResult endResult = xrEndFrame(session, &end);
            QueryPerformanceCounter(&ee);
            OutRunVrR24BlackScreenGuard::IntentionalMonoProjection.store(
                false, std::memory_order_release);
            const R23FinalCounters finalAfter =
                R23ReadFinalCounters();
            const char* actualFinalLayerKind =
                R23ActualFinalKind(
                    finalBefore, finalAfter, finalLayerKind);
            const std::uint32_t actualSubmittedFrameId =
                OutRunVrR23RuntimeHardening::LastSubmittedFrameId.load(
                    std::memory_order_acquire);
            const std::uint32_t actualSubmittedSourceKind =
                OutRunVrR23RuntimeHardening::LastSubmittedKind.load(
                    std::memory_order_acquire);
            const bool actualSubmittedLayer =
                OutRunVrR23RuntimeHardening::LastSubmittedLayer.load(
                    std::memory_order_acquire);
            R23Pixels.NoteFinalSubmission();
            CheckXr(endResult, "xrEndFrame");
            const double endFrameMs = timings.Ms(es, ee);
            timings.end.Add(endFrameMs);
            cadence.NoteFrame(frameFreshProjection,
                frameCachedProjection, layerReady,
                candidateCadenceRequestId);
            pipelineWindow.Note(candidateRejectReason,
                actualFinalLayerKind, candidateCaptureMs,
                candidateCommitCopyMs, frameRenderMs, endFrameMs);

            const ULONGLONG pipelineNowMs = GetTickCount64();
            if (lastPipelineTelemetryMs == 0 ||
                pipelineNowMs - lastPipelineTelemetryMs >= 5000)
            {
                lastPipelineTelemetryMs = pipelineNowMs;
                OutRunVrR23VerifiedBundle::Snapshot bundle{};
                const bool haveBundle =
                    OutRunVrR23VerifiedBundle::Read(bundle);
                const long long bundleAgeMs =
                    haveBundle && bundle.publishedAtMs &&
                    pipelineNowMs >= bundle.publishedAtMs
                    ? static_cast<long long>(
                        pipelineNowMs - bundle.publishedAtMs)
                    : -1;
                const std::string gameBuildTag =
                    haveBundle ? R23GameBuildTag(bundle.frame) :
                    std::string("unknown");
                const DWORD hostPid = GetCurrentProcessId();
                const DWORD activeGamePid = gamePid;
                std::ostringstream pipelineLine;
                pipelineLine
                    << "[R23 pipeline] hostBuild=" << OUTRUN_VR_BUILD_SHA
                    << " gameBuild=" << gameBuildTag
                    << " hostPid=" << hostPid
                    << " gamePid=" << activeGamePid
                    << " runKey=" << hostPid << "-" << activeGamePid
                    << " gameFrameId=" << candidateGameFrameId
                    << " gamePresentQpc=" << candidateGamePresentQpc
                    << " captureQpc=" << candidateCaptureQpc
                    << " captureAgeMs=" << candidateCaptureAgeMs
                    << " committedFrameId=" << lastProcessedStereoFrame
                    << " sourcePoseSequence=" << candidatePoseSequence
                    << " rejectReason=" << candidateRejectReason
                    << " requestedLayer=" << finalLayerKind
                    << " actualFinal=" << actualFinalLayerKind
                    << " actualFrameId=" << actualSubmittedFrameId
                    << " actualSourceKind=" << actualSubmittedSourceKind
                    << " actualLayer=" << (actualSubmittedLayer ? 1 : 0)
                    << " bundleFrameId="
                    << (haveBundle ? bundle.frameId : 0u)
                    << " bundleAgeMs=" << bundleAgeMs
                    << " captureMs=" << candidateCaptureMs
                    << " commitCopyMs=" << candidateCommitCopyMs
                    << " renderMs=" << frameRenderMs
                    << " xrWaitFrameMs=" << xrWaitFrameMs
                    << " xrFrameIntervalMs=" << xrFrameIntervalMs
                    << " cadenceSerialWaitMs=" << cadenceSerialWaitMs
                    << " candidateCadenceReq=" << candidateCadenceRequestId
                    << " gamePresentToConsumeMs=" << candidateConsumeAgeMs
                    << " xrEndFrameMs=" << endFrameMs
                    << " displayPeriodMs="
                    << (static_cast<double>(fs.predictedDisplayPeriod) / 1000000.0);
                pipelineWindow.AppendAndReset(pipelineLine);
                cadence.AppendAndReset(pipelineLine);
                pipelineLine << "\n";
                const std::string line = pipelineLine.str();
                std::cout << line;
                if (pipelineLog.is_open())
                {
                    // Let the stream buffer coalesce periodic telemetry writes.
                    // Per-frame forced flushes can serialize the XR loop on disk.
                    pipelineLog << line;
                }
            }
            timings.MaybeLog();
        }

        OutRunVrR23VerifiedBundle::Invalidate();
        compositor.Shutdown();
        if (viewSpace != XR_NULL_HANDLE) xrDestroySpace(viewSpace);
        if (localSpace != XR_NULL_HANDLE) xrDestroySpace(localSpace);
        if (session != XR_NULL_HANDLE) xrDestroySession(session);
        if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "OutRun VR host error: " << e.what() << "\n";
        OutRunVrR23VerifiedBundle::Invalidate();
        if (viewSpace != XR_NULL_HANDLE) xrDestroySpace(viewSpace);
        if (localSpace != XR_NULL_HANDLE) xrDestroySpace(localSpace);
        if (session != XR_NULL_HANDLE) xrDestroySession(session);
        if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);
        return 1;
    }
}
