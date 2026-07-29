#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct Environment;
struct ArrayValue;
struct MapValue;
struct RangeValue;
struct StructValue;
struct VariantValue;

struct FunctionValue {
    std::string name;
    std::size_t functionIndex = 0;
    std::size_t arity = 0;
    std::size_t identity = 0;
    std::shared_ptr<Environment> closure;
};

class Value {
public:
    enum class Type {
        Nil,
        Number,
        Bool,
        String,
        Function,
        Array,
        Map,
        Range,
        Struct,
        Variant,
    };

    static Value nil();
    static Value number(double value);
    static Value boolean(bool value);
    static Value string(std::string value);
    static Value function(FunctionValue value);
    static Value array(ArrayValue value);
    static Value map(MapValue value);
    static Value range(RangeValue value);
    static Value structure(StructValue value);
    static Value variant(VariantValue value);

    Type type() const;
    double asNumber() const;
    bool asBool() const;
    const std::string& asString() const;
    const FunctionValue& asFunction() const;
    const ArrayValue& asArray() const;
    const MapValue& asMap() const;
    const RangeValue& asRange() const;
    const StructValue& asStruct() const;
    const VariantValue& asVariant() const;

private:
    explicit Value(Type type);

    Type type_ = Type::Nil;
    double number_ = 0.0;
    bool boolean_ = false;
    std::string string_;
    FunctionValue function_;
    std::shared_ptr<ArrayValue> array_;
    std::shared_ptr<MapValue> map_;
    std::shared_ptr<RangeValue> range_;
    std::shared_ptr<StructValue> struct_;
    std::shared_ptr<VariantValue> variant_;
};

struct ArrayValue {
    std::size_t identity = 0;
    std::shared_ptr<std::vector<Value>> elements;
};

struct MapValue {
    std::size_t identity = 0;
    std::shared_ptr<std::vector<std::pair<Value, Value>>> entries;
};

struct RangeValue {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    std::int64_t step = 1;
    std::size_t length = 0;
};

struct StructValue {
    std::size_t identity = 0;
    std::optional<std::string> typeName;
    std::shared_ptr<std::vector<std::pair<std::string, Value>>> fields;
};

struct VariantValue {
    std::string enumName;
    std::string variantName;
    std::shared_ptr<std::vector<Value>> fields;
};

struct Cell {
    explicit Cell(Value value)
        : value(std::move(value))
    {
    }

    Value value;
};

struct Environment {
    std::unordered_map<std::string, std::shared_ptr<Cell>> values;
};

bool isTruthy(const Value& value);
bool valuesEqual(const Value& left, const Value& right);
std::uint32_t valueHash(const Value& value);
std::string valueToString(const Value& value);
std::ostream& operator<<(std::ostream& out, const Value& value);
