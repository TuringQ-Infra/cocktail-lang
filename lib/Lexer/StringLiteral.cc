#include "Cocktail/Lexer/StringLiteral.h"

#include "Cocktail/Common/CharacterSet.h"
#include "Cocktail/Common/Check.h"
#include "Cocktail/Diagnostics/DiagnosticEmitter.h"
#include "Cocktail/Lexer/LexHelpers.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"

namespace Cocktail {

using LexerDiagnosticEmitter = DiagnosticEmitter<const char*>;

static constexpr char MultiLineIndicator[] = R"(""")";

static auto GetMultiLineStringLiteralPrefixSize(llvm::StringRef source_text)
    -> int {
  if (!source_text.startswith(MultiLineIndicator)) {
    return 0;
  }

  auto prefix_end =
      source_text.find_first_of("#\n\"", strlen(MultiLineIndicator));
  if (prefix_end == llvm::StringRef::npos || source_text[prefix_end] != '\n') {
    return 0;
  }

  return prefix_end + 1;
}

auto LexedStringLiteral::Lex(llvm::StringRef source_text)
    -> llvm::Optional<LexedStringLiteral> {
  int64_t cursor = 0;
  const int64_t source_text_size = source_text.size();

  while (cursor < source_text_size && source_text[cursor] == '#') {
    ++cursor;
  }
  const int hash_level = cursor;

  llvm::SmallString<16> terminator("\"");
  llvm::SmallString<16> escape("\\");

  const int multi_line_prefix_size =
      GetMultiLineStringLiteralPrefixSize(source_text.substr(hash_level));
  const bool multi_line = multi_line_prefix_size > 0;
  if (multi_line) {
    cursor += multi_line_prefix_size;
    terminator = MultiLineIndicator;
  } else if (cursor < source_text_size && source_text[cursor] == '"') {
    ++cursor;
  } else {
    return llvm::None;
  }

  const int prefix_len = cursor;

  terminator.resize(terminator.size() + hash_level, '#');
  escape.resize(escape.size() + hash_level, '#');

  for (; cursor < source_text_size; ++cursor) {
    switch (source_text[cursor]) {
      case '\\':
        if (escape.size() == 1 ||
            source_text.substr(cursor).startswith(escape)) {
          cursor += escape.size();

          if (cursor >= source_text_size ||
              (!multi_line && source_text[cursor] == '\n')) {
            llvm::StringRef text = source_text.take_front(cursor);
            return LexedStringLiteral(text, text.drop_front(prefix_len),
                                      hash_level, multi_line,
                                      /*is_terminated=*/false);
          }
        }
        break;
      case '\n':
        if (!multi_line) {
          llvm::StringRef text = source_text.take_front(cursor);
          return LexedStringLiteral(text, text.drop_front(prefix_len),
                                    hash_level, multi_line,
                                    /*is_terminated=*/false);
        }
        break;
      case '\"': {
        if (terminator.size() == 1 ||
            source_text.substr(cursor).startswith(terminator)) {
          llvm::StringRef text =
              source_text.substr(0, cursor + terminator.size());
          llvm::StringRef content =
              source_text.substr(prefix_len, cursor - prefix_len);
          return LexedStringLiteral(text, content, hash_level, multi_line,
                                    /*is_terminated=*/true);
        }
        break;
      }
    }
  }

  return LexedStringLiteral(source_text, source_text.drop_front(prefix_len),
                            hash_level, multi_line,
                            /*is_terminated=*/false);
}

static auto ComputeIndentOfFinalLine(llvm::StringRef text) -> llvm::StringRef {
  int indent_end = text.size();
  for (int i = indent_end - 1; i >= 0; --i) {
    if (text[i] == '\n') {
      int indent_start = i + 1;
      return text.substr(indent_start, indent_end - indent_start);
    }
    if (!IsSpace(text[i])) {
      indent_end = i;
    }
  }
  llvm_unreachable("Given text is required to contain a newline.");
}

