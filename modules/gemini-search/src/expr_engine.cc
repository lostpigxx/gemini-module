#include "expr_engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

double ExprValue::AsNumber() const {
  if (type == Type::kNumber) return num;
  char* ep = nullptr;
  double v = std::strtod(str.c_str(), &ep);
  if (ep == str.c_str()) return 0.0;
  return v;
}

std::string ExprValue::AsString() const {
  if (type == Type::kString) return str;
  if (num == static_cast<long long>(num) && std::isfinite(num)) {
    return std::to_string(static_cast<long long>(num));
  }
  return std::to_string(num);
}

bool ExprValue::Truthy() const {
  if (type == Type::kNumber) return num != 0.0;
  return !str.empty();
}

// =============================================================
// Evaluator
// =============================================================

ExprValue EvalExpr(const ExprNode& node, const ExprRow& row) {
  switch (node.op) {
    case ExprNode::Op::kLitNum:
      return ExprValue::Num(node.num_val);
    case ExprNode::Op::kLitStr:
      return ExprValue::Str(node.str_val);
    case ExprNode::Op::kFieldRef: {
      auto it = row.find(node.str_val);
      if (it == row.end()) return ExprValue::Str("");
      return ExprValue::Str(it->second);
    }

    case ExprNode::Op::kAdd: {
      double a = EvalExpr(node.children[0], row).AsNumber();
      double b = EvalExpr(node.children[1], row).AsNumber();
      return ExprValue::Num(a + b);
    }
    case ExprNode::Op::kSub: {
      double a = EvalExpr(node.children[0], row).AsNumber();
      double b = EvalExpr(node.children[1], row).AsNumber();
      return ExprValue::Num(a - b);
    }
    case ExprNode::Op::kMul: {
      double a = EvalExpr(node.children[0], row).AsNumber();
      double b = EvalExpr(node.children[1], row).AsNumber();
      return ExprValue::Num(a * b);
    }
    case ExprNode::Op::kDiv: {
      double a = EvalExpr(node.children[0], row).AsNumber();
      double b = EvalExpr(node.children[1], row).AsNumber();
      if (b == 0.0) return ExprValue::Num(0.0);
      return ExprValue::Num(a / b);
    }
    case ExprNode::Op::kMod: {
      double a = EvalExpr(node.children[0], row).AsNumber();
      double b = EvalExpr(node.children[1], row).AsNumber();
      if (b == 0.0) return ExprValue::Num(0.0);
      return ExprValue::Num(std::fmod(a, b));
    }
    case ExprNode::Op::kPow: {
      double a = EvalExpr(node.children[0], row).AsNumber();
      double b = EvalExpr(node.children[1], row).AsNumber();
      return ExprValue::Num(std::pow(a, b));
    }

    case ExprNode::Op::kEq: {
      auto a = EvalExpr(node.children[0], row);
      auto b = EvalExpr(node.children[1], row);
      if (a.type == ExprValue::Type::kNumber && b.type == ExprValue::Type::kNumber)
        return ExprValue::Num(a.num == b.num ? 1.0 : 0.0);
      return ExprValue::Num(a.AsString() == b.AsString() ? 1.0 : 0.0);
    }
    case ExprNode::Op::kNe: {
      auto a = EvalExpr(node.children[0], row);
      auto b = EvalExpr(node.children[1], row);
      if (a.type == ExprValue::Type::kNumber && b.type == ExprValue::Type::kNumber)
        return ExprValue::Num(a.num != b.num ? 1.0 : 0.0);
      return ExprValue::Num(a.AsString() != b.AsString() ? 1.0 : 0.0);
    }
    case ExprNode::Op::kLt:
      return ExprValue::Num(
          EvalExpr(node.children[0], row).AsNumber() <
          EvalExpr(node.children[1], row).AsNumber() ? 1.0 : 0.0);
    case ExprNode::Op::kGt:
      return ExprValue::Num(
          EvalExpr(node.children[0], row).AsNumber() >
          EvalExpr(node.children[1], row).AsNumber() ? 1.0 : 0.0);
    case ExprNode::Op::kLe:
      return ExprValue::Num(
          EvalExpr(node.children[0], row).AsNumber() <=
          EvalExpr(node.children[1], row).AsNumber() ? 1.0 : 0.0);
    case ExprNode::Op::kGe:
      return ExprValue::Num(
          EvalExpr(node.children[0], row).AsNumber() >=
          EvalExpr(node.children[1], row).AsNumber() ? 1.0 : 0.0);

    case ExprNode::Op::kAnd:
      return ExprValue::Num(
          EvalExpr(node.children[0], row).Truthy() &&
          EvalExpr(node.children[1], row).Truthy() ? 1.0 : 0.0);
    case ExprNode::Op::kOr:
      return ExprValue::Num(
          EvalExpr(node.children[0], row).Truthy() ||
          EvalExpr(node.children[1], row).Truthy() ? 1.0 : 0.0);
    case ExprNode::Op::kNot:
      return ExprValue::Num(EvalExpr(node.children[0], row).Truthy() ? 0.0 : 1.0);

    case ExprNode::Op::kFnSqrt:
      return ExprValue::Num(std::sqrt(EvalExpr(node.children[0], row).AsNumber()));
    case ExprNode::Op::kFnLog:
      return ExprValue::Num(std::log(EvalExpr(node.children[0], row).AsNumber()));
    case ExprNode::Op::kFnLog2:
      return ExprValue::Num(std::log2(EvalExpr(node.children[0], row).AsNumber()));
    case ExprNode::Op::kFnAbs:
      return ExprValue::Num(std::fabs(EvalExpr(node.children[0], row).AsNumber()));
    case ExprNode::Op::kFnCeil:
      return ExprValue::Num(std::ceil(EvalExpr(node.children[0], row).AsNumber()));
    case ExprNode::Op::kFnFloor:
      return ExprValue::Num(std::floor(EvalExpr(node.children[0], row).AsNumber()));
    case ExprNode::Op::kFnRound:
      return ExprValue::Num(std::round(EvalExpr(node.children[0], row).AsNumber()));

    case ExprNode::Op::kFnLower: {
      auto s = EvalExpr(node.children[0], row).AsString();
      for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return ExprValue::Str(std::move(s));
    }
    case ExprNode::Op::kFnUpper: {
      auto s = EvalExpr(node.children[0], row).AsString();
      for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return ExprValue::Str(std::move(s));
    }
    case ExprNode::Op::kFnStrlen:
      return ExprValue::Num(
          static_cast<double>(EvalExpr(node.children[0], row).AsString().size()));
    case ExprNode::Op::kFnSubstr: {
      auto s = EvalExpr(node.children[0], row).AsString();
      auto off = static_cast<size_t>(std::max(0.0, EvalExpr(node.children[1], row).AsNumber()));
      auto len = static_cast<size_t>(std::max(0.0, EvalExpr(node.children[2], row).AsNumber()));
      if (off >= s.size()) return ExprValue::Str("");
      return ExprValue::Str(s.substr(off, len));
    }
    case ExprNode::Op::kFnFormat: {
      // format("%s costs $%s", @name, @price) → simple %s substitution
      auto fmt = EvalExpr(node.children[0], row).AsString();
      std::string result;
      size_t ci = 1;
      for (size_t fi = 0; fi < fmt.size(); fi++) {
        if (fmt[fi] == '%' && fi + 1 < fmt.size() && fmt[fi + 1] == 's') {
          if (ci < node.children.size()) {
            result += EvalExpr(node.children[ci], row).AsString();
            ci++;
          }
          fi++;
        } else {
          result += fmt[fi];
        }
      }
      return ExprValue::Str(std::move(result));
    }
    case ExprNode::Op::kFnIf: {
      if (EvalExpr(node.children[0], row).Truthy())
        return EvalExpr(node.children[1], row);
      return EvalExpr(node.children[2], row);
    }
  }
  return ExprValue::Num(0.0);
}

