#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct ExprValue {
  enum class Type { kNumber, kString };
  Type type = Type::kNumber;
  double num = 0.0;
  std::string str;

  static ExprValue Num(double v) { return {Type::kNumber, v, {}}; }
  static ExprValue Str(std::string s) { return {Type::kString, 0.0, std::move(s)}; }

  double AsNumber() const;
  std::string AsString() const;
  bool Truthy() const;
};

struct ExprNode {
  enum class Op {
    kLitNum, kLitStr, kFieldRef,
    kAdd, kSub, kMul, kDiv, kMod, kPow,
    kEq, kNe, kLt, kGt, kLe, kGe,
    kAnd, kOr, kNot,
    kFnSqrt, kFnLog, kFnLog2, kFnAbs, kFnCeil, kFnFloor, kFnRound,
    kFnLower, kFnUpper, kFnStrlen, kFnSubstr, kFnFormat,
    kFnIf,
  };

  Op op = Op::kLitNum;
  double num_val = 0.0;
  std::string str_val;
  std::vector<ExprNode> children;
};

using ExprRow = std::unordered_map<std::string, std::string>;

bool ParseExpr(const std::string& input, ExprNode& out, std::string& error);

ExprValue EvalExpr(const ExprNode& node, const ExprRow& row);
