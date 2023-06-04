#include "Cocktail/Lexer/NumericLiteral.h"

#include <bitset>

#include "Cocktail/Lexer/CharacterSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"

namespace Cocktail {

namespace {

struct EmptyDigitSequence : SimpleDiagnostic<EmptyDigitSequence> {
  static constexpr llvm::StringLiteral ShortName = "syntax-invalid-number";
  static constexpr llvm::StringLiteral Message =
      "Empty digit sequence in numeric literal.";
};

struct InvalidDigit {
  static constexpr llvm::StringLiteral ShortName = "syntax-invalid-number";

  char digit;
  int radix;

  auto Format() -> std::string {
    return llvm::formatv(
               "Invalid digit '{0}' in {1} numeric literal.", digit,
               (radix == 2 ? "binary"
                           : (radix == 16 ? "hexadecimal" : "decimal")))
        .str();
  }
};

struct InvalidDigitSeparator : SimpleDiagnostic<InvalidDigitSeparator> {
  static constexpr llvm::StringLiteral ShortName = "syntax-invalid-number";
  static constexpr llvm::StringLiteral Message =
      "Misplaced digit separator in numeric literal.";
};

struct IrregularDigitSeparators {
  static constexpr llvm::StringLiteral ShortName =
      "syntax-irregular-digit-separators";

  int radix;

  auto Format() -> std::string {
    assert((radix == 10 || radix == 16) && "unexpected radix");
    return llvm::formatv(
               "Digit separators in {0} number should appear every {1} "
               "characters from the right.",
               (radix == 10 ? "decimal" : "hexadecimal"),
               (radix == 10 ? "3" : "4"))
        .str();
  }
};

struct UnknownBaseSpecifier : SimpleDiagnostic<UnknownBaseSpecifier> {
  static constexpr llvm::StringLiteral ShortName = "syntax-invalid-number";
  static constexpr llvm::StringLiteral Message =
      "Unknown base specifier in numeric literal.";
};

struct BinaryRealLiteral : SimpleDiagnostic<BinaryRealLiteral> {
  static constexpr llvm::StringLiteral ShortName = "syntax-invalid-number";
  static constexpr llvm::StringLiteral Message =
      "Binary real number literals are not supported.";
};

struct WrongRealLiteralExponent {
  static constexpr llvm::StringLiteral ShortName = "syntax-invalid-number";

  char expected;

  auto Format() -> std::string {
    return llvm::formatv("Expected '{0}' to introduce exponent.", expected)
        .str();
  }
};

}  // namespace

auto LexedNumericLiteral::Lex(llvm::StringRef source_text)
    -> llvm::Optional<LexedNumericLiteral> {
  LexedNumericLiteral result;

  if (source_text.empty() || !IsDecimalDigit(source_text.front())) {
    return llvm::None;
  }

  bool seen_plus_minus = false;
  bool seen_radix_point = false;
  bool seen_potential_exponent = false;

  int i = 1;
  for (int n = source_text.size(); i != n; ++i) {
    char c = source_text[i];
    if (IsAlnum(c) || c == '_') {
      if (IsLower(c) && seen_radix_point && !seen_plus_minus) {
        result.exponent = i;
        seen_potential_exponent = true;
      }
      continue;
    }

    if (c == '.' && i + 1 != n && IsAlnum(source_text[i + 1]) &&
        !seen_radix_point) {
      result.radix_point = i;
      seen_radix_point = true;
      continue;
    }

    if ((c == '+' || c == '-') && seen_potential_exponent &&
        result.exponent == i - 1 && i + 1 != n && IsAlnum(source_text[i + 1])) {
      assert(!seen_plus_minus && "should only consume one + or -");
      seen_plus_minus = true;
      continue;
    }
    break;
  }

  result.text = source_text.substr(0, i);
  if (!seen_radix_point) {
    result.radix_point = i;
  }
  if (!seen_potential_exponent) {
    result.exponent = i;
  }

  return result;
}

class LexedNumericLiteral::Parser {
 public:
  Parser(DiagnosticEmitter<const char*>& emitter, LexedNumericLiteral literal);

  auto IsInteger() -> bool {
    return literal.radix_point == static_cast<int>(literal.Text().size());
  }

  auto Check() -> bool;

  auto GetRadix() const -> int { return radix; }

  auto GetMantissa() -> llvm::APInt;

