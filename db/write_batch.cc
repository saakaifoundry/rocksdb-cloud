//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]
// record :=
//    kTypeValue varstring varstring
//    kTypeMerge varstring varstring
//    kTypeDeletion varstring
//    kTypeColumnFamilyValue varint32 varstring varstring
//    kTypeColumnFamilyMerge varint32 varstring varstring
//    kTypeColumnFamilyDeletion varint32 varstring varstring
// varstring :=
//    len: varint32
//    data: uint8[len]

#include "rocksdb/write_batch.h"
#include "rocksdb/options.h"
#include "db/dbformat.h"
#include "db/db_impl.h"
#include "db/memtable.h"
#include "db/snapshot.h"
#include "db/write_batch_internal.h"
#include "util/coding.h"
#include "util/statistics_imp.h"
#include <stdexcept>

namespace rocksdb {

// WriteBatch header has an 8-byte sequence number followed by a 4-byte count.
static const size_t kHeader = 12;

WriteBatch::WriteBatch() {
  Clear();
}

WriteBatch::~WriteBatch() { }

WriteBatch::Handler::~Handler() { }

void WriteBatch::Handler::Put(const Slice& key, const Slice& value) {
  // you need to either implement Put or PutCF
  throw std::runtime_error("Handler::Put not implemented!");
}

void WriteBatch::Handler::Merge(const Slice& key, const Slice& value) {
  throw std::runtime_error("Handler::Merge not implemented!");
}

void WriteBatch::Handler::Delete(const Slice& key) {
  // you need to either implement Delete or DeleteCF
  throw std::runtime_error("Handler::Delete not implemented!");
}

void WriteBatch::Handler::LogData(const Slice& blob) {
  // If the user has not specified something to do with blobs, then we ignore
  // them.
}

bool WriteBatch::Handler::Continue() {
  return true;
}

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}

int WriteBatch::Count() const {
  return WriteBatchInternal::Count(this);
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }

  input.remove_prefix(kHeader);
  Slice key, value, blob;
  int found = 0;
  while (!input.empty() && handler->Continue()) {
    char tag = input[0];
    input.remove_prefix(1);
    uint32_t column_family = 0; // default
    switch (tag) {
      case kTypeColumnFamilyValue:
        if (!GetVarint32(&input, &column_family)) {
          return Status::Corruption("bad WriteBatch Put");
        }
        // intentional fallthrough
      case kTypeValue:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          handler->PutCF(column_family, key, value);
          found++;
        } else {
          return Status::Corruption("bad WriteBatch Put");
        }
        break;
      case kTypeColumnFamilyDeletion:
        if (!GetVarint32(&input, &column_family)) {
          return Status::Corruption("bad WriteBatch Delete");
        }
        // intentional fallthrough
      case kTypeDeletion:
        if (GetLengthPrefixedSlice(&input, &key)) {
          handler->DeleteCF(column_family, key);
          found++;
        } else {
          return Status::Corruption("bad WriteBatch Delete");
        }
        break;
      case kTypeColumnFamilyMerge:
        if (!GetVarint32(&input, &column_family)) {
          return Status::Corruption("bad WriteBatch Merge");
        }
        // intentional fallthrough
      case kTypeMerge:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          handler->MergeCF(column_family, key, value);
          found++;
        } else {
          return Status::Corruption("bad WriteBatch Merge");
        }
        break;
      case kTypeLogData:
        if (GetLengthPrefixedSlice(&input, &blob)) {
          handler->LogData(blob);
        } else {
          return Status::Corruption("bad WriteBatch Blob");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  } else {
    return Status::OK();
  }
}

int WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* b, int n) {
  EncodeFixed32(&b->rep_[8], n);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return SequenceNumber(DecodeFixed64(b->rep_.data()));
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}

