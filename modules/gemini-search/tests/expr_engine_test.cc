#include <gtest/gtest.h>
#include "expr_engine.h"

#include <cmath>
#include <string>
#include <unordered_map>

using Row = std::unordered_map<std::string, std::string>;

static ExprValue Eval(const std::string& expr, const Row& row = {}) {
  ExprNode node;
  std::string err;
  EXPECT_TRUE(ParseExpr(expr, node, err)) << err;
  return EvalExpr(node, row);
}

static bool ParseFails(const std::string& expr) {
  ExprNode node;
  std::string err;
  return !ParseExpr(expr, node, err);
}

// =============================================================
// Arithmetic
// =============================================================

TEST(ExprTest, Literals) {
  EXPECT_DOUBLE_EQ(Eval("42").AsNumber(), 42.0);
  EXPECT_DOUBLE_EQ(Eval("3.14").AsNumber(), 3.14);
  EXPECT_DOUBLE_EQ(Eval("-5").AsNumber(), -5.0);
}

TEST(ExprTest, StringLiteral) {
  EXPECT_EQ(Eval("\"hello\"").AsString(), "hello");
  EXPECT_EQ(Eval("'world'").AsString(), "world");
}

TEST(ExprTest, Addition) {
  EXPECT_DOUBLE_EQ(Eval("2 + 3").AsNumber(), 5.0);
}

TEST(ExprTest, Subtraction) {
  EXPECT_DOUBLE_EQ(Eval("10 - 4").AsNumber(), 6.0);
}

TEST(ExprTest, Multiplication) {
  EXPECT_DOUBLE_EQ(Eval("3 * 7").AsNumber(), 21.0);
}

TEST(ExprTest, Division) {
  EXPECT_DOUBLE_EQ(Eval("15 / 3").AsNumber(), 5.0);
  EXPECT_DOUBLE_EQ(Eval("1 / 0").AsNumber(), 0.0);
}

TEST(ExprTest, Modulo) {
  EXPECT_DOUBLE_EQ(Eval("10 % 3").AsNumber(), 1.0);
  EXPECT_DOUBLE_EQ(Eval("10 % 0").AsNumber(), 0.0);
}

TEST(ExprTest, Power) {
  EXPECT_DOUBLE_EQ(Eval("2 ^ 10").AsNumber(), 1024.0);
}

TEST(ExprTest, Precedence) {
  EXPECT_DOUBLE_EQ(Eval("2 + 3 * 4").AsNumber(), 14.0);
  EXPECT_DOUBLE_EQ(Eval("(2 + 3) * 4").AsNumber(), 20.0);
}

TEST(ExprTest, NestedParens) {
  EXPECT_DOUBLE_EQ(Eval("((1 + 2) * (3 + 4))").AsNumber(), 21.0);
}

// =============================================================
// Comparison
// =============================================================

TEST(ExprTest, Eq) {
  EXPECT_DOUBLE_EQ(Eval("5 == 5").AsNumber(), 1.0);
  EXPECT_DOUBLE_EQ(Eval("5 == 6").AsNumber(), 0.0);
}

TEST(ExprTest, Ne) {
  EXPECT_DOUBLE_EQ(Eval("5 != 6").AsNumber(), 1.0);
  EXPECT_DOUBLE_EQ(Eval("5 != 5").AsNumber(), 0.0);
}

TEST(ExprTest, LtGt) {
  EXPECT_DOUBLE_EQ(Eval("3 < 5").AsNumber(), 1.0);
  EXPECT_DOUBLE_EQ(Eval("5 < 3").AsNumber(), 0.0);
  EXPECT_DOUBLE_EQ(Eval("5 > 3").AsNumber(), 1.0);
  EXPECT_DOUBLE_EQ(Eval("3 > 5").AsNumber(), 0.0);
}

TEST(ExprTest, LeGe) {
  EXPECT_DOUBLE_EQ(Eval("3 <= 3").AsNumber(), 1.0);
  EXPECT_DOUBLE_EQ(Eval("3 <= 2").AsNumber(), 0.0);
  EXPECT_DOUBLE_EQ(Eval("3 >= 3").AsNumber(), 1.0);
  EXPECT_DOUBLE_EQ(Eval("4 >= 5").AsNumber(), 0.0);
}

// =============================================================
// Logical
// =============================================================

TEST(ExprTest, And) {
  EXPECT_DOUBLE_EQ(Eval("1 && 1").AsNumber(), 1.0);
  EXPECT_DOUBLE_EQ(Eval("1 && 0").AsNumber(), 0.0);
  EXPECT_DOUBLE_EQ(Eval("0 && 1").AsNumber(), 0.0);
}

TEST(ExprTest, Or) {
  EXPECT_DOUBLE_EQ(Eval("0 || 1").AsNumber(), 1.0);
  EXPECT_DOUBLE_EQ(Eval("0 || 0").AsNumber(), 0.0);
}

TEST(ExprTest, Not) {
  EXPECT_DOUBLE_EQ(Eval("!0").AsNumber(), 1.0);
  EXPECT_DOUBLE_EQ(Eval("!1").AsNumber(), 0.0);
  EXPECT_DOUBLE_EQ(Eval("!5").AsNumber(), 0.0);
}

// =============================================================
// Field references
// =============================================================

TEST(ExprTest, FieldRef) {
  Row row = {{"price", "100"}, {"name", "widget"}};
  EXPECT_DOUBLE_EQ(Eval("@price", row).AsNumber(), 100.0);
  EXPECT_EQ(Eval("@name", row).AsString(), "widget");
}

