//
// Copyright 2022 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "y_absl/log/internal/log_message.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <ios>
#include <memory>
#include <ostream>
#include <util/generic/string.h>
#include <tuple>

#include "y_absl/base/attributes.h"
#include "y_absl/base/config.h"
#include "y_absl/base/internal/raw_logging.h"
#include "y_absl/base/internal/strerror.h"
#include "y_absl/base/internal/sysinfo.h"
#include "y_absl/base/log_severity.h"
#include "y_absl/container/inlined_vector.h"
#include "y_absl/debugging/internal/examine_stack.h"
#include "y_absl/log/globals.h"
#include "y_absl/log/internal/append_truncated.h"
#include "y_absl/log/internal/globals.h"
#include "y_absl/log/internal/log_format.h"
#include "y_absl/log/internal/log_sink_set.h"
#include "y_absl/log/internal/proto.h"
#include "y_absl/log/log_entry.h"
#include "y_absl/log/log_sink.h"
#include "y_absl/log/log_sink_registry.h"
#include "y_absl/memory/memory.h"
#include "y_absl/strings/string_view.h"
#include "y_absl/time/clock.h"
#include "y_absl/time/time.h"
#include "y_absl/types/span.h"

extern "C" Y_ABSL_ATTRIBUTE_WEAK void Y_ABSL_INTERNAL_C_SYMBOL(
    AbslInternalOnFatalLogMessage)(const y_absl::LogEntry&) {
  // Default - Do nothing
}

static int IsFatalErrorsThrow = true;
namespace y_absl {
    bool FatalErrorsThrowException() {
        return IsFatalErrorsThrow;
    }
}

namespace y_absl {
Y_ABSL_NAMESPACE_BEGIN
namespace log_internal {

namespace {
// message `logging.proto.Event`
enum EventTag : uint8_t {
  kFileName = 2,
  kFileLine = 3,
  kTimeNsecs = 4,
  kSeverity = 5,
  kThreadId = 6,
  kValue = 7,
  kSequenceNumber = 9,
  kThreadName = 10,
};

// message `logging.proto.Value`
enum ValueTag : uint8_t {
  kString = 1,
  kStringLiteral = 6,
};

// Decodes a `logging.proto.Value` from `buf` and writes a string representation
// into `dst`.  The string representation will be truncated if `dst` is not
// large enough to hold it.  Returns false if `dst` has size zero or one (i.e.
// sufficient only for a nul-terminator) and no decoded data could be written.
// This function may or may not write a nul-terminator into `dst`, and it may or
// may not truncate the data it writes in order to do make space for that nul
// terminator.  In any case, `dst` will be advanced to point at the byte where
// subsequent writes should begin.
bool PrintValue(y_absl::Span<char>& dst, y_absl::Span<const char> buf) {
  if (dst.size() <= 1) return false;
  ProtoField field;
  while (field.DecodeFrom(&buf)) {
    switch (field.tag()) {
      case ValueTag::kString:
      case ValueTag::kStringLiteral:
        if (field.type() == WireType::kLengthDelimited)
          if (log_internal::AppendTruncated(field.string_value(), dst) <
              field.string_value().size())
            return false;
    }
  }
  return true;
}

// See `logging.proto.Severity`
int32_t ProtoSeverity(y_absl::LogSeverity severity, int verbose_level) {
  switch (severity) {
    case y_absl::LogSeverity::kInfo:
      if (verbose_level == y_absl::LogEntry::kNoVerbosityLevel) return 800;
      return 600 - verbose_level;
    case y_absl::LogSeverity::kWarning:
      return 900;
    case y_absl::LogSeverity::kError:
      return 950;
    case y_absl::LogSeverity::kFatal:
      return 1100;
    default:
      return 800;
  }
}

y_absl::string_view Basename(y_absl::string_view filepath) {
#ifdef _WIN32
  size_t path = filepath.find_last_of("/\\");
#else
  size_t path = filepath.find_last_of('/');
#endif
  if (path != filepath.npos) filepath.remove_prefix(path + 1);
  return filepath;
}

void WriteToString(const char* data, void* str) {
  reinterpret_cast<TString*>(str)->append(data);
}
void WriteToStream(const char* data, void* os) {
  auto* cast_os = static_cast<std::ostream*>(os);
  *cast_os << data;
}
}  // namespace

struct LogMessage::LogMessageData final {
  LogMessageData(const char* file, int line, y_absl::LogSeverity severity,
                 y_absl::Time timestamp);
  LogMessageData(const LogMessageData&) = delete;
  LogMessageData& operator=(const LogMessageData&) = delete;

