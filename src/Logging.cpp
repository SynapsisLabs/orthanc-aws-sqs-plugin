// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.

#include "Logging.h"

#include <atomic>

namespace synapsis::aws_sqs {

namespace {
std::atomic<OrthancPluginContext*> g_context{nullptr};
constexpr const char* kPrefix = "[aws-sqs] ";
}  // namespace

void SetContext(OrthancPluginContext* context) noexcept {
    g_context.store(context, std::memory_order_release);
}

OrthancPluginContext* Context() noexcept {
    return g_context.load(std::memory_order_acquire);
}

LogStream::~LogStream() {
    OrthancPluginContext* ctx = Context();
    const std::string msg = kPrefix + buffer_.str();
    if (ctx == nullptr) {
        // Plugin not yet initialised — fall back to stderr so we don't lose
        // diagnostics during early startup.
        std::fputs(msg.c_str(), stderr);
        std::fputc('\n', stderr);
        return;
    }
    switch (level_) {
        case Level::Info:
            OrthancPluginLogInfo(ctx, msg.c_str());
            break;
        case Level::Warn:
            OrthancPluginLogWarning(ctx, msg.c_str());
            break;
        case Level::Error:
            OrthancPluginLogError(ctx, msg.c_str());
            break;
    }
}

}  // namespace synapsis::aws_sqs
