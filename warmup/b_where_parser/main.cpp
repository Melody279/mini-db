// 手搓热身 B：递归下降解析 WHERE 谓词链
// 语法：WHERE pred (AND pred)* ; pred := IDENT OP VALUE ; OP ∈ {= <> < <= > >=}
// 目标：把这一步长成 src/query 的 Parser（W1），所以 AST 结构与正式代码保持一致。
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace {

enum class TokKind { kIdent, kOp, kNumber, kString, kEnd, kError };

struct Token {
  TokKind kind = TokKind::kEnd;
  std::string text;
};

struct Pred {
  std::string col;
  std::string op;
  std::string value;
  bool numeric = false;
};

std::string ToUpper(const std::string& text) {
  std::string out = text;
  for (auto& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return out;
}

std::vector<Token> Tokenize(const std::string& input, std::string* err) {
  std::vector<Token> tokens;
  size_t i = 0;
  while (i < input.size()) {
    const char ch = input[i];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ++i;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
      std::string word;
      while (i < input.size() &&
             (std::isalnum(static_cast<unsigned char>(input[i])) || input[i] == '_')) {
        word.push_back(input[i++]);
      }
      tokens.push_back(Token{TokKind::kIdent, word});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) ||
        (ch == '-' && i + 1 < input.size() && std::isdigit(static_cast<unsigned char>(input[i + 1])))) {
      std::string number;
      number.push_back(input[i++]);
      while (i < input.size() && std::isdigit(static_cast<unsigned char>(input[i]))) {
        number.push_back(input[i++]);
      }
      tokens.push_back(Token{TokKind::kNumber, number});
      continue;
    }
    if (ch == '\'') {
      ++i;
      std::string text;
      while (i < input.size() && input[i] != '\'') text.push_back(input[i++]);
      if (i >= input.size()) {
        if (err != nullptr) *err = "字符串缺少右引号";
        return tokens;
      }
      ++i;  // 跳过右引号
      tokens.push_back(Token{TokKind::kString, text});
      continue;
    }
    if (ch == '<' || ch == '>' || ch == '=') {
      std::string op;
      op.push_back(input[i++]);
      if (i < input.size() && (input[i] == '=' || (ch == '<' && input[i] == '>'))) {
        op.push_back(input[i++]);
      }
      if (op != "=" && op != "<" && op != ">" && op != "<=" && op != ">=" && op != "<>") {
        if (err != nullptr) *err = "非法比较运算符 " + op;
        return tokens;
      }
      tokens.push_back(Token{TokKind::kOp, op});
      continue;
    }
    if (err != nullptr) *err = std::string("无法识别的字符 '") + ch + "'";
    return tokens;
  }
  tokens.push_back(Token{TokKind::kEnd, ""});
  return tokens;
}

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  bool ParseWhere(std::vector<Pred>* out, std::string* err) {
    if (!ExpectIdent("WHERE")) return Fail(err, "缺少 WHERE 关键字");
    if (!ParsePredicate(out, err)) return false;
    while (PeekIsIdent("AND")) {
      Advance();
      if (!ParsePredicate(out, err)) return false;
    }
    if (!PeekIsEnd()) return Fail(err, "谓词之后出现多余记号");
    return true;
  }

 private:
  bool ParsePredicate(std::vector<Pred>* out, std::string* err) {
    Pred pred;
    if (Peek().kind != TokKind::kIdent) return Fail(err, "谓词左值应为列名");
    pred.col = Advance().text;
    if (Peek().kind != TokKind::kOp) return Fail(err, "谓词缺少比较运算符");
    pred.op = Advance().text;
    if (Peek().kind == TokKind::kNumber) {
      pred.value = Advance().text;
      pred.numeric = true;
    } else if (Peek().kind == TokKind::kString) {
      pred.value = Advance().text;
      pred.numeric = false;
    } else {
      return Fail(err, "谓词右值应为常量");
    }
    out->push_back(pred);
    return true;
  }

  bool ExpectIdent(const std::string& keyword) {
    if (!PeekIsIdent(keyword)) return false;
    Advance();
    return true;
  }

  bool PeekIsIdent(const std::string& keyword) const {
    return Peek().kind == TokKind::kIdent && ToUpper(Peek().text) == keyword;
  }

  bool PeekIsEnd() const { return Peek().kind == TokKind::kEnd; }

  const Token& Peek() const { return tokens_[pos_]; }

  const Token& Advance() {
    const Token& token = tokens_[pos_];
    if (pos_ + 1 < tokens_.size()) ++pos_;
    return token;
  }

  bool Fail(std::string* err, const std::string& message) {
    if (err != nullptr) *err = message;
    return false;
  }

  std::vector<Token> tokens_;
  size_t pos_ = 0;
};

struct Case {
  std::string sql;
  size_t expect_preds;
  bool expect_ok;
};

bool RunCase(const Case& c) {
  std::string err;
  const auto tokens = Tokenize(c.sql, &err);
  Parser parser(tokens);
  std::vector<Pred> preds;
  const bool ok = parser.ParseWhere(&preds, &err);

  std::printf("--- %s\n", c.sql.c_str());
  if (!ok) {
    std::printf("    parse failed: %s\n", err.c_str());
  } else {
    for (const auto& p : preds) {
      std::printf("    %s %s %s%s\n", p.col.c_str(), p.op.c_str(), p.value.c_str(),
                  p.numeric ? " (num)" : " (str)");
    }
  }

  const bool pass = (ok == c.expect_ok) && (!c.expect_ok || preds.size() == c.expect_preds);
  std::printf("    %s\n", pass ? "ok" : "FAIL");
  return pass;
}

}  // namespace

int main() {
  const std::vector<Case> cases = {
      {"WHERE state = 3", 1, true},
      {"WHERE pid = 1001 AND created_at >= '2026-11-01 00:00:00'", 2, true},
      {"WHERE distance_m > 3000 AND score <= 5 AND state <> 6", 3, true},
      {"WHERE = 3", 0, false},
  };

  int failed = 0;
  for (const auto& c : cases) {
    if (!RunCase(c)) ++failed;
  }
  std::printf("-------------------\n");
  std::printf("RESULT: %s (warmup_b_where_parser)\n", failed == 0 ? "PASS" : "FAIL");
  return failed == 0 ? 0 : 1;
}
