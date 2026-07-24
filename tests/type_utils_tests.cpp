#include "TypeUtils.hpp"

#include <cassert>
#include <memory>

int main()
{
    const TypeInfo t = typeParameterType("T");
    assert(typeInfoName(t) == "T");
    assert(SemanticTypes::isKnown(t));
    assert(!SemanticTypes::isKnown(unknownType()));
    assert(compatible(t, typeParameterType("T")));
    assert(!compatible(t, typeParameterType("U")));

    const TypeInfo number = simpleType(StaticType::Number);
    const TypeInfo boundedT = typeParameterType("T", number);
    assert(compatible(number, boundedT));
    assert(!compatible(simpleType(StaticType::String), boundedT));

    const TypeInfo identity = functionType(
        {t}, t, {"T"}, {std::make_shared<TypeInfo>(number)});
    assert(SemanticTypes::hasFunctionSignature(identity));
    assert(typeInfoName(identity) == "fun<T: number>(T): T");

    const TypeInfo boxedNumber = namedStructType("Box", {number});
    assert(typeInfoName(boxedNumber) == "Box<number>");
    assert(compatible(boxedNumber, namedStructType("Box", {number})));
    assert(!compatible(boxedNumber, namedStructType("Box", {simpleType(StaticType::String)})));

    const TypeInfo nested = arrayType(t);
    assert(compatible(nested, arrayType(typeParameterType("T"))));
    assert(!compatible(nested, arrayType(simpleType(StaticType::Number))));

    const TypeInfo stringNumberMap = mapType(
        simpleType(StaticType::String),
        simpleType(StaticType::Number));
    assert(typeInfoName(stringNumberMap) == "map<string, number>");
    assert(compatible(
        stringNumberMap,
        mapType(simpleType(StaticType::String), simpleType(StaticType::Number))));
    assert(!compatible(
        stringNumberMap,
        mapType(simpleType(StaticType::Number), simpleType(StaticType::Number))));
    assert(compatible(
        mapType(typeParameterType("K"), arrayType(typeParameterType("V"))),
        mapType(typeParameterType("K"), arrayType(typeParameterType("V")))));

    const TypeInfo range = simpleType(StaticType::Range);
    assert(typeInfoName(range) == "range");
    assert(compatible(range, simpleType(StaticType::Range)));
    assert(!compatible(range, simpleType(StaticType::Array)));

    TypeSubstitutions substitutions;
    substitutions.emplace("T", number);
    substitutions.emplace("U", simpleType(StaticType::String));
    const TypeInfo generic = functionType(
        {arrayType(t), mapType(t, typeParameterType("U"))},
        nullableType(t),
        {"T", "U"});
    const TypeInfo specialized = SemanticTypes::substituteTypeParameters(generic, substitutions);
    assert(typeInfoName(specialized) == "fun<T, U>([number], map<number, string>): number?");
    assert(SemanticTypes::isNullable(*specialized.returnType));
    assert(typeInfoName(substituteTypeParameters(t, substitutions)) == "number");
    assert(SemanticTypes::compatible(number, typeParameterType("T", number)));
    return 0;
}
