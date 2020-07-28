// This file contains the common code every implementation uses for stage2
// It is intended to be included multiple times and compiled multiple times
// We assume the file in which it is include already includes
// "simdjson/stage2.h" (this simplifies amalgation)

#include "generic/stage2/tape_writer.h"
#include "generic/stage2/logger.h"
#include "generic/stage2/atomparsing.h"
#include "generic/stage2/structural_iterator.h"

namespace { // Make everything here private
namespace SIMDJSON_IMPLEMENTATION {
namespace stage2 {

#ifdef SIMDJSON_USE_COMPUTED_GOTO
#define INIT_ADDRESSES() { &&array_continue, &&finish, &&object_continue }
#define CONTINUE(address) { goto *(address); }
#else // SIMDJSON_USE_COMPUTED_GOTO
#define INIT_ADDRESSES() { 0, 1, 2 };
#define CONTINUE(address)             \
  {                                   \
    switch(address) {                 \
      case 0: goto array_continue;    \
      case 1: goto finish;            \
      case 2: goto object_continue;   \
    }                                 \
  }
#endif // SIMDJSON_USE_COMPUTED_GOTO

struct unified_machine_addresses {
  ret_address_t array_continue;
  ret_address_t finish;
  ret_address_t object_continue;
};

struct structural_parser : structural_iterator {
  /** Lets you append to the tape */
  tape_writer tape;
  /** Next write location in the string buf for stage 2 parsing */
  uint8_t *current_string_buf_loc;
  /** Current depth (nested objects and arrays) */
  uint32_t depth{0};

  // For non-streaming, to pass an explicit 0 as next_structural, which enables optimizations
  really_inline structural_parser(dom_parser_implementation &_parser, uint32_t start_structural_index)
    : structural_iterator(_parser, start_structural_index),
      tape{parser.doc->tape.get()},
      current_string_buf_loc{parser.doc->string_buf.get()} {
  }

  WARN_UNUSED really_inline bool start_scope(ret_address_t continue_state) {
    parser.containing_scope[depth].tape_index = next_tape_index();
    parser.containing_scope[depth].count = 0;
    tape.skip(); // We don't actually *write* the start element until the end.
    parser.ret_address[depth] = continue_state;
    depth++;
    bool exceeded_max_depth = depth >= parser.max_depth();
    if (exceeded_max_depth) { log_error("Exceeded max depth!"); }
    return exceeded_max_depth;
  }

  WARN_UNUSED really_inline bool start_document(ret_address_t continue_state) {
    log_start_value("document");
    return start_scope(continue_state);
  }

  WARN_UNUSED really_inline bool start_object(ret_address_t continue_state) {
    log_start_value("object");
    return start_scope(continue_state);
  }

  WARN_UNUSED really_inline bool start_array(ret_address_t continue_state) {
    log_start_value("array");
    return start_scope(continue_state);
  }

  // this function is responsible for annotating the start of the scope
  really_inline void end_scope(internal::tape_type start, internal::tape_type end) noexcept {
    depth--;
    // write our doc->tape location to the header scope
    // The root scope gets written *at* the previous location.
    tape.append(parser.containing_scope[depth].tape_index, end);
    // count can overflow if it exceeds 24 bits... so we saturate
    // the convention being that a cnt of 0xffffff or more is undetermined in value (>=  0xffffff).
    const uint32_t start_tape_index = parser.containing_scope[depth].tape_index;
    const uint32_t count = parser.containing_scope[depth].count;
    const uint32_t cntsat = count > 0xFFFFFF ? 0xFFFFFF : count;
    // This is a load and an OR. It would be possible to just write once at doc->tape[d.tape_index]
    tape_writer::write(parser.doc->tape[start_tape_index], next_tape_index() | (uint64_t(cntsat) << 32), start);
  }

  really_inline uint32_t next_tape_index() {
    return uint32_t(tape.next_tape_loc - parser.doc->tape.get());
  }

