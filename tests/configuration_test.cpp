// Copyright (c) 2026 Synapsis Labs and contributors. Apache 2.0.

#include "Configuration.h"

#include <gtest/gtest.h>

using synapsis::aws_sqs::Configuration;

TEST(Configuration, MissingAwsSqsSectionLeavesPluginDisabled) {
    auto cfg = Configuration::Parse(R"({"OtherPlugin": {}})");
    EXPECT_FALSE(cfg.enabled);
    EXPECT_TRUE(cfg.queues.empty());
}

TEST(Configuration, NullAwsSqsSectionLeavesPluginDisabled) {
    auto cfg = Configuration::Parse(R"({"AwsSqs": null})");
    EXPECT_FALSE(cfg.enabled);
}

TEST(Configuration, DisabledShortCircuitsQueueParsing) {
    // Even a malformed Queues array should be ignored when disabled.
    auto cfg = Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": false,
            "Queues": "not-an-array"
        }
    })");
    EXPECT_FALSE(cfg.enabled);
}

TEST(Configuration, ParsesMinimalEnabledConfig) {
    auto cfg = Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true,
            "Region": "us-east-1",
            "Queues": [
                {"Name": "primary", "Url": "https://sqs.us-east-1.amazonaws.com/1/q"}
            ]
        }
    })");
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.region, "us-east-1");
    ASSERT_EQ(cfg.queues.size(), 1u);
    const auto& q = cfg.queues[0];
    EXPECT_EQ(q.name, "primary");
    EXPECT_EQ(q.url, "https://sqs.us-east-1.amazonaws.com/1/q");
    EXPECT_EQ(q.region, "us-east-1");  // inherited
    EXPECT_EQ(q.wait_time_seconds, 20);
    EXPECT_EQ(q.visibility_timeout_secs, 300);
    EXPECT_EQ(q.max_messages, 10);
    EXPECT_FALSE(q.delete_on_ingest_failure);
}

TEST(Configuration, QueueRegionOverridesTopLevel) {
    auto cfg = Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true,
            "Region": "us-east-1",
            "Queues": [
                {"Name": "eu",  "Url": "https://x", "Region": "eu-west-1"},
                {"Name": "us",  "Url": "https://y"}
            ]
        }
    })");
    ASSERT_EQ(cfg.queues.size(), 2u);
    EXPECT_EQ(cfg.queues[0].region, "eu-west-1");
    EXPECT_EQ(cfg.queues[1].region, "us-east-1");
}

TEST(Configuration, AllowsTopLevelRegionToBeOmittedIfQueuesSetTheirOwn) {
    auto cfg = Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true,
            "Queues": [
                {"Name": "q1", "Url": "https://x", "Region": "us-west-2"}
            ]
        }
    })");
    EXPECT_TRUE(cfg.enabled);
    ASSERT_EQ(cfg.queues.size(), 1u);
    EXPECT_EQ(cfg.queues[0].region, "us-west-2");
}

TEST(Configuration, ThrowsWhenEnabledButNoRegionAnywhere) {
    EXPECT_THROW(Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true,
            "Queues": [{"Name": "q", "Url": "https://x"}]
        }
    })"), std::runtime_error);
}

TEST(Configuration, ThrowsWhenQueueNameMissing) {
    EXPECT_THROW(Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true,
            "Region": "us-east-1",
            "Queues": [{"Url": "https://x"}]
        }
    })"), std::runtime_error);
}

TEST(Configuration, ThrowsWhenQueueUrlMissing) {
    EXPECT_THROW(Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true,
            "Region": "us-east-1",
            "Queues": [{"Name": "q"}]
        }
    })"), std::runtime_error);
}

TEST(Configuration, ThrowsWhenMaxMessagesOutOfRange) {
    EXPECT_THROW(Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true, "Region": "us-east-1",
            "Queues": [{"Name": "q", "Url": "https://x", "MaxMessages": 11}]
        }
    })"), std::runtime_error);
    EXPECT_THROW(Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true, "Region": "us-east-1",
            "Queues": [{"Name": "q", "Url": "https://x", "MaxMessages": 0}]
        }
    })"), std::runtime_error);
}

TEST(Configuration, ThrowsWhenWaitTimeOutOfRange) {
    EXPECT_THROW(Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true, "Region": "us-east-1",
            "Queues": [{"Name": "q", "Url": "https://x", "WaitTimeSeconds": 21}]
        }
    })"), std::runtime_error);
    EXPECT_THROW(Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true, "Region": "us-east-1",
            "Queues": [{"Name": "q", "Url": "https://x", "WaitTimeSeconds": -1}]
        }
    })"), std::runtime_error);
}

TEST(Configuration, ThrowsWhenVisibilityTimeoutNegative) {
    EXPECT_THROW(Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true, "Region": "us-east-1",
            "Queues": [{"Name": "q", "Url": "https://x", "VisibilityTimeoutSeconds": -5}]
        }
    })"), std::runtime_error);
}

TEST(Configuration, ThrowsWhenAwsSqsIsNotAnObject) {
    EXPECT_THROW(Configuration::Parse(R"({"AwsSqs": []})"), std::runtime_error);
}

TEST(Configuration, ThrowsWhenQueuesIsNotAnArray) {
    EXPECT_THROW(Configuration::Parse(R"({
        "AwsSqs": {"Enabled": true, "Region": "us-east-1", "Queues": {}}
    })"), std::runtime_error);
}

TEST(Configuration, AcceptsCustomTuningValues) {
    auto cfg = Configuration::Parse(R"({
        "AwsSqs": {
            "Enabled": true,
            "Region": "us-east-1",
            "Queues": [{
                "Name": "tuned",
                "Url":  "https://x",
                "WaitTimeSeconds": 10,
                "VisibilityTimeoutSeconds": 60,
                "MaxMessages": 5,
                "DeleteOnIngestFailure": true
            }]
        }
    })");
    ASSERT_EQ(cfg.queues.size(), 1u);
    const auto& q = cfg.queues[0];
    EXPECT_EQ(q.wait_time_seconds, 10);
    EXPECT_EQ(q.visibility_timeout_secs, 60);
    EXPECT_EQ(q.max_messages, 5);
    EXPECT_TRUE(q.delete_on_ingest_failure);
}
