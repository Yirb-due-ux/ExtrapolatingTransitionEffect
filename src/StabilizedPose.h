#pragma once

#include "Extrapolate.h"

#include <algorithm>

namespace XTE
{
    inline constexpr float kPredictionHalfLife = 0.080f;
    inline constexpr float kMinimumHalfLife = 0.030f;
    inline constexpr float kLn2 = 0.69314718056f;

    inline bool ValidPose(const Transform& pose) noexcept
    {
        using namespace DirectX;
        const auto q = XMLoadFloat4(&pose.rotation);
        const auto norm = XMVectorGetX(XMVector4LengthSq(q));
        const auto finite = [](const XMFLOAT4& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
        };
        return std::isfinite(norm) && norm > 1e-8f && finite(pose.translation) && finite(pose.scale);
    }

    inline float ShortestSign(DirectX::FXMVECTOR relative) noexcept
    {
        using namespace DirectX;
        if (const float w = XMVectorGetW(relative); w != 0) return std::copysign(1.0f, w);
        // At 180 degrees, use the dominant axis for a representation-independent tie.
        const float x = XMVectorGetX(relative), y = XMVectorGetY(relative), z = XMVectorGetZ(relative);
        const float xy = std::abs(x) >= std::abs(y) ? x : y;
        return std::copysign(1.0f, std::abs(xy) >= std::abs(z) ? xy : z);
    }

    inline Transform StabilizedDelta(const Transform& first, Transform& second) noexcept
    {
        using namespace DirectX;
        const auto oldQ = XMQuaternionNormalize(XMLoadFloat4(&first.rotation));
        const auto newQ = XMQuaternionNormalize(XMLoadFloat4(&second.rotation));
        const auto dq = XMQuaternionNormalize(MultiplyRotation(newQ, XMQuaternionConjugate(oldQ)));
        const auto step = XMVectorScale(dq, ShortestSign(dq));
        const auto ratio = [](float a, float b) {
            // A scale crossing zero has no real fractional transform power.
            const float r = a != 0 ? b / a : 1.0f;
            return r > 0 && std::isfinite(r) ? r : 1.0f;
        };
        Transform delta{{}, {}, {ratio(first.scale.x, second.scale.x), ratio(first.scale.y, second.scale.y),
            ratio(first.scale.z, second.scale.z), 1}};
        XMStoreFloat4(&second.rotation, newQ);
        XMStoreFloat4(&delta.rotation, step);
        XMStoreFloat4(&delta.translation, XMVectorSetW(XMVectorSubtract(XMLoadFloat4(&second.translation),
            XMVector3Rotate(XMLoadFloat4(&first.translation), step)), 0));
        return delta;
    }

    inline float RotationHalfAngle(DirectX::FXMVECTOR q) noexcept
    {
        using namespace DirectX;
        return std::atan2(XMVectorGetX(XMVector3Length(q)), XMVectorGetW(q));
    }

    inline DirectX::XMVECTOR SeedDisplacement(const Transform& delta, const Transform& seed) noexcept
    {
        using namespace DirectX;
        const auto p = XMLoadFloat4(&seed.translation);
        return XMVectorSubtract(XMVectorAdd(XMLoadFloat4(&delta.translation), XMVector3Rotate(p, XMLoadFloat4(&delta.rotation))), p);
    }

    inline float PredictionStepRate(const Transform& delta, const Transform& seed, float sampleInterval) noexcept
    {
        using namespace DirectX;
        const float angle = 2 * RotationHalfAngle(XMLoadFloat4(&delta.rotation));
        const float displacement = XMVectorGetX(XMVector3Length(SeedDisplacement(delta, seed)));
        const float extent = (std::max)(1.0f, XMVectorGetX(XMVector3Length(XMLoadFloat4(&seed.translation))));
        const float scaleChange = (std::max)({std::abs(std::log(delta.scale.x)), std::abs(std::log(delta.scale.y)),
            std::abs(std::log(delta.scale.z))});
        // A shared step rate preserves the path coupling between position, rotation and scale.
        return (std::min)({1 / sampleInterval, XM_2PI / (std::max)(angle, 1e-6f),
            4 * extent / (std::max)(displacement, 1e-6f), 4 * kLn2 / (std::max)(scaleChange, 1e-6f)});
    }

