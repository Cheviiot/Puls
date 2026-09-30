#include "puls/core/context.hpp"
#include "puls/core/error.hpp"
#include "puls/core/ip.hpp"
#include "puls/core/json.hpp"
#include "puls/core/text.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

namespace puls {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST(Context, BackgroundIsNeverDone) {
    const Context context;
    EXPECT_FALSE(context.done());
    EXPECT_FALSE(context.err());
    EXPECT_FALSE(context.deadline().has_value());
    EXPECT_FALSE(context.wait_for(1ms));
}

TEST(Context, CancelPropagatesToChildrenAndRunsCallbacksOnce) {
    CancelScope parent{Context()};
    CancelScope child(parent.context());
    std::atomic<int> calls{0};
    const auto registration = child.context().on_done([&calls] { calls.fetch_add(1); });
    parent.cancel();
    EXPECT_TRUE(child.context().done());
    EXPECT_TRUE(child.context().err().is(errors::canceled_tag));
    parent.cancel();
    child.cancel();
    EXPECT_EQ(calls.load(), 1);
}

TEST(Context, CancelingChildDoesNotCancelParent) {
    CancelScope parent{Context()};
    {
        CancelScope child(parent.context());
        child.cancel();
        EXPECT_TRUE(child.context().done());
    }
    EXPECT_FALSE(parent.context().done());
}

TEST(Context, TimeoutReportsDeadlineExceeded) {
    CancelScope scope(Context(), 30ms);
    const auto started = Clock::now();
    EXPECT_TRUE(scope.context().wait_for(2s));
    EXPECT_LT(Clock::now() - started, 1s);
    EXPECT_TRUE(scope.context().err().is(errors::deadline_exceeded_tag));
    scope.cancel();
    EXPECT_EQ(scope.context().cause(), CancelCause::deadline_exceeded);
}

TEST(Context, TimerRunsCallbacksWithoutWaiters) {
    CancelScope scope(Context(), 20ms);
    std::atomic<bool> called{false};
    const auto registration = scope.context().on_done([&called] { called.store(true); });
    const auto deadline = Clock::now() + 2s;
    while (!called.load() && Clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(called.load());
}

TEST(Context, ChildInheritsEarlierParentDeadline) {
    CancelScope parent(Context(), 50ms);
    CancelScope child(parent.context(), 10s);
    ASSERT_TRUE(child.context().deadline().has_value());
    EXPECT_EQ(*child.context().deadline(), *parent.context().deadline());
    EXPECT_TRUE(child.context().wait_for(2s));
    EXPECT_TRUE(child.context().err().is(errors::deadline_exceeded_tag));
}

TEST(Context, OnDoneRunsImmediatelyForFinishedContext) {
    CancelScope scope{Context()};
    scope.cancel();
    bool called = false;
    const auto registration = scope.context().on_done([&called] { called = true; });
    EXPECT_TRUE(called);
}

TEST(Context, UnregisteredCallbackIsNotInvoked) {
    CancelScope scope{Context()};
    bool called = false;
    {
        const auto registration = scope.context().on_done([&called] { called = true; });
    }
    scope.cancel();
    EXPECT_FALSE(called);
}

TEST(Context, UnregisterWaitsForRunningCallback) {
    CancelScope scope{Context()};
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    auto registration = scope.context().on_done([&] {
        entered.store(true);
        std::this_thread::sleep_for(50ms);
        finished.store(true);
    });
    std::thread canceler([&scope] { scope.cancel(); });
    while (!entered.load()) {
        std::this_thread::yield();
    }
    registration.reset();
    EXPECT_TRUE(finished.load());
    canceler.join();
}

TEST(Context, WaitReturnsPromptlyOnCancel) {
    CancelScope scope{Context()};
    std::thread canceler([&scope] {
        std::this_thread::sleep_for(20ms);
        scope.cancel();
    });
    const auto started = Clock::now();
    EXPECT_TRUE(scope.context().wait_for(5s));
    EXPECT_LT(Clock::now() - started, 1s);
    canceler.join();
}

TEST(Error, RendersLikeGoWrapping) {
    const Error cause = Error::make("bad frame");
    const Error wrapped = Error::wrap("получение", cause);
    EXPECT_EQ(wrapped.message(), "получение: bad frame");
    EXPECT_TRUE(wrapped.is(cause));
    EXPECT_FALSE(wrapped.is(Error::make("bad frame")));

    const Error joined = Error::join({Error::make("a"), Error(), Error::make("b")});
    EXPECT_EQ(joined.message(), "a\nb");
    EXPECT_FALSE(Error::join(std::vector<Error>{Error(), Error()}));

    const Error chained = Error::chain(errors::canceled(), joined);
    EXPECT_EQ(chained.message(), "context canceled: a\nb");
    EXPECT_TRUE(chained.is(errors::canceled_tag));
}

TEST(Error, AsFindsFirstDetailInPreOrder) {
    const Error inner = Error::with_detail(std::make_shared<NetworkError>(true), "timeout");
    const Error outer = Error::with_detail(std::make_shared<NetworkError>(false), "outer",
                                           Error::wrap("middle", inner));
    ASSERT_NE(outer.as<NetworkError>(), nullptr);
    EXPECT_FALSE(outer.as<NetworkError>()->timeout);
    EXPECT_TRUE(Error::wrap("x", inner).as<NetworkError>()->timeout);
    EXPECT_EQ(Error::make("plain").as<NetworkError>(), nullptr);
}

TEST(Result, HoldsValueOrError) {
    Result<int> value = 7;
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 7);
    Result<int> failure = Error::make("broken");
    ASSERT_FALSE(failure);
    EXPECT_EQ(failure.error().message(), "broken");
}

TEST(Text, QuoteMatchesStrconv) {
    EXPECT_EQ(text::quote("abc"), "\"abc\"");
    EXPECT_EQ(text::quote("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(text::quote("line\nnext\t"), "\"line\\nnext\\t\"");
    EXPECT_EQ(text::quote("Москва"), "\"Москва\"");
    EXPECT_EQ(text::quote(std::string("\x01\x7f", 2)), "\"\\x01\\x7f\"");
    EXPECT_EQ(text::quote("\xff"), "\"\\xff\"");
    EXPECT_EQ(text::quote("\xc2\xa0"), "\"\\u00a0\"");
    EXPECT_EQ(text::quote("\xe2\x80\xa8"), "\"\\u2028\"");
}

TEST(Text, TrimAndFieldsUseUnicodeSpace) {
    EXPECT_EQ(text::trim_space(" \t\u00a0value\u3000\n"), "value");
    EXPECT_EQ(text::trim_space("   "), "");
    const auto parts = text::fields(" PONG\u00a0 123 ");
    ASSERT_EQ(parts.size(), 2u);
    EXPECT_EQ(parts[0], "PONG");
    EXPECT_EQ(parts[1], "123");
}

TEST(Text, PadRightCountsRunes) {
    EXPECT_EQ(text::pad_right("abc", 6), "abc   ");
    EXPECT_EQ(text::pad_right("abcdefgh", 6), "abcdefgh");
    EXPECT_EQ(text::pad_right("Пинг", 10), "Пинг      ");
}

TEST(Text, FormatsDurationsLikeGo) {
    EXPECT_EQ(text::format_duration(0ns), "0s");
    EXPECT_EQ(text::format_duration(5s), "5s");
    EXPECT_EQ(text::format_duration(1500ms), "1.5s");
    EXPECT_EQ(text::format_duration(300ms), "300ms");
    EXPECT_EQ(text::format_duration(1500us), "1.5ms");
    EXPECT_EQ(text::format_duration(12us), "12µs");
    EXPECT_EQ(text::format_duration(90s), "1m30s");
    EXPECT_EQ(text::format_duration(3600s), "1h0m0s");
    EXPECT_EQ(text::format_duration(-2s), "-2s");
}

TEST(Text, FormatsNumbersLikeEncodingJSON) {
    EXPECT_EQ(text::format_json_number(95.0), "95");
    EXPECT_EQ(text::format_json_number(12.34), "12.34");
    EXPECT_EQ(text::format_json_number(0.1 + 0.2), "0.30000000000000004");
    EXPECT_EQ(text::format_json_number(-0.0), "-0");
    EXPECT_EQ(text::format_json_number(1e21), "1e+21");
    EXPECT_EQ(text::format_json_number(1.5e-7), "1.5e-7");
    EXPECT_EQ(text::format_json_number(1e20), "100000000000000000000");
    EXPECT_EQ(text::format_fixed(87.5678, 2), "87.57");
    EXPECT_EQ(text::format_fixed(11.0, 1), "11.0");
}

TEST(Text, ParseIntFollowsStrconv) {
    EXPECT_EQ(text::parse_int("16", 0).value, 16);
    EXPECT_EQ(text::parse_int("0x10", 0).value, 16);
    EXPECT_EQ(text::parse_int("010", 0).value, 8);
    EXPECT_EQ(text::parse_int("0o17", 0).value, 15);
    EXPECT_EQ(text::parse_int("0b101", 0).value, 5);
    EXPECT_EQ(text::parse_int("1_000", 0).value, 1000);
    EXPECT_EQ(text::parse_int("-0", 0).value, 0);
    EXPECT_EQ(text::parse_int("0", 0).value, 0);
    EXPECT_FALSE(text::parse_int("1__0", 0).value);
    EXPECT_FALSE(text::parse_int("_1", 0).value);
    EXPECT_FALSE(text::parse_int("0x", 0).value);
    EXPECT_FALSE(text::parse_int("", 0).value);
    EXPECT_FALSE(text::parse_int("1_000", 10).value);
    const auto overflow = text::parse_int("9223372036854775808", 10);
    EXPECT_FALSE(overflow.value);
    EXPECT_EQ(overflow.error, text::ParseIntError::range);
    EXPECT_EQ(text::parse_int("-9223372036854775808", 10).value,
              std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(text::atoi("+10").value, 10);
    EXPECT_EQ(text::atoi("010").value, 10);
    EXPECT_FALSE(text::atoi("1.5").value);
    EXPECT_EQ(text::format_int(35, 36), "z");
    EXPECT_EQ(text::format_int(-255, 16), "-ff");
}

TEST(IpAddress, ParsesAndFormatsCanonically) {
    EXPECT_EQ(IpAddress::parse("203.0.113.7")->to_string(), "203.0.113.7");
    EXPECT_EQ(IpAddress::parse("2001:DB8:0:0:0:0:0:1")->to_string(), "2001:db8::1");
    EXPECT_EQ(IpAddress::parse("2001:db8:0:0:1:0:0:1")->to_string(), "2001:db8::1:0:0:1");
    EXPECT_EQ(IpAddress::parse("::")->to_string(), "::");
    EXPECT_EQ(IpAddress::parse("::1")->to_string(), "::1");
    EXPECT_EQ(IpAddress::parse("1::")->to_string(), "1::");
    EXPECT_EQ(IpAddress::parse("::ffff:1.2.3.4")->unmap().to_string(), "1.2.3.4");
    EXPECT_TRUE(IpAddress::parse("::ffff:1.2.3.4")->unmap().is_v4());
    EXPECT_EQ(IpAddress::parse("fe80::1%eth0")->zone(), "eth0");
    for (const char* invalid : {"", "not-an-ip", "01.2.3.4", "1.2.3", "1.2.3.4.5", "256.1.1.1",
                                "1::2::3", "12345::1", "::1%", "%eth0", "1.2.3.4:80"}) {
        EXPECT_FALSE(IpAddress::parse(invalid).has_value()) << invalid;
    }
    EXPECT_EQ(*IpAddress::parse("::ffff:203.0.113.1"), *IpAddress::parse("::ffff:cb00:7101"));
}

TEST(Json, EncodesLikeGoEncoder) {
    boost::json::object connection;
    connection["status"] = "ok";
    connection["warnings"] = boost::json::array();
    connection["error"] = nullptr;
    boost::json::object root;
    root["schema_version"] = 1;
    root["connection"] = std::move(connection);
    root["value"] = 11.0;
    root["jitter"] = 1.5;
    root["results"] = boost::json::array();
    root["nested"] = boost::json::array{1, "x"};
    const auto encoded = json::encode_indented(root);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(*encoded, "{\n"
                        "  \"schema_version\": 1,\n"
                        "  \"connection\": {\n"
                        "    \"status\": \"ok\",\n"
                        "    \"warnings\": [],\n"
                        "    \"error\": null\n"
                        "  },\n"
                        "  \"value\": 11,\n"
                        "  \"jitter\": 1.5,\n"
                        "  \"results\": [],\n"
                        "  \"nested\": [\n"
                        "    1,\n"
                        "    \"x\"\n"
                        "  ]\n"
                        "}\n");
}

TEST(Json, EscapesStringsLikeEncodingJSON) {
    std::string output;
    json::append_string(output, "<a&b>\"\\\n\t\b\f\x01\xe2\x80\xa8\xff Ростелеком");
    EXPECT_EQ(output,
              "\"\\u003ca\\u0026b\\u003e\\\"\\\\\\n\\t\\b\\f\\u0001\\u2028\\ufffd Ростелеком\"");
    EXPECT_FALSE(json::encode_indented(boost::json::value(std::nan(""))));
}

TEST(Json, ParseRequiresExactlyOneValue) {
    EXPECT_TRUE(json::parse(" {\"value\":7} \n"));
    EXPECT_FALSE(json::parse("{\"value\":7}{}"));
    EXPECT_FALSE(json::parse("{\"value\":7} x"));
    EXPECT_FALSE(json::parse("{"));
    EXPECT_FALSE(json::parse(""));
}

TEST(Json, ObjectReaderUsesStructSemantics) {
    const auto value = json::parse(
        R"({"name":"x","count":5,"big":1.5,"null":null,"items":[{"id":1},null],"obj":{"k":"v"}})");
    ASSERT_TRUE(value);
    const auto reader = json::ObjectReader::from(*value);
    ASSERT_TRUE(reader);
    EXPECT_EQ(*reader->string("name"), "x");
    EXPECT_EQ(*reader->string("missing"), "");
    EXPECT_EQ(*reader->string("null"), "");
    EXPECT_FALSE(reader->string("count"));
    EXPECT_EQ(*reader->integer("count"), 5);
    EXPECT_FALSE(reader->integer("big"));
    EXPECT_EQ(*reader->integer("missing"), 0);
    EXPECT_FALSE(reader->optional_integer("null")->has_value());
    const auto items = reader->objects("items");
    ASSERT_TRUE(items);
    ASSERT_EQ(items->size(), 2u);
    EXPECT_EQ(*(*items)[0].integer("id"), 1);
    EXPECT_EQ(*(*items)[1].integer("id"), 0);
    EXPECT_EQ(*reader->object("obj")->string("k"), "v");
    EXPECT_EQ(*reader->object("missing")->string("k"), "");
    EXPECT_FALSE(reader->objects("name"));
    EXPECT_FALSE(json::ObjectReader::from(boost::json::value(1)));
    EXPECT_TRUE(json::ObjectReader::from(boost::json::value(nullptr)));
}

} // namespace
} // namespace puls
