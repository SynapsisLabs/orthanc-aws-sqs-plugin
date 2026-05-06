// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.

#include "S3EventParser.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>

namespace synapsis::aws_sqs {

using json = nlohmann::json;

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

}  // namespace synapsis::aws_sqs
