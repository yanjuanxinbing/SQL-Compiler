// 语法分析模块测试：覆盖六类语句的 AST 结构、表达式优先级、
// JOIN/GROUP BY/ORDER BY/LIMIT 子句解析与语法错误报错。
#include "test_framework.h"

#include "ast/AST.h"
#include "common/Error.h"
#include "lexer/Lexer.h"
#include "lexer/Token.h"
#include "parser/Parser.h"

using namespace sqlcompiler;

namespace {

StatementPtr ParseOne(const std::string& sql) {
    Lexer lexer(sql);
    auto tokens = lexer.Tokenize();
    Parser parser(std::move(tokens));
    return parser.Parse();
}

const std::shared_ptr<BinaryExpr> AsBinary(const ExprPtr& e) {
    return std::dynamic_pointer_cast<BinaryExpr>(e);
}
const std::shared_ptr<LiteralExpr> AsLiteral(const ExprPtr& e) {
    return std::dynamic_pointer_cast<LiteralExpr>(e);
}
const std::shared_ptr<ColumnRefExpr> AsColumn(const ExprPtr& e) {
    return std::dynamic_pointer_cast<ColumnRefExpr>(e);
}

// ---- SELECT ----

void TestSelectBasic() {
    auto stmt = ParseOne("SELECT id, name FROM student;");
    CHECK_EQ(stmt->GetType(), NodeType::SELECT_STMT);
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt);
    CHECK(sel != nullptr);
    if (!sel) return;
    CHECK_EQ(sel->from_table, std::string("student"));
    CHECK_EQ(sel->select_list.size(), static_cast<size_t>(2));
    CHECK(!sel->is_distinct);
    CHECK(sel->where_clause == nullptr);
    CHECK(sel->limit == -1);
    auto c0 = AsColumn(sel->select_list[0]);
    auto c1 = AsColumn(sel->select_list[1]);
    CHECK(c0 != nullptr);
    CHECK(c1 != nullptr);
    if (c0) CHECK_EQ(c0->column_name, std::string("id"));
    if (c1) CHECK_EQ(c1->column_name, std::string("name"));
}

void TestSelectStar() {
    auto stmt = ParseOne("SELECT * FROM t;");
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt);
    CHECK(sel != nullptr);
    if (!sel) return;
    CHECK_EQ(sel->select_list.size(), static_cast<size_t>(1));
}

void TestSelectWhereCondition() {
    auto stmt = ParseOne("SELECT id FROM student WHERE age > 18;");
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt);
    CHECK(sel != nullptr);
    if (!sel) return;
    auto where = AsBinary(sel->where_clause);
    CHECK(where != nullptr);
    if (!where) return;
    CHECK(where->op == BinaryOperator::GREATER);
    auto lhs = AsColumn(where->left);
    auto rhs = AsLiteral(where->right);
    CHECK(lhs != nullptr);
    CHECK(rhs != nullptr);
    if (lhs) CHECK_EQ(lhs->column_name, std::string("age"));
    if (rhs) CHECK_EQ(rhs->literal_type, LiteralType::INTEGER);
}

void TestSelectFullClauses() {
    auto stmt = ParseOne(
        "SELECT DISTINCT age, COUNT(*) FROM student WHERE id > 0 "
        "GROUP BY age HAVING COUNT(*) > 1 "
        "ORDER BY age ASC, id DESC LIMIT 10;");
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt);
    CHECK(sel != nullptr);
    if (!sel) return;
    CHECK(sel->is_distinct);
    CHECK_EQ(sel->select_list.size(), static_cast<size_t>(2));
    CHECK(sel->where_clause != nullptr);
    CHECK_EQ(sel->group_by.size(), static_cast<size_t>(1));
    CHECK(sel->having_clause != nullptr);
    CHECK_EQ(sel->order_by.size(), static_cast<size_t>(2));
    CHECK(sel->order_by[0].ascending);
    CHECK(!sel->order_by[1].ascending);
    CHECK_EQ(sel->limit, 10);
}

