#include "Value.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <utility>
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

    auto fields = std::make_shared<std::vector<std::pair<std::string, Value>>>();
    Value node = Value::structure(StructValue{7, std::string("Node"), fields});
    fields->push_back({"next", node});
    assert(valuesEqual(node, node));
    assert(valueToString(node) == "{next: <cycle>}");

    auto childFields = std::make_shared<std::vector<std::pair<std::string, Value>>>();
    Value child = Value::structure(StructValue{8, std::string("Node"), childFields});
    childFields->push_back({"value", Value::number(1)});
    auto parentFields = std::make_shared<std::vector<std::pair<std::string, Value>>>();
    Value parent = Value::structure(StructValue{9, std::string("Pair"), parentFields});
    parentFields->push_back({"left", child});
    parentFields->push_back({"right", child});
    assert(valueToString(parent) == "{left: {value: 1}, right: {value: 1}}");

    std::cout << "value hash tests: C++ primitive hash contract validated\n";
    return 0;
}
