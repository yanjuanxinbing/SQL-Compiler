// 语义分析模块测试：覆盖表/列存在性检查、各类语句的语义校验、
// 错误收集与符号表管理。
#include "test_framework.h"

#include "ast/AST.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "semantic/SemanticAnalyzer.h"
#include "semantic/SymbolTable.h"

using namespace sqlcompiler;

namespace {

// 构造一个注册了 student(id INT PRIMARY KEY, name VARCHAR NOT NULL, age INT) 的符号表
SymbolTable MakeStudentSymbolTable() {
    SymbolTable st;
    Lexer lexer("CREATE TABLE student (id INT PRIMARY KEY, name VARCHAR NOT NULL, age INT);");
    Parser parser(lexer.Tokenize());
    auto stmt = std::dynamic_pointer_cast<CreateTableStatement>(parser.Parse());
    CHECK(stmt != nullptr);
    if (stmt) st.AddTableFromCreateStatement(*stmt);
    return st;
}

bool AnalyzeSQL(SymbolTable& st, const std::string& sql,
                std::vector<SemanticError>* errors_out = nullptr) {
    Lexer lexer(sql);
    Parser parser(lexer.Tokenize());
    auto stmt = parser.Parse();
    SemanticAnalyzer analyzer(st);
    bool ok = analyzer.Analyze(stmt);
    if (errors_out) *errors_out = analyzer.GetErrors();
    return ok;
}

// ---- 符号表管理 ----

void TestSymbolTableRegistration() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(st.HasTable("student"));
    CHECK(!st.HasTable("nosuch"));
    const TableInfo* info = st.GetTable("student");
    CHECK(info != nullptr);
    if (info) {
        CHECK_EQ(info->table_name, std::string("student"));
        CHECK_EQ(info->columns.size(), static_cast<size_t>(3));
        CHECK(info->HasColumn("id"));
        CHECK(info->HasColumn("name"));
        CHECK(info->HasColumn("age"));
        CHECK(!info->HasColumn("nickname"));
        const ColumnInfo* pk = info->GetColumn("id");
        CHECK(pk != nullptr);
        if (pk) CHECK(pk->is_primary_key);
    }
}

void TestSymbolTableDuplicateRegistration() {
    SymbolTable st = MakeStudentSymbolTable();
    // 重复注册同名表应失败
    Lexer lexer("CREATE TABLE student (x INT);");
    Parser parser(lexer.Tokenize());
    auto dup = std::dynamic_pointer_cast<CreateTableStatement>(parser.Parse());
    CHECK(dup != nullptr);
    if (dup) CHECK(!st.AddTableFromCreateStatement(*dup));
}

void TestSymbolTableRemove() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(st.RemoveTable("student"));
    CHECK(!st.HasTable("student"));
    CHECK(!st.RemoveTable("student"));  // 二次删除失败
    auto names = st.GetAllTableNames();
    CHECK(names.empty());
}

// ---- SELECT 语义 ----

void TestValidSelectPasses() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(AnalyzeSQL(st, "SELECT id, name FROM student WHERE age > 18;"));
    CHECK(AnalyzeSQL(st, "SELECT * FROM student;"));
    CHECK(AnalyzeSQL(st, "SELECT student.id FROM student WHERE student.name = 'A';"));
    CHECK(AnalyzeSQL(st, "SELECT age FROM student WHERE age > 18 AND age < 60 ORDER BY age;"));
}

void TestSelectTableNotExists() {
    SymbolTable st = MakeStudentSymbolTable();
    std::vector<SemanticError> errors;
    bool ok = AnalyzeSQL(st, "SELECT * FROM ghost;", &errors);
    CHECK(!ok);
    CHECK(!errors.empty());
}

void TestSelectColumnNotExists() {
    SymbolTable st = MakeStudentSymbolTable();
    std::vector<SemanticError> errors;
    bool ok = AnalyzeSQL(st, "SELECT nickname FROM student;", &errors);
    CHECK(!ok);
    CHECK(!errors.empty());
}

void TestWhereColumnNotExists() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(!AnalyzeSQL(st, "SELECT id FROM student WHERE bad_col = 1;"));
}

void TestJoinTableNotExists() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(!AnalyzeSQL(st, "SELECT * FROM student INNER JOIN ghost ON student.id = ghost.id;"));
}

// ---- INSERT 语义 ----

void TestValidInsertPasses() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(AnalyzeSQL(st, "INSERT INTO student(id, name, age) VALUES (1, 'Alice', 20);"));
    CHECK(AnalyzeSQL(st, "INSERT INTO student VALUES (2, 'Bob', 21);"));
    CHECK(AnalyzeSQL(st, "INSERT INTO student(id, age) VALUES (3, NULL);"));
    CHECK(AnalyzeSQL(st,
                     "INSERT INTO student(id, name, age) VALUES (4, 'C', 22), (5, 'D', 23);"));
}

