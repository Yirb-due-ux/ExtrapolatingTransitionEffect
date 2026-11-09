#pragma once

#include <DirectXMath.h>
#include <cmath>

namespace XTE
{
    struct alignas(16) Transform
    {
        DirectX::XMFLOAT4 translation, rotation, scale;
    };
    static_assert(sizeof(Transform) == 0x30);

    inline constexpr Transform identity{{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};

    inline DirectX::XMVECTOR Dot3(DirectX::FXMVECTOR a, DirectX::FXMVECTOR b) noexcept
    {
        const auto products = _mm_mul_ps(a, b);
        const auto xy = _mm_add_ss(_mm_shuffle_ps(products, products, 0x55), products);
        const auto xyz = _mm_add_ss(_mm_shuffle_ps(products, products, 0xAA), xy);
        return _mm_shuffle_ps(xyz, xyz, 0);
    }

    inline DirectX::XMVECTOR MultiplyRotation(DirectX::FXMVECTOR a, DirectX::FXMVECTOR b) noexcept
    {
        using namespace DirectX;
        auto result = XMVectorAdd(XMVector3Cross(a, b), XMVectorScale(b, XMVectorGetW(a)));
        result = XMVectorAdd(result, XMVectorScale(a, XMVectorGetW(b)));
        const auto scalar = XMVectorSubtract(XMVectorMultiply(a, b), Dot3(a, b));
        return _mm_shuffle_ps(result, _mm_unpackhi_ps(result, scalar), 0xC4);
    }

    template <bool Inverse = false>
    inline DirectX::XMVECTOR RotateDirection(DirectX::FXMVECTOR q, DirectX::FXMVECTOR direction) noexcept
    {
        using namespace DirectX;
        // Havok's direction formula also applies to the non-unit sampled quaternions.
        const float real = XMVectorGetW(q);
        auto result = XMVectorScale(direction, real * real - 0.5f);
        result = XMVectorAdd(result, XMVectorMultiply(Dot3(q, direction), q));
        const auto cross = Inverse ? XMVector3Cross(direction, q) : XMVector3Cross(q, direction);
        result = XMVectorAdd(result, XMVectorScale(cross, real));
        return XMVectorAdd(result, result);
    }

    inline Transform Compose(const Transform& left, const Transform& right) noexcept
    {
        using namespace DirectX;
        Transform result;
        const auto a = XMLoadFloat4(&left.rotation);
        const auto b = XMLoadFloat4(&right.rotation);
        XMStoreFloat4(&result.translation, XMVectorAdd(XMLoadFloat4(&left.translation),
            RotateDirection(a, XMLoadFloat4(&right.translation))));
        XMStoreFloat4(&result.rotation, MultiplyRotation(a, b));
        XMStoreFloat4(&result.scale, XMVectorMultiply(XMLoadFloat4(&left.scale), XMLoadFloat4(&right.scale)));
        return result;
    }

    inline Transform Relative(const Transform& first, const Transform& second) noexcept
    {
        using namespace DirectX;
        Transform result;
        const auto previous = XMLoadFloat4(&first.rotation);
        const auto next = XMLoadFloat4(&second.rotation);
        const auto delta = MultiplyRotation(next, XMQuaternionConjugate(previous));
        const auto inverseTranslation = XMVectorNegate(RotateDirection<true>(previous, XMLoadFloat4(&first.translation)));
        XMStoreFloat4(&result.translation, XMVectorAdd(XMLoadFloat4(&second.translation), RotateDirection(next, inverseTranslation)));
        XMStoreFloat4(&result.rotation, delta);
        const auto priorScale = XMVectorSetW(XMLoadFloat4(&first.scale), 1);
        const auto estimate = _mm_rcp_ps(priorScale);
        const auto reciprocal = _mm_sub_ps(_mm_add_ps(estimate, estimate), _mm_mul_ps(_mm_mul_ps(estimate, priorScale), estimate));
        XMStoreFloat4(&result.scale, XMVectorSetW(XMVectorMultiply(XMLoadFloat4(&second.scale), reciprocal), 0));
        return result;
    }

    inline Transform Blend(const Transform& from, const Transform& to, float weight) noexcept
    {
        using namespace DirectX;
        Transform result;
        const auto a = XMLoadFloat4(&from.rotation);
        const auto b = XMLoadFloat4(&to.rotation);
        const auto aligned = XMVectorGetX(XMVector4Dot(a, b)) < 0 ? XMVectorNegate(b) : b;
        const auto q = XMVectorLerp(a, aligned, weight);
        const auto s = XMVectorLerp(XMLoadFloat4(&from.scale), XMLoadFloat4(&to.scale), weight);
        XMStoreFloat4(&result.translation, XMVectorLerp(XMLoadFloat4(&from.translation), XMLoadFloat4(&to.translation), weight));
        XMStoreFloat4(&result.rotation, XMVectorGetX(XMVector4LengthSq(q)) >= 1.1920929e-7f ? XMQuaternionNormalize(q) : XMQuaternionIdentity());
        XMStoreFloat4(&result.scale, XMVectorGetX(XMVector3LengthSq(s)) >= 1.1920929e-7f ? s : XMVectorReplicate(1));
        return result;
    }
}