// =============================================================
// Recursive descent expression parser
// =============================================================

struct ExprParser {
  const std::string& input;
  size_t pos = 0;
  std::string& error;

  ExprParser(const std::string& s, std::string& err) : input(s), error(err) {}

  char Peek() const { return pos < input.size() ? input[pos] : '\0'; }
  char Advance() { return pos < input.size() ? input[pos++] : '\0'; }

  void SkipSpaces() {
    while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t')) pos++;
  }

  bool MatchStr(const char* s) {
    size_t len = std::strlen(s);
    if (pos + len > input.size()) return false;
    if (strncasecmp(input.c_str() + pos, s, len) != 0) return false;
    if (pos + len < input.size() && std::isalnum(static_cast<unsigned char>(input[pos + len])))
      return false;
    pos += len;
    return true;
  }

  // primary: number | string | @field | func(...) | (expr) | !primary
  bool ParsePrimary(ExprNode& out) {
    SkipSpaces();
    char c = Peek();

    if (c == '!') {
      pos++;
      ExprNode child;
      if (!ParsePrimary(child)) return false;
      out.op = ExprNode::Op::kNot;
      out.children.push_back(std::move(child));
      return true;
    }

    if (c == '(') {
      pos++;
      if (!ParseOr(out)) return false;
      SkipSpaces();
      if (Peek() != ')') {
        error = "ERR expr: expected )";
        return false;
      }
      pos++;
      return true;
    }

    if (c == '@') {
      pos++;
      size_t start = pos;
      while (pos < input.size() &&
             (std::isalnum(static_cast<unsigned char>(input[pos])) || input[pos] == '_'))
        pos++;
      if (pos == start) {
        error = "ERR expr: empty field name";
        return false;
      }
      out.op = ExprNode::Op::kFieldRef;
      out.str_val = input.substr(start, pos - start);
      return true;
    }

    if (c == '"' || c == '\'') {
      char quote = c;
      pos++;
      size_t start = pos;
      while (pos < input.size() && input[pos] != quote) pos++;
      if (pos >= input.size()) {
        error = "ERR expr: unterminated string";
        return false;
      }
      out.op = ExprNode::Op::kLitStr;
      out.str_val = input.substr(start, pos - start);
      pos++;
      return true;
    }

    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
      char* ep = nullptr;
      double v = std::strtod(input.c_str() + pos, &ep);
      if (ep == input.c_str() + pos) {
        error = "ERR expr: invalid number";
        return false;
      }
      out.op = ExprNode::Op::kLitNum;
      out.num_val = v;
      pos = static_cast<size_t>(ep - input.c_str());
      return true;
    }

