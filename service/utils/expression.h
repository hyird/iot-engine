#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace service::utils {

// Bounded, side-effect-free numeric expressions. No scripts or external calls.
class Expression final {
  public:
    struct Node {
        std::string operation;
        double number{};
        std::vector<std::size_t> arguments;
    };

    explicit Expression(std::string_view text) : text_(text) {
        if (text.empty() || text.size() > 512) {
            fail();
        }
        root_ = parse(0, 0);
        space();
        if (position_ != text_.size()) {
            fail();
        }
        text_ = {};
    }

    const std::vector<std::string>& variables() const { return variables_; }

    double evaluate(const std::function<double(std::string_view)>& resolve) const {
        return value(root_, resolve);
    }

  private:
    [[noreturn]] static void fail() { throw std::invalid_argument("表达式无效或超过复杂度限制"); }

    void space() {
        while (position_ < text_.size() && std::string_view(" \t\r\n").find(text_[position_]) != std::string_view::npos) {
            ++position_;
        }
    }

    bool take(std::string_view token) {
        space();
        if (!text_.substr(position_).starts_with(token)) {
            return false;
        }
        position_ += token.size();
        return true;
    }

    std::size_t add(Node node) {
        if (nodes_.size() >= 128) {
            fail();
        }
        nodes_.push_back(std::move(node));
        return nodes_.size() - 1;
    }

    std::size_t atom(unsigned depth) {
        if (depth > 24) {
            fail();
        }
        if (take("(")) {
            auto node = parse(0, depth + 1);
            if (!take(")")) {
                fail();
            }
            return node;
        }
        for (const auto op : { "!", "-", "+" }) {
            if (take(op)) {
                return add({ std::string("unary") + op, 0, { atom(depth + 1) } });
            }
        }
        space();
        auto start = position_;
        while (position_ < text_.size()) {
            const auto c = text_[position_];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                  (position_ > start && c >= '0' && c <= '9'))) {
                break;
            }
            ++position_;
        }
        if (position_ != start) {
            std::string name(text_.substr(start, position_ - start));
            if (name == "true" || name == "false") {
                return add({ "number", name == "true" ? 1.0 : 0.0, {} });
            }
            if (take("(")) {
                std::vector<std::size_t> args{ parse(0, depth + 1) };
                while (take(",")) {
                    if (args.size() == 3) {
                        fail();
                    }
                    args.push_back(parse(0, depth + 1));
                }
                if (!take(")")) {
                    fail();
                }
                const auto count = name == "if" ? 3U : name == "min" || name == "max" ? 2U
                    : name == "abs" || name == "sqrt" || name == "round"              ? 1U
                                                                                      : 0U;
                if (count != args.size()) {
                    fail();
                }
                return add({ std::move(name), 0, std::move(args) });
            }
            if (std::find(variables_.begin(), variables_.end(), name) == variables_.end()) {
                variables_.push_back(name);
            }
            return add({ "variable:" + name, 0, {} });
        }
        double number{};
        const auto [end, error] = std::from_chars(text_.data() + position_, text_.data() + text_.size(), number);
        if (error != std::errc{} || end == text_.data() + position_ || !std::isfinite(number)) {
            fail();
        }
        position_ = static_cast<std::size_t>(end - text_.data());
        return add({ "number", number, {} });
    }

    std::size_t parse(int minimum, unsigned depth) {
        auto left = atom(depth);
        for (;;) {
            space();
            std::string_view op;
            int precedence = -1;
            for (const auto candidate : { "||", "&&", "==", "!=", "<=", ">=", "<", ">", "+", "-", "*", "/", "%" }) {
                if (text_.substr(position_).starts_with(candidate)) {
                    op = candidate;
                    precedence = op == "||" ? 0 : op == "&&"                 ? 1
                        : op == "==" || op == "!="                           ? 2
                        : op == "<=" || op == ">=" || op == "<" || op == ">" ? 3
                        : op == "+" || op == "-"                             ? 4
                                                                             : 5;
                    break;
                }
            }
            if (precedence < minimum) {
                break;
            }
            position_ += op.size();
            auto right = parse(precedence + 1, depth + 1);
            left = add({ std::string(op), 0, { left, right } });
        }
        return left;
    }

    double value(std::size_t index, const std::function<double(std::string_view)>& resolve) const {
        const auto& node = nodes_[index];
        const auto& op = node.operation;
        if (op == "number") {
            return node.number;
        }
        if (op.starts_with("variable:")) {
            const auto result = resolve(std::string_view(op).substr(9));
            if (!std::isfinite(result)) {
                throw std::domain_error("输入不是有限数值");
            }
            return result;
        }
        const auto a = value(node.arguments[0], resolve);
        if (op == "unary!") {
            return a == 0;
        }
        if (op == "unary+") {
            return a;
        }
        if (op == "unary-") {
            return -a;
        }
        if (op == "abs") {
            return std::abs(a);
        }
        if (op == "round") {
            return std::round(a);
        }
        if (op == "sqrt") {
            if (a < 0) {
                throw std::domain_error("负数不能开平方");
            }
            return std::sqrt(a);
        }
        if (op == "if") {
            return value(node.arguments[a != 0 ? 1 : 2], resolve);
        }
        if (op == "&&" && a == 0) {
            return 0;
        }
        if (op == "||" && a != 0) {
            return 1;
        }
        const auto b = value(node.arguments[1], resolve);
        if (op == "&&" || op == "||") {
            return b != 0;
        }
        if (op == "==") {
            return a == b;
        }
        if (op == "!=") {
            return a != b;
        }
        if (op == "<") {
            return a < b;
        }
        if (op == ">") {
            return a > b;
        }
        if (op == "<=") {
            return a <= b;
        }
        if (op == ">=") {
            return a >= b;
        }
        if (op == "min") {
            return (std::min)(a, b);
        }
        if (op == "max") {
            return (std::max)(a, b);
        }
        if ((op == "/" || op == "%") && b == 0) {
            throw std::domain_error("除数不能为零");
        }
        const auto result = op == "+" ? a + b : op == "-" ? a - b
            : op == "*"                                   ? a * b
            : op == "/"                                   ? a / b
                                                          : std::fmod(a, b);
        if (!std::isfinite(result)) {
            throw std::domain_error("计算结果溢出");
        }
        return result;
    }

    std::string_view text_;
    std::size_t position_{};
    std::size_t root_{};
    std::vector<Node> nodes_;
    std::vector<std::string> variables_;
};
} // namespace service::utils
