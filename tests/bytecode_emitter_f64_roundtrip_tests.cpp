#include "Bytecode.hpp"
#include "BytecodeTextEmitter.hpp"
#include "Value.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace {

bool sameBits(double left, double right)
{
    std::uint64_t leftBits = 0;
    std::uint64_t rightBits = 0;
    static_assert(sizeof(leftBits) == sizeof(left), "f64 must be 8 bytes");
    static_assert(sizeof(rightBits) == sizeof(right), "f64 must be 8 bytes");
    std::memcpy(&leftBits, &left, sizeof(left));
    std::memcpy(&rightBits, &right, sizeof(right));
    return leftBits == rightBits;
}

std::string emittedNumberText(double value)
{
    BytecodeProgram program;
    program.setConstants({Value::number(value)});

    std::ostringstream output;
    writeBytecodeText(output, program);
    const std::string text = output.str();

    const std::string marker = "c0 = number ";
    const std::size_t position = text.find(marker);
    assert(position != std::string::npos);
    const std::size_t start = position + marker.size();
    const std::size_t end = text.find('\n', start);
    assert(end != std::string::npos);
    return text.substr(start, end - start);
}

double reparsedValue(const std::string& text)
{
    return std::stod(text);
}

// Phase 0 baseline: the C++ bytecode text emitter formats f64 constants with
// std::setprecision(15), so values that need 16 or 17 significant digits lose
// precision across "double -> text bytecode -> parser -> double". Phase 14 of
// docs/cd-compiler-bytecode-0.2-execution-plan.md replaces 15 with
// std::numeric_limits<double>::max_digits10 and must then update these tests
// to require bitwise equality for every case.

void testSeventeenDigitFractionIsCurrentlyLossy()
{
    const double source = 0.30000000000000004;
    assert(!sameBits(source, reparsedValue(emittedNumberText(source))));
}

void testSeventeenDigitNeighborOfOneIsCurrentlyLossy()
{
    const double source = 1.0000000000000002;
    assert(!sameBits(source, reparsedValue(emittedNumberText(source))));
}

void testFifteenDigitValueRoundTripsExactly()
{
    const double source = 1.23456789012345;
    assert(sameBits(source, reparsedValue(emittedNumberText(source))));
}

void testSmallIntegerRoundTripsExactly()
{
    const double source = 7.0;
    assert(sameBits(source, reparsedValue(emittedNumberText(source))));
}

} // namespace

int main()
{
    testSeventeenDigitFractionIsCurrentlyLossy();
    testSeventeenDigitNeighborOfOneIsCurrentlyLossy();
    testFifteenDigitValueRoundTripsExactly();
    testSmallIntegerRoundTripsExactly();
    return 0;
}