    if (c == '-') {
      pos++;
      SkipSpaces();
      if (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '.') {
        char* ep = nullptr;
        double v = std::strtod(input.c_str() + pos, &ep);
        if (ep == input.c_str() + pos) {
          error = "ERR expr: invalid number";
          return false;
        }
        out.op = ExprNode::Op::kLitNum;
        out.num_val = -v;
        pos = static_cast<size_t>(ep - input.c_str());
        return true;
      }
      ExprNode child;
      if (!ParsePrimary(child)) return false;
      out.op = ExprNode::Op::kSub;
      ExprNode zero;
      zero.op = ExprNode::Op::kLitNum;
      zero.num_val = 0;
      out.children.push_back(std::move(zero));
      out.children.push_back(std::move(child));
      return true;
    }

    // function name
    if (std::isalpha(static_cast<unsigned char>(c))) {
      size_t start = pos;
      while (pos < input.size() &&
             (std::isalnum(static_cast<unsigned char>(input[pos])) || input[pos] == '_'))
        pos++;
      std::string name = input.substr(start, pos - start);
      for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

      SkipSpaces();
      if (Peek() != '(') {
        error = "ERR expr: expected ( after function name '" + name + "'";
        return false;
      }
      pos++;

      std::vector<ExprNode> args;
      SkipSpaces();
      if (Peek() != ')') {
        ExprNode arg;
        if (!ParseOr(arg)) return false;
        args.push_back(std::move(arg));
        while (true) {
          SkipSpaces();
          if (Peek() == ')') break;
          if (Peek() != ',') {
            error = "ERR expr: expected , or )";
            return false;
          }
          pos++;
          ExprNode next;
          if (!ParseOr(next)) return false;
          args.push_back(std::move(next));
        }
      }
      pos++; // skip )

      struct FnEntry { const char* name; ExprNode::Op op; int min_args; int max_args; };
      static const FnEntry fns[] = {
        {"sqrt", ExprNode::Op::kFnSqrt, 1, 1},
        {"log", ExprNode::Op::kFnLog, 1, 1},
        {"log2", ExprNode::Op::kFnLog2, 1, 1},
        {"abs", ExprNode::Op::kFnAbs, 1, 1},
        {"ceil", ExprNode::Op::kFnCeil, 1, 1},
        {"floor", ExprNode::Op::kFnFloor, 1, 1},
        {"round", ExprNode::Op::kFnRound, 1, 1},
        {"lower", ExprNode::Op::kFnLower, 1, 1},
        {"upper", ExprNode::Op::kFnUpper, 1, 1},
        {"strlen", ExprNode::Op::kFnStrlen, 1, 1},
        {"substr", ExprNode::Op::kFnSubstr, 3, 3},
        {"format", ExprNode::Op::kFnFormat, 1, 16},
        {"if", ExprNode::Op::kFnIf, 3, 3},
      };

      for (auto& fn : fns) {
        if (name == fn.name) {
          int nargs = static_cast<int>(args.size());
          if (nargs < fn.min_args || nargs > fn.max_args) {
            error = "ERR expr: " + name + " expects " +
                    std::to_string(fn.min_args) + "-" +
                    std::to_string(fn.max_args) + " args, got " +
                    std::to_string(nargs);
            return false;
          }
          out.op = fn.op;
          out.children = std::move(args);
          return true;
        }
      }
      error = "ERR expr: unknown function '" + name + "'";
      return false;
    }

