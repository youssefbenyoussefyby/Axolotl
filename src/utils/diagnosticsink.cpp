#include<utils/diagnosticsink.h>


namespace AxolotlDB::utils
{

    void DiagnosticSink::addDiagnostic(utils::Diagnostic diagnostic){
        diagnostics.push_back(diagnostic);
    }

    std::vector<utils::Diagnostic> DiagnosticSink::getDiagnostics(){ return diagnostics; }

    bool DiagnosticSink::hasError() const{
        return diagnostics.size() != 0;
    }
} // namespace AxolotlDB::utils