static auto CheckIndent(LexerDiagnosticEmitter& emitter, llvm::StringRef text,
                        llvm::StringRef content) -> llvm::StringRef {
  llvm::StringRef indent = ComputeIndentOfFinalLine(text);

  if (indent.end() != content.end()) {
    COCKTAIL_DIAGNOSTIC(
        ContentBeforeStringTerminator, Error,
        "Only whitespace is permitted before the closing `\"\"\"` of a "
        "multi-line string.");
    emitter.Emit(indent.end(), ContentBeforeStringTerminator);
  }

  return indent;
}

// Expand a `\u{HHHHHH}` escape sequence into a sequence of UTF-8 code units.
static auto ExpandUnicodeEscapeSequence(LexerDiagnosticEmitter& emitter,
                                        llvm::StringRef digits,
                                        std::string& result) -> bool {
  unsigned code_point;
  if (!CanLexInteger(emitter, digits)) {
    return false;
  }
  if (digits.getAsInteger(16, code_point) || code_point > 0x10FFFF) {
    COCKTAIL_DIAGNOSTIC(
        UnicodeEscapeTooLarge, Error,
        "Code point specified by `\\u{{...}}` escape is greater "
        "than 0x10FFFF.");
    emitter.Emit(digits.begin(), UnicodeEscapeTooLarge);
    return false;
  }

  if (code_point >= 0xD800 && code_point < 0xE000) {
    COCKTAIL_DIAGNOSTIC(UnicodeEscapeSurrogate, Error,
                        "Code point specified by `\\u{{...}}` escape is a "
                        "surrogate character.");
    emitter.Emit(digits.begin(), UnicodeEscapeSurrogate);
    return false;
  }

  const llvm::UTF32 utf32_code_units[1] = {code_point};
  llvm::UTF8 utf8_code_units[6];
  const llvm::UTF32* src_pos = utf32_code_units;
  llvm::UTF8* dest_pos = utf8_code_units;
  llvm::ConversionResult conv_result = llvm::ConvertUTF32toUTF8(
      &src_pos, src_pos + 1, &dest_pos, dest_pos + 6, llvm::strictConversion);
  if (conv_result != llvm::conversionOK) {
    llvm_unreachable("conversion of valid code point to UTF-8 cannot fail");
  }
  result.insert(result.end(), reinterpret_cast<char*>(utf8_code_units),
                reinterpret_cast<char*>(dest_pos));
  return true;
}

static auto ExpandAndConsumeEscapeSequence(LexerDiagnosticEmitter& emitter,
                                           llvm::StringRef& content,
                                           std::string& result) -> void {
  COCKTAIL_CHECK(!content.empty()) << "should have escaped closing delimiter";
  char first = content.front();
  content = content.drop_front(1);

  switch (first) {
    case 't':
      result += '\t';
      return;
    case 'n':
      result += '\n';
      return;
    case 'r':
      result += '\r';
      return;
    case '"':
      result += '"';
      return;
    case '\'':
      result += '\'';
      return;
    case '\\':
      result += '\\';
      return;
    case '0':
      result += '\0';
      if (!content.empty() && IsDecimalDigit(content.front())) {
        COCKTAIL_DIAGNOSTIC(
            DecimalEscapeSequence, Error,
            "Decimal digit follows `\\0` escape sequence. Use `\\x00` instead "
            "of `\\0` if the next character is a digit.");
        emitter.Emit(content.begin(), DecimalEscapeSequence);
        return;
      }
      return;
    case 'x':
      if (content.size() >= 2 && IsUpperHexDigit(content[0]) &&
          IsUpperHexDigit(content[1])) {
        result +=
            static_cast<char>(llvm::hexFromNibbles(content[0], content[1]));
        content = content.drop_front(2);
        return;
      }
      COCKTAIL_DIAGNOSTIC(HexadecimalEscapeMissingDigits, Error,
                          "Escape sequence `\\x` must be followed by two "
                          "uppercase hexadecimal digits, for example `\\x0F`.");
      emitter.Emit(content.begin(), HexadecimalEscapeMissingDigits);
      break;
    case 'u': {
      llvm::StringRef remaining = content;
      if (remaining.consume_front("{")) {
        llvm::StringRef digits = remaining.take_while(IsUpperHexDigit);
        remaining = remaining.drop_front(digits.size());
        if (!digits.empty() && remaining.consume_front("}")) {
          if (!ExpandUnicodeEscapeSequence(emitter, digits, result)) {
            break;
          }
          content = remaining;
          return;
        }
      }
      COCKTAIL_DIAGNOSTIC(
          UnicodeEscapeMissingBracedDigits, Error,
          "Escape sequence `\\u` must be followed by a braced sequence of "
          "uppercase hexadecimal digits, for example `\\u{{70AD}}`.");
      emitter.Emit(content.begin(), UnicodeEscapeMissingBracedDigits);
      break;
    }
    default:
      COCKTAIL_DIAGNOSTIC(UnknownEscapeSequence, Error,
                          "Unrecognized escape sequence `{0}`.", char);
      emitter.Emit(content.begin() - 1, UnknownEscapeSequence, first);
      break;
  }

  result += first;
}

