#include "waylaunch/calculator.h"
#include <cmath>
#include <sstream>
#include <stack>
#include <cctype>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace waylaunch {

class Calculator::Parser {
public:
    Result evaluate(const std::string& expression, bool degrees) {
        Result result;
        result.expression = expression;

        if (expression.empty()) {
            return result;
        }

        try {
            pos_ = 0;
            expr_ = expression;
            degrees_ = degrees;

            double value = parse_expression();
            result.valid = true;
            result.numeric_result = value;

            // Format result
            if (value == static_cast<int64_t>(value) && std::abs(value) < 1e15) {
                result.result = std::to_string(static_cast<int64_t>(value));
            } else {
                std::ostringstream oss;
                oss << value;
                result.result = oss.str();
            }
        } catch (...) {
            result.valid = false;
        }

        return result;
    }

private:
    double parse_expression() {
        double left = parse_term();
        while (pos_ < expr_.size()) {
            skip_space();
            if (pos_ >= expr_.size()) break;

            char op = expr_[pos_];
            if (op == '+' || op == '-') {
                pos_++;
                double right = parse_term();
                left = (op == '+') ? left + right : left - right;
            } else {
                break;
            }
        }
        return left;
    }

    double parse_term() {
        double left = parse_factor();
        while (pos_ < expr_.size()) {
            skip_space();
            if (pos_ >= expr_.size()) break;

            char op = expr_[pos_];
            if (op == '*' || op == '/') {
                pos_++;
                double right = parse_factor();
                if (op == '*') {
                    left *= right;
                } else {
                    if (right == 0) throw std::runtime_error("Division by zero");
                    left /= right;
                }
            } else if (op == '^') {
                pos_++;
                double right = parse_factor();
                left = std::pow(left, right);
            } else {
                break;
            }
        }
        return left;
    }

    double parse_factor() {
        skip_space();
        if (pos_ >= expr_.size()) throw std::runtime_error("Unexpected end");

        // Unary minus
        if (expr_[pos_] == '-') {
            pos_++;
            return -parse_factor();
        }

        // Unary plus
        if (expr_[pos_] == '+') {
            pos_++;
            return parse_factor();
        }

        // Parentheses
        if (expr_[pos_] == '(') {
            pos_++;
            double val = parse_expression();
            skip_space();
            if (pos_ < expr_.size() && expr_[pos_] == ')') {
                pos_++;
            }
            return val;
        }

        // Function calls
        if (std::isalpha(expr_[pos_])) {
            std::string func;
            while (pos_ < expr_.size() && std::isalpha(expr_[pos_])) {
                func += expr_[pos_++];
            }
            std::transform(func.begin(), func.end(), func.begin(), ::tolower);

            skip_space();
            if (pos_ < expr_.size() && expr_[pos_] == '(') {
                pos_++;
                double arg = parse_expression();
                skip_space();
                if (pos_ < expr_.size() && expr_[pos_] == ')') {
                    pos_++;
                }

                if (func == "sin") return degrees_ ? std::sin(arg * M_PI / 180) : std::sin(arg);
                if (func == "cos") return degrees_ ? std::cos(arg * M_PI / 180) : std::cos(arg);
                if (func == "tan") return degrees_ ? std::tan(arg * M_PI / 180) : std::tan(arg);
                if (func == "asin") return degrees_ ? std::asin(arg) * 180 / M_PI : std::asin(arg);
                if (func == "acos") return degrees_ ? std::acos(arg) * 180 / M_PI : std::acos(arg);
                if (func == "atan") return degrees_ ? std::atan(arg) * 180 / M_PI : std::atan(arg);
                if (func == "sqrt") return std::sqrt(arg);
                if (func == "cbrt") return std::cbrt(arg);
                if (func == "log") return std::log10(arg);
                if (func == "ln") return std::log(arg);
                if (func == "exp") return std::exp(arg);
                if (func == "abs") return std::abs(arg);
                if (func == "ceil") return std::ceil(arg);
                if (func == "floor") return std::floor(arg);
                if (func == "round") return std::round(arg);
                if (func == "sign") return (arg > 0) ? 1.0 : (arg < 0) ? -1.0 : 0.0;
            }

            // Constants
            if (func == "pi") return M_PI;
            if (func == "e") return M_E;
            if (func == "phi") return (1 + std::sqrt(5.0)) / 2;
            if (func == "inf") return std::numeric_limits<double>::infinity();
            if (func == "nan") return std::numeric_limits<double>::quiet_NaN();

            throw std::runtime_error("Unknown function: " + func);
        }

        // Number
        if (std::isdigit(expr_[pos_]) || expr_[pos_] == '.') {
            size_t start = pos_;
            while (pos_ < expr_.size() && (std::isdigit(expr_[pos_]) || expr_[pos_] == '.')) {
                pos_++;
            }
            return std::stod(expr_.substr(start, pos_ - start));
        }

        throw std::runtime_error("Unexpected character");
    }

    void skip_space() {
        while (pos_ < expr_.size() && std::isspace(expr_[pos_])) {
            pos_++;
        }
    }

    std::string expr_;
    size_t pos_ = 0;
    bool degrees_ = false;
};

Calculator::Calculator() = default;
Calculator::~Calculator() = default;

void Calculator::set_degrees_mode(bool degrees) {
    degrees_mode_ = degrees;
}

bool Calculator::is_degrees_mode() const {
    return degrees_mode_;
}

Calculator::Result Calculator::evaluate(const std::string& expression) const {
    Parser parser;
    return parser.evaluate(expression, degrees_mode_);
}

bool Calculator::is_calculator_query(const std::string& query) const {
    if (query.empty()) return false;

    // Check if query contains mathematical operators or functions
    for (char c : query) {
        if (c == '+' || c == '-' || c == '*' || c == '/' ||
            c == '^' || c == '(' || c == ')') {
            return true;
        }
    }

    // Check for math functions
    std::string lower = query;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    static const std::vector<std::string> funcs = {
        "sin", "cos", "tan", "asin", "acos", "atan",
        "sqrt", "cbrt", "log", "ln", "exp", "abs",
        "ceil", "floor", "round", "sign", "pi", "e"
    };

    for (const auto& f : funcs) {
        if (lower.find(f) != std::string::npos) {
            return true;
        }
    }

    return false;
}

} // namespace waylaunch
