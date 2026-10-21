#pragma once
// VHDL semantic model, part 1: types, compile-time values and their mapping
// to IR bits.
//
// Bit layout used everywhere (IR SigSpec offsets are LSB first):
//  * enumeration: the literal position (std_ulogic/bit/boolean: 1 bit; the
//    nine std_ulogic values map to 0/1/X/Z)
//  * integer: two's complement / unsigned according to the subtype range
//  * array: the left-most element occupies the most significant bits
//  * record: the first field occupies the most significant bits

#include "frontend/vhdl/VhdlAst.h"
#include "ir/Const.h"
#include "ir/SigSpec.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace opencdc::vhdl {

enum class TypeKind : std::uint8_t {
    Enum,
    Integer,
    Real,
    Physical,
    Array,
    Record,
    Access,
    File,
    Protected,
    UniversalInteger,
    UniversalReal,
    Incomplete,
};

/// Well-known types (identity of the predefined / IEEE types).
enum class TypeTag : std::uint8_t {
    None,
    Boolean,
    Bit,
    Character,
    SeverityLevel,
    Integer,
    Real,
    Time,
    String,
    BitVector,
    BooleanVector,
    IntegerVector,
    StdUlogic,
    StdUlogicVector,
    Unsigned,      ///< ieee.numeric_std.unsigned (and numeric_bit.unsigned)
    Signed,        ///< ieee.numeric_std.signed
    ArithUnsigned, ///< ieee.std_logic_arith.unsigned (Synopsys)
    ArithSigned,   ///< ieee.std_logic_arith.signed
    FileOpenKind,
    FileOpenStatus,
    Line,
    Text,
};

struct IntRange {
    std::int64_t left = 0;
    std::int64_t right = 0;
    bool ascending = true;

    [[nodiscard]] std::int64_t low() const noexcept { return ascending ? left : right; }
    [[nodiscard]] std::int64_t high() const noexcept { return ascending ? right : left; }
    [[nodiscard]] bool isNull() const noexcept { return ascending ? left > right : left < right; }
    [[nodiscard]] std::uint64_t length() const noexcept {
        return isNull() ? 0 : static_cast<std::uint64_t>(high() - low()) + 1;
    }
    [[nodiscard]] bool contains(std::int64_t v) const noexcept { return v >= low() && v <= high(); }
    /// 0-based position of index `v` counted from the left bound.
    [[nodiscard]] std::uint64_t positionOf(std::int64_t v) const noexcept {
        return static_cast<std::uint64_t>(ascending ? v - left : left - v);
    }
    [[nodiscard]] std::int64_t indexAt(std::uint64_t pos) const noexcept {
        return ascending ? left + static_cast<std::int64_t>(pos) : left - static_cast<std::int64_t>(pos);
    }
    friend bool operator==(const IntRange&, const IntRange&) = default;
};

struct RealRange {
    double left = 0;
    double right = 0;
    bool ascending = true;
};

struct Type;

struct EnumInfo {
    std::vector<std::string> keys;    ///< lookup keys ("idle", "'0'")
    std::vector<std::string> display; ///< as written
};

struct FieldInfo {
    std::string key;
    std::string display;
    const Type* type = nullptr;
};

struct PhysicalUnitInfo {
    std::string key;
    std::int64_t multiplier = 1; ///< in primary units
};

struct Type {
    TypeKind kind = TypeKind::Enum;
    TypeTag tag = TypeTag::None;
    std::string name;              ///< for messages
    const Type* base = nullptr;    ///< base type (itself for a base type)
    bool isSubtype = false;

    // scalar constraint (Enum/Integer/Physical: positions/values; Real: rrange)
    bool constrained = false;
    IntRange range;
    RealRange rrange;
    std::shared_ptr<EnumInfo> enumInfo;            ///< Enum (shared by subtypes)
    std::shared_ptr<std::vector<PhysicalUnitInfo>> units; ///< Physical

    // arrays
    std::vector<const Type*> indexTypes;            ///< per dimension: index subtype
    std::vector<std::optional<IntRange>> indexRanges; ///< per dimension: constraint (nullopt = unconstrained)
    const Type* element = nullptr;

    // records
    std::vector<FieldInfo> fields;

    // resolution function name (resolved subtypes such as std_logic)
    bool resolved = false;

