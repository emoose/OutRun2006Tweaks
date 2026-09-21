/*
 * OutRun2006Tweaks VR - D3D9 shader bytecode fingerprint helper
 *
 * The FNV-1 64-bit hash routine and the "hash shader bytecode, not COM pointer"
 * approach are adapted from 3Dmigoto (bo3b/3Dmigoto, util.h and
 * DirectX9/Direct3DDevice9Functions.h).
 *
 * 3Dmigoto source is released under GNU GPL version 3.
 * This file is therefore distributed under GPL-3.0-only.
 *
 * Adaptation for OutRun VR: 2026 thp32tt contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <d3d9.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

namespace OutRunVR::GplShaderFingerprint
{
    constexpr std::uint64_t Fnv64Prime = 0x100000001b3ULL;

    // 3Dmigoto-compatible FNV-1 64-bit byte-buffer hash.
    // Intentionally starts at zero to preserve 3Dmigoto's shader fingerprint
    // convention rather than the more common FNV offset-basis variant.
    inline std::uint64_t Fnv1_64(const void* buffer, std::size_t length) noexcept
    {
        std::uint64_t hash = 0;
        const auto* current = static_cast<const unsigned char*>(buffer);
        const auto* end = current + length;
        while (current < end)
        {
            hash *= Fnv64Prime;
            hash ^= static_cast<std::uint64_t>(*current++);
        }
        return hash;
    }

    struct ShaderHash
    {
        std::uint64_t value = 0;
        std::uint32_t bytecodeBytes = 0;
        bool valid = false;
    };

    struct ShaderPair
    {
        ShaderHash vertex{};
        ShaderHash pixel{};
        std::uintptr_t vertexIdentity = 0;
        std::uintptr_t pixelIdentity = 0;
        bool valid = false;
    };

    inline bool TraceEnabled() noexcept
    {
        // Diagnostics are opt-in because querying the active D3D9 shader on
        // every classified draw adds COM traffic. Set
        // OUTRUN_VR_SHADER_FINGERPRINT=1 for a diagnostic run.
        static const bool enabled = []() noexcept {
            char value[16]{};
            std::size_t required = 0;
#if defined(_MSC_VER)
            if (getenv_s(&required, value, sizeof(value),
                    "OUTRUN_VR_SHADER_FINGERPRINT") != 0 || required == 0)
                return false;
#else
            const char* env = std::getenv("OUTRUN_VR_SHADER_FINGERPRINT");
            if (!env)
                return false;
            std::snprintf(value, sizeof(value), "%s", env);
#endif
            return value[0] == '1' || value[0] == 'y' || value[0] == 'Y' ||
                value[0] == 't' || value[0] == 'T';
        }();
        return enabled;
    }

    template <typename ShaderT>
    inline ShaderHash HashBytecode(ShaderT* shader) noexcept
    {
        ShaderHash result{};
        if (!shader)
            return result;

        UINT size = 0;
        if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0 ||
            size > (4u * 1024u * 1024u))
            return result;

        try
        {
            std::vector<std::uint8_t> bytecode(size);
            UINT actual = size;
            if (FAILED(shader->GetFunction(bytecode.data(), &actual)) ||
                actual == 0 || actual > bytecode.size())
                return result;

            result.value = Fnv1_64(bytecode.data(), actual);
            result.bytecodeBytes = actual;
            result.valid = true;
            return result;
        }
        catch (...)
        {
            return result;
        }
    }

    template <typename ShaderT, std::size_t Capacity>
    inline ShaderHash HashBytecodeCached(
        ShaderT* shader,
        std::array<std::pair<std::uintptr_t, ShaderHash>, Capacity>& cache,
        std::size_t& nextSlot) noexcept
    {
        if (!shader)
            return {};

        const auto identity = reinterpret_cast<std::uintptr_t>(shader);
        for (const auto& entry : cache)
            if (entry.first == identity)
                return entry.second;

        const ShaderHash hash = HashBytecode(shader);
        if (hash.valid)
        {
            cache[nextSlot] = { identity, hash };
            nextSlot = (nextSlot + 1) % Capacity;
        }
        return hash;
    }

    inline ShaderPair CaptureCurrent(IDirect3DDevice9* device) noexcept
    {
        ShaderPair result{};
        if (!device)
            return result;

        IDirect3DVertexShader9* vertex = nullptr;
        IDirect3DPixelShader9* pixel = nullptr;
        const HRESULT vsHr = device->GetVertexShader(&vertex);
        const HRESULT psHr = device->GetPixelShader(&pixel);
        if (FAILED(vsHr) || FAILED(psHr))
        {
            if (vertex) vertex->Release();
            if (pixel) pixel->Release();
            return result;
        }

        result.vertexIdentity = reinterpret_cast<std::uintptr_t>(vertex);
        result.pixelIdentity = reinterpret_cast<std::uintptr_t>(pixel);

        // D3D9 rendering for OutRun is effectively render-thread owned here.
        // Keep the cache protected anyway so diagnostics remain safe if a
        // wrapper/runtime changes call threading later.
        static std::mutex cacheMutex;
        static std::array<std::pair<std::uintptr_t, ShaderHash>, 256> vsCache{};
        static std::array<std::pair<std::uintptr_t, ShaderHash>, 256> psCache{};
        static std::size_t vsNext = 0;
        static std::size_t psNext = 0;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            result.vertex = HashBytecodeCached(vertex, vsCache, vsNext);
            result.pixel = HashBytecodeCached(pixel, psCache, psNext);
        }

        if (vertex) vertex->Release();
        if (pixel) pixel->Release();

        // Fixed-function paths legitimately have a null vertex/pixel shader.
        result.valid = result.vertex.valid || result.pixel.valid ||
            (result.vertexIdentity == 0 && result.pixelIdentity == 0);
        return result;
    }

    inline bool RememberPair(const ShaderPair& pair) noexcept
    {
        if (!pair.valid)
            return false;

        struct Seen
        {
            std::uint64_t vs = 0;
            std::uint64_t ps = 0;
        };

        static std::mutex seenMutex;
        static std::array<Seen, 256> seen{};
        static std::size_t seenCount = 0;
        static std::size_t nextSeen = 0;
        std::lock_guard<std::mutex> lock(seenMutex);

        // Bytecode fingerprints are deliberately the identity here. COM object
        // addresses can change after Reset/recreation or between launches.
        for (std::size_t i = 0; i < seenCount; ++i)
        {
            if (seen[i].vs == pair.vertex.value &&
                seen[i].ps == pair.pixel.value)
                return false;
        }

        const Seen item{ pair.vertex.value, pair.pixel.value };
        seen[nextSeen] = item;
        nextSeen = (nextSeen + 1) % seen.size();
        if (seenCount < seen.size())
            ++seenCount;
        return true;
    }
}
