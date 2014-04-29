// Copyright 2010 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "../include/v8stdint.h"

#include "preparse-data-format.h"
#include "preparse-data.h"

#include "checks.h"
#include "globals.h"
#include "hashmap.h"

namespace v8 {
namespace internal {


template <typename Char>
static int vector_hash(Vector<const Char> string) {
  int hash = 0;
  for (int i = 0; i < string.length(); i++) {
    int c = static_cast<int>(string[i]);
    hash += c;
    hash += (hash << 10);
    hash ^= (hash >> 6);
  }
  return hash;
}


static bool vector_compare(void* a, void* b) {
  CompleteParserRecorder::Key* string1 =
      reinterpret_cast<CompleteParserRecorder::Key*>(a);
  CompleteParserRecorder::Key* string2 =
      reinterpret_cast<CompleteParserRecorder::Key*>(b);
  if (string1->is_one_byte != string2->is_one_byte) return false;
  int length = string1->literal_bytes.length();
  if (string2->literal_bytes.length() != length) return false;
  return memcmp(string1->literal_bytes.start(),
                string2->literal_bytes.start(), length) == 0;
}


CompleteParserRecorder::CompleteParserRecorder()
    : function_store_(0),
      literal_chars_(0),
      symbol_store_(0),
      symbol_keys_(0),
      string_table_(vector_compare),
      symbol_id_(0) {
  preamble_[PreparseDataConstants::kMagicOffset] =
      PreparseDataConstants::kMagicNumber;
  preamble_[PreparseDataConstants::kVersionOffset] =
      PreparseDataConstants::kCurrentVersion;
  preamble_[PreparseDataConstants::kHasErrorOffset] = false;
  preamble_[PreparseDataConstants::kFunctionsSizeOffset] = 0;
  preamble_[PreparseDataConstants::kSymbolCountOffset] = 0;
  preamble_[PreparseDataConstants::kSizeOffset] = 0;
  ASSERT_EQ(6, PreparseDataConstants::kHeaderSize);
#ifdef DEBUG
  prev_start_ = -1;
#endif
}


void CompleteParserRecorder::LogMessage(int start_pos,
                                        int end_pos,
                                        const char* message,
                                        const char* arg_opt,
                                        bool is_reference_error) {
  if (has_error()) return;
  preamble_[PreparseDataConstants::kHasErrorOffset] = true;
  function_store_.Reset();
  STATIC_ASSERT(PreparseDataConstants::kMessageStartPos == 0);
  function_store_.Add(start_pos);
  STATIC_ASSERT(PreparseDataConstants::kMessageEndPos == 1);
  function_store_.Add(end_pos);
  STATIC_ASSERT(PreparseDataConstants::kMessageArgCountPos == 2);
  function_store_.Add((arg_opt == NULL) ? 0 : 1);
  STATIC_ASSERT(PreparseDataConstants::kIsReferenceErrorPos == 3);
  function_store_.Add(is_reference_error ? 1 : 0);
  STATIC_ASSERT(PreparseDataConstants::kMessageTextPos == 4);
  WriteString(CStrVector(message));
  if (arg_opt != NULL) WriteString(CStrVector(arg_opt));
}


void CompleteParserRecorder::WriteString(Vector<const char> str) {
  function_store_.Add(str.length());
  for (int i = 0; i < str.length(); i++) {
    function_store_.Add(str[i]);
  }
}


void CompleteParserRecorder::LogOneByteSymbol(int start,
                                              Vector<const uint8_t> literal) {
  int hash = vector_hash(literal);
  LogSymbol(start, hash, true, literal);
}


void CompleteParserRecorder::LogTwoByteSymbol(int start,
                                              Vector<const uint16_t> literal) {
  int hash = vector_hash(literal);
  LogSymbol(start, hash, false, Vector<const byte>::cast(literal));
}


void CompleteParserRecorder::LogSymbol(int start,
                                       int hash,
                                       bool is_one_byte,
                                       Vector<const byte> literal_bytes) {
  Key key = { is_one_byte, literal_bytes };
  HashMap::Entry* entry = string_table_.Lookup(&key, hash, true);
  int id = static_cast<int>(reinterpret_cast<intptr_t>(entry->value));
  if (id == 0) {
    // Copy literal contents for later comparison.
    key.literal_bytes =
        Vector<const byte>::cast(literal_chars_.AddBlock(literal_bytes));
    // Put (symbol_id_ + 1) into entry and increment it.
    id = ++symbol_id_;
    entry->value = reinterpret_cast<void*>(id);
    Vector<Key> symbol = symbol_keys_.AddBlock(1, key);
    entry->key = &symbol[0];
  }
  WriteNumber(id - 1);
}


Vector<unsigned> CompleteParserRecorder::ExtractData() {
  int function_size = function_store_.size();
  // Add terminator to symbols, then pad to unsigned size.
  int symbol_size = symbol_store_.size();
  int padding = sizeof(unsigned) - (symbol_size % sizeof(unsigned));
  symbol_store_.AddBlock(padding, PreparseDataConstants::kNumberTerminator);
  symbol_size += padding;
  int total_size = PreparseDataConstants::kHeaderSize + function_size
      + (symbol_size / sizeof(unsigned));
  Vector<unsigned> data = Vector<unsigned>::New(total_size);
  preamble_[PreparseDataConstants::kFunctionsSizeOffset] = function_size;
  preamble_[PreparseDataConstants::kSymbolCountOffset] = symbol_id_;
  OS::MemCopy(data.start(), preamble_, sizeof(preamble_));
  int symbol_start = PreparseDataConstants::kHeaderSize + function_size;
  if (function_size > 0) {
    function_store_.WriteTo(data.SubVector(PreparseDataConstants::kHeaderSize,
                                           symbol_start));
  }
  if (!has_error()) {
    symbol_store_.WriteTo(
        Vector<byte>::cast(data.SubVector(symbol_start, total_size)));
  }
  return data;
}


void CompleteParserRecorder::WriteNumber(int number) {
  // Split the number into chunks of 7 bits. Write them one after another (the
  // most significant first). Use the MSB of each byte for signalling that the
  // number continues. See ScriptDataImpl::ReadNumber for the reading side.
  ASSERT(number >= 0);

  int mask = (1 << 28) - 1;
  int i = 28;
  // 26 million symbols ought to be enough for anybody.
  ASSERT(number <= mask);
  while (number < mask) {
    mask >>= 7;
    i -= 7;
  }
  while (i > 0) {
    symbol_store_.Add(static_cast<byte>(number >> i) | 0x80u);
    number &= mask;
    mask >>= 7;
    i -= 7;
  }
  ASSERT(number < (1 << 7));
  symbol_store_.Add(static_cast<byte>(number));
}


} }  // namespace v8::internal.
