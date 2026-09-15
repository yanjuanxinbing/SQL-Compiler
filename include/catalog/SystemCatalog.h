#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <vector>

#include "catalog/IndexInfo.h"
#include "index/BPlusTree.h"
#include "semantic/SymbolTable.h"
#include "storage/BufferPoolManager.h"
#include "storage/StorageAccess.h"
#include "storage_engine/TableHeap.h"

namespace sqlcompiler {

// 前向声明：避免 catalog 头引入 txn/LogManager 完整定义。
class LogManager;

// 系统目录（System Catalog）：维护数据库的元数据（表名、列名、列类型等）。
//
// 要求"系统目录本身作为一张特殊的表进行存储和管理"，因此设计为：
//   - 内存态：复用编译器模块的SymbolTable，供SemanticAnalyzer/Planner快速查询；
//   - 持久态：将所有表的元数据序列化后，存放在一张固定的、
//     数据库启动时即存在的堆表 sys_tables 中（其首页page_id通常约定为0，
//     由Bootstrap()负责在数据库初始化时创建）。
//   - 数据库启动时通过LoadFromDisk()把sys_tables堆表中的记录反序列化，
//     重建内存态的SymbolTable与各表的TableHeap句柄。
//
// 存储接入：构造时接受 StorageAccess*（统一的存储门面）。BPlusTree 等需要
// 直接持有 BufferPoolManager 的模块从 storage_ 取 BPM 引用。
class SystemCatalog {
public:
    explicit SystemCatalog(StorageAccess* storage);
    ~SystemCatalog();

    // ---- Phase B：注入 LogManager，让 catalog 持有的 TableHeap / BPlusTree
    // 也写 WAL。恢复完成后所有 heap 都获得同一 log_manager 引用。
    void SetLogManager(LogManager* lm);

    // 把当前事务挂到 catalog 持有的所有 TableHeap 与 BPlusTree 上，
    // 让 sys_tables / sys_indexes 的写路径在 WAL 中记录正确 txn_id。
    // CreateTableExecutor / CreateIndexExecutor 等 DDL 算子在调用 catalog
    // 写入前调用一次，写入后置回 nullptr。
    void SetActiveTransaction(Transaction* txn);

    // 全新数据库首次创建时调用：初始化sys_tables自身的存储结构
    void Bootstrap();

    // 已存在的数据库启动时调用：从sys_tables堆表加载所有表的元数据到内存，
    // 并为每张用户表重建TableHeap句柄
    void LoadFromDisk();

    // 注册一张新表：写入内存态SymbolTable，并将其元数据持久化到sys_tables，
    // 同时为该表分配一个新的TableHeap（数据存储堆）
    bool CreateTable(const TableInfo& table_info);

    // 删除一张表的元数据记录（对应数据页的回收由调用方结合TableHeap完成）
    bool DropTable(const std::string& table_name);

    // 清空一张表中的所有数据（保留表结构）。表不存在返回false。
    bool TruncateTable(const std::string& table_name);

    bool HasTable(const std::string& table_name) const;
    const TableInfo* GetTable(const std::string& table_name) const;

    // ---- 46_meta: 内省接口 ----
    // 列出 catalog 中的所有基本表（不含视图、不含系统目录表 __sys_*）。
    // 用于 SHOW TABLES。
    std::vector<std::string> ListAllTables() const;
    // 表的列信息。SHOW COLUMNS FROM t 直接读取这一副本，避免把 const TableInfo
    // 引用泄露出去后被外部误改。表不存在时返回空 vector。
    std::vector<ColumnInfo> GetColumnInfos(const std::string& table_name) const;
    // 重建 CREATE TABLE 文本：当 catalog 没有保留 create_sql blob 时，
    // 根据当前 schema 推断一个最贴近原语句的 SQL 字符串。
    // SHOW CREATE TABLE 使用本接口输出。表不存在时返回空串。
    std::string BuildCreateTableSQL(const std::string& table_name) const;

    // 获取某张表对应的数据存储堆，供执行引擎读写记录；表不存在返回nullptr
    TableHeap* GetTableHeap(const std::string& table_name);

    // 提供内存态元数据视图，供语义分析/计划生成阶段复用（避免与编译器模块重复实现）
    SymbolTable& GetSymbolTable();

