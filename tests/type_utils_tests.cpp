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

    const TypeInfo eq = capabilityType("Eq");
    const TypeInfo ord = capabilityType("Ord");
    const TypeInfo hash = capabilityType("Hash");
    assert(typeInfoName(eq) == "Eq");
    assert(typeInfoName(ord) == "Ord");
    assert(typeInfoName(hash) == "Hash");
    assert(SemanticTypes::isCapability(eq, "Eq"));
    assert(!SemanticTypes::isCapability(eq, "Ord"));
    assert(SemanticTypes::satisfiesCapability(number, eq));
    assert(SemanticTypes::satisfiesCapability(number, ord));
    assert(SemanticTypes::satisfiesCapability(number, hash));
    assert(SemanticTypes::satisfiesCapability(simpleType(StaticType::String), hash));
    assert(SemanticTypes::satisfiesCapability(typeParameterType("T", ord), eq));
    assert(!SemanticTypes::satisfiesCapability(typeParameterType("T", ord), hash));
    assert(!SemanticTypes::satisfiesCapability(typeParameterType("T", hash), eq));
    assert(SemanticTypes::satisfiesCapability(typeParameterType("T", number), ord));
    assert(!SemanticTypes::satisfiesCapability(typeParameterType("T"), eq));
    assert(SemanticTypes::satisfiesCapability(simpleType(StaticType::String), ord));

    const TypeInfo eqAndHash = capabilitySetType({eq, hash});
    assert(typeInfoName(eqAndHash) == "Eq + Hash");
    assert(SemanticTypes::isCapability(eqAndHash, "Eq"));
    assert(SemanticTypes::isCapability(eqAndHash, "Hash"));
    assert(SemanticTypes::satisfiesCapability(number, eqAndHash));
    assert(!SemanticTypes::satisfiesCapability(typeParameterType("T", eq), hash));

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

    TypeSubstitutions inferred;
    const TypeInfo genericSignature = functionType(
        {arrayType(typeParameterType("T"))},
        nullableType(typeParameterType("U")));
    const TypeInfo concreteSignature = functionType(
        {arrayType(number)},
        nullableType(simpleType(StaticType::String)));
    assert(!SemanticTypes::inferTypeArguments(genericSignature, concreteSignature, inferred));
    assert(typeInfoName(inferred.at("T")) == "number");
    assert(typeInfoName(inferred.at("U")) == "string");

    TypeSubstitutions conflicting;
    const auto conflict = SemanticTypes::inferTypeArguments(
        mapType(typeParameterType("T"), typeParameterType("T")),
        mapType(number, simpleType(StaticType::String)),
        conflicting);
    assert(conflict);
    assert(conflict->parameterName == "T");
    assert(typeInfoName(conflict->first) == "number");
    assert(typeInfoName(conflict->second) == "string");

    const std::vector<std::string> constrainedParameters{"T"};
    const std::vector<std::shared_ptr<TypeInfo>> constraints{
        std::make_shared<TypeInfo>(number)};
    TypeSubstitutions validConstraint;
    validConstraint.emplace("T", number);
    assert(!SemanticTypes::validateTypeParameterConstraints(
        constrainedParameters, constraints, validConstraint));

    TypeSubstitutions invalidConstraint;
    invalidConstraint.emplace("T", simpleType(StaticType::String));
    const auto violation = SemanticTypes::validateTypeParameterConstraints(
        constrainedParameters, constraints, invalidConstraint);
    assert(violation);
    assert(violation->parameterName == "T");
    assert(typeInfoName(violation->constraint) == "number");
    assert(typeInfoName(violation->actual) == "string");

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