void WriteBatch::Put(uint32_t column_family_id, const Slice& key,
                     const Slice& value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  if (column_family_id == 0) {
    // save some data on disk by not writing default column family
    rep_.push_back(static_cast<char>(kTypeValue));
  } else {
    rep_.push_back(static_cast<char>(kTypeColumnFamilyValue));
    PutVarint32(&rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Put(uint32_t column_family_id, const SliceParts& key,
                     const SliceParts& value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  if (column_family_id == 0) {
    rep_.push_back(static_cast<char>(kTypeValue));
  } else {
    rep_.push_back(static_cast<char>(kTypeColumnFamilyValue));
    PutVarint32(&rep_, column_family_id);
  }
  PutLengthPrefixedSliceParts(&rep_, key);
  PutLengthPrefixedSliceParts(&rep_, value);
}

void WriteBatch::Delete(uint32_t column_family_id, const Slice& key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  if (column_family_id == 0) {
    rep_.push_back(static_cast<char>(kTypeDeletion));
  } else {
    rep_.push_back(static_cast<char>(kTypeColumnFamilyDeletion));
    PutVarint32(&rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&rep_, key);
}

void WriteBatch::Merge(uint32_t column_family_id, const Slice& key,
                       const Slice& value) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  if (column_family_id == 0) {
    rep_.push_back(static_cast<char>(kTypeMerge));
  } else {
    rep_.push_back(static_cast<char>(kTypeColumnFamilyMerge));
    PutVarint32(&rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::PutLogData(const Slice& blob) {
  rep_.push_back(static_cast<char>(kTypeLogData));
  PutLengthPrefixedSlice(&rep_, blob);
}

namespace {
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber sequence_;
  MemTable* mem_;
  const Options* options_;
  DBImpl* db_;
  const bool filter_deletes_;

  MemTableInserter(SequenceNumber sequence, MemTable* mem, const Options* opts,
                   DB* db, const bool filter_deletes)
    : sequence_(sequence),
      mem_(mem),
      options_(opts),
      db_(reinterpret_cast<DBImpl*>(db)),
      filter_deletes_(filter_deletes) {
    assert(mem_);
    if (filter_deletes_) {
      assert(options_);
      assert(db_);
    }
  }

  virtual void PutCF(uint32_t column_family_id, const Slice& key,
                     const Slice& value) {
    if (options_->inplace_update_support
        && mem_->Update(sequence_, kTypeValue, key, value)) {
      RecordTick(options_->statistics.get(), NUMBER_KEYS_UPDATED);
    } else {
      mem_->Add(sequence_, kTypeValue, key, value);
    }
    sequence_++;
  }
  virtual void MergeCF(uint32_t column_family_id, const Slice& key,
                       const Slice& value) {
    mem_->Add(sequence_, kTypeMerge, key, value);
    sequence_++;
  }
  virtual void DeleteCF(uint32_t column_family_id, const Slice& key) {
    if (filter_deletes_) {
      SnapshotImpl read_from_snapshot;
      read_from_snapshot.number_ = sequence_;
      ReadOptions ropts;
      ropts.snapshot = &read_from_snapshot;
      std::string value;
      if (!db_->KeyMayExist(ropts, key, &value)) {
        RecordTick(options_->statistics.get(), NUMBER_FILTERED_DELETES);
        return;
      }
    }
    mem_->Add(sequence_, kTypeDeletion, key, Slice());
    sequence_++;
  }
};
}  // namespace

Status WriteBatchInternal::InsertInto(const WriteBatch* b, MemTable* mem,
                                      const Options* opts, DB* db,
                                      const bool filter_deletes) {
  MemTableInserter inserter(WriteBatchInternal::Sequence(b), mem, opts, db,
                            filter_deletes);
  return b->Iterate(&inserter);
}

void WriteBatchInternal::SetContents(WriteBatch* b, const Slice& contents) {
  assert(contents.size() >= kHeader);
  b->rep_.assign(contents.data(), contents.size());
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}

}  // namespace rocksdb