  really_inline void end_object() {
    log_end_value("object");
    end_scope(internal::tape_type::START_OBJECT, internal::tape_type::END_OBJECT);
  }
  really_inline void end_array() {
    log_end_value("array");
    end_scope(internal::tape_type::START_ARRAY, internal::tape_type::END_ARRAY);
  }
  really_inline void end_document() {
    log_end_value("document");
    end_scope(internal::tape_type::ROOT, internal::tape_type::ROOT);
  }

  // increment_count increments the count of keys in an object or values in an array.
  // Note that if you are at the level of the values or elements, the count
  // must be increment in the preceding depth (depth-1) where the array or
  // the object resides.
  really_inline void increment_count() {
    parser.containing_scope[depth - 1].count++; // we have a key value pair in the object at parser.depth - 1
  }

  really_inline uint8_t *on_start_string() noexcept {
    // we advance the point, accounting for the fact that we have a NULL termination
    tape.append(current_string_buf_loc - parser.doc->string_buf.get(), internal::tape_type::STRING);
    return current_string_buf_loc + sizeof(uint32_t);
  }

  really_inline void on_end_string(uint8_t *dst) noexcept {
    uint32_t str_length = uint32_t(dst - (current_string_buf_loc + sizeof(uint32_t)));
    // TODO check for overflow in case someone has a crazy string (>=4GB?)
    // But only add the overflow check when the document itself exceeds 4GB
    // Currently unneeded because we refuse to parse docs larger or equal to 4GB.
    memcpy(current_string_buf_loc, &str_length, sizeof(uint32_t));
    // NULL termination is still handy if you expect all your strings to
    // be NULL terminated? It comes at a small cost
    *dst = 0;
    current_string_buf_loc = dst + 1;
  }

  WARN_UNUSED really_inline bool parse_string(bool key = false) {
    log_value(key ? "key" : "string");
    uint8_t *dst = on_start_string();
    dst = stringparsing::parse_string(current(), dst);
    if (dst == nullptr) {
      log_error("Invalid escape in string");
      return true;
    }
    on_end_string(dst);
    return false;
  }

  WARN_UNUSED really_inline bool parse_number(const uint8_t *src) {
    log_value("number");
    bool succeeded = numberparsing::parse_number(src, tape);
    if (!succeeded) { log_error("Invalid number"); }
    return !succeeded;
  }
  WARN_UNUSED really_inline bool parse_number() {
    return parse_number(current());
  }

  really_inline bool parse_root_number() {
    /**
    * We need to make a copy to make sure that the string is space terminated.
    * This is not about padding the input, which should already padded up
    * to len + SIMDJSON_PADDING. However, we have no control at this stage
    * on how the padding was done. What if the input string was padded with nulls?
    * It is quite common for an input string to have an extra null character (C string).
    * We do not want to allow 9\0 (where \0 is the null character) inside a JSON
    * document, but the string "9\0" by itself is fine. So we make a copy and
    * pad the input with spaces when we know that there is just one input element.
    * This copy is relatively expensive, but it will almost never be called in
    * practice unless you are in the strange scenario where you have many JSON
    * documents made of single atoms.
    */
    uint8_t *copy = static_cast<uint8_t *>(malloc(parser.len + SIMDJSON_PADDING));
    if (copy == nullptr) {
      return true;
    }
    memcpy(copy, buf, parser.len);
    memset(copy + parser.len, ' ', SIMDJSON_PADDING);
    size_t idx = *current_structural;
    bool result = parse_number(&copy[idx]); // parse_number does not throw
    free(copy);
    return result;
  }

  WARN_UNUSED really_inline bool parse_true_atom() {
    log_value("true");
    if (!atomparsing::is_valid_true_atom(current())) { return true; }
    tape.append(0, internal::tape_type::TRUE_VALUE);
    return false;
  }

  WARN_UNUSED really_inline bool parse_root_true_atom() {
    log_value("true");
    if (!atomparsing::is_valid_true_atom(current(), remaining_len())) { return true; }
    tape.append(0, internal::tape_type::TRUE_VALUE);
    return false;
  }

  WARN_UNUSED really_inline bool parse_false_atom() {
    log_value("false");
    if (!atomparsing::is_valid_false_atom(current())) { return true; }
    tape.append(0, internal::tape_type::FALSE_VALUE);
    return false;
  }

