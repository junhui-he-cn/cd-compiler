#include "Formatter.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"

#include <cassert>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

LosslessSourceFileView losslessViewFor(const std::string& source)
{
    Lexer lexer(source);
    const std::vector<Token> tokens = lexer.scanTokens();
    return buildLosslessSourceFileView(
        SourceFile{"formatter.cd", source, SourceFileId{0}},
        tokens);
}

Program parseProgram(const std::string& source)
{
    Lexer lexer(source);
    Parser parser(lexer.scanTokens());
    return parser.parse();
}

std::string astFor(const std::string& source)
{
    Program program = parseProgram(source);
    std::ostringstream output;
    program.print(output);
    return output.str();
}

void test_formats_lossless_source_and_preserves_semantics()
{
    const std::string source =
        "// header\n"
        "fun identity<T>(x:T):T{// body\n"
        "return x;\n"
        "}\n"
        "let data={\"a\":1,\"b\":[2,3]}; // data\n"
        "if(data[\"a\"]>0){// branch\n"
        "print data[\"a\"];\n"
        "}else{print 0;}\n";

    const LosslessSourceFileView view = losslessViewFor(source);
    assert(view.roundTrips(source));
    const std::string formatted = formatLosslessSource(view);
    const std::string expected =
        "// header\n"
        "fun identity<T>(x: T): T { // body\n"
        "  return x;\n"
        "}\n"
        "let data = {\n"
        "  \"a\": 1,\n"
        "  \"b\": [2, 3]\n"
        "}; // data\n"
        "if (data[\"a\"] > 0) { // branch\n"
        "  print data[\"a\"];\n"
        "} else {\n"
        "  print 0;\n"
        "}\n";
    assert(formatted == expected);
    assert(formatLosslessSource(losslessViewFor(formatted)) == formatted);
    assert(astFor(source) == astFor(formatted));

    const std::string wide = formatLosslessSource(view, FormatterOptions{4});
    assert(wide.find("    return x;") != std::string::npos);
    assert(wide.find("    print data[\"a\"];") != std::string::npos);

    const std::size_t first = formatted.find("// header");
    const std::size_t second = formatted.find("// body");
    const std::size_t third = formatted.find("// data");
    const std::size_t fourth = formatted.find("// branch");
    assert(first < second && second < third && third < fourth);
}

void test_formats_generic_calls_and_for_headers()
{
    const std::string source =
        "fun id<T>(value:T):T{return value;}\n"
        "let x=id<number>(1);\n"
        "for let i=0;i<2;i+=1{print id<number>(i);}\n";
    const std::string formatted = formatLosslessSource(losslessViewFor(source));
    const std::string expected =
        "fun id<T>(value: T): T {\n"
        "  return value;\n"
        "}\n"
        "let x = id<number>(1);\n"
        "for let i = 0; i < 2; i += 1 {\n"
        "  print id<number>(i);\n"
        "}\n";
    assert(formatted == expected);
    assert(astFor(source) == astFor(formatted));
}

void test_preserves_top_level_blank_lines_only()
{
    const std::string source =
        "\n\n"
        "let first=1;\n\n\n"
        "// between\n\n\n"
        "let second=2;\n"
        "if(true){\n"
        "print 1;\n\n"
        "print 2;\n"
        "}\n\n\n"
        "let third=3;\n\n";
    const std::string formatted = formatLosslessSource(losslessViewFor(source));
    const std::string expected =
        "let first = 1;\n\n"
        "// between\n\n"
        "let second = 2;\n"
        "if (true) {\n"
        "  print 1;\n"
        "  print 2;\n"
        "}\n\n"
        "let third = 3;\n";
    assert(formatted == expected);
    assert(formatLosslessSource(losslessViewFor(formatted)) == formatted);
    assert(astFor(source) == astFor(formatted));
}

void test_preserves_supported_trailing_commas()
{
    const std::string source =
        "enum Choice{First,Second,}\n"
        "let value=match 1{1=>2,_=>0,};\n";
    const std::string formatted = formatLosslessSource(losslessViewFor(source));
    const std::string expected =
        "enum Choice {\n"
        "  First,\n"
        "  Second,\n"
        "}\n"
        "let value = match 1 {\n"
        "  1 => 2,\n"
        "  _ => 0,\n"
        "};\n";
    assert(formatted == expected);
    assert(formatLosslessSource(losslessViewFor(formatted)) == formatted);
    assert(astFor(source) == astFor(formatted));
}

void test_wraps_long_delimited_lists()
{
    const std::string first = "123456789012345678901234567890123456789012345";
    const std::string second = "234567890123456789012345678901234567890123456";
    const std::string third = "345678901234567890123456789012345678901234567";
    const std::string firstString = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const std::string secondString = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const std::string source =
        "let values=[" + first + "," + second + "," + third + "];\n"
        "let result=concat(\"" + firstString + "\",\"" + secondString + "\");\n"
        "let expression=111111111111111111111111111111111111111111111111111111111111+222222222222222222222222222222222222222222222222222222222222;\n"
        "let grouped=(111111111111111111111111111111111111111111111111111111111111+222222222222222222222222222222222222222222222222222222222222);\n";
    const std::string formatted = formatLosslessSource(losslessViewFor(source));
    const std::string expected =
        "let values = [\n"
        "  " + first + ",\n"
        "  " + second + ",\n"
        "  " + third + "\n"
        "];\n"
        "let result = concat(\n"
        "  \"" + firstString + "\",\n"
        "  \"" + secondString + "\"\n"
        ");\n"
        "let expression = 111111111111111111111111111111111111111111111111111111111111 + 222222222222222222222222222222222222222222222222222222222222;\n"
        "let grouped = (111111111111111111111111111111111111111111111111111111111111 + 222222222222222222222222222222222222222222222222222222222222);\n";
    assert(formatted == expected);
    assert(formatLosslessSource(losslessViewFor(formatted)) == formatted);
    assert(astFor(source) == astFor(formatted));
}

void test_empty_and_invalid_options()
{
    assert(formatLosslessSource(losslessViewFor("")) == "");
    try {
        static_cast<void>(formatLosslessSource(losslessViewFor("let x=1;"), FormatterOptions{0}));
    } catch (const std::invalid_argument& error) {
        assert(std::string(error.what()) == "formatter indentation width must be positive");
        return;
    }
    assert(false && "expected invalid formatter options");
}

} // namespace

int main()
{
    test_formats_lossless_source_and_preserves_semantics();
    test_formats_generic_calls_and_for_headers();
    test_preserves_top_level_blank_lines_only();
    test_preserves_supported_trailing_commas();
    test_wraps_long_delimited_lists();
    test_empty_and_invalid_options();
    return 0;
}
