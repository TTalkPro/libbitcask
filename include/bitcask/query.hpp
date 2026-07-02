// 查询抽象语法树（Query AST）— 布尔搜索支持。
//
// 支持三种查询操作：
//   MUST      — 必需匹配（AND 语义）
//   SHOULD    — 可选匹配（OR 语义，得分加成）
//   MUST_NOT  — 必须不匹配（排除）
//
// 查询语法（前缀表达式）：
//   `+term`  → MUST（必须包含）
//   `-term`  → MUST_NOT（必须不包含）
//   `term`   → SHOULD（可选，包含则加分）
//   多个词默认用 SHOULD 组合（向后兼容词袋模式）

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bitcask::bm25 {

// 查询操作类型。
enum class QueryOp : std::uint8_t {
    MUST,      // 必需匹配：所有 MUST term 必须在文档中出现
    SHOULD,    // 可选匹配：任意 SHOULD term 出现即加分
    MUST_NOT   // 必须不匹配：排除包含该 term 的文档
};

// 查询节点。
// 叶子节点：op + term（非空，children 空）
// 非叶子节点：op + 空 term + children（用于组合）
struct QueryNode {
    QueryOp               op;
    std::string           term;                     // 叶子节点查询词
    std::string           field;                    // 字段限定（S8.6，空=默认字段）
    float                 boost = 1.0F;             // 字段/词权重（S8.6）
    std::vector<QueryNode> children;               // 非叶子子节点
    // S13-D9：短语叶子。term 存引号内原文；phrase_terms 由 SearchLayer 用
    // analyzer 填充（有序词列表），执行侧按 positions 匹配。
    bool                     is_phrase = false;
    std::vector<std::string> phrase_terms;

    // 工厂方法：创建叶子节点
    static QueryNode must_term(std::string t);
    static QueryNode should_term(std::string t);
    static QueryNode must_not_term(std::string t);

    // 工厂方法：创建组合节点
    static QueryNode must_all(std::vector<QueryNode> children);
    static QueryNode should_any(std::vector<QueryNode> children);
};

// 解析查询字符串为 QueryNode AST。
// 语法规则：
//   - 以空白分割 token
//   - `+term` → MUST
//   - `-term` → MUST_NOT
//   - `term`  → SHOULD
//   - 所有 token 均为 SHOULD 时，返回单一 SHOULD 节点（向后兼容）
// 空字符串返回空 children 的 SHOULD 节点。
[[nodiscard]] auto parse_query(std::string_view input) -> QueryNode;

// S13-D9：递归下降解析（括号嵌套 + 引号短语）。语法：
//   expr    := clause+
//   clause  := [+|-] primary          （+ = MUST，- = MUST_NOT，裸 = SHOULD）
//   primary := '(' expr ')' | '"' 任意字符 '"' | token（含 field:term^boost）
// 组语义：组内 MUST 交集为基集（无 MUST 则 SHOULD 并集），MUST_NOT 差集；
// 组自身的 +/- 前缀决定它在父组中的角色。容错：未闭合引号取到行尾；
// 多余 ')' 忽略；未闭合 '(' 组取到行尾。
// 仅当查询含 '(' 或 '"' 时才需要本入口——SearchLayer 按此路由，
// 无新语法的查询仍走 parse_query（扁平路径，行为位级不变）。
[[nodiscard]] auto parse_query_tree(std::string_view input) -> QueryNode;

// 将 QueryNode AST 展平为三个 term 集合。
// 用于 bool_search 算法：
//   must_terms     — 所有 MUST 叶子节点的 term
//   should_terms   — 所有 SHOULD 叶子节点的 term
//   must_not_terms — 所有 MUST_NOT 叶子节点的 term
void collect_terms(
    const QueryNode& node,
    std::vector<std::string>& must_terms,
    std::vector<std::string>& should_terms,
    std::vector<std::string>& must_not_terms);

}  // namespace bitcask::bm25