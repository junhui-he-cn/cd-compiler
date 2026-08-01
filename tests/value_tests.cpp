#include "Value.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

int main()
{
    assert(valueHash(Value::nil()) == 84696351u);
    assert(valueHash(Value::number(42.0)) == 1983088465u);
    assert(valueHash(Value::boolean(true)) == 1551600396u);
    assert(valueHash(Value::string("hello")) == 910946861u);
    assert(valueHash(Value::number(-0.0)) == valueHash(Value::number(0.0)));
    assert(valueHash(Value::string("hello")) != valueHash(Value::string("world")));
    assert(valueHash(Value::nil()) != valueHash(Value::boolean(false)));

    const auto arrayElements = std::make_shared<std::vector<Value>>(
        std::vector<Value>{Value::number(1.0)});
    const Value arrayKey = Value::array(ArrayValue{41, arrayElements});
    const Value arrayAlias = arrayKey;
    const std::uint32_t arrayHash = valueHash(arrayKey);
    assert(valuesEqual(arrayKey, arrayAlias));
    arrayElements->push_back(Value::number(2.0));
    assert(valuesEqual(arrayKey, arrayAlias));
    assert(valueHash(arrayKey) == arrayHash);

    const auto distinctArrayElements = std::make_shared<std::vector<Value>>(
        std::vector<Value>{Value::number(1.0), Value::number(2.0)});
    const Value distinctArray = Value::array(ArrayValue{42, distinctArrayElements});
    assert(!valuesEqual(arrayKey, distinctArray));

    const auto structFields = std::make_shared<std::vector<std::pair<std::string, Value>>>(
        std::vector<std::pair<std::string, Value>>{{"value", Value::number(1.0)}});
    const Value structKey = Value::structure(StructValue{7, "MutableKey", structFields});
    const std::uint32_t structHash = valueHash(structKey);
    (*structFields)[0].second = Value::number(2.0);
    assert(valueHash(structKey) == structHash);
    assert(valuesEqual(structKey, structKey));

    std::cout << "value hash tests: C++ primitive hash contract validated\n";
    std::cout << "value hash tests: reference key identity remains stable after mutation\n";
    return 0;
}
