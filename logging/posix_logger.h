//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Logger implementation that can be shared by all environments
// where enough posix functionality is available.

#pragma once
#include <algorithm>
#include <stdio.h>
#include "port/sys_time.h"
#include <time.h>
#include <fcntl.h>

#ifdef OS_LINUX
#ifndef FALLOC_FL_KEEP_SIZE
#include <linux/falloc.h>
#endif
#endif

#include <atomic>
#include "env/io_posix.h"
#include "monitoring/iostats_context_imp.h"
#include "rocksdb/env.h"
#include "test_util/sync_point.h"

namespace ROCKSDB_NAMESPACE {

class PosixLogger : public Logger {
 private:
  Status PosixCloseHelper() {
    int ret;

    ret = fclose(file_);
    if (ret) {
      return IOError("Unable to close log file", "", ret);
    }
    return Status::OK();
  }
  FILE* file_;
  uint64_t (*gettid_)();  // Return the thread id for the current thread
  std::atomic_size_t log_size_;
  int fd_;
  const static uint64_t flush_every_seconds_ = 5;
  std::atomic_uint_fast64_t last_flush_micros_;
  Env* env_;
  std::atomic<bool> flush_pending_;

 protected:
  virtual Status CloseImpl() override { return PosixCloseHelper(); }

 public:
  PosixLogger(FILE* f, uint64_t (*gettid)(), Env* env,
              const InfoLogLevel log_level = InfoLogLevel::ERROR_LEVEL)
      : Logger(log_level),
        file_(f),
        gettid_(gettid),
        log_size_(0),
        fd_(fileno(f)),
        last_flush_micros_(0),
        env_(env),
        flush_pending_(false) {}
  virtual ~PosixLogger() {
    if (!closed_) {
      closed_ = true;
      PosixCloseHelper().PermitUncheckedError();
    }
  }
  virtual void Flush() override {
    TEST_SYNC_POINT("PosixLogger::Flush:Begin1");
    TEST_SYNC_POINT("PosixLogger::Flush:Begin2");
  #if defined(ROCKSDB_UNIT_TEST)
    // keep this code to make rockdb unit tests happy
    if (flush_pending_) {
      flush_pending_ = false;
      fflush(file_);
    }
  #else
    // Keep It Simple Stupid: always flush, and keep code change minimal
    fflush(file_);
  #endif
    last_flush_micros_ = env_->NowMicros();
  }

  using Logger::Logv;
  virtual void Logv(const char* format, va_list ap) override {
    IOSTATS_TIMER_GUARD(logger_nanos);

    const uint64_t thread_id = (*gettid_)();

    // We try twice: the first time with a fixed-size stack allocated buffer,
    // and the second time with a much larger dynamically allocated buffer.
    char buffer[500];
    for (int iter = 0; iter < 2; iter++) {
      char* base;
      int bufsize;
      if (iter == 0) {
        bufsize = sizeof(buffer);
        base = buffer;
      } else {
        bufsize = 65536;
        base = new char[bufsize];
      }
      char* p = base;
      char* limit = base + bufsize;

      port::TimeVal now_tv;
      port::GetTimeOfDay(&now_tv, nullptr);
      const time_t seconds = now_tv.tv_sec;
      struct tm t;
      port::LocalTimeR(&seconds, &t);
      p += snprintf(p, limit - p, "%04d/%02d/%02d-%02d:%02d:%02d.%06d %llu ",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                    t.tm_min, t.tm_sec, static_cast<int>(now_tv.tv_usec),
                    static_cast<long long unsigned int>(thread_id));

      // Print the message
      if (p < limit) {
        va_list backup_ap;
        va_copy(backup_ap, ap);
        p += vsnprintf(p, limit - p, format, backup_ap);
        va_end(backup_ap);
      }

      // Truncate to available space if necessary
      if (p >= limit) {
        if (iter == 0) {
          continue;       // Try again with larger buffer
        } else {
          p = limit - 1;
        }
      }

      // Add newline if necessary
      if (p == base || p[-1] != '\n') {
        *p++ = '\n';
      }

      assert(p <= limit);
      const size_t write_size = p - base;

#ifdef ROCKSDB_FALLOCATE_PRESENT
      const int kDebugLogChunkSize = 128 * 1024;

      // If this write would cross a boundary of kDebugLogChunkSize
      // space, pre-allocate more space to avoid overly large
      // allocations from filesystem allocsize options.
      const size_t log_size = log_size_;
      const size_t last_allocation_chunk =
        ((kDebugLogChunkSize - 1 + log_size) / kDebugLogChunkSize);
      const size_t desired_allocation_chunk =
        ((kDebugLogChunkSize - 1 + log_size + write_size) /
           kDebugLogChunkSize);
      if (last_allocation_chunk != desired_allocation_chunk) {
        fallocate(
            fd_, FALLOC_FL_KEEP_SIZE, 0,
            static_cast<off_t>(desired_allocation_chunk * kDebugLogChunkSize));
      }
#endif

      size_t sz = fwrite(base, 1, write_size, file_);
      flush_pending_ = true;
      if (sz > 0) {
        log_size_ += write_size;
      }
      uint64_t now_micros = static_cast<uint64_t>(now_tv.tv_sec) * 1000000 +
        now_tv.tv_usec;
      if (now_micros - last_flush_micros_ >= flush_every_seconds_ * 1000000) {
        Flush();
      }
      if (base != buffer) {
        delete[] base;
      }
      break;
    }
  }
  size_t GetLogFileSize() const override { return log_size_; }
};

}  // namespace ROCKSDB_NAMESPACE
