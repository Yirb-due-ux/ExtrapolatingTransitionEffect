#pragma once

#include <SKSE/Impl/PCH.h>
#include <RE/H/hkBitField.h>
#include <RE/H/hkbGeneratorOutput.h>

#include "Extrapolate.h"

namespace XTE
{
    inline constexpr std::size_t kMaxBones = 256;
    inline constexpr std::int32_t kMaxTrackBytes = 32768;
    // Experimental parser limit. The SE engine-wide maximum is unverified.
    inline constexpr std::int32_t kMaxTrackCount = 25;

    struct Track
    {
        std::byte* data{};
        std::size_t count{};
        float fraction{};
        bool hasBoneMask{};

        Transform At(std::size_t index) const noexcept
        {
            assert(index < count);
            Transform result;
            std::memcpy(&result, data + index * sizeof(Transform), sizeof(result));
            return result;
        }

        RE::hkBitField Mask() const noexcept
        {
            if (!hasBoneMask) return {};
            const auto words = static_cast<std::int32_t>(count / 32 + 1);
            return RE::hkBitField::Borrow(reinterpret_cast<std::uint32_t*>(data + count * sizeof(Transform)),
                words, static_cast<std::int32_t>(count + 1));
        }

        bool Has(std::size_t index) const noexcept
        {
            if (index >= count || !hasBoneMask) return false;
            const auto mask = Mask();
            return mask.Test(count) || mask.Test(index);
        }
    };

    inline Track FindQsTrack(std::span<std::byte> packet, RE::hkbGeneratorOutput::TrackId id) noexcept
    {
        using Output = RE::hkbGeneratorOutput;
        constexpr std::int32_t masterBytes = sizeof(Output::TrackMasterHeader);
        constexpr std::int32_t headerBytes = sizeof(Output::TrackHeader);
        if (packet.size() < masterBytes) return {};

        Output::TrackMasterHeader master;
        std::memcpy(&master, packet.data(), sizeof(master));
        const auto index = static_cast<std::int32_t>(id);
        if (master.numBytes < masterBytes || master.numBytes > kMaxTrackBytes ||
            static_cast<std::size_t>(master.numBytes) > packet.size() ||
            index < 0 || master.numTracks <= index || master.numTracks > kMaxTrackCount) return {};

        const auto directoryBytes = masterBytes + master.numTracks * headerBytes;
        if (master.numBytes < directoryBytes) return {};
        Output::TrackHeader header;
        std::memcpy(&header, packet.data() + masterBytes + index * headerBytes, sizeof(header));
        using Flag = Output::TrackFlagBits;
        if (header.type != Output::TrackType::kQsTransform ||
            header.flags.any(Flag::kAdditivePose, Flag::kPalette, Flag::kSparse) ||
            header.elementSizeBytes != sizeof(Transform) || header.numData <= 0 ||
            header.numData > static_cast<std::int16_t>(kMaxBones) || header.numData > header.capacity ||
            header.dataOffset < directoryBytes || header.dataOffset % alignof(Transform)) return {};

        const auto count = static_cast<std::size_t>(header.numData);
        const auto offset = static_cast<std::size_t>(header.dataOffset);
        const bool hasBoneMask = id == Output::TrackId::kPose;
        const auto maskBytes = hasBoneMask ? (count / 32 + 1) * sizeof(std::uint32_t) : 0;
        if (offset + count * sizeof(Transform) + maskBytes > static_cast<std::size_t>(master.numBytes)) return {};
        auto* data = packet.data() + offset;
        // Native blending consumes the pose and its mask through aligned pointers.
        if (hasBoneMask && reinterpret_cast<std::uintptr_t>(data) % alignof(Transform)) return {};
        return {data, count, header.onFraction, hasBoneMask};
    }

    inline Track QsTrack(const RE::hkbGeneratorOutput& output, RE::hkbGeneratorOutput::TrackId id) noexcept
    {
        // For a live output the engine owns the packet and supplies its extent.
        const auto bytes = output.GetNumBytes();
        if (bytes < static_cast<std::int32_t>(sizeof(RE::hkbGeneratorOutput::TrackMasterHeader)) ||
            bytes > kMaxTrackBytes) return {};
        return FindQsTrack(std::span{reinterpret_cast<std::byte*>(output.tracks), static_cast<std::size_t>(bytes)}, id);
    }

    inline Transform TrackValue(const Track& track) noexcept
    {
        return track.count ? track.At(0) : identity;
    }
}
