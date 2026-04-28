// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.
//
// Thin wrappers around Orthanc's logging API so plugin code reads naturally
// while still routing into Orthanc's log file at the right severity.
//
// Usage:
//     LOG_INFO()  << "queue=" << queue.name << " received " << n << " messages";
//     LOG_WARN()  << "no s3 records in message body";
//     LOG_ERROR() << "ingest failed: " << e.what();

#pragma once

#include <orthanc/OrthancCPlugin.h>

#include <sstream>
#include <string>

namespace synapsis::aws_sqs {

void SetContext(OrthancPluginContext* context) noexcept;
OrthancPluginContext* Context() noexcept;

class LogStream {
public:
    enum class Level { Info, Warn, Error };

    explicit LogStream(Level level) : level_(level) {}

    // Disallow copy, allow temporaries to flush on destruction.
    LogStream(const LogStream&) = delete;
    LogStream& operator=(const LogStream&) = delete;

    template <typename T>
    LogStream& operator<<(const T& value) {
        buffer_ << value;
        return *this;
    }

    ~LogStream();

private:
    Level level_;
    std::ostringstream buffer_;
};

}  // namespace synapsis::aws_sqs

#define LOG_INFO()  ::synapsis::aws_sqs::LogStream(::synapsis::aws_sqs::LogStream::Level::Info)
#define LOG_WARN()  ::synapsis::aws_sqs::LogStream(::synapsis::aws_sqs::LogStream::Level::Warn)
#define LOG_ERROR() ::synapsis::aws_sqs::LogStream(::synapsis::aws_sqs::LogStream::Level::Error)