    inline float TargetHalfLife(const Transform& delta, const Transform& seed, const Transform& target, float stepRate) noexcept
    {
        using namespace DirectX;
        if (stepRate <= 0) return kPredictionHalfLife;
        float budget = kPredictionHalfLife;
        const auto q = XMLoadFloat4(&delta.rotation);
        const float angle = 2 * RotationHalfAngle(q);
        if (angle > 1e-4f) {
            const auto error = XMQuaternionNormalize(MultiplyRotation(XMQuaternionNormalize(XMLoadFloat4(&target.rotation)),
                XMQuaternionConjugate(XMLoadFloat4(&seed.rotation))));
            const auto lifted = XMVectorScale(error, ShortestSign(error));
            const auto errorAxis = XMVector3Normalize(lifted);
            const float forward = 2 * RotationHalfAngle(lifted) * XMVectorGetX(Dot3(XMVector3Normalize(q), errorAxis));
            budget = (std::min)(budget, kLn2 * forward / (angle * stepRate));
        }
        const auto dp = SeedDisplacement(delta, seed);
        const float dpSquared = XMVectorGetX(Dot3(dp, dp));
        if (dpSquared > 1e-4f) {
            const auto distance = XMVectorSubtract(XMLoadFloat4(&target.translation), XMLoadFloat4(&seed.translation));
            budget = (std::min)(budget, kLn2 * XMVectorGetX(Dot3(dp, distance)) / (dpSquared * stepRate));
        }
        return (std::max)(budget, kMinimumHalfLife);
    }

    inline float DampedSeconds(float age, float goalAge, float goalHalfLife) noexcept
    {
        const auto integral = [](float t, float halfLife) { return -std::expm1(-kLn2 * t / halfLife) * halfLife / kLn2; };
        if (goalAge < 0 || age <= goalAge) return integral(age, kPredictionHalfLife);
        return integral(goalAge, kPredictionHalfLife) + std::exp2(-goalAge / kPredictionHalfLife) * integral(age - goalAge, goalHalfLife);
    }

    inline Transform AdvanceFraction(const Transform& delta, const Transform& seed, float steps) noexcept
    {
        using namespace DirectX;
        if (steps == 0) return seed;
        const auto q = XMLoadFloat4(&delta.rotation);
        const auto translation = XMLoadFloat4(&delta.translation);
        const auto p = XMLoadFloat4(&seed.translation);
        const float halfAngle = RotationHalfAngle(q);
        auto moved = XMVectorAdd(p, XMVectorScale(translation, steps));
        Transform result = seed;
        if (halfAngle > 1e-6f) {
            const auto axis = XMVector3Normalize(q);
            const auto parallel = XMVectorMultiply(axis, Dot3(axis, translation));
            const auto perpendicular = XMVectorSubtract(translation, parallel);
            const auto power = XMQuaternionRotationNormal(axis, 2 * halfAngle * steps);
            // Fractional geometric sum of rotated translations in D^steps.
            const auto phase = XMQuaternionRotationNormal(axis, halfAngle * (steps - 1));
            const auto sum = XMVectorAdd(XMVectorScale(parallel, steps), XMVectorScale(XMVector3Rotate(perpendicular, phase),
                std::sin(steps * halfAngle) / std::sin(halfAngle)));
            moved = XMVectorAdd(sum, XMVector3Rotate(p, power));
            XMStoreFloat4(&result.rotation, XMQuaternionNormalize(MultiplyRotation(power, XMLoadFloat4(&seed.rotation))));
        }
        XMStoreFloat4(&result.translation, XMVectorSetW(moved, seed.translation.w));
        result.scale = {seed.scale.x * std::pow(delta.scale.x, steps), seed.scale.y * std::pow(delta.scale.y, steps),
            seed.scale.z * std::pow(delta.scale.z, steps), seed.scale.w};
        return result;
    }

    inline bool ContinuousRotation(const Transform& from, const Transform& to, float weight,
        DirectX::XMFLOAT4& history, bool commit, DirectX::XMFLOAT4& output) noexcept
    {
        using namespace DirectX;
        if (weight <= 0 || weight >= 1 || !ValidPose(to)) return false;
        const auto a = XMLoadFloat4(&from.rotation);
        const auto b = XMLoadFloat4(&to.rotation);
        const auto previous = XMLoadFloat4(&history);
        const auto relative = XMQuaternionNormalize(MultiplyRotation(b, XMQuaternionConjugate(a)));
        const float d = XMVectorGetX(XMVector4Dot(previous, relative));
        const float sign = d != 0 ? std::copysign(1.0f, d) : ShortestSign(relative);
        const auto lifted = XMVectorScale(relative, sign);
        if (commit) XMStoreFloat4(&history, lifted);
        if (sign == (XMVectorGetX(XMVector4Dot(a, b)) < 0 ? -1.0f : 1.0f)) return false;
        auto blended = XMVectorLerp(a, XMVectorScale(b, sign), weight);
        if (XMVectorGetX(XMVector4LengthSq(blended)) < 1e-7f) {
            // A full-turn lift makes linear normalization singular. Continue about the last known axis.
            const auto axis = XMVectorGetX(XMVector3LengthSq(lifted)) > 1e-8f ? lifted : previous;
            if (XMVectorGetX(XMVector3LengthSq(axis)) <= 1e-8f) return false;
            blended = MultiplyRotation(XMQuaternionRotationNormal(XMVector3Normalize(axis),
                2 * RotationHalfAngle(lifted) * weight), a);
        }
        XMStoreFloat4(&output, XMQuaternionNormalize(blended));
        return true;
    }
}