    // ---- 索引管理 ----
    //
    // 索引元数据存放在另一张系统堆 __sys_indexes__ 中，而不是塞进表元数据 blob。
    // 理由：CREATE/DROP INDEX 会频繁改写索引元数据，若与表元数据同处一条记录，
    // 每次建索引都要重写整张表的定义，徒增写放大与损坏风险。

    // 创建索引：校验可建性、建空树、持久化元数据。不做数据回填（由调用方决定）。
    // 失败时通过 error 返回原因。
    bool CreateIndex(const IndexInfo& index_info, std::string* error);
    bool DropIndex(const std::string& index_name);
    // 把某张表的所有索引重置为空树（TRUNCATE 用）。索引定义保留，内容清空。
    void ResetIndexesOfTable(const std::string& table_name);

    const IndexInfo* GetIndex(const std::string& index_name) const;
    BPlusTree* GetIndexTree(const std::string& index_name);
    // 某张表上的全部索引（含主键索引）
    std::vector<const IndexInfo*> GetIndexesForTable(const std::string& table_name) const;
    // 覆盖指定主键列组的唯一索引；没有则返回 nullptr（此时约束校验回退到全表扫描）
    BPlusTree* GetPrimaryKeyIndexTree(const std::string& table_name,
                                      const std::vector<std::string>& pk_columns);

    // ---- 40_txn_view_udf：视图 / UDF / 触发器注册表 ----
    //
    // 视图：CREATE VIEW name AS <select>。我们保存原始 SELECT 语句的 AST 副本，
    // SELECT FROM view 时由 Semantic/Planner 把它翻译成子计划。
    // UDF：CREATE FUNCTION name(args) RETURNS type RETURN expr。保存参数列表
    // 与返回表达式，调用时把参数值代入表达式求值。
    // 触发器：仅记录存在性，no-op 执行（最小可用实现）。
    //
    // 当前实现：仅内存态，不持久化；与 Catalog 的现有内存 SymbolTable 风格一致。
    // 重新启动数据库时这些对象会丢失——对最小实现足够。
    struct ViewDefinition {
        std::string view_name;
        SelectStatementPtr query;
        // 60_view_trigger (Category 9): WITH [CASCADED|LOCAL] CHECK OPTION 状态。
        // has_check_option=true 时视图的 WHERE 表达式需要在 INSERT/UPDATE 通过该
        // 视图时进行校验；check_option_cascaded=true 表示 CASCADED，默认 LOCAL。
        ExprPtr check_option_where = nullptr;
        bool has_check_option = false;
        bool check_option_cascaded = false;
    };
    struct FunctionDefinition {
        std::string function_name;
        std::vector<FunctionParameter> parameters;
        std::string return_type;
        int32_t return_char_length = -1;
        // 函数体：有序语句列表（DECLARE / SET / IF / WHILE / RETURN 等）。
        // 简单形式可以是单条 RETURN expr，由 UdfExecutor 顺序执行。
        std::vector<StatementPtr> body_statements;
    };
    struct TriggerDefinition {
        std::string trigger_name;
        TriggerTiming timing = TriggerTiming::BEFORE;
        TriggerEvent event = TriggerEvent::INSERT;
        std::string table_name;
        // 60_view_trigger (Category 9)：FOR EACH ROW (默认 true) /
        // FOR EACH STATEMENT (false)。STATEMENT 级触发器在每条 DML 上只跑一次。
        bool for_each_row = true;
        std::vector<std::pair<std::string, ExprPtr>> assignments;
    };

    // 60_view_trigger (Category 9)：物化视图的元数据。
    // backing_table 字段是 catalog 内真实存在的一张堆表（默认名 "__mv_<view_name>"）；
    // query_text 字段保存原始 SELECT 文本，REFRESH 时由 Planner 重新解析并执行。
    struct MaterializedViewInfo {
        std::string view_name;
        std::string backing_table;     // 通常 "__mv_<view_name>"
        std::vector<ColumnDefinition> columns;  // 输出列定型
        std::string query_text;         // 原始 SELECT 文本（用于 REFRESH 重解析）
    };

