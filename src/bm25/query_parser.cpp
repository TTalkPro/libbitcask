#include "bitcask/query.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace bitcask::bm25 {

auto QueryNode::must_term(std::string t) -> QueryNode {
    QueryNode node;
    node.op = QueryOp::MUST;
    node.term = std::move(t);
    return node;
}

auto QueryNode::should_term(std::string t) -> QueryNode {
    QueryNode node;
    node.op = QueryOp::SHOULD;
    node.term = std::move(t);
    return node;
}

auto QueryNode::must_not_term(std::string t) -> QueryNode {
    QueryNode node;
    node.op = QueryOp::MUST_NOT;
    node.term = std::move(t);
    return node;
}

auto QueryNode::must_all(std::vector<QueryNode> children) -> QueryNode {
    QueryNode node;
    node.op = QueryOp::MUST;
    node.children = std::move(children);
    return node;
}

auto QueryNode::should_any(std::vector<QueryNode> children) -> QueryNode {
    QueryNode node;
    node.op = QueryOp::SHOULD;
    node.children = std::move(children);
    return node;
}

static constexpr bool is_plus_prefix(std::string_view sv) {
    return sv.size() >= 2 && sv[0] == '+';
}

static constexpr bool is_minus_prefix(std::string_view sv) {
    return sv.size() >= 2 && sv[0] == '-';
}

// 合法字段名字符：字母/数字/下划线（R5：避免 http://、12:30 被误判为字段限定）。
static constexpr bool is_field_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// 从 token 解析可选的 `field:` 前缀和 `^boost` 后缀（S8.6）。
// 仅当冒号左侧非空且全为合法字段名字符时才识别为字段限定。
// 返回 {field, term, boost}；field 空表示无字段限定。
struct ParsedToken { std::string field; std::string term; float boost; };

static ParsedToken parse_field_boost(std::string_view tok) {
    ParsedToken out;
    out.boost = 1.0F;

    // 尾部 ^boost
    if (auto caret = tok.rfind('^');
        caret != std::string_view::npos && caret + 1 < tok.size()) {
        auto bstr = tok.substr(caret + 1);
        bool numeric = !bstr.empty();
        bool seen_dot = false;
        for (char c : bstr) {
            if (c == '.' && !seen_dot) { seen_dot = true; continue; }
            if (c < '0' || c > '9') { numeric = false; break; }
        }
        if (numeric) {
            out.boost = std::strtof(std::string(bstr).c_str(), nullptr);
            if (out.boost <= 0.0F) out.boost = 1.0F;
            tok = tok.substr(0, caret);
        }
    }

    // 前缀 field:（R5：冒号左非空且全为合法字段名字符；冒号右不以 '/' 开头，
    // 排除 http:// 这类 URL）。
    if (auto colon = tok.find(':');
        colon != std::string_view::npos && colon > 0 && colon + 1 < tok.size() &&
        tok[colon + 1] != '/') {
        auto cand = tok.substr(0, colon);
        bool valid = true;
        for (char c : cand) {
            if (!is_field_char(c)) { valid = false; break; }
        }
        if (valid) {
            out.field = std::string(cand);
            tok = tok.substr(colon + 1);
        }
    }

    out.term = std::string(tok);
    return out;
}

auto parse_query(std::string_view input) -> QueryNode {
    std::vector<QueryNode> leaves;

    std::vector<std::string_view> tokens;
    std::string_view remaining = input;

    while (!remaining.empty()) {
        while (!remaining.empty() && std::isspace(static_cast<unsigned char>(remaining[0]))) {
            remaining.remove_prefix(1);
        }
        if (remaining.empty()) break;

        auto space_pos = remaining.find_first_of(" \t\n\r");
        auto token = remaining.substr(0, space_pos);
        if (token.data() == nullptr) break;

        if (space_pos == std::string_view::npos) {
            remaining = {};
        } else {
            remaining = remaining.substr(space_pos);
        }

        if (token.empty()) continue;

        QueryOp op = QueryOp::SHOULD;
        std::string_view body = token;
        if (is_plus_prefix(token)) { op = QueryOp::MUST; body = token.substr(1); }
        else if (is_minus_prefix(token)) { op = QueryOp::MUST_NOT; body = token.substr(1); }

        auto pt = parse_field_boost(body);
        QueryNode leaf;
        leaf.op = op;
        leaf.term = std::move(pt.term);
        leaf.field = std::move(pt.field);
        leaf.boost = pt.boost;
        leaves.push_back(std::move(leaf));
    }

    if (leaves.empty()) {
        return QueryNode::should_term({});
    }

    bool all_should = true;
    for (const auto& leaf : leaves) {
        if (leaf.op != QueryOp::SHOULD) {
            all_should = false;
            break;
        }
    }

    if (all_should && leaves.size() == 1) {
        return leaves[0];
    }

    return QueryNode::should_any(std::move(leaves));
}

