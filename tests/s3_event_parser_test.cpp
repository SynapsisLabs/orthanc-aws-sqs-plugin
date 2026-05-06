// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.

#include "S3EventParser.h"

#include <gtest/gtest.h>

using synapsis::aws_sqs::ExtractS3Records;
using synapsis::aws_sqs::UrlDecodeS3Key;

TEST(UrlDecodeS3Key, PassthroughForUnreservedChars) {
    EXPECT_EQ(UrlDecodeS3Key("path/to/file.dcm"), "path/to/file.dcm");
}

TEST(UrlDecodeS3Key, DecodesPercentEncodedSpaces) {
    EXPECT_EQ(UrlDecodeS3Key("dir/file%20name.dcm"), "dir/file name.dcm");
}

TEST(UrlDecodeS3Key, DecodesPlusAsSpace) {
    // S3 event notifications use form-style "+" for spaces in the key field.
    EXPECT_EQ(UrlDecodeS3Key("dir/file+name.dcm"), "dir/file name.dcm");
}

TEST(UrlDecodeS3Key, DecodesMultiByteUtf8Sequences) {
    // "你" is U+4F60 → UTF-8: 0xE4 0xBD 0xA0
    EXPECT_EQ(UrlDecodeS3Key("dir/%E4%BD%A0.dcm"), "dir/\xE4\xBD\xA0.dcm");
}

TEST(UrlDecodeS3Key, MixedPercentAndPlus) {
    EXPECT_EQ(UrlDecodeS3Key("a+b%2Fc%20d"), "a b/c d");
}

TEST(UrlDecodeS3Key, AcceptsLowercaseHex) {
    EXPECT_EQ(UrlDecodeS3Key("a%2fb"), "a/b");
}

TEST(UrlDecodeS3Key, LeavesLonePercentAtEndUntouched) {
    // Truncated "%" or "%X" — defensive: don't decode, don't crash.
    EXPECT_EQ(UrlDecodeS3Key("file%"), "file%");
    EXPECT_EQ(UrlDecodeS3Key("file%2"), "file%2");
}

TEST(UrlDecodeS3Key, LeavesNonHexAfterPercentUntouched) {
    EXPECT_EQ(UrlDecodeS3Key("file%ZZ.dcm"), "file%ZZ.dcm");
}

TEST(UrlDecodeS3Key, EmptyString) {
    EXPECT_EQ(UrlDecodeS3Key(""), "");
}

TEST(ExtractS3Records, ReturnsBucketAndDecodedKey) {
    const std::string body = R"({
        "Records": [{
            "s3": {
                "bucket": {"name": "my-bucket"},
                "object": {"key": "studies/2026/file%20one.dcm"}
            }
        }]
    })";
    auto records = ExtractS3Records(body);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].first, "my-bucket");
    EXPECT_EQ(records[0].second, "studies/2026/file one.dcm");
}

TEST(ExtractS3Records, HandlesMultipleRecords) {
    const std::string body = R"({
        "Records": [
            {"s3": {"bucket": {"name": "b1"}, "object": {"key": "k1"}}},
            {"s3": {"bucket": {"name": "b2"}, "object": {"key": "dir/k+2"}}}
        ]
    })";
    auto records = ExtractS3Records(body);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].first, "b1");
    EXPECT_EQ(records[0].second, "k1");
    EXPECT_EQ(records[1].first, "b2");
    EXPECT_EQ(records[1].second, "dir/k 2");
}

TEST(ExtractS3Records, ReturnsEmptyForS3TestEvent) {
    // S3 emits this when a bucket notification is first wired up.
    const std::string body = R"({
        "Service": "Amazon S3",
        "Event": "s3:TestEvent",
        "Time": "2026-05-01T00:00:00.000Z",
        "Bucket": "my-bucket"
    })";
    EXPECT_TRUE(ExtractS3Records(body).empty());
}

TEST(ExtractS3Records, ReturnsEmptyWhenRecordsArrayMissing) {
    EXPECT_TRUE(ExtractS3Records(R"({"foo": "bar"})").empty());
}

TEST(ExtractS3Records, ReturnsEmptyWhenRecordsIsNotAnArray) {
    EXPECT_TRUE(ExtractS3Records(R"({"Records": {}})").empty());
}

TEST(ExtractS3Records, SkipsRecordsMissingS3Object) {
    const std::string body = R"({
        "Records": [
            {"s3": {"bucket": {"name": "b1"}}},
            {"s3": {"bucket": {"name": "b2"}, "object": {"key": "k2"}}}
        ]
    })";
    auto records = ExtractS3Records(body);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].second, "k2");
}

TEST(ExtractS3Records, SkipsRecordsWithEmptyBucketOrKey) {
    const std::string body = R"({
        "Records": [
            {"s3": {"bucket": {"name": ""},   "object": {"key": "k1"}}},
            {"s3": {"bucket": {"name": "b2"}, "object": {"key": ""}}},
            {"s3": {"bucket": {"name": "b3"}, "object": {"key": "k3"}}}
        ]
    })";
    auto records = ExtractS3Records(body);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].first, "b3");
}

TEST(ExtractS3Records, ThrowsOnMalformedJson) {
    EXPECT_THROW(ExtractS3Records("{not json"), std::exception);
}