    error = "ERR expr: unexpected character";
    return false;
  }

  // power: primary ('^' primary)*
  bool ParsePower(ExprNode& out) {
    if (!ParsePrimary(out)) return false;
    while (true) {
      SkipSpaces();
      if (Peek() != '^') break;
      pos++;
      ExprNode right;
      if (!ParsePrimary(right)) return false;
      ExprNode combined;
      combined.op = ExprNode::Op::kPow;
      combined.children.push_back(std::move(out));
      combined.children.push_back(std::move(right));
      out = std::move(combined);
    }
    return true;
  }

  // mul: power (('*'|'/'|'%') power)*
  bool ParseMul(ExprNode& out) {
    if (!ParsePower(out)) return false;
    while (true) {
      SkipSpaces();
      ExprNode::Op op;
      if (Peek() == '*') op = ExprNode::Op::kMul;
      else if (Peek() == '/') op = ExprNode::Op::kDiv;
      else if (Peek() == '%') op = ExprNode::Op::kMod;
      else break;
      pos++;
      ExprNode right;
      if (!ParsePower(right)) return false;
      ExprNode combined;
      combined.op = op;
      combined.children.push_back(std::move(out));
      combined.children.push_back(std::move(right));
      out = std::move(combined);
    }
    return true;
  }

  // add: mul (('+'|'-') mul)*
  bool ParseAdd(ExprNode& out) {
    if (!ParseMul(out)) return false;
    while (true) {
      SkipSpaces();
      ExprNode::Op op;
      if (Peek() == '+') op = ExprNode::Op::kAdd;
      else if (Peek() == '-') op = ExprNode::Op::kSub;
      else break;
      pos++;
      ExprNode right;
      if (!ParseMul(right)) return false;
      ExprNode combined;
      combined.op = op;
      combined.children.push_back(std::move(out));
      combined.children.push_back(std::move(right));
      out = std::move(combined);
    }
    return true;
  }

  // comparison: add (('=='|'!='|'<='|'>='|'<'|'>') add)*
  bool ParseCmp(ExprNode& out) {
    if (!ParseAdd(out)) return false;
    while (true) {
      SkipSpaces();
      ExprNode::Op op;
      if (pos + 1 < input.size() && input[pos] == '=' && input[pos + 1] == '=') {
        op = ExprNode::Op::kEq; pos += 2;
      } else if (pos + 1 < input.size() && input[pos] == '!' && input[pos + 1] == '=') {
        op = ExprNode::Op::kNe; pos += 2;
      } else if (pos + 1 < input.size() && input[pos] == '<' && input[pos + 1] == '=') {
        op = ExprNode::Op::kLe; pos += 2;
      } else if (pos + 1 < input.size() && input[pos] == '>' && input[pos + 1] == '=') {
        op = ExprNode::Op::kGe; pos += 2;
      } else if (Peek() == '<') {
        op = ExprNode::Op::kLt; pos++;
      } else if (Peek() == '>') {
        op = ExprNode::Op::kGt; pos++;
      } else {
        break;
      }
      ExprNode right;
      if (!ParseAdd(right)) return false;
      ExprNode combined;
      combined.op = op;
      combined.children.push_back(std::move(out));
      combined.children.push_back(std::move(right));
      out = std::move(combined);
    }
    return true;
  }

  // and: cmp ('&&' cmp)*
  bool ParseAnd(ExprNode& out) {
    if (!ParseCmp(out)) return false;
    while (true) {
      SkipSpaces();
      if (pos + 1 >= input.size() || input[pos] != '&' || input[pos + 1] != '&') break;
      pos += 2;
      ExprNode right;
      if (!ParseCmp(right)) return false;
      ExprNode combined;
      combined.op = ExprNode::Op::kAnd;
      combined.children.push_back(std::move(out));
      combined.children.push_back(std::move(right));
      out = std::move(combined);
    }
    return true;
  }

  // or: and ('||' and)*
  bool ParseOr(ExprNode& out) {
    if (!ParseAnd(out)) return false;
    while (true) {
      SkipSpaces();
      if (pos + 1 >= input.size() || input[pos] != '|' || input[pos + 1] != '|') break;
      pos += 2;
      ExprNode right;
      if (!ParseAnd(right)) return false;
      ExprNode combined;
      combined.op = ExprNode::Op::kOr;
      combined.children.push_back(std::move(out));
      combined.children.push_back(std::move(right));
      out = std::move(combined);
    }
    return true;
  }
};

bool ParseExpr(const std::string& input, ExprNode& out, std::string& error) {
  size_t start = input.find_first_not_of(" \t");
  if (start == std::string::npos) {
    error = "ERR expr: empty expression";
    return false;
  }
  size_t end = input.find_last_not_of(" \t");
  std::string trimmed = input.substr(start, end - start + 1);

  ExprParser parser(trimmed, error);
  if (!parser.ParseOr(out)) return false;
  parser.SkipSpaces();
  if (parser.pos != trimmed.size()) {
    error = "ERR expr: unexpected trailing input";
    return false;
  }
  return true;
}