    bool CreateView(const ViewDefinition& def);
    bool DropView(const std::string& view_name);
    bool HasView(const std::string& view_name) const;
    const ViewDefinition* GetView(const std::string& view_name) const;
    // 返回值同 GetView，但大小写不敏感（UDF 调用大小写差异时的回退路径）。
    const ViewDefinition* LookupView(const std::string& view_name) const;
    // 60_view_trigger: 在视图已注册后回填 WITH CHECK OPTION 配置。
    // 由 Planner::PlanCreateView 在用户写了 WITH CHECK OPTION 时调用。
    void SetViewCheckOption(const std::string& view_name,
                            ExprPtr where_expr,
                            bool cascaded);

    // 60_view_trigger (Category 9)：物化视图注册表。
    bool CreateMaterializedView(const MaterializedViewInfo& info);
    bool DropMaterializedView(const std::string& view_name);
    bool HasMaterializedView(const std::string& view_name) const;
    const MaterializedViewInfo* GetMaterializedView(const std::string& view_name) const;
    const MaterializedViewInfo* LookupMaterializedView(const std::string& view_name) const;
    // 物化视图默认 backing 表名规则："__mv_<view_name>"。
    // 视图名包含空格 / 大小写时保持原样，便于调试。
    static std::string MaterializedViewBackingTable(const std::string& view_name);

    bool CreateFunction(const FunctionDefinition& def);
    bool DropFunction(const std::string& function_name);
    bool HasFunction(const std::string& function_name) const;
    const FunctionDefinition* GetFunction(const std::string& function_name) const;
    // 大小写不敏感的函数查找：UDF 调用方可能使用与 CREATE 时不同的大小写。
    const FunctionDefinition* LookupFunction(const std::string& name) const;

    // ---- 59_procs (Category 8)：过程定义 ----
    //
    // 过程与函数共享 body_statements 的形态（DECLARE / SET / IF / WHILE /
    // LOOP / REPEAT / CASE / LEAVE / ITERATE / SIGNAL / HANDLER / CURSOR /
    // RETURN），但过程没有 RETURN 值（V1 中执行器也允许 RETURN 但忽略结果）。
    // OUT 参数由调用方按名字取回（见 ExecutionContext::out_args_）。
    struct ProcedureDefinition {
        std::string procedure_name;
        std::vector<FunctionParameter> parameters;
        std::vector<StatementPtr> body_statements;
    };

    bool CreateProcedure(const ProcedureDefinition& def);
    bool DropProcedure(const std::string& procedure_name, bool if_exists);
    bool HasProcedure(const std::string& procedure_name) const;
    const ProcedureDefinition* GetProcedure(const std::string& procedure_name) const;
    // 大小写不敏感的查找。
    const ProcedureDefinition* LookupProcedure(const std::string& name) const;

    bool CreateTrigger(const TriggerDefinition& def);
    bool DropTrigger(const std::string& trigger_name);
    bool HasTrigger(const std::string& trigger_name) const;
    // 列出匹配 (table, timing, event) 的全部触发器，按注册顺序返回。
    // 触发器内部无序约束，目前任意顺序均可。
    std::vector<const TriggerDefinition*> LookupTriggers(
        const std::string& table_name,
        TriggerTiming timing,
        TriggerEvent event) const;

    // 60_view_trigger (Category 9)：触发器持久化。
    // 把 trigger 元数据写到一张独立的 __sys_triggers__ 堆（与 __sys_indexes__
    // 类似的做法）；LoadFromDisk 时再读回内存态，让重启后 CREATE TRIGGER 的
    // 结果仍生效。
    //
    // 设计取舍：触发器体内的 assignments 是 ExprPtr（AST 节点），落盘前需要
    // 转成可重新解析的文本；恢复时用 Parser 把文本重新解析回 ExprPtr。
    // 为简化实现，assignments 序列化成一个拼接字符串（lhs1 = expr1; lhs2 = expr2; ...），
    // DROP 时按 trigger_name 精确匹配并整行删除。

