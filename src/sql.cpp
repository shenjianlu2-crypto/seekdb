#include "sql.h"
#include <cctype>
#include <algorithm>

namespace mvdb {

namespace {

struct Token {
  enum Type { IDENT, NUMBER, STRING, SYM, END } type = END;
  std::string text;
};

struct Lexer {
  const std::string& s;
  size_t pos = 0;
  explicit Lexer(const std::string& x) : s(x) {}

  void skip_ws() {
    for (;;) {
      while (pos < s.size() && isspace((unsigned char)s[pos])) pos++;
      if (pos + 1 < s.size() && s[pos] == '-' && s[pos + 1] == '-') {
        while (pos < s.size() && s[pos] != '\n') pos++;
        continue;
      }
      if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '*') {
        pos += 2;
        while (pos + 1 < s.size() && !(s[pos] == '*' && s[pos + 1] == '/')) pos++;
        pos = pos + 1 < s.size() ? pos + 2 : s.size();
        continue;
      }
      break;
    }
  }

  Token next() {
    skip_ws();
    Token t;
    if (pos >= s.size()) return t;
    char c = s[pos];
    if (c == '\'' || c == '"') {
      char q = c;
      pos++;
      std::string out;
      for (;;) {
        if (pos >= s.size()) throw MVError(ER_SYNTAX, "unterminated string");
        char ch = s[pos++];
        if (ch == q) {
          if (pos < s.size() && s[pos] == q) { out += q; pos++; continue; }
          break;
        }
        if (ch == '\\' && pos < s.size()) {
          char e = s[pos++];
          switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case '0': out += '\0'; break;
            default: out += e;
          }
          continue;
        }
        out += ch;
      }
      t.type = Token::STRING;
      t.text = out;
      return t;
    }
    if (c == '`') {
      pos++;
      size_t start = pos;
      while (pos < s.size() && s[pos] != '`') pos++;
      if (pos >= s.size()) throw MVError(ER_SYNTAX, "unquoted identifier");
      t.type = Token::IDENT;
      t.text = s.substr(start, pos - start);
      pos++;
      return t;
    }
    if (isdigit((unsigned char)c) || (c == '.' && pos + 1 < s.size() && isdigit((unsigned char)s[pos + 1]))) {
      size_t start = pos;
      while (pos < s.size() && (isdigit((unsigned char)s[pos]) || s[pos] == '.')) pos++;
      if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
        pos++;
        if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
        while (pos < s.size() && isdigit((unsigned char)s[pos])) pos++;
      }
      t.type = Token::NUMBER;
      t.text = s.substr(start, pos - start);
      return t;
    }
    if (isalpha((unsigned char)c) || c == '_') {
      size_t start = pos;
      while (pos < s.size() && (isalnum((unsigned char)s[pos]) || s[pos] == '_')) pos++;
      t.type = Token::IDENT;
      t.text = s.substr(start, pos - start);
      return t;
    }
    if (c == '<' || c == '>') {
      pos++;
      if (pos < s.size() && s[pos] == '=') { pos++; t.text = std::string(1, c) + "="; }
      else t.text = std::string(1, c);
      t.type = Token::SYM;
      return t;
    }
    pos++;
    t.type = Token::SYM;
    t.text = std::string(1, c);
    return t;
  }
};

std::string upper(std::string s) {
  for (auto& c : s) c = toupper((unsigned char)c);
  return s;
}
std::string lower(std::string s) {
  for (auto& c : s) c = tolower((unsigned char)c);
  return s;
}

struct Parser {
  std::vector<Token> toks;
  size_t p = 0;

  explicit Parser(const std::string& sql) {
    Lexer lx(sql);
    for (;;) {
      Token t = lx.next();
      if (t.type == Token::END) break;
      toks.push_back(t);
    }
  }

  const Token& peek() { static Token end; return p < toks.size() ? toks[p] : end; }
  Token take() { return p < toks.size() ? toks[p++] : Token(); }
  bool is_kw(const char* kw) { return peek().type == Token::IDENT && upper(peek().text) == kw; }
  bool take_kw(const char* kw) { if (is_kw(kw)) { p++; return true; } return false; }
  void expect_kw(const char* kw) { if (!take_kw(kw)) throw MVError(ER_SYNTAX, std::string("expected ") + kw); }
  bool is_sym(const char* sy) { return peek().type == Token::SYM && peek().text == sy; }
  bool take_sym(const char* sy) { if (is_sym(sy)) { p++; return true; } return false; }
  void expect_sym(const char* sy) { if (!take_sym(sy)) throw MVError(ER_SYNTAX, std::string("expected ") + sy); }
  std::string take_ident() {
    if (peek().type != Token::IDENT) throw MVError(ER_SYNTAX, "expected identifier");
    return take().text;
  }

