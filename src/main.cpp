#include <iostream>
#include <string>

#include "db/Database.h"

namespace {

void PrintResult(const sqlcompiler::ExecutionResult& result) {
    if (!result.success) {
        std::cerr << "Error: " << result.message << std::endl;
        return;
    }
    if (!result.column_names.empty()) {
        for (size_t i = 0; i < result.column_names.size(); ++i) {
            if (i) std::cout << " | ";
            std::cout << result.column_names[i];
        }
        std::cout << std::endl;
        for (size_t i = 0; i < result.column_names.size(); ++i) {
            if (i) std::cout << "-+-";
            std::cout << "---";
        }
        std::cout << std::endl;
        for (const auto& row : result.rows) {
            for (size_t i = 0; i < row.ColumnCount(); ++i) {
                if (i) std::cout << " | ";
                std::cout << row.GetValue(i).ToString();
            }
            std::cout << std::endl;
        }
        if (result.rows.empty()) {
            std::cout << "(0 rows)" << std::endl;
        }
    } else {
        std::cout << result.message << std::endl;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string db_file = (argc > 1) ? argv[1] : "sqlcompiler.db";

    sqlcompiler::Database* database = nullptr;
    try {
        database = new sqlcompiler::Database(db_file);
    } catch (const std::exception& e) {
        std::cerr << "Failed to open database '" << db_file << "': " << e.what()
                  << std::endl;
        std::cerr << "Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    std::string line;
    std::string sql;
    std::cout << "sqlcompiler> " << std::flush;
    while (std::getline(std::cin, line)) {
        sql += line;
        sql += "\n";
        if (sql.find(';') == std::string::npos) {
            std::cout << "       -> " << std::flush;
            continue;
        }
        // Trim
        std::string trimmed = sql;
        size_t a = trimmed.find_first_not_of(" \t\r\n");
        size_t b = trimmed.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) {
            trimmed = trimmed.substr(a, b - a + 1);
        }
        if (trimmed == "exit;" || trimmed == "quit;" || trimmed == "exit" || trimmed == "quit") {
            break;
        }
        auto result = database->ExecuteSQL(trimmed);
        PrintResult(result);
        sql.clear();
        std::cout << "sqlcompiler> " << std::flush;
    }
    database->Shutdown();
    delete database;
    return 0;
}