    // ---- ALTER TABLE 支撑 ----
    //
    // 这些表层变更原语供执行器在 ALTER TABLE 各分支中调用：
    //   - DropPersistedTableMetadata  删除 sys_tables 中旧记录
    //   - PersistTableInfo            写入 sys_tables 新记录
    //   - RenameTableHeapKey          移动 table_heaps_ 的键（重命名用）
    //   - DropIndexesForTable         失效并清掉该表所有索引（schema 可能变化，
    //                                 重建/迁移索引超出本任务范围）
    //   - UpdateTableSchema           一站式封装：把 in-memory 状态更新到新 schema,
    //                                 并替换 sys_tables 记录。执行器负责行的字节重写。
    bool DropPersistedTableMetadata(const std::string& table_name);
    bool PersistTableInfo(const TableInfo& info);
    bool RenameTableHeapKey(const std::string& old_name, const std::string& new_name);
    void DropIndexesForTable(const std::string& table_name);
    // 应用 schema 变更到目录内存态（含表名变更与索引清理），但不重写 TableHeap 的
    // 行字节——这一部分由 AlterTableExecutor 负责，因其需要新/旧 column_types 映射。
    // new_info.table_name 必须与 old_name 相同除非是 RENAME TO。
    bool UpdateTableSchema(const std::string& old_name, const TableInfo& new_info);

    // ---- 53_ddl: 命名空间与序列 ----
    //
    // Schema：catalog 维护一组已知 schema 名称；CREATE TABLE schema.tbl 时若
    //   schema 不在集合中则报错；DROP SCHEMA 时若还有属于该 schema 的表则报错。
    // Sequence：catalog 维护一组 sequence；NEXTVAL FOR 由 ExpressionEvaluator
    //   走 NextSequence() 原子推进并返回当前值。

    bool CreateSchema(const std::string& schema_name, bool if_not_exists);
    bool DropSchema(const std::string& schema_name, bool if_exists);
    bool HasSchema(const std::string& schema_name) const;
    // 把形如 "finance.txn" 的限定名拆成 (schema, table)；schema 为空表示无限定。
    static std::pair<std::string, std::string> SplitQualifiedName(
        const std::string& maybe_qualified);

    struct SequenceState {
        int64_t current_value = 1;  // 最近一次 NEXTVAL 之前已发出的值；下一次返回 current + step
        int64_t step = 1;
        int64_t start_value = 1;
    };
    bool CreateSequence(const std::string& name, int64_t start_value,
                        int64_t step, bool if_not_exists);
    bool DropSequence(const std::string& name, bool if_exists);
    bool HasSequence(const std::string& name) const;
    // 推进序列并返回新当前值。序列不存在时返回 false。
    bool NextSequence(const std::string& name, int64_t* out_value);

    // ---- 53_ddl: FOREIGN KEY 约束元数据 ----
    //
    // 存储格式：每条 FK 关联一对 (child_table, parent_table)，按 child_table
    // 索引。执行期在 INSERT/UPDATE 子表写入时检查 parent 行存在；在 DELETE/
    // UPDATE 父表时根据 on_delete_action 决定 CASCADE / RESTRICT / SET NULL。
    // 表元数据 blob 不含 FK 部分（避免改动历史二进制布局），运行时仅维护
    // 内存态即可。
    bool AddForeignKey(const std::string& child_table,
                       const std::vector<std::string>& child_cols,
                       const std::string& parent_table,
                       const std::vector<std::string>& parent_cols,
                       int on_delete_action, int on_update_action);
    // child_table 上的全部 FK。子表不存在返回空 vector。
    std::vector<ForeignKeyDef> GetForeignKeysForChild(
        const std::string& child_table) const;
    // 引用 parent_table 的全部 FK（child + child_cols + actions）。
    // 用于 DELETE/UPDATE 父表时扫描所有需要级联 / 限制 / 置空的子行。
    std::vector<std::pair<std::string, ForeignKeyDef>> GetForeignKeysReferencing(
        const std::string& parent_table) const;

private:
    // 主存句柄：统一存储门面。TableHeap 等高层组件走 storage_；
    // BPlusTree / PageGuard 等需要直接持有 BPM 的低层组件通过
    // storage_->GetBufferPoolManager() 拿到 buffer_pool_manager_。
    StorageAccess* storage_;
    // 由 storage_ 在构造期一次性取得，供 BPlusTree 等继续使用。
    BufferPoolManager* buffer_pool_manager_;
    LogManager* log_manager_ = nullptr;  // Phase B：可选 WAL 写出器
    SymbolTable symbol_table_;  // 内存态元数据缓存

