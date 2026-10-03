#pragma once

#include "PCH.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace XPMF {

/**
 * @brief Where the attributes this plugin reads and writes sit inside one packed BSTriShape vertex
 *
 * A BSGraphics vertex descriptor is a 64 bit value: the low nibble is the vertex size in dwords,
 * the nibbles after it are the attribute offsets in dwords (position, uv, uv2, normal, tangent,
 * color, skinning, land data, eye data, in that order), and bits 44 and up say which attributes
 * exist. A static's vertex is position (three floats plus a fourth slot holding a bitangent
 * component), uv (two half floats), normal (three unsigned bytes mapping 0..255 to -1..1, plus a
 * bitangent byte), tangent (the same, plus the last bitangent byte), and an RGBA color.
 *
 * The position is always float in Skyrim. VF_FULLPREC and half precision positions are
 * Fallout 4's: every vanilla SSE mesh has the flag clear (0x03B on a typical static) with the 32
 * byte stride only float positions add up to, and CommonLib's VertexDesc::GetSize counts
 * VF_VERTEX as four floats unconditionally. Reading the flag as "half" - which this struct once
 * did - turns every position into noise around the model's origin.
 */
struct VertexLayout {
    std::uint32_t stride {}; /**< Bytes per vertex */
    std::uint32_t uvOffset {}; /**< Byte offset of the first uv; meaningless without hasUV */
    std::uint32_t uv2Offset {}; /**< Byte offset of the second uv; meaningless without hasUV2 */
    std::uint32_t normalOffset {}; /**< Byte offset of the normal; meaningless without hasNormals */
    std::uint32_t tangentOffset {}; /**< Byte offset of the tangent; meaningless without hasTangents */
    std::uint32_t colorOffset {}; /**< Byte offset of the RGBA color; meaningless without hasColors */
    bool hasUV {}; /**< VF_UV */
    bool hasUV2 {}; /**< VF_UV_2 */
    bool hasNormals {}; /**< VF_NORMAL */
    bool hasTangents {}; /**< VF_TANGENT, which the engine only writes next to a normal */
    bool hasColors {}; /**< VF_COLORS */

    constexpr static std::uint64_t SIZE_MASK = 0xF; /**< The descriptor's low nibble: vertex size in dwords */
    constexpr static std::uint32_t POSITION_SIZE = 4 * sizeof(float); /**< xyz plus the bitangent slot */
    constexpr static std::uint32_t BITANGENT_X_OFFSET = 3 * sizeof(float); /**< The bitangent's x, a float after xyz */
    constexpr static std::uint32_t UV_SIZE = 2 * sizeof(std::uint16_t); /**< Two half floats */
    constexpr static std::uint32_t COLOR_SIZE = 4; /**< One RGBA color */
    constexpr static std::uint8_t COLOR_MAX = 255; /**< A channel's full value: white, or an alpha of 1 */
    constexpr static std::uint32_t NORMAL_COMPONENTS = 3; /**< x, y and z, a byte each */
    constexpr static std::uint32_t NORMAL_SIZE = NORMAL_COMPONENTS + 1; /**< ...followed by a bitangent byte */
    constexpr static std::uint32_t MAX_STRIDE = 15 * sizeof(std::uint32_t); /**< What the size nibble can say */

    /**
     * @brief Reads the layout of a plain static vertex
     *
     * @param desc The shape's vertex descriptor
     * @return std::optional<VertexLayout> std::nullopt for anything that is not a rigid static
     *         vertex with a position (skinned, landscape, eye and instanced layouts keep data
     *         behind the color, and none of them carries projected snow)
     */
    [[nodiscard]] static auto from(RE::BSGraphics::VertexDesc desc) -> std::optional<VertexLayout>
    {
        using RE::BSGraphics::Vertex;
        if (!desc.HasFlag(Vertex::VF_VERTEX)
            || desc.HasFlag(static_cast<Vertex::Flags>(Vertex::VF_SKINNED | Vertex::VF_LANDDATA | Vertex::VF_EYEDATA
                                                       | Vertex::VF_INSTANCEDATA))) {
            return std::nullopt;
        }

        VertexLayout layout;
        layout.stride
            = static_cast<std::uint32_t>((std::bit_cast<std::uint64_t>(desc) & SIZE_MASK) * sizeof(std::uint32_t));
        layout.hasUV = desc.HasFlag(Vertex::VF_UV);
        layout.hasUV2 = desc.HasFlag(Vertex::VF_UV_2);
        layout.hasNormals = desc.HasFlag(Vertex::VF_NORMAL);
        layout.hasTangents = layout.hasNormals && desc.HasFlag(Vertex::VF_TANGENT);
        layout.hasColors = desc.HasFlag(Vertex::VF_COLORS);
        layout.uvOffset = desc.GetAttributeOffset(Vertex::VA_TEXCOORD0);
        layout.uv2Offset = desc.GetAttributeOffset(Vertex::VA_TEXCOORD1);
        layout.normalOffset = desc.GetAttributeOffset(Vertex::VA_NORMAL);
        layout.tangentOffset = desc.GetAttributeOffset(Vertex::VA_BINORMAL);
        layout.colorOffset = desc.GetAttributeOffset(Vertex::VA_COLOR);

        const auto fits = [&](bool has, std::uint32_t offset, std::uint32_t size) -> bool {
            return !has || offset + size <= layout.stride;
        };
        if (layout.stride < POSITION_SIZE || !fits(layout.hasUV, layout.uvOffset, UV_SIZE)
            || !fits(layout.hasUV2, layout.uv2Offset, UV_SIZE) || !fits(layout.hasNormals, layout.normalOffset, NORMAL_SIZE)
            || !fits(layout.hasTangents, layout.tangentOffset, NORMAL_SIZE)
            || !fits(layout.hasColors, layout.colorOffset, COLOR_SIZE)) {
            return std::nullopt;
        }
        return layout;
    }

