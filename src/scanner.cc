#include <tree_sitter/parser.h>
#include <vector>
#include <cwctype>
#include <cstring>
#include <cassert>
#include <stdio.h>
namespace {

using std::vector;
using std::iswspace;
using std::memcpy;

enum TokenType {
  NEWLINE,
  STRING_START,
  STRING_CONTENT,
  STRING_END,
};

struct Delimiter {
  enum {
    DoubleQuote = 1 << 0,
    Raw = 1 << 1,
    Format = 1 << 2,
    Triple = 1 << 3,
    Bytes = 1 << 4,
  };

  Delimiter() : flags(0) {}

  bool is_format() const {
    return flags & Format;
  }

  bool is_raw() const {
    return flags & Raw;
  }

  bool is_triple() const {
    return flags & Triple;
  }

  bool is_bytes() const {
    return flags & Bytes;
  }

  int32_t end_character() const {
    if (flags & DoubleQuote) return '"';
    return 0;
  }

  void set_format() {
    flags |= Format;
  }

  void set_raw() {
    flags |= Raw;
  }

  void set_triple() {
    flags |= Triple;
  }

  void set_bytes() {
    flags |= Bytes;
  }

  void set_end_character(int32_t character) {
    switch (character) {
      case '"':
        flags |= DoubleQuote;
        break;
      default:
        assert(false);
    }
  }

  char flags;
};

struct Scanner {
  Scanner() {
    assert(sizeof(Delimiter) == sizeof(char));
    deserialize(NULL, 0);
  }

  unsigned serialize(char *buffer) {
    size_t i = 0;

    size_t delimiter_count = delimiter_stack.size();
    if (delimiter_count > UINT8_MAX) delimiter_count = UINT8_MAX;
    buffer[i++] = delimiter_count;

    if (delimiter_count > 0) {
      memcpy(&buffer[i], delimiter_stack.data(), delimiter_count);
    }
    i += delimiter_count;

    return i;
  }

  void deserialize(const char *buffer, unsigned length) {
    delimiter_stack.clear();

    if (length > 0) {
      size_t i = 0;

      size_t delimiter_count = (uint8_t)buffer[i++];
      delimiter_stack.resize(delimiter_count);
      if (delimiter_count > 0) {
        memcpy(delimiter_stack.data(), &buffer[i], delimiter_count);
      }
      i += delimiter_count;
    }
  }

  void advance(TSLexer *lexer) {
    lexer->advance(lexer, false);
  }

  void skip(TSLexer *lexer) {
    lexer->advance(lexer, true);
  }

