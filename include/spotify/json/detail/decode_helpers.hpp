/*
 * Copyright (c) 2014-2016 Spotify AB
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include <spotify/json/decode_context.hpp>
#include <spotify/json/detail/char_traits.hpp>
#include <spotify/json/detail/macros.hpp>
#include <spotify/json/detail/skip_chars.hpp>

#if _MSC_VER
#pragma intrinsic (memcmp)
#endif

namespace spotify {
namespace json {
namespace detail {

template <typename string_type>
json_never_inline json_noreturn void fail(
    const decode_context &context,
    const string_type &error,
    const ptrdiff_t d = 0) {
  throw decode_exception(error, context.offset(d));
}

template <typename string_type, typename condition_type>
json_force_inline void fail_if(
    const decode_context &context,
    const condition_type condition,
    const string_type &error,
    const ptrdiff_t d = 0) {
  if (json_unlikely(condition)) {
    fail(context, error, d);
  }
}

template <size_t num_required_bytes, typename string_type>
json_force_inline void require_bytes(
    const decode_context &context,
    const string_type &error = "Unexpected end of input") {
  fail_if(context, context.remaining() < num_required_bytes, error);
}

template <size_t num_required_bytes>
json_force_inline void require_bytes(const decode_context &context) {
  require_bytes<num_required_bytes>(context, "Unexpected end of input");
}

json_force_inline char peek_unchecked(const decode_context &context) {
  return *context.position;
}

/**
 * Peek at the current character that a decode_context refers to. If the
 * decode_context has ended, '\0' is returned. This is a useful helper when
 * while decoding it is necessary to ensure that the current character is a
 * specific one, for example '['.
 */
json_force_inline char peek(const decode_context &context) {
  return (context.remaining() ? peek_unchecked(context) : 0);
}

json_force_inline char next_unchecked(decode_context &context) {
  return *(context.position++);
}

template <typename string_type>
json_force_inline char next(decode_context &context, const string_type &error) {
  require_bytes<1>(context, error);
  return next_unchecked(context);
}

json_force_inline char next(decode_context &context) {
  return next(context, "Unexpected end of input");
}

json_force_inline void skip_unchecked(decode_context &context, const size_t num_bytes) {
  context.position += num_bytes;
}

json_force_inline void skip_unchecked(decode_context &context) {
  context.position++;
}

json_force_inline void skip(decode_context &context, const size_t num_bytes) {
  fail_if(context, context.remaining() < num_bytes, "Unexpected end of input");
  skip_unchecked(context, num_bytes);
}

json_force_inline void skip(decode_context &context) {
  require_bytes<1>(context, "Unexpected end of input");
  context.position++;
}

/**
 * Advance past a specific character. If the context position does not point to
 * a matching character, a decode_exception is thrown.
 */
inline void advance_past(decode_context &context, char character) {
  fail_if(context, next(context) != character, "Unexpected input", -1);
}

/**
 * Advance past 4 specific characters. If the context position does not point to
 * matching characters, a decode_exception is thrown. 'characters' must be a C
 * string of at least length 4. Only the first four characters will be read.
 */
inline void advance_past_four(decode_context &context, const char *characters) {
  require_bytes<4>(context);
  fail_if(context, memcmp(characters, context.position, 4), "Unexpected input");
  context.position += 4;
}

/**
 * Helper function for parsing the comma separated entities in JSON: objects
 * and arrays. intro and outro are the characters before and after the entity:
 * {} and [], respectively. parse is a callback that is called for each
 * element in the comma separated list. It should advance the parse context to
 * after that element or mark the context as failed.
 *
 * The parse callback must mark the context as failed if it sees a premature end
 * of input, otherwise this function might enter an infinite loop!
 *
 * context.has_failed() must be false when this function is called.
 */
template <typename Parse>
void advance_past_comma_separated(decode_context &context, char intro, char outro, Parse parse) {
  advance_past(context, intro);
  skip_past_whitespace(context);

  if (json_likely(peek(context) != outro)) {
    parse();
    skip_past_whitespace(context);

    while (json_likely(peek(context) != outro)) {
      advance_past(context, ',');
      skip_past_whitespace(context);
      parse();
      skip_past_whitespace(context);
    }
  }

  context.position++;
}

/**
 * Helper for parsing JSON objects. callback is called once for each key/value
 * pair. It is given the already parsed key and is expected to parse the value
 * and store it away as needed. The callback may be invoked a few times even if
 * parsing fails later on.
 */
template <typename KeyCodec, typename Callback>
void advance_past_object(decode_context &context, const Callback &callback) {
  auto codec = KeyCodec();
  advance_past_comma_separated(context, '{', '}', [&]{
    auto key = codec.decode(context);
    skip_past_whitespace(context);
    advance_past(context, ':');
    skip_past_whitespace(context);
    callback(std::move(key));
  });
}

inline void advance_past_true(decode_context &context) {
  advance_past_four(context, "true");
}

inline void advance_past_false(decode_context &context) {
  context.position++;  // skip past the 'f' in 'false', we know it is there
  advance_past_four(context, "alse");
}

inline void advance_past_null(decode_context &context) {
  advance_past_four(context, "null");
}

inline void advance_past_string_escape_after_slash(decode_context &context) {
  switch (next(context, "Unterminated string")) {
    case '"':
    case '\\':
    case '/':
    case 'b':
    case 'f':
    case 'n':
    case 'r':
    case 't':
      break;
   case 'u': {
      require_bytes<4>(context, "\\u must be followed by 4 hex digits");
      const bool h0 = char_traits<char>::is_hex_digit(*(context.position++));
      const bool h1 = char_traits<char>::is_hex_digit(*(context.position++));
      const bool h2 = char_traits<char>::is_hex_digit(*(context.position++));
      const bool h3 = char_traits<char>::is_hex_digit(*(context.position++));
      fail_if(context, !(h0 && h1 && h2 && h3), "\\u must be followed by 4 hex digits");
      break;
    }
   default:
    fail(context, "Invalid escape character", -1);
  }
}

inline void advance_past_string(decode_context &context) {
  advance_past(context, '"');
  for (;;) {
    switch (next(context, "Unterminated string")) {
      case '"': return;
      case '\\': advance_past_string_escape_after_slash(context); break;
    }
  }
}

}  // namespace detail
}  // namespace json
}  // namespace spotify