void TestInsertTableNotExists() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(!AnalyzeSQL(st, "INSERT INTO ghost(id) VALUES (1);"));
}

void TestInsertColumnNotExists() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(!AnalyzeSQL(st, "INSERT INTO student(bad_col) VALUES (1);"));
}

// ---- UPDATE 语义 ----

void TestValidUpdatePasses() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(AnalyzeSQL(st, "UPDATE student SET age = 21 WHERE id = 1;"));
    CHECK(AnalyzeSQL(st, "UPDATE student SET name = 'X', age = 0;"));
}

void TestUpdateColumnNotExists() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(!AnalyzeSQL(st, "UPDATE student SET bad_col = 1 WHERE id = 1;"));
}

void TestUpdateTableNotExists() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(!AnalyzeSQL(st, "UPDATE ghost SET x = 1;"));
}

// ---- DELETE 语义 ----

void TestValidDeletePasses() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(AnalyzeSQL(st, "DELETE FROM student WHERE id = 1;"));
    CHECK(AnalyzeSQL(st, "DELETE FROM student;"));
}

void TestDeleteTableNotExists() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(!AnalyzeSQL(st, "DELETE FROM ghost;"));
}

// ---- DDL 语义 ----

void TestValidCreatePasses() {
    SymbolTable st;
    CHECK(AnalyzeSQL(st, "CREATE TABLE t (a INT, b VARCHAR, c FLOAT);"));
    CHECK(st.HasTable("t"));  // CREATE 语句本身会把表登记进符号表
}

void TestDropTableNotExists() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(!AnalyzeSQL(st, "DROP TABLE ghost;"));
}

void TestDropThenReferenceFails() {
    SymbolTable st = MakeStudentSymbolTable();
    CHECK(AnalyzeSQL(st, "DROP TABLE student;"));
    // 删除后再查询应报"表不存在"
    CHECK(!AnalyzeSQL(st, "SELECT * FROM student;"));
}

// ---- 错误信息内容 ----

void TestErrorMessageNonEmpty() {
    SymbolTable st = MakeStudentSymbolTable();
    std::vector<SemanticError> errors;
    AnalyzeSQL(st, "SELECT nope FROM student;", &errors);
    CHECK(!errors.empty());
    if (!errors.empty()) CHECK(!errors[0].message.empty());
}

void TestErrorCollectorAccumulates() {
    SymbolTable st;
    SemanticAnalyzer analyzer(st);
    Lexer lexer("SELECT * FROM ghost;");
    Parser parser(lexer.Tokenize());
    auto stmt = parser.Parse();
    analyzer.Analyze(stmt);
    // GetErrors 与 ClearErrors 的可复用性
    size_t first = analyzer.GetErrors().size();
    CHECK(first > 0);
    analyzer.ClearErrors();
    CHECK(analyzer.GetErrors().empty());
}

}  // namespace

int main() {
    testfw::Run("语义: 符号表注册与查询", TestSymbolTableRegistration);
    testfw::Run("语义: 重复建表拒绝", TestSymbolTableDuplicateRegistration);
    testfw::Run("语义: 删表与二次删除", TestSymbolTableRemove);
    testfw::Run("语义: 合法SELECT通过", TestValidSelectPasses);
    testfw::Run("语义: 查询不存在的表", TestSelectTableNotExists);
    testfw::Run("语义: 查询不存在的列", TestSelectColumnNotExists);
    testfw::Run("语义: WHERE中列不存在", TestWhereColumnNotExists);
    testfw::Run("语义: JOIN表不存在", TestJoinTableNotExists);
    testfw::Run("语义: 合法INSERT通过", TestValidInsertPasses);
    testfw::Run("语义: 插入不存在的表", TestInsertTableNotExists);
    testfw::Run("语义: 插入不存在的列", TestInsertColumnNotExists);
    testfw::Run("语义: 合法UPDATE通过", TestValidUpdatePasses);
    testfw::Run("语义: 更新不存在的列", TestUpdateColumnNotExists);
    testfw::Run("语义: 更新不存在的表", TestUpdateTableNotExists);
    testfw::Run("语义: 合法DELETE通过", TestValidDeletePasses);
    testfw::Run("语义: 删除不存在的表", TestDeleteTableNotExists);
    testfw::Run("语义: 合法CREATE通过并登记", TestValidCreatePasses);
    testfw::Run("语义: 删除不存在的表(DROP)", TestDropTableNotExists);
    testfw::Run("语义: DROP后引用失败", TestDropThenReferenceFails);
    testfw::Run("语义: 错误信息非空", TestErrorMessageNonEmpty);
    testfw::Run("语义: 错误收集与清空", TestErrorCollectorAccumulates);
    return testfw::Summary("semantic_test");
}
