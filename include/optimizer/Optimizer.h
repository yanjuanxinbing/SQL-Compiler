#pragma once

#include "ast/AST.h"
#include "catalog/SystemCatalog.h"
#include "plan/Plan.h"

#include <unordered_set>

namespace sqlcompiler {

// 查询优化器：对逻辑执行计划进行等价改写以提升执行效率
class Optimizer {
public:
    // catalog 可为空：此时不做基于索引的访问路径选择，其余优化照常。
    // 让它可空是为了保证优化器在没有目录的场景（如单元测试）仍可独立使用。
    explicit Optimizer(SystemCatalog* catalog = nullptr);

    // 优化入口：输入原始逻辑计划，返回优化后的逻辑计划
    PlanNodePtr Optimize(PlanNodePtr plan);

private:
    // 谓词下推：将Filter尽可能下推到靠近数据源的位置
    PlanNodePtr PushDownPredicates(PlanNodePtr plan);

    // 访问路径选择：把 Filter -> SeqScan 改写成 IndexScan
    PlanNodePtr ChooseAccessPaths(PlanNodePtr plan);

    // U3-1：JOIN 顺序启发式。对「全 INNER 的左深 JOIN 链」按估计基数小表驱动重排，
    // 支持 3 表以上；非安全形态（含 OUTER JOIN / 非左深 / 条件含未限定列）保持原顺序。
    PlanNodePtr ReorderJoins(PlanNodePtr plan);

    // U3-2：聚合下推。把「单表 + 全部裸 COUNT/SUM（无 DISTINCT/AVG/MIN/MAX/标量包装）」
    // 的 AggregateNode 改写为 PreAggScanNode：聚合状态在扫描层直接累计、分组用哈希表，
    // 替代 AggregateExecutor 的线性扫描分组。非合格形态保持原样零回归。
    PlanNodePtr PushDownAggregates(PlanNodePtr plan);

    // 判定 AggregateNode 是否可安全改写为 PreAggScanNode：
    //   1) 子树为单表访问路径（Filter* -> SeqScan 或直接 SeqScan，无 JOIN/Project 等）；
    //   2) 每个 aggregate_expr 为「裸 COUNT/SUM」或「不含任何聚合调用的普通表达式」；
    //   3) GROUP BY 表达式不含聚合调用（防御）。
    bool IsPreAggEligible(const PlanNodePtr& agg_node) const;

    // 判断表达式是否为裸 COUNT/SUM：FunctionCallExpr、无 DISTINCT、参数 ≤1，
    // 且参数（若有）不含嵌套聚合/窗口调用（PreAggScanExecutor 会在扫描行上直接求值参数）。
    bool IsBareCountSumExpr(const ExprPtr& e) const;

    // 尝试重排一棵左深 JOIN 子树（in-place）。仅在可行时修改并返回 true。
    bool TryReorderJoinSubtree(PlanNodePtr& subtree);

    // 收集一棵左 deep JOIN 子树的基础表（SeqScan）与每级 join 条件/类型。
    // 返回 false 表示不是「全 INNER + 右子恒为 SeqScan」的可重排形态。
    using SpineTables = std::vector<PlanNodePtr>;         // 叶序（最深在前）
    using SpineConds = std::vector<std::pair<ExprPtr, JoinType>>; // conds[k] 引入 tables[k+1]
    bool CollectLeftDeepSpine(const PlanNodePtr& node,
                              SpineTables& tables, SpineConds& conds) const;

    // 提取表达式中所有被引用的表限定符（ColumnRefExpr.table_name）。遇到无法
    // 遍历的复杂节点置 fail=true，调用方应中止重排以避免误挂条件。
    void CollectQualifiers(const ExprPtr& e, std::vector<std::string>& quals,
                           bool& fail) const;

    // 把表限定符解析为 SpineTables 中的下标（按别名或表名，大小写不敏感）；
    // 无法唯一解析返回 -1。
    int ResolveTableIndex(const std::string& qual, const SpineTables& tables) const;