  // `LogEntry` sent to `LogSink`s; contains metadata.
  y_absl::LogEntry entry;

  // true => this was first fatal msg
  bool first_fatal;
  // true => all failures should be quiet
  bool fail_quietly;
  // true => PLOG was requested
  bool is_perror;

  // Extra `LogSink`s to log to, in addition to `global_sinks`.
  y_absl::InlinedVector<y_absl::LogSink*, 16> extra_sinks;
  // If true, log to `extra_sinks` but not to `global_sinks` or hardcoded
  // non-sink targets (e.g. stderr, log files).
  bool extra_sinks_only;

  std::ostream manipulated;  // ostream with IO manipulators applied

  // A `logging.proto.Event` proto message is built into `encoded_buf`.
  std::array<char, kLogMessageBufferSize> encoded_buf;
  // `encoded_remaining()` is the suffix of `encoded_buf` that has not been
  // filled yet.  If a datum to be encoded does not fit into
  // `encoded_remaining()` and cannot be truncated to fit, the size of
  // `encoded_remaining()` will be zeroed to prevent encoding of any further
  // data.  Note that in this case its `data()` pointer will not point past the
  // end of `encoded_buf`.
  // The first use of `encoded_remaining()` is our chance to record metadata
  // after any modifications (e.g. by `AtLocation()`) but before any data have
  // been recorded.  We want to record metadata before data so that data are
  // preferentially truncated if we run out of buffer.
  y_absl::Span<char>& encoded_remaining() {
    if (encoded_remaining_actual_do_not_use_directly.data() == nullptr) {
      encoded_remaining_actual_do_not_use_directly =
          y_absl::MakeSpan(encoded_buf);
      InitializeEncodingAndFormat();
    }
    return encoded_remaining_actual_do_not_use_directly;
  }
  y_absl::Span<char> encoded_remaining_actual_do_not_use_directly;

  // A formatted string message is built in `string_buf`.
  std::array<char, kLogMessageBufferSize> string_buf;