void TestSelectJoinTypes() {
    auto inner = std::dynamic_pointer_cast<SelectStatement>(
        ParseOne("SELECT * FROM a INNER JOIN b ON a.id = b.id;"));
    CHECK(inner != nullptr);
    if (inner) {
        CHECK_EQ(inner->joins.size(), static_cast<size_t>(1));
        if (inner->joins.size() == 1) {
            CHECK(inner->joins[0].join_type == JoinType::INNER);
            CHECK(inner->joins[0].on_condition != nullptr);
            CHECK_EQ(inner->joins[0].table_name, std::string("b"));
        }
    }
    auto left = std::dynamic_pointer_cast<SelectStatement>(
        ParseOne("SELECT * FROM a LEFT JOIN b ON a.id = b.id;"));
    CHECK(left != nullptr);
    if (left && left->joins.size() == 1) CHECK(left->joins[0].join_type == JoinType::LEFT);

    auto right = std::dynamic_pointer_cast<SelectStatement>(
        ParseOne("SELECT * FROM a RIGHT JOIN b ON a.id = b.id;"));
    CHECK(right != nullptr);
    if (right && right->joins.size() == 1) CHECK(right->joins[0].join_type == JoinType::RIGHT);
}

void TestQualifiedColumnRef() {
    auto stmt = ParseOne("SELECT t.id FROM t;");
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt);
    CHECK(sel != nullptr);
    if (!sel || sel->select_list.empty()) return;
    auto col = AsColumn(sel->select_list[0]);
    CHECK(col != nullptr);
    if (col) {
        CHECK_EQ(col->table_name, std::string("t"));
        CHECK_EQ(col->column_name, std::string("id"));
    }
}

void TestFunctionCallExpr() {
    auto stmt = ParseOne("SELECT COUNT(*) FROM t;");
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt);
    CHECK(sel != nullptr);
    if (!sel || sel->select_list.empty()) return;
    CHECK_EQ(sel->select_list[0]->GetType(), NodeType::FUNCTION_CALL_EXPR);
}

// ---- INSERT / UPDATE / DELETE ----

void TestInsertSingleRow() {
    auto stmt = ParseOne("INSERT INTO student(id, name, age) VALUES (1, 'Alice', 20);");
    CHECK_EQ(stmt->GetType(), NodeType::INSERT_STMT);
    auto ins = std::dynamic_pointer_cast<InsertStatement>(stmt);
    CHECK(ins != nullptr);
    if (!ins) return;
    CHECK_EQ(ins->table_name, std::string("student"));
    CHECK_EQ(ins->columns.size(), static_cast<size_t>(3));
    CHECK_EQ(ins->values_list.size(), static_cast<size_t>(1));
    CHECK_EQ(ins->values_list[0].size(), static_cast<size_t>(3));
    auto v0 = AsLiteral(ins->values_list[0][0]);
    CHECK(v0 != nullptr);
    if (v0) CHECK_EQ(v0->value, std::string("1"));
    auto v1 = AsLiteral(ins->values_list[0][1]);
    CHECK(v1 != nullptr);
    if (v1) CHECK(v1->literal_type == LiteralType::STRING);
}

void TestInsertMultiRowNoColumnList() {
    auto stmt = ParseOne("INSERT INTO student VALUES (1, 'A', 20), (2, 'B', 21);");
    auto ins = std::dynamic_pointer_cast<InsertStatement>(stmt);
    CHECK(ins != nullptr);
    if (!ins) return;
    CHECK(ins->columns.empty());  // 未显式指定列 -> 按表定义顺序
    CHECK_EQ(ins->values_list.size(), static_cast<size_t>(2));
    CHECK_EQ(ins->values_list[1][0]->GetType(), NodeType::LITERAL_EXPR);
}

void TestUpdateStatement() {
    auto stmt = ParseOne("UPDATE student SET age = 21, name = 'Bob' WHERE id = 1;");
    CHECK_EQ(stmt->GetType(), NodeType::UPDATE_STMT);
    auto upd = std::dynamic_pointer_cast<UpdateStatement>(stmt);
    CHECK(upd != nullptr);
    if (!upd) return;
    CHECK_EQ(upd->table_name, std::string("student"));
    CHECK_EQ(upd->assignments.size(), static_cast<size_t>(2));
    CHECK_EQ(upd->assignments[0].first, std::string("age"));
    CHECK_EQ(upd->assignments[1].first, std::string("name"));
    CHECK(upd->where_clause != nullptr);
}

void TestUpdateWithoutWhere() {
    auto stmt = ParseOne("UPDATE student SET age = 0;");
    auto upd = std::dynamic_pointer_cast<UpdateStatement>(stmt);
    CHECK(upd != nullptr);
    if (!upd) return;
    CHECK(upd->where_clause == nullptr);
}

