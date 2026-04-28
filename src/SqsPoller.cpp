// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.

#include "SqsPoller.h"

#include "Logging.h"
#include "S3Downloader.h"

#include <aws/core/client/ClientConfiguration.h>
#include <aws/sqs/model/DeleteMessageRequest.h>
#include <aws/sqs/model/Message.h>
#include <aws/sqs/model/MessageSystemAttributeName.h>
#include <aws/sqs/model/ReceiveMessageRequest.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace synapsis::aws_sqs {

using json = nlohmann::json;

namespace {

Aws::Client::ClientConfiguration MakeAwsConfig(const std::string& region) {
    Aws::Client::ClientConfiguration cfg;
    cfg.region = region;
    cfg.connectTimeoutMs = 5000;
    cfg.requestTimeoutMs = 30000;
    // Use the SDK's standard retry mode (exponential backoff + jitter)
    cfg.retryStrategy =
        Aws::MakeShared<Aws::Client::StandardRetryStrategy>("aws-sqs-plugin", 3);
    return cfg;
}

// Decode an S3-event-notification key. AWS encodes object keys with a mix
// of percent-encoding (`%XX`) and form-encoding (`+` for space). We accept
// both — `+` is decoded to space, `%XX` to its byte. Lone `%` or `%X`
// (truncated) is left as-is (AWS shouldn't emit those, but we don't want
// to crash if it does).
std::string UrlDecodeS3Key(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < src.size()
                   && std::isxdigit(static_cast<unsigned char>(src[i + 1]))
                   && std::isxdigit(static_cast<unsigned char>(src[i + 2]))) {
            const char hex[3] = { src[i + 1], src[i + 2], '\0' };
            out.push_back(static_cast<char>(std::strtol(hex, nullptr, 16)));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Parse an S3 Event Notification body. Returns a list of (bucket, key) pairs.
// AWS publishes:
//   { "Records": [
//       { "s3": { "bucket": { "name": "..." }, "object": { "key": "..." } } },
//       ...
//   ] }
//
// "Test events" published when the notification is first wired up have the
// shape { "Service": "Amazon S3", "Event": "s3:TestEvent", ... } and have
// no Records[] — we silently treat those as a no-op.
//
// IMPORTANT: object keys arrive URL-encoded (per the S3 event notification
// spec). They must be decoded before being passed to S3 GetObject.
std::vector<std::pair<std::string, std::string>>
ExtractS3Records(const std::string& message_body) {
    std::vector<std::pair<std::string, std::string>> out;
    json body = json::parse(message_body, /*cb*/ nullptr,
                            /*allow_exceptions*/ true);

    if (!body.is_object() || !body.contains("Records")) {
        return out;  // test event, or non-S3 payload
    }
    const json& records = body["Records"];
    if (!records.is_array()) {
        return out;
    }
    out.reserve(records.size());
    for (const auto& rec : records) {
        if (!rec.is_object()) continue;
        if (!rec.contains("s3")) continue;
        const json& s3 = rec["s3"];
        if (!s3.is_object()) continue;
        std::string bucket;
        std::string key_encoded;
        if (s3.contains("bucket") && s3["bucket"].is_object()) {
            bucket = s3["bucket"].value("name", std::string{});
        }
        if (s3.contains("object") && s3["object"].is_object()) {
            key_encoded = s3["object"].value("key", std::string{});
        }
        if (!bucket.empty() && !key_encoded.empty()) {
            out.emplace_back(std::move(bucket), UrlDecodeS3Key(key_encoded));
        }
    }
    return out;
}

}  // namespace

SqsPoller::SqsPoller(OrthancPluginContext* context, QueueConfig config)
    : context_(context), config_(std::move(config)) {
    auto aws_config = MakeAwsConfig(config_.region);
    sqs_client_ = Aws::MakeShared<Aws::SQS::SQSClient>(
        "aws-sqs-plugin", aws_config);
    s3_client_  = Aws::MakeShared<Aws::S3::S3Client>(
        "aws-sqs-plugin", aws_config);
    downloader_ = std::make_unique<S3Downloader>(s3_client_);
}

SqsPoller::~SqsPoller() {
    Stop();
}

void SqsPoller::Start() {
    LOG_INFO() << "[" << config_.name << "] starting poller (region="
               << config_.region << ")";
    thread_ = std::thread([this] { this->Run(); });
}

void SqsPoller::Stop() {
    if (stop_.exchange(true)) {
        return;  // already stopping
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    LOG_INFO() << "[" << config_.name << "] poller stopped";
}

void SqsPoller::Run() {
    using namespace std::chrono_literals;

    while (!stop_.load(std::memory_order_relaxed)) {
        Aws::SQS::Model::ReceiveMessageRequest req;
        req.SetQueueUrl(config_.url.c_str());
        req.SetMaxNumberOfMessages(config_.max_messages);
        req.SetWaitTimeSeconds(config_.wait_time_seconds);
        req.SetVisibilityTimeout(config_.visibility_timeout_secs);
        req.AddMessageSystemAttributeNames(
            Aws::SQS::Model::MessageSystemAttributeName::SentTimestamp);

        auto outcome = sqs_client_->ReceiveMessage(req);
        if (!outcome.IsSuccess()) {
            const auto& err = outcome.GetError();
            LOG_ERROR() << "[" << config_.name
                        << "] ReceiveMessage failed: "
                        << err.GetExceptionName() << ": " << err.GetMessage();
            // Backoff on failure to avoid hammering the API
            for (int i = 0; i < 5 && !stop_.load(std::memory_order_relaxed); ++i) {
                std::this_thread::sleep_for(1s);
            }
            continue;
        }

        const auto& messages = outcome.GetResult().GetMessages();
        if (messages.empty()) {
            continue;  // long-poll exhausted, loop will re-poll
        }
        LOG_INFO() << "[" << config_.name << "] received "
                   << messages.size() << " messages";

        for (const auto& msg : messages) {
            if (stop_.load(std::memory_order_relaxed)) break;

            // Log how long this message has been sitting in SQS — useful
            // for catching pipeline backlog before queue depth alarms fire.
            const auto& attrs = msg.GetAttributes();
            const auto sent_it = attrs.find(
                Aws::SQS::Model::MessageSystemAttributeName::SentTimestamp);
            if (sent_it != attrs.end()) {
                long long sent_ms = std::strtoll(sent_it->second.c_str(),
                                                 nullptr, 10);
                long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                long long age_ms = now_ms - sent_ms;
                if (age_ms > 30000) {  // log only if > 30s — keeps the chatter low
                    LOG_INFO() << "[" << config_.name
                               << "] message age " << (age_ms / 1000) << "s";
                }
            }

            bool ok = false;
            try {
                ok = ProcessMessage(msg);
            } catch (const std::exception& e) {
                LOG_ERROR() << "[" << config_.name
                            << "] message processing threw: " << e.what();
                ok = false;
            } catch (...) {
                LOG_ERROR() << "[" << config_.name
                            << "] message processing threw unknown exception";
                ok = false;
            }

            if (ok || config_.delete_on_ingest_failure) {
                Aws::SQS::Model::DeleteMessageRequest del;
                del.SetQueueUrl(config_.url.c_str());
                del.SetReceiptHandle(msg.GetReceiptHandle());
                auto del_outcome = sqs_client_->DeleteMessage(del);
                if (!del_outcome.IsSuccess()) {
                    LOG_ERROR() << "[" << config_.name
                                << "] DeleteMessage failed: "
                                << del_outcome.GetError().GetMessage();
                }
            }
            // If !ok and !delete_on_ingest_failure, we leave the message
            // visible — it'll be re-delivered after VisibilityTimeout, and
            // eventually go to DLQ via SQS's redrive policy if it keeps
            // failing.
        }
    }
}

bool SqsPoller::ProcessMessage(const Aws::SQS::Model::Message& message) {
    const std::string& body = message.GetBody();
    auto records = ExtractS3Records(body);
    if (records.empty()) {
        // S3 test event or empty — succeed and delete
        LOG_INFO() << "[" << config_.name
                   << "] message has no S3 records (test event?); deleting";
        return true;
    }

    bool all_ok = true;
    for (const auto& [bucket, key] : records) {
        try {
            IngestS3Object(bucket, key);
        } catch (const std::exception& e) {
            LOG_ERROR() << "[" << config_.name << "] ingest failed for s3://"
                        << bucket << "/" << key << ": " << e.what();
            all_ok = false;
            // Continue processing the remaining records — we only set
            // all_ok=false to signal "do not delete this message".
            // Note: because SQS messages are atomic, partial-failure means
            // the whole message will be redelivered. The Orthanc on-stored
            // callback de-dup logic should make repeated ingests a no-op.
        }
    }
    return all_ok;
}

void SqsPoller::IngestS3Object(const std::string& bucket,
                               const std::string& key) {
    const auto t0 = std::chrono::steady_clock::now();

    auto bytes = downloader_->Download(bucket, key);

    if (!PostInstance(bytes)) {
        throw std::runtime_error("Orthanc rejected /instances POST");
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    LOG_INFO() << "[" << config_.name << "] ingested s3://" << bucket << "/" << key
               << " (" << bytes.size() << " bytes, " << elapsed.count() << " ms)";
}

bool SqsPoller::PostInstance(const std::vector<uint8_t>& dicom_bytes) {
    OrthancPluginMemoryBuffer response{};
    OrthancPluginErrorCode code = OrthancPluginRestApiPost(
        context_,
        &response,
        "/instances",
        reinterpret_cast<const char*>(dicom_bytes.data()),
        dicom_bytes.size());

    // Orthanc fills `response` even on success; we don't care about its
    // contents (just whether the call succeeded), but we must free it.
    OrthancPluginFreeMemoryBuffer(context_, &response);

    if (code != OrthancPluginErrorCode_Success) {
        LOG_ERROR() << "[" << config_.name
                    << "] OrthancPluginRestApiPost(/instances) failed (code "
                    << static_cast<int>(code) << ")";
        return false;
    }
    return true;
}

}  // namespace synapsis::aws_sqs
