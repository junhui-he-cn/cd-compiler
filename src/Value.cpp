#include "Value.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {

class Fnv1a32 {
public:
    void byte(std::uint8_t value)
    {
        hash_ ^= value;
        hash_ *= 16777619u;
    }

    void number(std::uint64_t value)
    {
        for (std::size_t index = 0; index < sizeof(value); ++index) {
            byte(static_cast<std::uint8_t>(value >> (index * 8)));
        }
    }

    void text(const std::string& value)
    {
        number(static_cast<std::uint64_t>(value.size()));
        for (const unsigned char character : value) {
            byte(character);
        }
    }

    std::uint32_t finish() const
    {
        return hash_;
    }

private:
    std::uint32_t hash_ = 2166136261u;
};

struct ActiveReference {
    Value::Type type;
    std::size_t identity;

    bool operator==(const ActiveReference& other) const
    {
        return type == other.type && identity == other.identity;
    }
};

struct ActiveReferenceHash {
    std::size_t operator()(const ActiveReference& reference) const
    {
        const std::size_t typeHash = std::hash<int>{}(static_cast<int>(reference.type));
        const std::size_t identityHash = std::hash<std::size_t>{}(reference.identity);
        return typeHash ^ (identityHash + static_cast<std::size_t>(0x9e3779b9)
            + (typeHash << 6) + (typeHash >> 2));
    }
};

using ActiveReferences = std::unordered_set<ActiveReference, ActiveReferenceHash>;

void hashValueInto(Fnv1a32& hash, const Value& value)
{
    hash.byte(static_cast<std::uint8_t>(value.type()));
    switch (value.type()) {
    case Value::Type::Nil:
        return;
    case Value::Type::Number: {
        const double number = value.asNumber();
        std::uint64_t bits = 0;
        if (std::isnan(number)) {
            bits = 0x7ff8000000000000ULL;
        } else if (number != 0.0) {
            static_assert(sizeof(number) == sizeof(bits));
            std::memcpy(&bits, &number, sizeof(bits));
        }
        hash.number(bits);
        return;
    }
    case Value::Type::Bool:
        hash.byte(value.asBool() ? 1 : 0);
        return;
    case Value::Type::String:
        hash.text(value.asString());
        return;
    case Value::Type::Function:
        hash.number(static_cast<std::uint64_t>(value.asFunction().identity));
        return;
    case Value::Type::Array:
        hash.number(static_cast<std::uint64_t>(value.asArray().identity));
        return;
    case Value::Type::Map:
        hash.number(static_cast<std::uint64_t>(value.asMap().identity));
        return;
    case Value::Type::Range: {
        const RangeValue& range = value.asRange();
        hash.number(static_cast<std::uint64_t>(range.start));
        hash.number(static_cast<std::uint64_t>(range.stop));
        hash.number(static_cast<std::uint64_t>(range.step));
        return;
    }
    case Value::Type::Struct:
        hash.number(static_cast<std::uint64_t>(value.asStruct().identity));
        return;
    case Value::Type::Variant: {
        const VariantValue& variant = value.asVariant();
        hash.text(variant.enumName);
        hash.text(variant.variantName);
        hash.number(static_cast<std::uint64_t>(variant.fields->size()));
        for (const Value& field : *variant.fields) {
            hashValueInto(hash, field);
        }
        return;
    }
    }
}

} // namespace

Value::Value(Type type)
    : type_(type)
{
}

Value Value::nil()
{
    return Value(Type::Nil);
}

Value Value::number(double value)
{
    Value result(Type::Number);
    result.number_ = value;
    return result;
}

Value Value::boolean(bool value)
{
    Value result(Type::Bool);
    result.boolean_ = value;
    return result;
}

Value Value::string(std::string value)
{
    Value result(Type::String);
    result.string_ = std::move(value);
    return result;
}

Value Value::function(FunctionValue value)
{
    Value result(Type::Function);
    result.function_ = std::move(value);
    return result;
}

Value Value::array(ArrayValue value)
{
    Value result(Type::Array);
    result.array_ = std::make_shared<ArrayValue>(std::move(value));
    return result;
}

Value Value::map(MapValue value)
{
    Value result(Type::Map);
    result.map_ = std::make_shared<MapValue>(std::move(value));
    return result;
}

Value Value::range(RangeValue value)
{
    Value result(Type::Range);
    result.range_ = std::make_shared<RangeValue>(std::move(value));
    return result;
}

Value Value::structure(StructValue value)
{
    Value result(Type::Struct);
    result.struct_ = std::make_shared<StructValue>(std::move(value));
    return result;
}

Value Value::variant(VariantValue value)
{
    Value result(Type::Variant);
    result.variant_ = std::make_shared<VariantValue>(std::move(value));
    return result;
}

Value::Type Value::type() const
{
    return type_;
}

double Value::asNumber() const
{
    if (type_ != Type::Number) {
        throw std::runtime_error("value is not a number");
    }
    return number_;
}

bool Value::asBool() const
{
    if (type_ != Type::Bool) {
        throw std::runtime_error("value is not a bool");
    }
    return boolean_;
}

const std::string& Value::asString() const
{
    if (type_ != Type::String) {
        throw std::runtime_error("value is not a string");
    }
    return string_;
}

const FunctionValue& Value::asFunction() const
{
    if (type_ != Type::Function) {
        throw std::runtime_error("value is not a function");
    }
    return function_;
}

const ArrayValue& Value::asArray() const
{
    if (type_ != Type::Array || !array_) {
        throw std::runtime_error("value is not an array");
    }
    return *array_;
}