    // ==================== U3-3：子查询去关联 ====================
    // 把 WHERE 中可改写为连接的相关子查询（EXISTS → SEMI、NOT EXISTS → ANTI、
    // IN/ANY → SEMI）从 Filter 谓词中抽出，改写为 SEMI/ANTI JOIN，消除逐行
    // 重跑子计划。非相关子查询保持原求值路径（ExecutionEngine 物化缓存）。
    // 仅在形态完全安全时改写，任何疑点都保持原样（保守守底零回归）。
    PlanNodePtr DecorrelateSubqueries(PlanNodePtr plan);

    // 尝试把一个 FilterNode 中可去关联的子查询合取项改写为 SEMI/ANTI 连接。
    // 成功返回新的计划子树根（JoinNode），失败返回 nullptr（保持原 Filter）。
    PlanNodePtr TryDecorrelateFilter(const PlanNodePtr& filter);

    // 判断子查询 AST 是否为可去关联的简单形态：单表、无 GROUP BY/HAVING/
    // ORDER BY/DISTINCT/LIMIT/派生表/JOIN、内层计划形状为
    // Project(顶层可选) → Filter* → SeqScan；IN/ANY 另要求 select_list 为单裸列。
    bool IsSubqueryDecorrelatable(const SubqueryExprNode& sq) const;

    // 内层计划形状守卫：剥掉顶层可选 Project 后只允许 Filter* 与单个 SeqScan。
    bool IsSimpleSubqueryPlanShape(const PlanNodePtr& plan) const;

    // 收集子查询 AST 的内层表限定符集合（表名 + 别名，已小写）。
    void CollectInnerQualifiers(const SelectStatement& sub,
                                std::unordered_set<std::string>& quals) const;

    // 表达式是否含嵌套子查询（SubqueryExprNode 节点）。
    bool ExprHasNestedSubquery(const ExprPtr& e) const;

    // 表达式是否引用了「不属于内层表限定符集合」的限定列（即相关列引用）。
    // 嵌套子查询节点按「非外层引用」处理（相关性判定只关心限定列）。
    bool ReferencesOuterTable(const ExprPtr& e,
                              const std::unordered_set<std::string>& inner_quals) const;

    // 表达式是否含未限定（无表名）的列引用。
    bool HasUnqualifiedColumnRef(const ExprPtr& e) const;

    // 校验连接条件中的所有限定列引用都可解析到左/右子树的扫描表，
    // 且（catalog 可用时）列名存在于对应表模式。未限定列仅当左侧单表时允许。
    bool CondRefsResolve(const ExprPtr& cond, const PlanNodePtr& left,
                         const PlanNodePtr& right) const;

    // 把表达式拆成顶层 AND 合取子句（原地返回子句列表）。
    void SplitConjuncts(const ExprPtr& expr, std::vector<ExprPtr>& out) const;

    // 把合取子句列表重新 AND 成单个表达式（空列表返回 nullptr）。
    ExprPtr CombineConjuncts(const std::vector<ExprPtr>& conjuncts) const;

    // 深层克隆内层计划（仅 PROJECT/FILTER/SEQ_SCAN 独立克隆，其余节点类型
    // 共享原指针——形状守卫保证内层计划只含这些简单节点）。用于子查询去关联时
    // 对子计划做手术（剥 Project、移除相关子句）而不污染原 subquery_plan。
    PlanNodePtr ClonePlan(const PlanNodePtr& node);

    // 在内层计划中递归移除引用外层表的 Filter 合取项；Filter 子句清空时
    // 整层移除。返回手术后的根。
    PlanNodePtr StripCorrelatedFilters(const PlanNodePtr& node,
                                       const std::unordered_set<std::string>& inner_quals);

    // 从子查询 AST 提取可改写到连接条件的「相关合取项」（WHERE 中引用外层表的子句）。
    void CollectCorrelatedConjuncts(const SelectStatement& sub,
                                    const std::unordered_set<std::string>& inner_quals,
                                    std::vector<ExprPtr>& out) const;

    SystemCatalog* catalog_;

    // 列裁剪：去除Project中未被上层使用的列
    PlanNodePtr PruneColumns(PlanNodePtr plan);

    // 常量折叠：在编译期计算表达式中的常量子表达式
    ExprPtr FoldConstants(ExprPtr expr);

    // 判断表达式是否为常量表达式（不含列引用）
    bool IsConstantExpr(const ExprPtr& expr) const;
};

}  // namespace sqlcompiler