// Expand any escape sequences in the given string literal.
static auto ExpandEscapeSequencesAndRemoveIndent(
    LexerDiagnosticEmitter& emitter, llvm::StringRef contents, int hash_level,
    llvm::StringRef indent) -> std::string {
  std::string result;
  result.reserve(contents.size());

  llvm::SmallString<16> escape("\\");
  escape.resize(1 + hash_level, '#');

  while (true) {
    if (!contents.consume_front(indent)) {
      const char* line_start = contents.begin();
      contents = contents.drop_while(IsHorizontalWhitespace);
      if (!contents.startswith("\n")) {
        COCKTAIL_DIAGNOSTIC(
            MismatchedIndentInString, Error,
            "Indentation does not match that of the closing \"\"\" in "
            "multi-line string literal.");
        emitter.Emit(line_start, MismatchedIndentInString);
      }
    }

    while (true) {
      auto end_of_regular_text = contents.find_if([](char c) {
        return c == '\n' || c == '\\' ||
               (IsHorizontalWhitespace(c) && c != ' ');
      });
      result += contents.substr(0, end_of_regular_text);
      contents = contents.substr(end_of_regular_text);

      if (contents.empty()) {
        return result;
      }

      if (contents.consume_front("\n")) {
        while (!result.empty() && result.back() != '\n' &&
               IsSpace(result.back())) {
          result.pop_back();
        }
        result += '\n';
        break;
      }

      if (IsHorizontalWhitespace(contents.front())) {
        COCKTAIL_CHECK(contents.front() != ' ')
            << "should not have stopped at a plain space";
        auto after_space = contents.find_if_not(IsHorizontalWhitespace);
        if (after_space == llvm::StringRef::npos ||
            contents[after_space] != '\n') {
          COCKTAIL_DIAGNOSTIC(
              InvalidHorizontalWhitespaceInString, Error,
              "Whitespace other than plain space must be expressed with an "
              "escape sequence in a string literal.");
          emitter.Emit(contents.begin(), InvalidHorizontalWhitespaceInString);
          result += contents.substr(0, after_space);
        }
        contents = contents.substr(after_space);
        continue;
      }

      if (!contents.consume_front(escape)) {
        result += contents.front();
        contents = contents.drop_front(1);
        continue;
      }

      if (contents.consume_front("\n")) {
        break;
      }

      ExpandAndConsumeEscapeSequence(emitter, contents, result);
    }
  }
}

auto LexedStringLiteral::ComputeValue(LexerDiagnosticEmitter& emitter) const
    -> std::string {
  if (!is_terminated_) {
    return "";
  }
  llvm::StringRef indent =
      multi_line_ ? CheckIndent(emitter, text_, content_) : llvm::StringRef();
  return ExpandEscapeSequencesAndRemoveIndent(emitter, content_, hash_level_,
                                              indent);
}
}  // namespace Cocktail