const MapValue& Value::asMap() const
{
    if (type_ != Type::Map || !map_) {
        throw std::runtime_error("value is not a map");
    }
    return *map_;
}

const RangeValue& Value::asRange() const
{
    if (type_ != Type::Range || !range_) {
        throw std::runtime_error("value is not a range");
    }
    return *range_;
}

const StructValue& Value::asStruct() const
{
    if (type_ != Type::Struct || !struct_) {
        throw std::runtime_error("value is not a struct");
    }
    return *struct_;
}

const VariantValue& Value::asVariant() const
{
    if (type_ != Type::Variant || !variant_) {
        throw std::runtime_error("value is not an enum variant");
    }
    return *variant_;
}

bool isTruthy(const Value& value)
{
    if (value.type() == Value::Type::Nil) {
        return false;
    }
    if (value.type() == Value::Type::Bool) {
        return value.asBool();
    }
    return true;
}

bool valuesEqual(const Value& left, const Value& right)
{
    if (left.type() != right.type()) {
        return false;
    }

    switch (left.type()) {
    case Value::Type::Nil:
        return true;
    case Value::Type::Number:
        return left.asNumber() == right.asNumber();
    case Value::Type::Bool:
        return left.asBool() == right.asBool();
    case Value::Type::String:
        return left.asString() == right.asString();
    case Value::Type::Function:
        return left.asFunction().identity == right.asFunction().identity;
    case Value::Type::Array:
        return left.asArray().identity == right.asArray().identity;
    case Value::Type::Map:
        return left.asMap().identity == right.asMap().identity;
    case Value::Type::Range:
        return left.asRange().start == right.asRange().start
            && left.asRange().stop == right.asRange().stop
            && left.asRange().step == right.asRange().step;
    case Value::Type::Struct:
        return left.asStruct().identity == right.asStruct().identity;
    case Value::Type::Variant: {
        const VariantValue& leftVariant = left.asVariant();
        const VariantValue& rightVariant = right.asVariant();
        if (leftVariant.enumName != rightVariant.enumName
            || leftVariant.variantName != rightVariant.variantName
            || leftVariant.fields->size() != rightVariant.fields->size()) {
            return false;
        }
        for (std::size_t i = 0; i < leftVariant.fields->size(); ++i) {
            if (!valuesEqual((*leftVariant.fields)[i], (*rightVariant.fields)[i])) {
                return false;
            }
        }
        return true;
    }
    }

    return false;
}

std::uint32_t valueHash(const Value& value)
{
    Fnv1a32 hash;
    hashValueInto(hash, value);
    return hash.finish();
}

std::string valueToString(const Value& value, ActiveReferences& activeReferences)
{
    switch (value.type()) {
    case Value::Type::Nil:
        return "nil";
    case Value::Type::Number: {
        std::ostringstream out;
        out << std::setprecision(15) << value.asNumber();
        return out.str();
    }
    case Value::Type::Bool:
        return value.asBool() ? "true" : "false";
    case Value::Type::String:
        return value.asString();
    case Value::Type::Function:
        return "<fun " + value.asFunction().name + ">";
    case Value::Type::Array: {
        const ActiveReference reference{Value::Type::Array, value.asArray().identity};
        if (!activeReferences.insert(reference).second) {
            return "<cycle>";
        }
        std::ostringstream out;
        out << '[';
        const auto& elements = *value.asArray().elements;
        for (std::size_t i = 0; i < elements.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << valueToString(elements[i], activeReferences);
        }
        out << ']';
        activeReferences.erase(reference);
        return out.str();
    }
    case Value::Type::Map: {
        const ActiveReference reference{Value::Type::Map, value.asMap().identity};
        if (!activeReferences.insert(reference).second) {
            return "<cycle>";
        }
        std::ostringstream out;
        out << "map{";
        const auto& entries = *value.asMap().entries;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << valueToString(entries[i].first, activeReferences) << ": "
                << valueToString(entries[i].second, activeReferences);
        }
        out << '}';
        activeReferences.erase(reference);
        return out.str();
    }
    case Value::Type::Range: {
        const RangeValue& range = value.asRange();
        return "range(" + std::to_string(range.start) + ", "
            + std::to_string(range.stop) + ", " + std::to_string(range.step) + ")";
    }
    case Value::Type::Struct: {
        const ActiveReference reference{Value::Type::Struct, value.asStruct().identity};
        if (!activeReferences.insert(reference).second) {
            return "<cycle>";
        }
        std::ostringstream out;
        out << '{';
        const auto& fields = *value.asStruct().fields;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << fields[i].first << ": "
                << valueToString(fields[i].second, activeReferences);
        }
        out << '}';
        activeReferences.erase(reference);
        return out.str();
    }
    case Value::Type::Variant: {
        const VariantValue& variant = value.asVariant();
        std::ostringstream out;
        out << variant.enumName << '.' << variant.variantName;
        if (!variant.fields->empty()) {
            out << '(';
            for (std::size_t i = 0; i < variant.fields->size(); ++i) {
                if (i != 0) {
                    out << ", ";
                }
                out << valueToString((*variant.fields)[i], activeReferences);
            }
            out << ')';
        }
        return out.str();
    }
    }

    return "<unknown>";
}

std::string valueToString(const Value& value)
{
    ActiveReferences activeReferences;
    return valueToString(value, activeReferences);
}

std::ostream& operator<<(std::ostream& out, const Value& value)
{
    out << valueToString(value);
    return out;
}