    page_id_t sys_tables_first_page_id_;  // 系统目录自身存储表的首页
    // 索引目录堆的首页。旧版本数据库没有这张堆，此时为 INVALID_PAGE_ID，
    // 首次 CREATE INDEX 时惰性创建——这样旧库文件仍能正常打开。
    page_id_t sys_indexes_first_page_id_;
    // 60_view_trigger (Category 9): 触发器目录堆的首页。旧库没有时为 INVALID_PAGE_ID，
    // 首次 CREATE TRIGGER 时惰性创建。
    page_id_t sys_triggers_first_page_id_;

    // 各用户表对应的数据堆，key为表名
    std::unordered_map<std::string, std::unique_ptr<TableHeap>> table_heaps_;

    // 索引元数据与对应的 B+Tree，key 为索引名。Catalog 持有所有权，
    // getter 返回裸指针（与 table_heaps_ 一致的所有权约定）。
    std::unique_ptr<TableHeap> index_heap_;  // __sys_indexes__ 堆
    std::unordered_map<std::string, IndexInfo> indexes_;
    std::unordered_map<std::string, std::unique_ptr<BPlusTree>> index_trees_;

    // 60_view_trigger (Category 9)：触发器目录堆与物化视图字典。
    std::unique_ptr<TableHeap> trigger_heap_;  // __sys_triggers__ 堆
    std::unordered_map<std::string, MaterializedViewInfo> materialized_views_;

    // 40_txn_view_udf：视图 / UDF / 触发器字典。
    std::unordered_map<std::string, ViewDefinition> views_;
    std::unordered_map<std::string, FunctionDefinition> functions_;
    std::unordered_map<std::string, TriggerDefinition> triggers_;
    // 59_procs (Category 8)：过程字典。
    std::unordered_map<std::string, ProcedureDefinition> procedures_;

    // 53_ddl：schema / sequence / FK 内存态。
    std::unordered_set<std::string> schemas_;
    std::unordered_map<std::string, SequenceState> sequences_;
    // FK 按 child_table 索引；每条 FK 的 child_table 字段冗余存储以便遍历。
    std::unordered_map<std::string, std::vector<ForeignKeyDef>> foreign_keys_;

    // 将一条表的元数据（表名、列定义列表）编码为记录，追加写入sys_tables堆表
    bool PersistTableMetadata(const TableInfo& table_info);

    // 将sys_tables堆表中的一条记录解码为TableInfo
    TableInfo DecodeTableMetadata(const Tuple& tuple) const;

    // ---- 索引目录内部实现 ----
    // 确保 __sys_indexes__ 堆存在（必要时创建并把首页 id 记入 sys_tables）
    bool EnsureSysIndexesHeap();
    bool PersistIndexMetadata(const IndexInfo& index_info);
    // 从 __sys_indexes__ 删除某条索引元数据
    void RemoveIndexMetadata(const std::string& index_name);
    // 从 __sys_indexes__ 重建全部索引元数据与 B+Tree 句柄
    void LoadIndexesFromDisk();
    // 打开一棵已持久化的索引树，失败返回 false
    bool OpenIndexTree(const IndexInfo& index_info);
    // 删除某张表的全部索引（含 B+Tree 页面回收）
    void DropIndexesOfTable(const std::string& table_name);

    // ---- 60_view_trigger (Category 9)：触发器目录内部实现 ----
    // 确保 __sys_triggers__ 堆存在（必要时创建并把首页 id 记入 sys_tables）。
    bool EnsureSysTriggersHeap();
    // 把一条 trigger 元数据写到 __sys_triggers__ 堆。
    bool PersistTriggerMetadata(const TriggerDefinition& def);
    // 从 __sys_triggers__ 删除 trigger_name 对应的那条记录。
    void RemoveTriggerMetadata(const std::string& trigger_name);
    // 从 __sys_triggers__ 重新加载全部触发器到内存态。
    void LoadTriggersFromDisk();
    // 把 trigger 的 assignments 列表序列化为单一字符串（"lhs1 = expr1; lhs2 = expr2; ..."）。
    static std::string SerializeTriggerAssignments(
        const std::vector<std::pair<std::string, ExprPtr>>& assignments);
    // 反序列化（用 Parser 把字符串重新解析为 ExprPtr）；失败返回空 vector。
    static std::vector<std::pair<std::string, ExprPtr>> DeserializeTriggerAssignments(
        const std::string& text);
};

}  // namespace sqlcompiler
