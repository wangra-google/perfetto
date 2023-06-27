/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/db/storage/string_storage.h"

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/storage/types.h"

#include "src/trace_processor/db/storage/utils.h"
#include "src/trace_processor/util/glob.h"

namespace perfetto {
namespace trace_processor {
namespace storage {

namespace {
using Range = RowMap::Range;

struct Greater {
  bool operator()(StringPool::Id lhs, NullTermStringView rhs) const {
    return pool_->Get(lhs) > rhs;
  }
  const StringPool* pool_;
};

struct GreaterEqual {
  bool operator()(StringPool::Id lhs, NullTermStringView rhs) const {
    return pool_->Get(lhs) >= rhs;
  }
  const StringPool* pool_;
};

struct Less {
  bool operator()(StringPool::Id lhs, NullTermStringView rhs) const {
    return pool_->Get(lhs) < rhs;
  }
  const StringPool* pool_;
};

struct LessEqual {
  bool operator()(StringPool::Id lhs, NullTermStringView rhs) const {
    return pool_->Get(lhs) <= rhs;
  }
  const StringPool* pool_;
};

struct NotEqual {
  bool operator()(StringPool::Id lhs, StringPool::Id rhs) {
    return lhs != StringPool::Id::Null() && lhs != rhs;
  }
};

struct Glob {
  bool operator()(StringPool::Id rhs, util::GlobMatcher& matcher) const {
    return rhs != StringPool::Id::Null() && matcher.Matches(pool_->Get(rhs));
  }
  const StringPool* pool_;
};

struct IsNull {
  bool operator()(StringPool::Id rhs, StringPool::Id) const {
    return rhs == StringPool::Id::Null();
  }
};

struct IsNotNull {
  bool operator()(StringPool::Id rhs, StringPool::Id) const {
    return rhs != StringPool::Id::Null();
  }
};

}  // namespace

RangeOrBitVector StringStorage::Search(FilterOp op,
                                       SqlValue sql_val,
                                       RowMap::Range range) const {
  if (sql_val.is_null() &&
      (op != FilterOp::kIsNotNull && op != FilterOp::kIsNull)) {
    return RangeOrBitVector(Range());
  }
  StringPool::Id val =
      (op == FilterOp::kIsNull || op == FilterOp::kIsNotNull)
          ? StringPool::Id::Null()
          : string_pool_->InternString(base::StringView(sql_val.AsString()));
  const StringPool::Id* start = data_ + range.start;
  BitVector::Builder builder(range.end, range.start);

  switch (op) {
    case FilterOp::kEq:
      utils::LinearSearchWithComparator(
          val, start, std::equal_to<StringPool::Id>(), builder);
      break;
    case FilterOp::kNe:
      utils::LinearSearchWithComparator(val, start, NotEqual(), builder);
      break;
    case FilterOp::kLe:
      utils::LinearSearchWithComparator(string_pool_->Get(val), start,
                                        LessEqual{string_pool_}, builder);
      break;
    case FilterOp::kLt:
      utils::LinearSearchWithComparator(string_pool_->Get(val), start,
                                        Less{string_pool_}, builder);
      break;
    case FilterOp::kGt:
      utils::LinearSearchWithComparator(string_pool_->Get(val), start,
                                        Greater{string_pool_}, builder);
      break;
    case FilterOp::kGe:
      utils::LinearSearchWithComparator(string_pool_->Get(val), start,
                                        GreaterEqual{string_pool_}, builder);
      break;
    case FilterOp::kGlob: {
      util::GlobMatcher matcher =
          util::GlobMatcher::FromPattern(sql_val.AsString());
      if (matcher.IsEquality()) {
        utils::LinearSearchWithComparator(
            val, start, std::equal_to<StringPool::Id>(), builder);
        break;
      }
      utils::LinearSearchWithComparator(std::move(matcher), start,
                                        Glob{string_pool_}, builder);
      break;
    }
    case FilterOp::kIsNull:
      utils::LinearSearchWithComparator(val, start, IsNull(), builder);
      break;
    case FilterOp::kIsNotNull:
      utils::LinearSearchWithComparator(val, start, IsNotNull(), builder);
  }

  return RangeOrBitVector(std::move(builder).Build());
}

RangeOrBitVector StringStorage::IndexSearch(FilterOp op,
                                            SqlValue sql_val,
                                            uint32_t* indices,
                                            uint32_t indices_size,
                                            bool) const {
  if (sql_val.is_null() &&
      (op != FilterOp::kIsNotNull && op != FilterOp::kIsNull)) {
    return RangeOrBitVector(Range());
  }
  StringPool::Id val =
      (op == FilterOp::kIsNull || op == FilterOp::kIsNotNull)
          ? StringPool::Id::Null()
          : string_pool_->InternString(base::StringView(sql_val.AsString()));
  const StringPool::Id* start = data_;

  BitVector::Builder builder(indices_size);

  switch (op) {
    case FilterOp::kEq:
      utils::IndexSearchWithComparator(
          val, start, indices, std::equal_to<StringPool::Id>(), builder);
      break;
    case FilterOp::kNe:
      utils::IndexSearchWithComparator(val, start, indices, NotEqual(),
                                       builder);
      break;
    case FilterOp::kLe:
      utils::IndexSearchWithComparator(string_pool_->Get(val), start, indices,
                                       LessEqual{string_pool_}, builder);
      break;
    case FilterOp::kLt:
      utils::IndexSearchWithComparator(string_pool_->Get(val), start, indices,
                                       Less{string_pool_}, builder);
      break;
    case FilterOp::kGt:
      utils::IndexSearchWithComparator(string_pool_->Get(val), start, indices,
                                       Greater{string_pool_}, builder);
      break;
    case FilterOp::kGe:
      utils::IndexSearchWithComparator(string_pool_->Get(val), start, indices,
                                       GreaterEqual{string_pool_}, builder);
      break;
    case FilterOp::kGlob: {
      util::GlobMatcher matcher =
          util::GlobMatcher::FromPattern(sql_val.AsString());
      if (matcher.IsEquality()) {
        utils::IndexSearchWithComparator(
            val, start, indices, std::equal_to<StringPool::Id>(), builder);
        break;
      }
      utils::IndexSearchWithComparator(
          util::GlobMatcher::FromPattern(string_pool_->Get(val)), start,
          indices, Glob{string_pool_}, builder);
      break;
    }

    case FilterOp::kIsNull:
      utils::IndexSearchWithComparator(val, start, indices, IsNull(), builder);
      break;
    case FilterOp::kIsNotNull:
      utils::IndexSearchWithComparator(val, start, indices, IsNotNull(),
                                       builder);
      break;
  }

  return RangeOrBitVector(std::move(builder).Build());
}

RowMap::Range StringStorage::BinarySearchExtrinsic(FilterOp,
                                                   SqlValue,
                                                   uint32_t*,
                                                   uint32_t) const {
  PERFETTO_FATAL("Not implemented");
}
void StringStorage::StableSort(uint32_t* indices, uint32_t indices_size) const {
  std::stable_sort(indices, indices + indices_size,
                   [this](uint32_t a_idx, uint32_t b_idx) {
                     return string_pool_->Get(data_[a_idx]) <
                            string_pool_->Get(data_[b_idx]);
                   });
}

void StringStorage::Sort(uint32_t* indices, uint32_t indices_size) const {
  std::sort(indices, indices + indices_size,
            [this](uint32_t a_idx, uint32_t b_idx) {
              return string_pool_->Get(data_[a_idx]) <
                     string_pool_->Get(data_[b_idx]);
            });
}

}  // namespace storage
}  // namespace trace_processor
}  // namespace perfetto