  WARN_UNUSED really_inline bool parse_root_false_atom() {
    log_value("false");
    if (!atomparsing::is_valid_false_atom(current(), remaining_len())) { return true; }
    tape.append(0, internal::tape_type::FALSE_VALUE);
    return false;
  }

  WARN_UNUSED really_inline bool parse_null_atom() {
    log_value("null");
    if (!atomparsing::is_valid_null_atom(current())) { return true; }
    tape.append(0, internal::tape_type::NULL_VALUE);
    return false;
  }

  WARN_UNUSED really_inline bool parse_root_null_atom() {
    log_value("null");
    if (!atomparsing::is_valid_null_atom(current(), remaining_len())) { return true; }
    tape.append(0, internal::tape_type::NULL_VALUE);
    return false;
  }

  WARN_UNUSED really_inline error_code finish() {
    end_document();
    parser.next_structural_index = uint32_t(current_structural + 1 - &parser.structural_indexes[0]);

    if (depth != 0) {
      log_error("Unclosed objects or arrays!");
      return parser.error = TAPE_ERROR;
    }

    return SUCCESS;
  }

  WARN_UNUSED really_inline error_code error() {
    // At this point in the code, we have all the time in the world.
    // Note that we know exactly where we are in the document so we could,
    // without any overhead on the processing code, report a specific
    // location.
    // We could even trigger special code paths to assess what happened
    // carefully,
    // all without any added cost.
    //
    if (depth >= parser.max_depth()) {
      return parser.error = DEPTH_ERROR;
    }
    switch (current_char()) {
    case '"':
      return parser.error = STRING_ERROR;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case '-':
      return parser.error = NUMBER_ERROR;
    case 't':
      return parser.error = T_ATOM_ERROR;
    case 'n':
      return parser.error = N_ATOM_ERROR;
    case 'f':
      return parser.error = F_ATOM_ERROR;
    default:
      return parser.error = TAPE_ERROR;
    }
  }

  really_inline void init() {
    log_start();
    parser.error = UNINITIALIZED;
  }

  WARN_UNUSED really_inline error_code start(ret_address_t finish_state) {
    // If there are no structurals left, return EMPTY
    if (at_end(parser.n_structural_indexes)) {
      return parser.error = EMPTY;
    }

    init();
    // Push the root scope (there is always at least one scope)
    if (start_document(finish_state)) {
      return parser.error = DEPTH_ERROR;
    }
    return SUCCESS;
  }

  really_inline void log_value(const char *type) {
    logger::log_line(*this, "", type, "");
  }

  static really_inline void log_start() {
    logger::log_start();
  }

  really_inline void log_start_value(const char *type) {
    logger::log_line(*this, "+", type, "");
    if (logger::LOG_ENABLED) { logger::log_depth++; }
  }

  really_inline void log_end_value(const char *type) {
    if (logger::LOG_ENABLED) { logger::log_depth--; }
    logger::log_line(*this, "-", type, "");
  }