// S13-D9：递归下降解析器（契约见 query.hpp）。
namespace {

struct TreeParser {
    std::string_view in;
    std::size_t pos = 0;

    void skip_ws() {
        while (pos < in.size() &&
               std::isspace(static_cast<unsigned char>(in[pos]))) {
            ++pos;
        }
    }

    // 解析一串 clause 直到 ')' 或输入尾。返回组节点（op 由 caller 设置）。
    QueryNode parse_group() {
        QueryNode group;
        group.op = QueryOp::SHOULD;
        while (true) {
            skip_ws();
            if (pos >= in.size()) break;
            if (in[pos] == ')') { ++pos; break; }  // 组结束（多余 ')' 由外层忽略）
            QueryOp op = QueryOp::SHOULD;
            if (in[pos] == '+' && pos + 1 < in.size()) { op = QueryOp::MUST; ++pos; }
            else if (in[pos] == '-' && pos + 1 < in.size() &&
                     in[pos + 1] != ' ') { op = QueryOp::MUST_NOT; ++pos; }
            skip_ws();
            if (pos >= in.size()) break;

            if (in[pos] == '(') {
                ++pos;
                QueryNode child = parse_group();
                child.op = op;
                if (!child.children.empty() || !child.term.empty()) {
                    group.children.push_back(std::move(child));
                }
            } else if (in[pos] == '"') {
                ++pos;
                const auto close = in.find('"', pos);
                const auto end = (close == std::string_view::npos)
                                     ? in.size() : close;  // 容错：未闭合取到尾
                QueryNode leaf;
                leaf.op = op;
                leaf.is_phrase = true;
                leaf.term = std::string(in.substr(pos, end - pos));
                pos = (close == std::string_view::npos) ? in.size() : close + 1;
                if (!leaf.term.empty()) group.children.push_back(std::move(leaf));
            } else {
                // 普通 token：读到空白 / ')' / '"'。
                const std::size_t start = pos;
                while (pos < in.size() && in[pos] != ')' && in[pos] != '"' &&
                       !std::isspace(static_cast<unsigned char>(in[pos]))) {
                    ++pos;
                }
                auto tok = in.substr(start, pos - start);
                if (!tok.empty()) {
                    auto pt = parse_field_boost(tok);
                    QueryNode leaf;
                    leaf.op = op;
                    leaf.term = std::move(pt.term);
                    leaf.field = std::move(pt.field);
                    leaf.boost = pt.boost;
                    if (!leaf.term.empty()) group.children.push_back(std::move(leaf));
                }
            }
        }
        return group;
    }
};

}  // namespace

auto parse_query_tree(std::string_view input) -> QueryNode {
    TreeParser tp{input};
    QueryNode root = tp.parse_group();
    // 顶层多余 ')' 之后可能还有 clause——继续吞并入 root（容错语义）。
    while (tp.pos < tp.in.size()) {
        QueryNode more = tp.parse_group();
        for (auto& c : more.children) root.children.push_back(std::move(c));
    }
    if (root.children.empty()) return QueryNode::should_term({});
    return root;
}

void collect_terms(
    const QueryNode& node,
    std::vector<std::string>& must_terms,
    std::vector<std::string>& should_terms,
    std::vector<std::string>& must_not_terms) {

    // S13-D9：短语叶子——成分词整体入所属桶（供缓存词集/失效判定）。
    if (node.is_phrase) {
        auto& dst = node.op == QueryOp::MUST ? must_terms
                    : node.op == QueryOp::MUST_NOT ? must_not_terms
                                                   : should_terms;
        dst.insert(dst.end(), node.phrase_terms.begin(),
                   node.phrase_terms.end());
        return;
    }
    if (!node.term.empty()) {
        switch (node.op) {
            case QueryOp::MUST:
                must_terms.push_back(node.term);
                break;
            case QueryOp::SHOULD:
                should_terms.push_back(node.term);
                break;
            case QueryOp::MUST_NOT:
                must_not_terms.push_back(node.term);
                break;
        }
        return;
    }

    for (const auto& child : node.children) {
        collect_terms(child, must_terms, should_terms, must_not_terms);
    }
}

}  // namespace bitcask::bm25