  bool scan(TSLexer *lexer, const bool *valid_symbols) {
    if (valid_symbols[STRING_CONTENT] && !delimiter_stack.empty()) {
      Delimiter delimiter = delimiter_stack.back();
      int32_t end_character = delimiter.end_character();
      bool has_content = false;
      while (lexer->lookahead) {
        if (lexer->lookahead == '{' && delimiter.is_format()) {
          lexer->mark_end(lexer);
          lexer->advance(lexer, false);
          if (lexer->lookahead == '{') {
            lexer->advance(lexer, false);
          } else {
            lexer->result_symbol = STRING_CONTENT;
            return has_content;
          }
        } else if (lexer->lookahead == '\\') {
          if (delimiter.is_raw()) {
            lexer->advance(lexer, false);
          } else if (delimiter.is_bytes()) {
              lexer->mark_end(lexer);
              lexer->advance(lexer, false);
              if (lexer->lookahead == 'N' || lexer->lookahead == 'u' || lexer->lookahead == 'U') {
                // In bytes string, \N{...}, \uXXXX and \UXXXXXXXX are not escape sequences
                // https://docs.kython.org/3/reference/lexical_analysis.html#string-and-bytes-literals
                lexer->advance(lexer, false);
              } else {
                  lexer->result_symbol = STRING_CONTENT;
                  return has_content;
              }
          } else {
            lexer->mark_end(lexer);
            lexer->result_symbol = STRING_CONTENT;
            return has_content;
          }
        } else if (lexer->lookahead == end_character) {
          if (delimiter.is_triple()) {
            lexer->mark_end(lexer);
            lexer->advance(lexer, false);
            if (lexer->lookahead == end_character) {
              lexer->advance(lexer, false);
              if (lexer->lookahead == end_character) {
                if (has_content) {
                  lexer->result_symbol = STRING_CONTENT;
                } else {
                  lexer->advance(lexer, false);
                  lexer->mark_end(lexer);
                  delimiter_stack.pop_back();
                  lexer->result_symbol = STRING_END;
                }
                return true;
              }
            }
          } else {
            if (has_content) {
              lexer->result_symbol = STRING_CONTENT;
            } else {
              lexer->advance(lexer, false);
              delimiter_stack.pop_back();
              lexer->result_symbol = STRING_END;
            }
            lexer->mark_end(lexer);
            return true;
          }
        } else if (lexer->lookahead == '\n' && has_content && !delimiter.is_triple()) {
          return false;
        }
        advance(lexer);
        has_content = true;
      }
    }

    lexer->mark_end(lexer);

    bool found_end_of_line = false;
    for (;;) {
      if (lexer->lookahead == '\n') {
        found_end_of_line = true;
        skip(lexer);
      } else if (lexer->lookahead == ' ') {
        skip(lexer);
      } else if (lexer->lookahead == '\r') {
        skip(lexer);
      } else if (lexer->lookahead == '\t') {
        skip(lexer);
      } else if (lexer->lookahead == '#') {
        while (lexer->lookahead && lexer->lookahead != '\n') {
          skip(lexer);
        }
        skip(lexer);
      } else if (lexer->lookahead == '\\') {
        skip(lexer);
        if (iswspace(lexer->lookahead)) {
          skip(lexer);
        } else {
          return false;
        }
      } else if (lexer->lookahead == '\f') {
        skip(lexer);
      } else if (lexer->lookahead == 0) {
        found_end_of_line = true;
        break;
      } else {
        break;
      }
    }

    if (found_end_of_line) {
      if (valid_symbols[NEWLINE]) {
        lexer->result_symbol = NEWLINE;
        return true;
      }
    }

    if (valid_symbols[STRING_START]) {
      Delimiter delimiter;

      bool has_flags = false;
      while (lexer->lookahead) {
        if (lexer->lookahead == 'f' || lexer->lookahead == 'F') {
          delimiter.set_format();
        } else if (lexer->lookahead == 'r' || lexer->lookahead == 'R') {
          delimiter.set_raw();
        } else if (lexer->lookahead == 'b' || lexer->lookahead == 'B') {
          delimiter.set_bytes();
        } else if (lexer->lookahead != 'u' && lexer->lookahead != 'U') {
          break;
        }
        has_flags = true;
        advance(lexer);
      }

      if (lexer->lookahead == '"') {
        delimiter.set_end_character('"');
        advance(lexer);
        lexer->mark_end(lexer);
        if (lexer->lookahead == '"') {
          advance(lexer);
          if (lexer->lookahead == '"') {
            advance(lexer);
            lexer->mark_end(lexer);
            delimiter.set_triple();
          }
        }
      }

      if (delimiter.end_character()) {
        delimiter_stack.push_back(delimiter);
        lexer->result_symbol = STRING_START;
        return true;
      } else if (has_flags) {
        return false;
      }
    }

    return false;
  }

  vector<Delimiter> delimiter_stack;
};

}

extern "C" {

void *tree_sitter_kython_external_scanner_create() {
  return new Scanner();
}

bool tree_sitter_kython_external_scanner_scan(void *payload, TSLexer *lexer,
                                            const bool *valid_symbols) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  return scanner->scan(lexer, valid_symbols);
}

unsigned tree_sitter_kython_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  return scanner->serialize(buffer);
}

void tree_sitter_kython_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  scanner->deserialize(buffer, length);
}

void tree_sitter_kython_external_scanner_destroy(void *payload) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  delete scanner;
}

}
