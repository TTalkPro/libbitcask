#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bitcask::text {

class SynonymMap {
public:
    SynonymMap() = default;

    [[nodiscard]] bool load_from_file(const std::string& path) {
        std::ifstream file(path);
        if (!file) return false;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            std::vector<std::string> terms;
            std::stringstream ss(line);
            std::string term;
            while (std::getline(ss, term, ',')) {
                auto start = term.find_first_not_of(" \t\r\n");
                if (start == std::string::npos) continue;
                auto end = term.find_last_not_of(" \t\r\n");
                terms.push_back(term.substr(start, end - start + 1));
            }
            if (terms.size() >= 2) {
                add_group(std::move(terms));
            }
        }
        return true;
    }

    void add_group(std::vector<std::string> terms) {
        // C2:shared_ptr 共享组 → O(n log n) 排序去重 + O(n) 指针赋值（旧 O(n²) 全组拷贝）。
        std::sort(terms.begin(), terms.end());
        terms.erase(std::unique(terms.begin(), terms.end()), terms.end());

        // 检查是否已有 term 存在 → 合并已有组与新 terms 为单一 shared_ptr。
        std::shared_ptr<const std::vector<std::string>> merged;
        for (const auto& term : terms) {
            auto it = term_to_group_.find(term);
            if (it != term_to_group_.end()) {
                std::vector<std::string> combined;
                combined.reserve(it->second->size() + terms.size());
                std::set_union(it->second->begin(), it->second->end(),
                               terms.begin(), terms.end(),
                               std::back_inserter(combined));
                merged = std::make_shared<const std::vector<std::string>>(std::move(combined));
                break;
            }
        }
        if (!merged) {
            merged = std::make_shared<const std::vector<std::string>>(std::move(terms));
        }

        // 所有 term 指向同一 shared_ptr —— 零 vector 拷贝。
        for (const auto& term : *merged) {
            term_to_group_[term] = merged;
        }
    }

    [[nodiscard]] std::span<const std::string> expand(const std::string& term) const {
        auto it = term_to_group_.find(term);
        if (it == term_to_group_.end()) return {};
        return *it->second;
    }

    [[nodiscard]] std::vector<std::string> expand_terms(
        const std::vector<std::string>& terms) const {
        std::unordered_set<std::string> seen;
        std::vector<std::string> result;
        result.reserve(terms.size() * 2);
        for (const auto& term : terms) {
            auto syns = expand(term);
            if (syns.empty()) {
                if (seen.insert(term).second) result.push_back(term);
            } else {
                for (const auto& syn : syns) {
                    if (seen.insert(syn).second) result.push_back(syn);
                }
            }
        }
        return result;
    }

private:
    std::unordered_map<std::string,
                       std::shared_ptr<const std::vector<std::string>>> term_to_group_;
};

}  // namespace bitcask::text