    /**
     * @brief The layout (and descriptor) this one becomes when a color is appended to every vertex
     *
     * The color goes behind everything else: for the rigid layouts from() accepts nothing
     * follows it in the engine's own attribute order either, and the engine builds its input
     * layouts from the offset nibbles rather than from an assumed order.
     *
     * @param desc The descriptor to extend; rewritten in place
     * @return std::optional<VertexLayout> std::nullopt when the vertex cannot grow any further
     */
    [[nodiscard]] auto withColors(RE::BSGraphics::VertexDesc& desc) const -> std::optional<VertexLayout>
    {
        if (hasColors || stride + COLOR_SIZE > MAX_STRIDE) {
            return std::nullopt;
        }
        VertexLayout grown = *this;
        grown.hasColors = true;
        grown.colorOffset = stride;
        grown.stride = stride + COLOR_SIZE;

        desc.SetFlag(RE::BSGraphics::Vertex::VF_COLORS);
        desc.SetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR, grown.colorOffset);
        desc = std::bit_cast<RE::BSGraphics::VertexDesc>((std::bit_cast<std::uint64_t>(desc) & ~SIZE_MASK)
                                                         | (grown.stride / sizeof(std::uint32_t)));
        return grown;
    }

    /**
     * @brief Model space position of a vertex
     */
    [[nodiscard]] static auto position(std::span<const std::uint8_t> vertex) -> RE::NiPoint3
    {
        RE::NiPoint3 point;
        std::memcpy(&point, vertex.data(), sizeof(point));
        return point;
    }

    /**
     * @brief Model space normal of a vertex (not renormalized; the byte encoding is close enough)
     */
    [[nodiscard]] auto normal(std::span<const std::uint8_t> vertex) const -> RE::NiPoint3
    {
        const auto bytes = vertex.subspan(normalOffset, NORMAL_COMPONENTS);
        return {byteToUnit(bytes[0]), byteToUnit(bytes[1]), byteToUnit(bytes[2])};
    }

    /**
     * @brief A normal or tangent component's byte as a unit value: 0..255 to -1..1
     */
    [[nodiscard]] static auto byteToUnit(std::uint8_t byte) -> float
    {
        constexpr float BYTE_TO_UNIT = 2.0F / 255.0F;
        return (static_cast<float>(byte) * BYTE_TO_UNIT) - 1.0F;
    }

    /**
     * @brief The byte of a unit value, rounded to nearest
     */
    [[nodiscard]] static auto unitToByte(float unit) -> std::uint8_t
    {
        constexpr float UNIT_TO_BYTE = 255.0F / 2.0F;
        const float scaled = (unit + 1.0F) * UNIT_TO_BYTE;
        return static_cast<std::uint8_t>(scaled <= 0.0F ? 0.0F : scaled >= 255.0F ? 255.0F : scaled + 0.5F);
    }

    /**
     * @brief IEEE half to float, subnormals and all
     */
    [[nodiscard]] static auto halfToFloat(std::uint16_t half) -> float
    {
        constexpr std::uint32_t SIGN_SHIFT = 15;
        constexpr std::uint32_t EXPONENT_SHIFT = 10;
        constexpr std::uint32_t EXPONENT_MASK = 0x1F;
        constexpr std::uint32_t MANTISSA_MASK = 0x3FF;
        constexpr std::uint32_t IMPLICIT_ONE = 0x400;
        constexpr std::uint32_t BIAS_DIFFERENCE = 127 - 15;
        constexpr std::uint32_t FLOAT_EXPONENT_SHIFT = 23;
        constexpr std::uint32_t FLOAT_SIGN_SHIFT = 31;
        constexpr std::uint32_t MANTISSA_SHIFT = 13; /**< 23 - 10 */
        constexpr std::uint32_t FLOAT_INFINITY = 0x7F800000;

        const std::uint32_t sign = (static_cast<std::uint32_t>(half) >> SIGN_SHIFT) & 1U;
        const std::uint32_t exponent = (static_cast<std::uint32_t>(half) >> EXPONENT_SHIFT) & EXPONENT_MASK;
        std::uint32_t mantissa = half & MANTISSA_MASK;
        std::uint32_t bits = sign << FLOAT_SIGN_SHIFT;
        if (exponent == 0) {
            if (mantissa != 0) { // subnormal: renormalize
                std::uint32_t adjust = 0;
                while ((mantissa & IMPLICIT_ONE) == 0) {
                    mantissa <<= 1U;
                    ++adjust;
                }
                bits |= ((BIAS_DIFFERENCE + 1 - adjust) << FLOAT_EXPONENT_SHIFT) | ((mantissa & MANTISSA_MASK) << MANTISSA_SHIFT);
            }
        } else if (exponent == EXPONENT_MASK) {
            bits |= FLOAT_INFINITY | (mantissa << MANTISSA_SHIFT);
        } else {
            bits |= ((exponent + BIAS_DIFFERENCE) << FLOAT_EXPONENT_SHIFT) | (mantissa << MANTISSA_SHIFT);
        }
        return std::bit_cast<float>(bits);
    }

    /**
     * @brief Float to IEEE half, rounded to nearest even; overflow gives an infinity
     */
    [[nodiscard]] static auto floatToHalf(float value) -> std::uint16_t
    {
        constexpr std::uint32_t FLOAT_EXPONENT_MASK = 0xFF;
        constexpr std::uint32_t FLOAT_MANTISSA_MASK = 0x7FFFFF;
        constexpr std::uint32_t FLOAT_IMPLICIT_ONE = 0x800000;
        constexpr std::uint32_t FLOAT_EXPONENT_SHIFT = 23;
        constexpr std::uint32_t HALF_SIGN = 0x8000;
        constexpr std::uint32_t HALF_INFINITY = 0x7C00;
        constexpr std::uint32_t HALF_QUIET_NAN = 0x200;
        constexpr std::int32_t BIAS_DIFFERENCE = 127 - 15;
        constexpr std::int32_t HALF_MAX_EXPONENT = 31;
        constexpr std::int32_t HALF_MIN_SUBNORMAL_EXPONENT = -10;
        constexpr std::uint32_t MANTISSA_SHIFT = 13;
        constexpr std::uint32_t ROUND_MASK = 0x1FFF;
        constexpr std::uint32_t ROUND_HALF = 0x1000;
        constexpr std::uint32_t EXPONENT_SHIFT = 10;
        constexpr std::int32_t SUBNORMAL_SHIFT_BASE = 14;

        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        const std::uint32_t sign = (bits >> 16U) & HALF_SIGN;
        const auto floatExponent = static_cast<std::int32_t>((bits >> FLOAT_EXPONENT_SHIFT) & FLOAT_EXPONENT_MASK);
        std::uint32_t mantissa = bits & FLOAT_MANTISSA_MASK;
        if (floatExponent == static_cast<std::int32_t>(FLOAT_EXPONENT_MASK)) { // infinity or NaN
            return static_cast<std::uint16_t>(sign | HALF_INFINITY | (mantissa != 0 ? HALF_QUIET_NAN : 0));
        }
        const std::int32_t exponent = floatExponent - BIAS_DIFFERENCE;
        if (exponent >= HALF_MAX_EXPONENT) {
            return static_cast<std::uint16_t>(sign | HALF_INFINITY);
        }
        if (exponent <= 0) {
            if (exponent < HALF_MIN_SUBNORMAL_EXPONENT) {
                return static_cast<std::uint16_t>(sign); // too small: zero
            }
            mantissa |= FLOAT_IMPLICIT_ONE;
            const auto shift = static_cast<std::uint32_t>(SUBNORMAL_SHIFT_BASE - exponent);
            std::uint32_t half = mantissa >> shift;
            const std::uint32_t remainder = mantissa & ((1U << shift) - 1U);
            const std::uint32_t halfway = 1U << (shift - 1U);
            if (remainder > halfway || (remainder == halfway && (half & 1U) != 0)) {
                ++half;
            }
            return static_cast<std::uint16_t>(sign | half);
        }
        std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << EXPONENT_SHIFT) | (mantissa >> MANTISSA_SHIFT);
        const std::uint32_t remainder = mantissa & ROUND_MASK;
        if (remainder > ROUND_HALF || (remainder == ROUND_HALF && (half & 1U) != 0)) {
            ++half; // a carry into the exponent rounds up to the next power of two, or to infinity
        }
        return static_cast<std::uint16_t>(half);
    }
};

} // namespace XPMF
