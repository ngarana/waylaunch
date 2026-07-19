#pragma once

#include <string>
#include <variant>
#include <optional>

namespace waylaunch {

class Calculator {
public:
    struct Result {
        bool valid = false;
        std::string expression;
        std::string result;
        double numeric_result = 0.0;
    };

    Calculator();
    ~Calculator();

    void set_degrees_mode(bool degrees);
    bool is_degrees_mode() const;

    Result evaluate(const std::string& expression) const;
    bool is_calculator_query(const std::string& query) const;

private:
    // Expression parser
    class Parser;
    
    bool degrees_mode_ = false;
};

} // namespace waylaunch