  void InitializeEncodingAndFormat();
  void FinalizeEncodingAndFormat();
};

LogMessage::LogMessageData::LogMessageData(const char* file, int line,
                                           y_absl::LogSeverity severity,
                                           y_absl::Time timestamp)
    : extra_sinks_only(false), manipulated(nullptr) {
  // Legacy defaults for LOG's ostream:
  manipulated.setf(std::ios_base::showbase | std::ios_base::boolalpha);
  entry.full_filename_ = file;
  entry.base_filename_ = Basename(file);
  entry.line_ = line;
  entry.prefix_ = y_absl::ShouldPrependLogPrefix();
  entry.severity_ = y_absl::NormalizeLogSeverity(severity);
  entry.verbose_level_ = y_absl::LogEntry::kNoVerbosityLevel;
  entry.timestamp_ = timestamp;
  entry.tid_ = y_absl::base_internal::GetCachedTID();
}

void LogMessage::LogMessageData::InitializeEncodingAndFormat() {
  EncodeStringTruncate(EventTag::kFileName, entry.source_filename(),
                       &encoded_remaining());
  EncodeVarint(EventTag::kFileLine, entry.source_line(), &encoded_remaining());
  EncodeVarint(EventTag::kTimeNsecs, y_absl::ToUnixNanos(entry.timestamp()),
               &encoded_remaining());
  EncodeVarint(EventTag::kSeverity,
               ProtoSeverity(entry.log_severity(), entry.verbosity()),
               &encoded_remaining());
  EncodeVarint(EventTag::kThreadId, entry.tid(), &encoded_remaining());
}

void LogMessage::LogMessageData::FinalizeEncodingAndFormat() {
  // Note that `encoded_remaining()` may have zero size without pointing past
  // the end of `encoded_buf`, so the difference between `data()` pointers is
  // used to compute the size of `encoded_data`.
  y_absl::Span<const char> encoded_data(
      encoded_buf.data(),
      static_cast<size_t>(encoded_remaining().data() - encoded_buf.data()));
  // `string_remaining` is the suffix of `string_buf` that has not been filled
  // yet.
  y_absl::Span<char> string_remaining(string_buf);
  // We may need to write a newline and nul-terminator at the end of the decoded
  // string data.  Rather than worry about whether those should overwrite the
  // end of the string (if the buffer is full) or be appended, we avoid writing
  // into the last two bytes so we always have space to append.
  string_remaining.remove_suffix(2);
  entry.prefix_len_ =
      entry.prefix() ? log_internal::FormatLogPrefix(
                           entry.log_severity(), entry.timestamp(), entry.tid(),
                           entry.source_basename(), entry.source_line(),
                           log_internal::ThreadIsLoggingToLogSink()
                               ? PrefixFormat::kRaw
                               : PrefixFormat::kNotRaw,
                           string_remaining)
                     : 0;
  // Decode data from `encoded_buf` until we run out of data or we run out of
  // `string_remaining`.
  ProtoField field;
  while (field.DecodeFrom(&encoded_data)) {
    switch (field.tag()) {
      case EventTag::kValue:
        if (field.type() != WireType::kLengthDelimited) continue;
        if (PrintValue(string_remaining, field.bytes_value())) continue;
        break;
    }
  }
  auto chars_written =
      static_cast<size_t>(string_remaining.data() - string_buf.data());
    string_buf[chars_written++] = '\n';
  string_buf[chars_written++] = '\0';
  entry.text_message_with_prefix_and_newline_and_nul_ =
      y_absl::MakeSpan(string_buf).subspan(0, chars_written);
}

LogMessage::LogMessage(const char* file, int line, y_absl::LogSeverity severity)
    : data_(y_absl::make_unique<LogMessageData>(file, line, severity,
                                              y_absl::Now())) {
  data_->first_fatal = false;
  data_->is_perror = false;
  data_->fail_quietly = false;

  // This logs a backtrace even if the location is subsequently changed using
  // AtLocation.  This quirk, and the behavior when AtLocation is called twice,
  // are fixable but probably not worth fixing.
  LogBacktraceIfNeeded();
}

LogMessage::LogMessage(const char* file, int line, InfoTag)
    : LogMessage(file, line, y_absl::LogSeverity::kInfo) {}
LogMessage::LogMessage(const char* file, int line, WarningTag)
    : LogMessage(file, line, y_absl::LogSeverity::kWarning) {}
LogMessage::LogMessage(const char* file, int line, ErrorTag)
    : LogMessage(file, line, y_absl::LogSeverity::kError) {}

LogMessage::~LogMessage() {
#ifdef Y_ABSL_MIN_LOG_LEVEL
  if (data_->entry.log_severity() <
          static_cast<y_absl::LogSeverity>(Y_ABSL_MIN_LOG_LEVEL) &&
      data_->entry.log_severity() < y_absl::LogSeverity::kFatal) {
    return;
  }
#endif
  Flush();
}

LogMessage& LogMessage::AtLocation(y_absl::string_view file, int line) {
  data_->entry.full_filename_ = file;
  data_->entry.base_filename_ = Basename(file);
  data_->entry.line_ = line;
  LogBacktraceIfNeeded();
  return *this;
}

LogMessage& LogMessage::NoPrefix() {
  data_->entry.prefix_ = false;
  return *this;
}

LogMessage& LogMessage::WithVerbosity(int verbose_level) {
  if (verbose_level == y_absl::LogEntry::kNoVerbosityLevel) {
    data_->entry.verbose_level_ = y_absl::LogEntry::kNoVerbosityLevel;
  } else {
    data_->entry.verbose_level_ = std::max(0, verbose_level);
  }
  return *this;
}

LogMessage& LogMessage::WithTimestamp(y_absl::Time timestamp) {
  data_->entry.timestamp_ = timestamp;
  return *this;
}

LogMessage& LogMessage::WithThreadID(y_absl::LogEntry::tid_t tid) {
  data_->entry.tid_ = tid;
  return *this;
}

LogMessage& LogMessage::WithMetadataFrom(const y_absl::LogEntry& entry) {
  data_->entry.full_filename_ = entry.full_filename_;
  data_->entry.base_filename_ = entry.base_filename_;
  data_->entry.line_ = entry.line_;
  data_->entry.prefix_ = entry.prefix_;
  data_->entry.severity_ = entry.severity_;
  data_->entry.verbose_level_ = entry.verbose_level_;
  data_->entry.timestamp_ = entry.timestamp_;
  data_->entry.tid_ = entry.tid_;
  return *this;
}

LogMessage& LogMessage::WithPerror() {
  data_->is_perror = true;
  return *this;
}

LogMessage& LogMessage::ToSinkAlso(y_absl::LogSink* sink) {
  Y_ABSL_INTERNAL_CHECK(sink, "null LogSink*");
  data_->extra_sinks.push_back(sink);
  return *this;
}

LogMessage& LogMessage::ToSinkOnly(y_absl::LogSink* sink) {
  Y_ABSL_INTERNAL_CHECK(sink, "null LogSink*");
  data_->extra_sinks.clear();
  data_->extra_sinks.push_back(sink);
  data_->extra_sinks_only = true;
  return *this;
}

#ifdef __ELF__
extern "C" void __gcov_dump() Y_ABSL_ATTRIBUTE_WEAK;
extern "C" void __gcov_flush() Y_ABSL_ATTRIBUTE_WEAK;
#endif

void LogMessage::FailWithoutStackTrace() {
  // Now suppress repeated trace logging:
  log_internal::SetSuppressSigabortTrace(true);
#if defined _DEBUG && defined COMPILER_MSVC
  // When debugging on windows, avoid the obnoxious dialog.
  __debugbreak();
#endif

#ifdef __ELF__
  // For b/8737634, flush coverage if we are in coverage mode.
  if (&__gcov_dump != nullptr) {
    __gcov_dump();
  } else if (&__gcov_flush != nullptr) {
    __gcov_flush();
  }
#endif

  abort();
}

void LogMessage::FailQuietly() {
  // _exit. Calling abort() would trigger all sorts of death signal handlers
  // and a detailed stack trace. Calling exit() would trigger the onexit
  // handlers, including the heap-leak checker, which is guaranteed to fail in
  // this case: we probably just new'ed the TString that we logged.
  // Anyway, if you're calling Fail or FailQuietly, you're trying to bail out
  // of the program quickly, and it doesn't make much sense for FailQuietly to
  // offer different guarantees about exit behavior than Fail does. (And as a
  // consequence for QCHECK and CHECK to offer different exit behaviors)
  _exit(1);
}

LogMessage& LogMessage::operator<<(const TString& v) {
  CopyToEncodedBuffer<StringType::kNotLiteral>(v);
  return *this;
}

LogMessage& LogMessage::operator<<(y_absl::string_view v) {
  CopyToEncodedBuffer<StringType::kNotLiteral>(v);
  return *this;
}
LogMessage& LogMessage::operator<<(std::ostream& (*m)(std::ostream& os)) {
  OstreamView view(*data_);
  data_->manipulated << m;
  return *this;
}
LogMessage& LogMessage::operator<<(std::ios_base& (*m)(std::ios_base& os)) {
  OstreamView view(*data_);
  data_->manipulated << m;
  return *this;
}
template LogMessage& LogMessage::operator<<(const char& v);
template LogMessage& LogMessage::operator<<(const signed char& v);
template LogMessage& LogMessage::operator<<(const unsigned char& v);
template LogMessage& LogMessage::operator<<(const short& v);           // NOLINT
template LogMessage& LogMessage::operator<<(const unsigned short& v);  // NOLINT
template LogMessage& LogMessage::operator<<(const int& v);
template LogMessage& LogMessage::operator<<(const unsigned int& v);
template LogMessage& LogMessage::operator<<(const long& v);           // NOLINT
template LogMessage& LogMessage::operator<<(const unsigned long& v);  // NOLINT
template LogMessage& LogMessage::operator<<(const long long& v);      // NOLINT
template LogMessage& LogMessage::operator<<(
    const unsigned long long& v);  // NOLINT
template LogMessage& LogMessage::operator<<(void* const& v);
template LogMessage& LogMessage::operator<<(const void* const& v);
template LogMessage& LogMessage::operator<<(const float& v);
template LogMessage& LogMessage::operator<<(const double& v);
template LogMessage& LogMessage::operator<<(const bool& v);

void LogMessage::Flush() {
  if (data_->entry.log_severity() < y_absl::MinLogLevel()) return;

  if (data_->is_perror) {
    InternalStream() << ": " << y_absl::base_internal::StrError(errno_saver_())
                     << " [" << errno_saver_() << "]";
  }

  // Have we already seen a fatal message?
  Y_ABSL_CONST_INIT static std::atomic<bool> seen_fatal(false);
  if (data_->entry.log_severity() == y_absl::LogSeverity::kFatal &&
      y_absl::log_internal::ExitOnDFatal()) {
    // Exactly one LOG(FATAL) message is responsible for aborting the process,
    // even if multiple threads LOG(FATAL) concurrently.
    bool expected_seen_fatal = false;
    if (seen_fatal.compare_exchange_strong(expected_seen_fatal, true,
                                           std::memory_order_relaxed)) {
      data_->first_fatal = true;
    }
  }

  data_->FinalizeEncodingAndFormat();
  data_->entry.encoding_ =
      y_absl::string_view(data_->encoded_buf.data(),
                        static_cast<size_t>(data_->encoded_remaining().data() -
                                            data_->encoded_buf.data()));
  SendToLog();
}

void LogMessage::SetFailQuietly() { data_->fail_quietly = true; }

LogMessage::OstreamView::OstreamView(LogMessageData& message_data)
    : data_(message_data), encoded_remaining_copy_(data_.encoded_remaining()) {
  // This constructor sets the `streambuf` up so that streaming into an attached
  // ostream encodes string data in-place.  To do that, we write appropriate
  // headers into the buffer using a copy of the buffer view so that we can
  // decide not to keep them later if nothing is ever streamed in.  We don't
  // know how much data we'll get, but we can use the size of the remaining
  // buffer as an upper bound and fill in the right size once we know it.
  message_start_ =
      EncodeMessageStart(EventTag::kValue, encoded_remaining_copy_.size(),
                         &encoded_remaining_copy_);
  string_start_ =
      EncodeMessageStart(ValueTag::kString, encoded_remaining_copy_.size(),
                         &encoded_remaining_copy_);
  setp(encoded_remaining_copy_.data(),
       encoded_remaining_copy_.data() + encoded_remaining_copy_.size());
  data_.manipulated.rdbuf(this);
}

LogMessage::OstreamView::~OstreamView() {
  data_.manipulated.rdbuf(nullptr);
  if (!string_start_.data()) {
    // The second field header didn't fit.  Whether the first one did or not, we
    // shouldn't commit `encoded_remaining_copy_`, and we also need to zero the
    // size of `data_->encoded_remaining()` so that no more data are encoded.
    data_.encoded_remaining().remove_suffix(data_.encoded_remaining().size());
    return;
  }
  const y_absl::Span<const char> contents(pbase(),
                                        static_cast<size_t>(pptr() - pbase()));
  if (contents.empty()) return;
  encoded_remaining_copy_.remove_prefix(contents.size());
  EncodeMessageLength(string_start_, &encoded_remaining_copy_);
  EncodeMessageLength(message_start_, &encoded_remaining_copy_);
  data_.encoded_remaining() = encoded_remaining_copy_;
}

std::ostream& LogMessage::OstreamView::stream() { return data_.manipulated; }

bool LogMessage::IsFatal() const {
  return data_->entry.log_severity() == y_absl::LogSeverity::kFatal &&
         y_absl::log_internal::ExitOnDFatal();
}

void LogMessage::PrepareToDie() {
  // If we log a FATAL message, flush all the log destinations, then toss
  // a signal for others to catch. We leave the logs in a state that
  // someone else can use them (as long as they flush afterwards)
  if (data_->first_fatal) {
    // Notify observers about the upcoming fatal error.
    Y_ABSL_INTERNAL_C_SYMBOL(AbslInternalOnFatalLogMessage)(data_->entry);
  }

  if (!data_->fail_quietly) {
    // Log the message first before we start collecting stack trace.
    log_internal::LogToSinks(data_->entry, y_absl::MakeSpan(data_->extra_sinks),
                             data_->extra_sinks_only);

    // `DumpStackTrace` generates an empty string under MSVC.
    // Adding the constant prefix here simplifies testing.
    data_->entry.stacktrace_ = "*** Check failure stack trace: ***\n";
    debugging_internal::DumpStackTrace(
        0, log_internal::MaxFramesInLogStackTrace(),
        log_internal::ShouldSymbolizeLogStackTrace(), WriteToString,
        &data_->entry.stacktrace_);
  }
}

void LogMessage::Die() {
  y_absl::FlushLogSinks();

  if (data_->fail_quietly) {
    FailQuietly();
  } else {
    FailWithoutStackTrace();
  }
}

class FatalException : public std::exception {
 public:
  FatalException(const char* filename, int line, const TString& message)
      : filename_(filename), line_(line), message_(message) {}
  virtual ~FatalException() throw() {}
  const char* what() const throw() override { return message_.c_str(); }
  const char* filename() const { return filename_; }
  int line() const { return line_; }
  const TString& message() const { return message_; }

