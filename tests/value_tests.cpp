#include "Value.hpp"

#include <cassert>
#include <iostream>

int main()
{
    assert(valueHash(Value::nil()) == 84696351u);
    assert(valueHash(Value::number(42.0)) == 1983088465u);
    assert(valueHash(Value::boolean(true)) == 1551600396u);
    assert(valueHash(Value::string("hello")) == 910946861u);
    assert(valueHash(Value::number(-0.0)) == valueHash(Value::number(0.0)));
    assert(valueHash(Value::string("hello")) != valueHash(Value::string("world")));
    assert(valueHash(Value::nil()) != valueHash(Value::boolean(false)));
    std::cout << "value hash tests: C++ primitive hash contract validated\n";
    return 0;
}