  really_inline void log_error(const char *error) {
    logger::log_line(*this, "", "ERROR", error);
  }
}; // struct structural_parser

template<bool STREAMING>
WARN_UNUSED static error_code parse_structurals(dom_parser_implementation &dom_parser, dom::document &doc) noexcept {
  dom_parser.doc = &doc;
  static constexpr stage2::unified_machine_addresses addresses = INIT_ADDRESSES();
  stage2::structural_parser parser(dom_parser, STREAMING ? dom_parser.next_structural_index : 0);
  error_code result = parser.start(addresses.finish);
  if (result) { return result; }

  //
  // Read first value
  //
  switch (parser.current_char()) {
  case '{':
    if ( parser.start_object(addresses.finish) ) { goto error; };
    goto object_begin;
  case '[':
    if ( parser.start_array(addresses.finish) ) { goto error; }
    // Make sure the outer array is closed before continuing; otherwise, there are ways we could get
    // into memory corruption. See https://github.com/simdjson/simdjson/issues/906
    if (!STREAMING) {
      if (parser.buf[dom_parser.structural_indexes[dom_parser.n_structural_indexes - 1]] != ']') {
        goto error;
      }
    }
    goto array_begin;
  case '"': if ( parser.parse_string() ) { goto error; }; goto finish;
  case 't': if ( parser.parse_root_true_atom() ) { goto error; }; goto finish;
  case 'f': if ( parser.parse_root_false_atom() ) { goto error; }; goto finish;
  case 'n': if ( parser.parse_root_null_atom() ) { goto error; }; goto finish;
  case '-':
  case '0': case '1': case '2': case '3': case '4':
  case '5': case '6': case '7': case '8': case '9':
    // Next line used to be an interesting functional programming exercise with
    // a lambda that gets passed to another function via a closure. This would confuse the
    // clangcl compiler under Visual Studio 2019 (recent release).
    if ( parser.parse_root_number() ) { goto error; }
    goto finish;
  default:
    parser.log_error("Document starts with a non-value character");
    goto error;
  }

//
// Object parser states
//
object_begin:
  switch (parser.advance_char()) {
  case '"': {
    parser.increment_count();
    if ( parser.parse_string(true) ) { goto error; }
    goto object_key_state;
  }
  case '}':
    parser.end_object();
    goto scope_end;
  default:
    parser.log_error("Object does not start with a key");
    goto error;
  }

object_key_state:
  if (parser.advance_char() != ':' ) { parser.log_error("Missing colon after key in object"); goto error; }
  switch (parser.advance_char()) {
    case '{': if ( parser.start_object(addresses.object_continue) ) { goto error; } else { goto object_begin; }
    case '[': if ( parser.start_array(addresses.object_continue) ) { goto error; } else { goto array_begin; }
    case '"': if ( parser.parse_string() ) { goto error; }; break;
    case 't': if ( parser.parse_true_atom() ) { goto error; }; break;
    case 'f': if ( parser.parse_false_atom() ) { goto error; }; break;
    case 'n': if ( parser.parse_null_atom() ) { goto error; }; break;
    case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
      if ( parser.parse_number() ) { goto error; }; break;
    default:
      parser.log_error("Non-value found when value was expected!");
      goto error;
  }

object_continue:
  switch (parser.advance_char()) {
  case ',':
    parser.increment_count();
    if (parser.advance_char() != '"' ) { parser.log_error("Key string missing at beginning of field in object"); goto error; }
    if ( parser.parse_string(true) ) { goto error; }
    goto object_key_state;
  case '}':
    parser.end_object();
    goto scope_end;
  default:
    parser.log_error("No comma between object fields");
    goto error;
  }

scope_end:
  CONTINUE( parser.parser.ret_address[parser.depth] );

//
// Array parser states
//
array_begin:
  if (parser.peek_next_char() == ']') {
    parser.advance_char();
    parser.end_array();
    goto scope_end;
  }
  parser.increment_count();

main_array_switch:
  switch (parser.advance_char()) {
    case '{': if ( parser.start_object(addresses.array_continue) ) { goto error; } else { goto object_begin; }
    case '[': if ( parser.start_array(addresses.array_continue) ) { goto error; } else { goto array_begin; }
    case '"': if ( parser.parse_string() ) { goto error; }; break;
    case 't': if ( parser.parse_true_atom() ) { goto error; }; break;
    case 'f': if ( parser.parse_false_atom() ) { goto error; }; break;
    case 'n': if ( parser.parse_null_atom() ) { goto error; }; break;
    case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
      if ( parser.parse_number() ) { goto error; }; break; 
    default:
      parser.log_error("Non-value found when value was expected!");
      goto error;
  }

array_continue:
  switch (parser.advance_char()) {
  case ',':
    parser.increment_count();
    goto main_array_switch;
  case ']':
    parser.end_array();
    goto scope_end;
  default:
    parser.log_error("Missing comma between array values");
    goto error;
  }

finish:
  return parser.finish();

error:
  return parser.error();
}

} // namespace stage2
} // namespace SIMDJSON_IMPLEMENTATION
} // namespace {