    [[nodiscard]] const Type& baseType() const noexcept { return base ? *base : *this; }
    [[nodiscard]] bool isScalar() const noexcept {
        return kind == TypeKind::Enum || kind == TypeKind::Integer || kind == TypeKind::Real ||
               kind == TypeKind::Physical || kind == TypeKind::UniversalInteger || kind == TypeKind::UniversalReal;
    }
    [[nodiscard]] bool isDiscrete() const noexcept {
        return kind == TypeKind::Enum || kind == TypeKind::Integer || kind == TypeKind::UniversalInteger;
    }
    [[nodiscard]] bool isIntegerLike() const noexcept {
        return kind == TypeKind::Integer || kind == TypeKind::UniversalInteger;
    }
    [[nodiscard]] bool isRealLike() const noexcept {
        return kind == TypeKind::Real || kind == TypeKind::UniversalReal;
    }
    [[nodiscard]] bool isComposite() const noexcept { return kind == TypeKind::Array || kind == TypeKind::Record; }
    [[nodiscard]] bool isArray() const noexcept { return kind == TypeKind::Array; }
    [[nodiscard]] bool isFullyConstrained() const noexcept;
    [[nodiscard]] TypeTag baseTag() const noexcept { return baseType().tag; }
    /// Array whose elements are a character-literal enumeration (bit, std_ulogic, character).
    [[nodiscard]] bool isStringLike() const noexcept;
    /// One-dimensional array of std_ulogic / bit (incl. unsigned/signed).
    [[nodiscard]] bool isLogicVector() const noexcept;
    /// std_ulogic / bit / boolean (1-bit scalar).
    [[nodiscard]] bool isLogicScalar() const noexcept;
};

/// Owns all types created during elaboration (stable addresses).
class TypeArena {
public:
    Type* make(Type t);
    /// Subtype of `base` with a scalar range constraint.
    const Type* rangeSubtype(const Type* base, IntRange r, std::string name = {});
    /// Constrained subtype of the (possibly unconstrained) array type `t`.
    const Type* constrainedArray(const Type* t, std::vector<IntRange> ranges);
    /// Array of `element` constrained to `range` (anonymous base type).
    const Type* anonymousArray(const Type* element, const Type* indexType, IntRange range);

    /// One-dimensional array base types, in creation order (predefined first).
    [[nodiscard]] const std::vector<const Type*>& arrays() const noexcept { return arrays_; }

private:
    std::deque<Type> types_;
    std::vector<const Type*> arrays_;
};

// ---------------------------------------------------------------------------
// Compile-time values
// ---------------------------------------------------------------------------

struct Value {
    enum class Kind : std::uint8_t { None, Int, Real, Composite };
    Kind kind = Kind::None;
    std::int64_t i = 0;          ///< Int: integer value, enumeration position, physical value
    double r = 0.0;              ///< Real
    std::vector<Value> elems;    ///< Composite: array elements left to right, record fields in order

    static Value integer(std::int64_t v) {
        Value x;
        x.kind = Kind::Int;
        x.i = v;
        return x;
    }
    static Value real(double v) {
        Value x;
        x.kind = Kind::Real;
        x.r = v;
        return x;
    }
    static Value composite(std::vector<Value> e) {
        Value x;
        x.kind = Kind::Composite;
        x.elems = std::move(e);
        return x;
    }
    [[nodiscard]] bool isNone() const noexcept { return kind == Kind::None; }
    friend bool operator==(const Value&, const Value&) = default;
};

/// std_ulogic positions.
enum StdLogic : std::int64_t { SL_U = 0, SL_X, SL_0, SL_1, SL_Z, SL_W, SL_L, SL_H, SL_DC };

// ---------------------------------------------------------------------------
// Bit mapping
// ---------------------------------------------------------------------------

/// Number of bits of a value of (fully constrained) type `t`; nullopt for
/// types without a hardware representation (real, access, file,
/// unconstrained).
[[nodiscard]] std::optional<std::uint32_t> bitWidth(const Type& t);
/// Integer subtypes whose range includes negative values are signed.
[[nodiscard]] bool isSignedInteger(const Type& t);
/// Bits needed for an integer range.
[[nodiscard]] std::uint32_t integerBits(std::int64_t low, std::int64_t high);

/// Encodes a compile-time value of type `t` as constant bits.
[[nodiscard]] std::optional<ir::ConstBits> encode(const Type& t, const Value& v);
/// Decodes fully-defined constant bits; nullopt if bits are X/Z (or
/// unrepresentable).
[[nodiscard]] std::optional<Value> decode(const Type& t, const ir::ConstBits& bits);

/// Readable rendering of a value (for diagnostics, generic strings).
[[nodiscard]] std::string formatValue(const Type& t, const Value& v);

} // namespace opencdc::vhdl