  Value parse_number(const std::string& text) {
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find('E') == std::string::npos) {
      errno = 0;
      long long x = strtoll(text.c_str(), nullptr, 10);
      if (errno == ERANGE) throw MVError(ER_BAD_VALUE, "integer overflow");
      return Value::bigint((int64_t)x);
    }
    double d = strtod(text.c_str(), nullptr);
    if (!std::isfinite(d)) throw MVError(ER_BAD_VALUE, "number not finite");
    return Value::bigint((int64_t)llround(d));
  }

  // The sign is folded into the text so that the BIGINT minimum parses too.
  Value literal(bool neg = false) {
    Token t = take();
    if (t.type == Token::NUMBER) return parse_number((neg ? "-" : "") + t.text);
    if (t.type == Token::STRING) return Value::varchar(t.text);
    throw MVError(ER_SYNTAX, "expected literal");
  }

  bool next_is_lparen() {
    return p + 1 < toks.size() && toks[p + 1].type == Token::SYM && toks[p + 1].text == "(";
  }

  ExprPtr expr() {
    if ((is_kw("L2_DISTANCE") || is_kw("VECTOR_DISTANCE") || is_kw("DISTANCE")) && next_is_lparen()) {
      take();
      expect_sym("(");
      ExprPtr a = expr();
      expect_sym(",");
      ExprPtr b = expr();
      if (is_kw("L2_DISTANCE") || is_kw("EUCLIDEAN")) { take(); }
      expect_sym(")");
      auto e = std::make_shared<Expr>();
      e->kind = Expr::L2;
      e->a = a;
      e->b = b;
      return e;
    }
    if (is_sym("-") || is_sym("+")) {
      std::string sign = take().text;
      if (peek().type != Token::NUMBER) throw MVError(ER_SYNTAX, "sign expects a number");
      auto e = std::make_shared<Expr>();
      e->kind = Expr::LIT;
      e->lit = parse_number((sign == "-" ? "-" : "") + take().text);
      return e;
    }
    Token t = peek();
    if (t.type == Token::IDENT) {
      take();
      auto e = std::make_shared<Expr>();
      e->kind = Expr::COL;
      e->name = lower(t.text);
      return e;
    }
    if (t.type == Token::NUMBER || t.type == Token::STRING) {
      auto e = std::make_shared<Expr>();
      e->kind = Expr::LIT;
      e->lit = literal();
      return e;
    }
    throw MVError(ER_SYNTAX, "expected expression");
  }

  ExprPtr where_expr() {
    ExprPtr left = cmp_expr();
    while (take_kw("AND")) {
      ExprPtr right = cmp_expr();
      auto e = std::make_shared<Expr>();
      e->kind = Expr::LIT;
      e->name = "AND";
      e->a = left;
      e->b = right;
      left = e;
    }
    if (is_kw("OR")) throw MVError(ER_UNSUPPORTED, "OR not supported");
    return left;
  }

  ExprPtr cmp_expr() {
    ExprPtr a = expr();
    Token t = peek();
    std::string op;
    if (t.type == Token::SYM && (t.text == "=" || t.text == "<" || t.text == "<=" || t.text == ">" || t.text == ">=")) {
      op = take().text;
    } else {
      throw MVError(ER_SYNTAX, "expected comparison operator");
    }
    ExprPtr b = expr();
    auto e = std::make_shared<Expr>();
    e->kind = Expr::LIT;
    e->name = op;
    e->a = a;
    e->b = b;
    return e;
  }

  Stmt parse() {
    Stmt st;
    if (take_kw("CREATE")) {
      if (take_kw("TABLE")) return create_table(st);
      if (take_kw("VECTOR")) { expect_kw("INDEX"); return create_index(st); }
      if (take_kw("INDEX")) throw MVError(ER_UNSUPPORTED, "only VECTOR INDEX supported");
      throw MVError(ER_SYNTAX, "expected TABLE or VECTOR INDEX");
    }
    if (take_kw("INSERT")) return insert(st);
    if (take_kw("SELECT")) return select(st);
    if (take_kw("UPDATE")) return update(st);
    if (take_kw("DELETE")) return delete_(st);
    if (take_kw("BEGIN")) { st.kind = Stmt::BEGIN_STMT; return st; }
    if (take_kw("START")) { expect_kw("TRANSACTION"); st.kind = Stmt::BEGIN_STMT; return st; }
    if (take_kw("COMMIT")) { st.kind = Stmt::COMMIT_STMT; return st; }
    if (take_kw("ROLLBACK")) { st.kind = Stmt::ROLLBACK_STMT; return st; }
    if (take_kw("SET")) return set_mode(st);
    if (take_kw("EXIT") || take_kw("QUIT")) { st.kind = Stmt::EXIT_STMT; return st; }
    if (take_kw("DROP") || take_kw("ALTER") || take_kw("TRUNCATE"))
      throw MVError(ER_UNSUPPORTED, "statement not supported");
    throw MVError(ER_SYNTAX, "unknown statement");
  }

  Stmt create_table(Stmt st) {
    st.kind = Stmt::CREATE_TABLE;
    st.table = lower(take_ident());
    expect_sym("(");
    for (;;) {
      ColDef c;
      c.name = lower(take_ident());
      if (take_kw("BIGINT")) c.type = ColType::BIGINT;
      else if (take_kw("INT") || take_kw("INTEGER")) c.type = ColType::BIGINT;
      else if (take_kw("VARCHAR")) {
        c.type = ColType::VARCHAR;
        expect_sym("(");
        Token n = take();
        if (n.type != Token::NUMBER) throw MVError(ER_SYNTAX, "varchar length");
        c.len = atoi(n.text.c_str());
        if (c.len <= 0) throw MVError(ER_SYNTAX, "varchar length must be positive");
        expect_sym(")");
      } else if (take_kw("VECTOR")) {
        c.type = ColType::VECTOR;
        expect_sym("(");
        Token n = take();
        if (n.type != Token::NUMBER) throw MVError(ER_SYNTAX, "vector dimension");
        c.len = atoi(n.text.c_str());
        if (c.len <= 0) throw MVError(ER_SYNTAX, "vector dimension must be positive");
        expect_sym(")");
      } else {
        throw MVError(ER_UNSUPPORTED, "column type not supported");
      }
      if (take_kw("PRIMARY")) { expect_kw("KEY"); c.pk = true; }
      st.cols.push_back(c);
      if (take_sym(",")) continue;
      expect_sym(")");
      break;
    }
    return st;
  }

  Stmt create_index(Stmt st) {
    st.kind = Stmt::CREATE_INDEX;
    st.index = lower(take_ident());
    expect_kw("ON");
    st.table = lower(take_ident());
    expect_sym("(");
    st.cols.push_back(ColDef{lower(take_ident()), ColType::VECTOR, 0, false});
    expect_sym(")");
    if (take_kw("WITH")) {
      expect_sym("(");
      for (;;) {
        std::string k = lower(take_ident());
        expect_sym("=");
        Token v = take();
        (void)k; (void)v;
        if (take_sym(",")) continue;
        expect_sym(")");
        break;
      }
    }
    return st;
  }

  Stmt insert(Stmt st) {
    st.kind = Stmt::INSERT;
    expect_kw("INTO");
    st.table = lower(take_ident());
    if (take_sym("(")) {
      for (;;) {
        st.cols.push_back(ColDef{lower(take_ident()), ColType::BIGINT, 0, false});
        if (take_sym(",")) continue;
        expect_sym(")");
        break;
      }
    }
    expect_kw("VALUES");
    for (;;) {
      expect_sym("(");
      std::vector<Value> row;
      for (;;) {
        Token t = peek();
        if (t.type == Token::STRING) {
          std::string s = take().text;
          std::string tr = s;
          size_t b = tr.find_first_not_of(" \t\r\n");
          if (b != std::string::npos && tr[b] == '[') {
            Value v;
            v.type = ColType::VECTOR;
            v.s = s;
            v.v = parse_vector_literal(s);
            row.push_back(v);
          } else {
            row.push_back(Value::varchar(s));
          }
        } else if (t.type == Token::NUMBER) {
          row.push_back(literal());
        } else if (t.type == Token::SYM && t.text == "-") {
          take();
          row.push_back(literal(true));
        } else {
          throw MVError(ER_SYNTAX, "expected value");
        }
        if (take_sym(",")) continue;
        expect_sym(")");
        break;
      }
      st.rows.push_back(row);
      if (take_sym(",")) continue;
      break;
    }
    return st;
  }

  Stmt select(Stmt st) {
    st.kind = Stmt::SELECT;
    for (;;) {
      SelectItem it;
      if (take_sym("*")) {
        it.star = true;
      } else {
        it.expr = expr();
        if (take_kw("AS")) it.alias = lower(take_ident());
        else if (peek().type == Token::IDENT && !is_kw("FROM")) it.alias = lower(take_ident());
      }
      st.items.push_back(it);
      if (take_sym(",")) continue;
      break;
    }
    expect_kw("FROM");
    st.table = lower(take_ident());
    if (take_kw("WHERE")) { st.has_where = true; st.where = where_expr(); }
    if (take_kw("ORDER")) {
      expect_kw("BY");
      for (;;) {
        OrderItem o;
        o.expr = expr();
        if (take_kw("DESC")) o.desc = true;
        else take_kw("ASC");
        st.order.push_back(o);
        if (take_sym(",")) continue;
        break;
      }
    }
    if (take_kw("LIMIT")) {
      Token t = take();
      if (t.type != Token::NUMBER) throw MVError(ER_SYNTAX, "limit must be integer");
      st.limit = atoll(t.text.c_str());
      if (st.limit < 0) throw MVError(ER_SYNTAX, "limit must be non-negative");
    }
    return st;
  }

  Stmt update(Stmt st) {
    st.kind = Stmt::UPDATE;
    st.table = lower(take_ident());
    expect_kw("SET");
    for (;;) {
      std::string col = lower(take_ident());
      expect_sym("=");
      Value v;
      Token t = peek();
      if (t.type == Token::STRING) {
        std::string s = take().text;
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b != std::string::npos && s[b] == '[') {
          v.type = ColType::VECTOR;
          v.s = s;
          v.v = parse_vector_literal(s);
        } else v = Value::varchar(s);
      } else if (t.type == Token::NUMBER) v = literal();
      else if (t.type == Token::SYM && t.text == "-") { take(); v = literal(true); }
      else throw MVError(ER_SYNTAX, "expected value in SET");
      st.assigns.push_back({col, v});
      if (take_sym(",")) continue;
      break;
    }
    if (take_kw("WHERE")) { st.has_where = true; st.where = where_expr(); }
    return st;
  }

  Stmt delete_(Stmt st) {
    st.kind = Stmt::DELETE;
    expect_kw("FROM");
    st.table = lower(take_ident());
    if (take_kw("WHERE")) { st.has_where = true; st.where = where_expr(); }
    return st;
  }

  Stmt set_mode(Stmt st) {
    std::string key = lower(take_ident());
    while (take_sym(".")) key += "." + lower(take_ident());
    if (!take_sym("=")) throw MVError(ER_SYNTAX, "expected = in SET");
    Token v = take();
    std::string val = v.type == Token::STRING ? v.text : lower(v.text);
    if (key.find("mode") != std::string::npos || key.find("ann") != std::string::npos ||
        key.find("search") != std::string::npos) {
      st.kind = Stmt::SET_MODE;
      st.mode = val;
      return st;
    }
    throw MVError(ER_UNSUPPORTED, "SET variable not supported");
  }
};

}  // namespace

Stmt parse_sql(const std::string& sql) {
  Parser p(sql);
  Stmt st = p.parse();
  // The JSONL protocol hands over the SQL exactly as the case file wrote it,
  // including its terminating semicolon.
  while (p.p < p.toks.size() && p.toks[p.p].type == Token::SYM && p.toks[p.p].text == ";") p.p++;
  if (p.p != p.toks.size()) {
    if (st.kind == Stmt::UNSUPPORTED) throw MVError(ER_SYNTAX, "unknown statement");
    throw MVError(ER_SYNTAX, "unexpected tokens after statement");
  }
  return st;
}

}  // namespace mvdb