void TestDeleteStatement() {
    auto stmt = ParseOne("DELETE FROM student WHERE id = 1;");
    CHECK_EQ(stmt->GetType(), NodeType::DELETE_STMT);
    auto del = std::dynamic_pointer_cast<DeleteStatement>(stmt);
    CHECK(del != nullptr);
    if (!del) return;
    CHECK_EQ(del->table_name, std::string("student"));
    CHECK(del->where_clause != nullptr);
}

void TestDeleteWithoutWhere() {
    auto stmt = ParseOne("DELETE FROM student;");
    auto del = std::dynamic_pointer_cast<DeleteStatement>(stmt);
    CHECK(del != nullptr);
    if (!del) return;
    CHECK(del->where_clause == nullptr);
}

// ---- DDL ----

void TestCreateTable() {
    auto stmt = ParseOne(
        "CREATE TABLE student (id INT PRIMARY KEY, name VARCHAR NOT NULL, age INT);");
    CHECK_EQ(stmt->GetType(), NodeType::CREATE_TABLE_STMT);
    auto create = std::dynamic_pointer_cast<CreateTableStatement>(stmt);
    CHECK(create != nullptr);
    if (!create) return;
    CHECK_EQ(create->table_name, std::string("student"));
    CHECK_EQ(create->columns.size(), static_cast<size_t>(3));
    CHECK_EQ(create->columns[0].column_name, std::string("id"));
    CHECK_EQ(create->columns[0].data_type, std::string("INT"));
    CHECK(create->columns[0].is_primary_key);
    CHECK(!create->columns[0].is_not_null);
    CHECK_EQ(create->columns[1].column_name, std::string("name"));
    CHECK(create->columns[1].is_not_null);
    CHECK(!create->columns[1].is_primary_key);
}

void TestDropTable() {
    auto stmt = ParseOne("DROP TABLE student;");
    CHECK_EQ(stmt->GetType(), NodeType::DROP_TABLE_STMT);
    auto drop = std::dynamic_pointer_cast<DropTableStatement>(stmt);
    CHECK(drop != nullptr);
    if (drop) CHECK_EQ(drop->table_name, std::string("student"));
}

// ---- 表达式优先级 ----

void TestPrecedenceAndOverOr() {
    // AND 优先级高于 OR：根节点应为 OR，右子树为 AND
    auto stmt = ParseOne("SELECT * FROM t WHERE a = 1 OR b = 2 AND c = 3;");
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt);
    CHECK(sel != nullptr);
    if (!sel) return;
    auto root = AsBinary(sel->where_clause);
    CHECK(root != nullptr);
    if (!root) return;
    CHECK(root->op == BinaryOperator::OR);
    auto right = AsBinary(root->right);
    CHECK(right != nullptr);
    if (right) CHECK(right->op == BinaryOperator::AND);
}

void TestPrecedenceMulOverAdd() {
    auto stmt = ParseOne("SELECT * FROM t WHERE x = 1 + 2 * 3;");
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt);
    CHECK(sel != nullptr);
    if (!sel) return;
    auto eq = AsBinary(sel->where_clause);
    CHECK(eq != nullptr);
    if (!eq) return;
    CHECK(eq->op == BinaryOperator::EQUAL);
    auto add = AsBinary(eq->right);
    CHECK(add != nullptr);
    if (!add) return;
    CHECK(add->op == BinaryOperator::ADD);
    auto mul = AsBinary(add->right);
    CHECK(mul != nullptr);
    if (mul) CHECK(mul->op == BinaryOperator::MUL);
}

void TestParenthesesOverridePrecedence() {
    auto stmt = ParseOne("SELECT * FROM t WHERE x = (1 + 2) * 3;");
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt);
    CHECK(sel != nullptr);
    if (!sel) return;
    auto eq = AsBinary(sel->where_clause);
    CHECK(eq != nullptr);
    if (!eq) return;
    auto mul = AsBinary(eq->right);
    CHECK(mul != nullptr);
    if (!mul) return;
    CHECK(mul->op == BinaryOperator::MUL);
    auto add = AsBinary(mul->left);
    CHECK(add != nullptr);
    if (add) CHECK(add->op == BinaryOperator::ADD);
}

void TestUnaryExpr() {
    auto stmt = ParseOne("SELECT * FROM t WHERE NOT a = 1;");
    auto sel = std::dynamic_pointer_cast<SelectStatement>(stmt);
    CHECK(sel != nullptr);
    if (!sel) return;
    CHECK_EQ(sel->where_clause->GetType(), NodeType::UNARY_EXPR);
}