 private:
  const char* filename_;
  const int line_;
  const TString message_;
};

void LogMessage::SendToLog() {
  if (IsFatal() && !::y_absl::FatalErrorsThrowException()) PrepareToDie();

  // Also log to all registered sinks, even if OnlyLogToStderr() is set.
  log_internal::LogToSinks(data_->entry, y_absl::MakeSpan(data_->extra_sinks),
                           data_->extra_sinks_only);
  if (IsFatal() && !::y_absl::FatalErrorsThrowException()) Die();
}

void LogMessage::LogBacktraceIfNeeded() {
  if (!y_absl::log_internal::IsInitialized()) return;

  if (!y_absl::log_internal::ShouldLogBacktraceAt(data_->entry.source_basename(),
                                                data_->entry.source_line()))
    return;
  OstreamView view(*data_);
  view.stream() << " (stacktrace:\n";
  debugging_internal::DumpStackTrace(
      1, log_internal::MaxFramesInLogStackTrace(),
      log_internal::ShouldSymbolizeLogStackTrace(), WriteToStream,
      &view.stream());
  view.stream() << ") ";
}

// Encodes into `data_->encoded_remaining()` a partial `logging.proto.Event`
// containing the specified string data using a `Value` field appropriate to
// `str_type`.  Truncates `str` if necessary, but emits nothing and marks the
// buffer full if  even the field headers do not fit.
template <LogMessage::StringType str_type>
void LogMessage::CopyToEncodedBuffer(y_absl::string_view str) {
  auto encoded_remaining_copy = data_->encoded_remaining();
  auto start = EncodeMessageStart(
      EventTag::kValue, BufferSizeFor(WireType::kLengthDelimited) + str.size(),
      &encoded_remaining_copy);
  // If the `logging.proto.Event.value` field header did not fit,
  // `EncodeMessageStart` will have zeroed `encoded_remaining_copy`'s size and
  // `EncodeStringTruncate` will fail too.
  if (EncodeStringTruncate(str_type == StringType::kLiteral
                               ? ValueTag::kStringLiteral
                               : ValueTag::kString,
                           str, &encoded_remaining_copy)) {
    // The string may have been truncated, but the field header fit.
    EncodeMessageLength(start, &encoded_remaining_copy);
    data_->encoded_remaining() = encoded_remaining_copy;
  } else {
    // The field header(s) did not fit; zero `encoded_remaining()` so we don't
    // write anything else later.
    data_->encoded_remaining().remove_suffix(data_->encoded_remaining().size());
  }
}
template void LogMessage::CopyToEncodedBuffer<LogMessage::StringType::kLiteral>(
    y_absl::string_view str);
template void LogMessage::CopyToEncodedBuffer<
    LogMessage::StringType::kNotLiteral>(y_absl::string_view str);
template <LogMessage::StringType str_type>
void LogMessage::CopyToEncodedBuffer(char ch, size_t num) {
  auto encoded_remaining_copy = data_->encoded_remaining();
  auto value_start = EncodeMessageStart(
      EventTag::kValue, BufferSizeFor(WireType::kLengthDelimited) + num,
      &encoded_remaining_copy);
  auto str_start = EncodeMessageStart(str_type == StringType::kLiteral
                                          ? ValueTag::kStringLiteral
                                          : ValueTag::kString,
                                      num, &encoded_remaining_copy);
  if (str_start.data()) {
    // The field headers fit.
    log_internal::AppendTruncated(ch, num, encoded_remaining_copy);
    EncodeMessageLength(str_start, &encoded_remaining_copy);
    EncodeMessageLength(value_start, &encoded_remaining_copy);
    data_->encoded_remaining() = encoded_remaining_copy;
  } else {
    // The field header(s) did not fit; zero `encoded_remaining()` so we don't
    // write anything else later.
    data_->encoded_remaining().remove_suffix(data_->encoded_remaining().size());
  }
}
template void LogMessage::CopyToEncodedBuffer<LogMessage::StringType::kLiteral>(
    char ch, size_t num);
template void LogMessage::CopyToEncodedBuffer<
    LogMessage::StringType::kNotLiteral>(char ch, size_t num);

// We intentionally don't return from these destructors. Disable MSVC's warning
// about the destructor never returning as we do so intentionally here.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4722)
#endif

LogMessageFatal::LogMessageFatal(const char* file, int line)
    : LogMessage(file, line, y_absl::LogSeverity::kFatal), file_{file}, line_{line} {}

LogMessageFatal& LogMessageFatal::TryThrow() {
    if (::y_absl::FatalErrorsThrowException()) {
        throw FatalException(file_, line_, "LogMessageFatal exception");
    }
    return *this;
}

LogMessageFatal::LogMessageFatal(const char* file, int line,
                                 y_absl::string_view failure_msg)
    : LogMessage(file, line, y_absl::LogSeverity::kFatal) {
  *this << "Check failed: " << failure_msg << " ";
}

LogMessageFatal::~LogMessageFatal() {
  Flush();
  if (!::y_absl::FatalErrorsThrowException())
  FailWithoutStackTrace();
}

LogMessageDebugFatal::LogMessageDebugFatal(const char* file, int line)
    : LogMessage(file, line, y_absl::LogSeverity::kFatal) {}

LogMessageDebugFatal::~LogMessageDebugFatal() {
  Flush();
  FailWithoutStackTrace();
}

LogMessageQuietlyDebugFatal::LogMessageQuietlyDebugFatal(const char* file,
                                                         int line)
    : LogMessage(file, line, y_absl::LogSeverity::kFatal) {
  SetFailQuietly();
}

LogMessageQuietlyDebugFatal::~LogMessageQuietlyDebugFatal() {
  Flush();
  FailQuietly();
}

LogMessageQuietlyFatal::LogMessageQuietlyFatal(const char* file, int line)
    : LogMessage(file, line, y_absl::LogSeverity::kFatal) {
  SetFailQuietly();
}

LogMessageQuietlyFatal::LogMessageQuietlyFatal(const char* file, int line,
                                               y_absl::string_view failure_msg)
    : LogMessageQuietlyFatal(file, line) {
    *this << "Check failed: " << failure_msg << " ";
}

LogMessageQuietlyFatal::~LogMessageQuietlyFatal() {
  Flush();
  FailQuietly();
}
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

}  // namespace log_internal

Y_ABSL_NAMESPACE_END
}  // namespace y_absl
