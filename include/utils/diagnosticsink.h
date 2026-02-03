#pragma once

#include<string>
#include <cstdint>
#include<vector>

namespace AxolotlDB::utils{
    struct Diagnostic{
        std::string message;
        uint16_t line ;
        uint16_t column;
        std::string lexem; 
    };

    class DiagnosticSink{
        public:

            DiagnosticSink() = default;
            void addDiagnostic(Diagnostic);
            [[nodiscard]] std::vector<Diagnostic> getDiagnostics();
            [[nodiscard]] bool hasError() const;
        private:
            std::vector<Diagnostic> diagnostics;


    };
}