  auto GetExponent() -> llvm::APInt;

 private:
  struct CheckDigitSequenceResult {
    bool ok;
    bool has_digit_separators = false;
  };

  auto CheckDigitSequence(llvm::StringRef text, int radix,
                          bool allow_digit_separators = true)
      -> CheckDigitSequenceResult;

  auto CheckDigitSeparatorPlacement(llvm::StringRef text, int radix,
                                    int num_digit_separators) -> void;
  auto CheckLeadingZero() -> bool;
  auto CheckIntPart() -> bool;
  auto CheckFractionalPart() -> bool;
  auto CheckExponentPart() -> bool;

 private:
  DiagnosticEmitter<const char*>& emitter;
  LexedNumericLiteral literal;

  // The radix of the literal: 2, 10, or 16
  int radix = 10;

  // [radix] int_part [. fract_part [[ep] [+-] exponent_part]]
  llvm::StringRef int_part;
  llvm::StringRef fract_part;
  llvm::StringRef exponent_part;

  bool mantissa_needs_cleaning = false;
  bool exponent_needs_cleaning = false;

  // True if we found a `-` before `exponent_part`.
  bool exponent_is_negative = false;
};

LexedNumericLiteral::Parser::Parser(DiagnosticEmitter<const char*>& emitter,
                                    LexedNumericLiteral literal)
    : emitter(emitter), literal(literal) {
  int_part = literal.text.substr(0, literal.radix_point);
  if (int_part.consume_front("0x")) {
    radix = 16;
  } else if (int_part.consume_front("0b")) {
    radix = 2;
  }

  fract_part = literal.text.substr(literal.radix_point + 1,
                                   literal.exponent - literal.radix_point - 1);

  exponent_part = literal.text.substr(literal.exponent + 1);
  if (!exponent_part.consume_front("+")) {
    exponent_is_negative = exponent_part.consume_front("-");
  }
}

auto LexedNumericLiteral::Parser::Check() -> bool {
  return CheckLeadingZero() && CheckIntPart() && CheckFractionalPart() &&
         CheckExponentPart();
}

static auto ParseInteger(llvm::StringRef digits, int radix, bool needs_cleaning)
    -> llvm::APInt {
  llvm::SmallString<32> cleaned;
  if (needs_cleaning) {
    cleaned.reserve(digits.size());
    std::remove_copy_if(digits.begin(), digits.end(),
                        std::back_inserter(cleaned),
                        [](char c) { return c == '_' || c == '.'; });
    digits = cleaned;
  }

  llvm::APInt value;
  if (digits.getAsInteger(radix, value)) {
    llvm_unreachable("should never fail");
  }
  return value;
}

auto LexedNumericLiteral::Parser::GetMantissa() -> llvm::APInt {
  const char* end = IsInteger() ? int_part.end() : fract_part.end();
  llvm::StringRef digits(int_part.begin(), end - int_part.begin());
  return ParseInteger(digits, radix, mantissa_needs_cleaning);
}

auto LexedNumericLiteral::Parser::GetExponent() -> llvm::APInt {
  llvm::APInt exponent(64, 0);
  if (!exponent_part.empty()) {
    exponent = ParseInteger(exponent_part, 10, exponent_needs_cleaning);

    if (exponent.isSignBitSet() || exponent.getBitWidth() < 64) {
      exponent = exponent.zext(std::max(64U, exponent.getBitWidth() + 1));
    }

    if (exponent_is_negative) {
      exponent.negate();
    }
  }

  int excess_exponent = fract_part.size();
  if (radix == 16) {
    excess_exponent *= 4;
  }
  exponent -= excess_exponent;
  if (exponent_is_negative && !exponent.isNegative()) {
    exponent = exponent.zext(exponent.getBitWidth() + 1);
    exponent.setSignBit();
  }
  return exponent;
}

auto LexedNumericLiteral::Parser::CheckDigitSequence(
    llvm::StringRef text, int radix, bool allow_digit_separators)
    -> CheckDigitSequenceResult {
  assert((radix == 2 || radix == 10 || radix == 16) && "unknown radix");

  std::bitset<256> valid_digits;
  if (radix == 2) {
    for (char c : "01") {
      valid_digits[static_cast<unsigned char>(c)] = true;
    }
  } else if (radix == 10) {
    for (char c : "0123456789") {
      valid_digits[static_cast<unsigned char>(c)] = true;
    }
  } else {
    for (char c : "0123456789ABCDEF") {
      valid_digits[static_cast<unsigned char>(c)] = true;
    }
  }

  int num_digit_separators = 0;

  for (int i = 0, n = text.size(); i != n; ++i) {
    char c = text[i];
    if (valid_digits[static_cast<unsigned char>(c)]) {
      continue;
    }

    if (c == '_') {
      if (!allow_digit_separators || i == 0 || text[i - 1] == '_' ||
          i + 1 == n) {
        emitter.EmitError<InvalidDigitSeparator>(text.begin() + i);
      }
      ++num_digit_separators;
      continue;
    }

    emitter.EmitError<InvalidDigit>(text.begin() + i,
                                    {.digit = c, .radix = radix});
    return {.ok = false};
  }

  if (num_digit_separators == static_cast<int>(text.size())) {
    emitter.EmitError<EmptyDigitSequence>(text.begin());
    return {.ok = false};
  }

  if (num_digit_separators) {
    CheckDigitSeparatorPlacement(text, radix, num_digit_separators);
  }

  return {.ok = true, .has_digit_separators = (num_digit_separators != 0)};
}

auto LexedNumericLiteral::Parser::CheckDigitSeparatorPlacement(
    llvm::StringRef text, int radix, int num_digit_separators) -> void {
  assert(std::count(text.begin(), text.end(), '_') == num_digit_separators &&
         "given wrong number of digit separators");

  if (radix == 2) {
    return;
  }

  assert((radix == 10 || radix == 16) &&
         "unexpected radix for digit separator checks");

  auto diagnose_irregular_digit_separators = [&] {
    emitter.EmitError<IrregularDigitSeparators>(text.begin(), {.radix = radix});
  };

  int stride = (radix == 10 ? 4 : 5);
  int remaining_digit_separators = num_digit_separators;
  const auto* pos = text.end();
  while (pos - text.begin() >= stride) {
    pos -= stride;
    if (*pos != '_') {
      diagnose_irregular_digit_separators();
      return;
    }

    --remaining_digit_separators;
  }

  if (remaining_digit_separators) {
    diagnose_irregular_digit_separators();
  }
}

auto LexedNumericLiteral::Parser::CheckLeadingZero() -> bool {
  if (radix == 10 && int_part.startswith("0") && int_part != "0") {
    emitter.EmitError<UnknownBaseSpecifier>(int_part.begin());
    return false;
  }
  return true;
}

auto LexedNumericLiteral::Parser::CheckIntPart() -> bool {
  auto int_result = CheckDigitSequence(int_part, radix);
  mantissa_needs_cleaning |= int_result.has_digit_separators;
  return int_result.ok;
}

auto LexedNumericLiteral::Parser::CheckFractionalPart() -> bool {
  if (IsInteger()) {
    return true;
  }

  if (radix == 2) {
    emitter.EmitError<BinaryRealLiteral>(literal.text.begin() +
                                         literal.radix_point);
  }

  mantissa_needs_cleaning = true;
  return CheckDigitSequence(fract_part, radix, false).ok;
}

auto LexedNumericLiteral::Parser::CheckExponentPart() -> bool {
  if (literal.exponent == static_cast<int>(literal.text.size())) {
    return true;
  }

  char expected_exponent_kind = (radix == 10 ? 'e' : 'p');
  if (literal.text[literal.exponent] != expected_exponent_kind) {
    emitter.EmitError<WrongRealLiteralExponent>(
        literal.text.begin() + literal.exponent,
        {.expected = expected_exponent_kind});
    return false;
  }

  auto exponent_result = CheckDigitSequence(exponent_part, 10);
  exponent_needs_cleaning = exponent_result.has_digit_separators;
  return exponent_result.ok;
}

// Parse the token and compute its value.
auto LexedNumericLiteral::ComputeValue(
    DiagnosticEmitter<const char*>& emitter) const -> Value {
  Parser parser(emitter, *this);

  if (!parser.Check()) {
    return UnrecoverableError();
  }

  if (parser.IsInteger()) {
    return IntegerValue{.value = parser.GetMantissa()};
  }

  return RealValue{.radix = (parser.GetRadix() == 10 ? 10 : 2),
                   .mantissa = parser.GetMantissa(),
                   .exponent = parser.GetExponent()};
}

}  // namespace Cocktail