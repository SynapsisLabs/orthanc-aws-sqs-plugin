// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.
//
// Pure helpers for parsing AWS S3 Event Notification messages off SQS.
// Kept free of AWS SDK / Orthanc dependencies so they're unit-testable.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace synapsis::aws_sqs {

// Decode an S3-event-notification key. AWS encodes object keys with a mix
// of percent-encoding (`%XX`) and form-encoding (`+` for space). We accept
// both — `+` is decoded to space, `%XX` to its byte. Lone `%` or `%X`
// (truncated) is left as-is (AWS shouldn't emit those, but we don't want
// to crash if it does).
std::string UrlDecodeS3Key(const std::string& src);

// Parse an S3 Event Notification body. Returns a list of (bucket, key) pairs
// with keys already URL-decoded. Returns empty vector for S3 "test events"
// or any payload without a Records[] array.
//
// Throws on malformed JSON.
std::vector<std::pair<std::string, std::string>>
ExtractS3Records(const std::string& message_body);

}  // namespace synapsis::aws_sqs
