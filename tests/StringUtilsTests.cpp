#include "util/StringUtils.h"

#include <gtest/gtest.h>

using namespace loganalyzer::util;

TEST(StringUtilsTest, TrimRemovesSurroundingWhitespace) {
    EXPECT_EQ(trim("  hello \r\n"), "hello");
    EXPECT_EQ(trim(""), "");
    EXPECT_EQ(trim(" \t "), "");
    EXPECT_EQ(trimLeft("  a "), "a ");
    EXPECT_EQ(trimRight("  a \r"), "  a");
}

TEST(StringUtilsTest, CaseConversionIsAsciiOnly) {
    EXPECT_EQ(toLower("ERROR Db-42"), "error db-42");
    EXPECT_EQ(toUpper("warning"), "WARNING");
}

TEST(StringUtilsTest, EqualsIgnoreCase) {
    EXPECT_TRUE(equalsIgnoreCase("Error", "ERROR"));
    EXPECT_FALSE(equalsIgnoreCase("Error", "Errors"));
    EXPECT_TRUE(equalsIgnoreCase("", ""));
}

TEST(StringUtilsTest, ContainsIgnoreCase) {
    EXPECT_TRUE(containsIgnoreCase("Database connection failed", "DATABASE"));
    EXPECT_TRUE(containsIgnoreCase("Database connection failed", "failed"));
    EXPECT_FALSE(containsIgnoreCase("Database connection failed", "timeout"));
    EXPECT_FALSE(containsIgnoreCase("db", "database"));
    EXPECT_TRUE(containsIgnoreCase("anything", ""));
    EXPECT_TRUE(containsIgnoreCase("", ""));
}

TEST(StringUtilsTest, SplitKeepsEmptyFields) {
    const auto parts = split("a,,b", ',');
    ASSERT_EQ(parts.size(), 3u);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "");
    EXPECT_EQ(parts[2], "b");
    EXPECT_EQ(split("single", ',').size(), 1u);
}

TEST(StringUtilsTest, ParseUnsignedRejectsGarbageAndOverflow) {
    EXPECT_EQ(parseUnsigned("42"), 42u);
    EXPECT_EQ(parseUnsigned("0"), 0u);
    EXPECT_FALSE(parseUnsigned("").has_value());
    EXPECT_FALSE(parseUnsigned("-1").has_value());
    EXPECT_FALSE(parseUnsigned("12abc").has_value());
    EXPECT_FALSE(parseUnsigned("99999999999999999999999").has_value());
}

TEST(StringUtilsTest, TruncateAddsEllipsis) {
    EXPECT_EQ(truncate("short", 10), "short");
    EXPECT_EQ(truncate("0123456789ABC", 10), "0123456...");
    EXPECT_EQ(truncate("abcdef", 2), "ab");
}

TEST(StringUtilsTest, FormatWithSeparators) {
    EXPECT_EQ(formatWithSeparators(0), "0");
    EXPECT_EQ(formatWithSeparators(7), "7");
    EXPECT_EQ(formatWithSeparators(80), "80");
    EXPECT_EQ(formatWithSeparators(999), "999");
    EXPECT_EQ(formatWithSeparators(12345), "12,345");
    EXPECT_EQ(formatWithSeparators(123456), "123,456");
    EXPECT_EQ(formatWithSeparators(1000), "1,000");
    EXPECT_EQ(formatWithSeparators(1234567), "1,234,567");
    EXPECT_EQ(formatWithSeparators(18446744073709551615ull), "18,446,744,073,709,551,615");
}