// ---- 多语句与错误 ----

void TestParseAllMultipleStatements() {
    Lexer lexer("INSERT INTO t VALUES (1); SELECT * FROM t; DROP TABLE t;");
    Parser parser(lexer.Tokenize());
    auto stmts = parser.ParseAll();
    CHECK_EQ(stmts.size(), static_cast<size_t>(3));
    CHECK(stmts[0]->GetType() == NodeType::INSERT_STMT);
    CHECK(stmts[1]->GetType() == NodeType::SELECT_STMT);
    CHECK(stmts[2]->GetType() == NodeType::DROP_TABLE_STMT);
}

void TestSyntaxErrorMissingExpression() {
    try {
        ParseOne("SELECT FROM t;");
        CHECK(false);
    } catch (const CompilerException& e) {
        CHECK_EQ(e.GetStage(), ErrorStage::SYNTAX);
    }
}

void TestSyntaxErrorUnclosedParen() {
    CHECK_THROW(ParseOne("SELECT * FROM t WHERE (a = 1;"));
}

void TestSyntaxErrorIncompleteInsert() {
    CHECK_THROW(ParseOne("INSERT INTO t VALUES (1, 2,;"));
}

void TestSyntaxErrorIncompleteCreate() {
    CHECK_THROW(ParseOne("CREATE TABLE t (id INT, name VARCHAR;"));
}

void TestSyntaxErrorTrailingWhere() {
    CHECK_THROW(ParseOne("DELETE FROM t WHERE;"));
}

void TestSyntaxErrorUnknownStatement() {
    try {
        ParseOne("GRANT ALL ON t TO user;");
        CHECK(false);
    } catch (const CompilerException& e) {
        CHECK_EQ(e.GetStage(), ErrorStage::SYNTAX);
    }
}

}  // namespace

int main() {
    testfw::Run("语法: 基础SELECT", TestSelectBasic);
    testfw::Run("语法: SELECT星号", TestSelectStar);
    testfw::Run("语法: WHERE条件结构", TestSelectWhereCondition);
    testfw::Run("语法: 完整子句(DISTINCT/GROUP/HAVING/ORDER/LIMIT)", TestSelectFullClauses);
    testfw::Run("语法: 三种JOIN类型", TestSelectJoinTypes);
    testfw::Run("语法: 限定列名t.col", TestQualifiedColumnRef);
    testfw::Run("语法: 函数调用表达式", TestFunctionCallExpr);
    testfw::Run("语法: 单行INSERT", TestInsertSingleRow);
    testfw::Run("语法: 多行INSERT无列清单", TestInsertMultiRowNoColumnList);
    testfw::Run("语法: UPDATE语句", TestUpdateStatement);
    testfw::Run("语法: 无WHERE的UPDATE", TestUpdateWithoutWhere);
    testfw::Run("语法: DELETE语句", TestDeleteStatement);
    testfw::Run("语法: 无WHERE的DELETE", TestDeleteWithoutWhere);
    testfw::Run("语法: CREATE TABLE约束", TestCreateTable);
    testfw::Run("语法: DROP TABLE", TestDropTable);
    testfw::Run("语法: AND优先于OR", TestPrecedenceAndOverOr);
    testfw::Run("语法: 乘法优先于加法", TestPrecedenceMulOverAdd);
    testfw::Run("语法: 括号改变优先级", TestParenthesesOverridePrecedence);
    testfw::Run("语法: 一元NOT表达式", TestUnaryExpr);
    testfw::Run("语法: 多语句解析", TestParseAllMultipleStatements);
    testfw::Run("语法: 错误-缺表达式", TestSyntaxErrorMissingExpression);
    testfw::Run("语法: 错误-括号未闭合", TestSyntaxErrorUnclosedParen);
    testfw::Run("语法: 错误-INSERT不完整", TestSyntaxErrorIncompleteInsert);
    testfw::Run("语法: 错误-CREATE不完整", TestSyntaxErrorIncompleteCreate);
    testfw::Run("语法: 错误-WHERE后缺失条件", TestSyntaxErrorTrailingWhere);
    testfw::Run("语法: 错误-非法语句开头", TestSyntaxErrorUnknownStatement);
    return testfw::Summary("parser_test");
}