TEST(ExprTest, FieldArithmetic) {
  Row row = {{"price", "100"}, {"tax", "0.1"}};
  EXPECT_DOUBLE_EQ(Eval("@price * (1 + @tax)", row).AsNumber(), 110.0);
}

TEST(ExprTest, MissingFieldReturnsEmpty) {
  Row row;
  EXPECT_EQ(Eval("@missing", row).AsString(), "");
  EXPECT_DOUBLE_EQ(Eval("@missing", row).AsNumber(), 0.0);
}

// =============================================================
// Math functions
// =============================================================

TEST(ExprTest, Sqrt) {
  EXPECT_DOUBLE_EQ(Eval("sqrt(16)").AsNumber(), 4.0);
}

TEST(ExprTest, Log) {
  EXPECT_NEAR(Eval("log(2.718281828)").AsNumber(), 1.0, 0.001);
}

TEST(ExprTest, Log2) {
  EXPECT_DOUBLE_EQ(Eval("log2(8)").AsNumber(), 3.0);
}

TEST(ExprTest, Abs) {
  EXPECT_DOUBLE_EQ(Eval("abs(-5)").AsNumber(), 5.0);
  EXPECT_DOUBLE_EQ(Eval("abs(5)").AsNumber(), 5.0);
}

TEST(ExprTest, CeilFloorRound) {
  EXPECT_DOUBLE_EQ(Eval("ceil(3.2)").AsNumber(), 4.0);
  EXPECT_DOUBLE_EQ(Eval("floor(3.8)").AsNumber(), 3.0);
  EXPECT_DOUBLE_EQ(Eval("round(3.5)").AsNumber(), 4.0);
  EXPECT_DOUBLE_EQ(Eval("round(3.4)").AsNumber(), 3.0);
}

// =============================================================
// String functions
// =============================================================

TEST(ExprTest, Lower) {
  EXPECT_EQ(Eval("lower(\"HELLO\")").AsString(), "hello");
}

TEST(ExprTest, Upper) {
  EXPECT_EQ(Eval("upper(\"hello\")").AsString(), "HELLO");
}

TEST(ExprTest, Strlen) {
  EXPECT_DOUBLE_EQ(Eval("strlen(\"hello\")").AsNumber(), 5.0);
}

TEST(ExprTest, Substr) {
  EXPECT_EQ(Eval("substr(\"hello world\", 6, 5)").AsString(), "world");
  EXPECT_EQ(Eval("substr(\"hello\", 0, 3)").AsString(), "hel");
  EXPECT_EQ(Eval("substr(\"hello\", 10, 5)").AsString(), "");
}

TEST(ExprTest, Format) {
  Row row = {{"name", "Widget"}, {"price", "9.99"}};
  EXPECT_EQ(Eval("format(\"%s costs $%s\", @name, @price)", row).AsString(),
            "Widget costs $9.99");
}

// =============================================================
// Conditional
// =============================================================

TEST(ExprTest, IfTrue) {
  EXPECT_DOUBLE_EQ(Eval("if(1, 10, 20)").AsNumber(), 10.0);
}

TEST(ExprTest, IfFalse) {
  EXPECT_DOUBLE_EQ(Eval("if(0, 10, 20)").AsNumber(), 20.0);
}

TEST(ExprTest, IfWithComparison) {
  Row row = {{"price", "150"}};
  EXPECT_EQ(Eval("if(@price > 100, \"expensive\", \"cheap\")", row).AsString(), "expensive");
}

// =============================================================
// Complex expressions
// =============================================================

TEST(ExprTest, ComplexExpression) {
  Row row = {{"qty", "5"}, {"unit_price", "20"}, {"discount", "0.1"}};
  auto val = Eval("@qty * @unit_price * (1 - @discount)", row);
  EXPECT_DOUBLE_EQ(val.AsNumber(), 90.0);
}

TEST(ExprTest, NestedFunctions) {
  EXPECT_DOUBLE_EQ(Eval("abs(floor(-3.7))").AsNumber(), 4.0);
}

// =============================================================
// ExprValue
// =============================================================

TEST(ExprValueTest, NumAsString) {
  EXPECT_EQ(ExprValue::Num(42.0).AsString(), "42");
  EXPECT_EQ(ExprValue::Num(0.0).AsString(), "0");
}

TEST(ExprValueTest, StringAsNumber) {
  EXPECT_DOUBLE_EQ(ExprValue::Str("3.14").AsNumber(), 3.14);
  EXPECT_DOUBLE_EQ(ExprValue::Str("abc").AsNumber(), 0.0);
}

TEST(ExprValueTest, Truthy) {
  EXPECT_TRUE(ExprValue::Num(1.0).Truthy());
  EXPECT_FALSE(ExprValue::Num(0.0).Truthy());
  EXPECT_TRUE(ExprValue::Str("hello").Truthy());
  EXPECT_FALSE(ExprValue::Str("").Truthy());
}

// =============================================================
// Parse errors
// =============================================================

TEST(ExprParseTest, EmptyExpr) {
  EXPECT_TRUE(ParseFails(""));
  EXPECT_TRUE(ParseFails("   "));
}

TEST(ExprParseTest, UnclosedParen) {
  EXPECT_TRUE(ParseFails("(1 + 2"));
}

TEST(ExprParseTest, UnknownFunction) {
  EXPECT_TRUE(ParseFails("foobar(1)"));
}

TEST(ExprParseTest, WrongArgCount) {
  EXPECT_TRUE(ParseFails("sqrt(1, 2)"));
  EXPECT_TRUE(ParseFails("if(1, 2)"));
  EXPECT_TRUE(ParseFails("substr(\"a\", 1)"));
}

TEST(ExprParseTest, TrailingInput) {
  EXPECT_TRUE(ParseFails("1 + 2 3"));
}
