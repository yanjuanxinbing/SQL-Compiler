#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <vector>

#include "catalog/IndexInfo.h"
#include "index/BPlusTree.h"
#include "semantic/SymbolTable.h"
#include "storage/BufferPoolManager.h"
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
class SystemCatalog {
public:
    explicit SystemCatalog(BufferPoolManager* buffer_pool_manager);
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

    // MVCC 快照隔离：对全部表的堆做惰性真空回收（回收 begin_csn < 最老活动快照
    // 的非 head 旧版本槽位）。以最老活动快照为界防误删仍可能被读取的版本。
    void VacuumAll(int64_t oldest_active_csn);

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
        std::vector<std::pair<std::string, ExprPtr>> assignments;
    };

    bool CreateView(const ViewDefinition& def);
    bool DropView(const std::string& view_name);
    bool HasView(const std::string& view_name) const;
    const ViewDefinition* GetView(const std::string& view_name) const;
    // 返回值同 GetView，但大小写不敏感（UDF 调用大小写差异时的回退路径）。
    const ViewDefinition* LookupView(const std::string& view_name) const;

    bool CreateFunction(const FunctionDefinition& def);
    bool DropFunction(const std::string& function_name);
    bool HasFunction(const std::string& function_name) const;
    const FunctionDefinition* GetFunction(const std::string& function_name) const;
    // 大小写不敏感的函数查找：UDF 调用方可能使用与 CREATE 时不同的大小写。
    const FunctionDefinition* LookupFunction(const std::string& name) const;

    bool CreateTrigger(const TriggerDefinition& def);
    bool DropTrigger(const std::string& trigger_name);
    bool HasTrigger(const std::string& trigger_name) const;
    // 列出匹配 (table, timing, event) 的全部触发器，按注册顺序返回。
    // 触发器内部无序约束，目前任意顺序均可。
    std::vector<const TriggerDefinition*> LookupTriggers(
        const std::string& table_name,
        TriggerTiming timing,
        TriggerEvent event) const;

    // ---- ALTER TABLE 支撑 ----
    //
    // 这些表层变更原语供执行器在 ALTER TABLE 各分支中调用：
    //   - DropPersistedTableMetadata  删除 sys_tables 中旧记录
    //   - PersistTableInfo            写入 sys_tables 新记录
    //   - RenameTableHeapKey          移动 table_heaps_ 的键（重命名用）
    //   - DropIndexesForTable         失效并清掉该表所有索引（schema 可能变化，
    //                                 重建/迁移索引超出本任务范围）
    //   - UpdateTableSchema           一站式封装：把 in-memory 状态更新到新 schema，
    //                                 并替换 sys_tables 记录。执行器负责行的字节重写。
    bool DropPersistedTableMetadata(const std::string& table_name);
    bool PersistTableInfo(const TableInfo& info);
    bool RenameTableHeapKey(const std::string& old_name, const std::string& new_name);
    void DropIndexesForTable(const std::string& table_name);
    // 应用 schema 变更到目录内存态（含表名变更与索引清理），但不重写 TableHeap 的
    // 行字节——这一部分由 AlterTableExecutor 负责，因其需要新/旧 column_types 映射。
    // new_info.table_name 必须与 old_name 相同除非是 RENAME TO。
    bool UpdateTableSchema(const std::string& old_name, const TableInfo& new_info);

private:
    BufferPoolManager* buffer_pool_manager_;
    LogManager* log_manager_ = nullptr;  // Phase B：可选 WAL 写出器
    SymbolTable symbol_table_;  // 内存态元数据缓存

    page_id_t sys_tables_first_page_id_;  // 系统目录自身存储表的首页
    // 索引目录堆的首页。旧版本数据库没有这张堆，此时为 INVALID_PAGE_ID，
    // 首次 CREATE INDEX 时惰性创建——这样旧库文件仍能正常打开。
    page_id_t sys_indexes_first_page_id_;

    // 各用户表对应的数据堆，key为表名
    std::unordered_map<std::string, std::unique_ptr<TableHeap>> table_heaps_;

    // 索引元数据与对应的 B+Tree，key 为索引名。Catalog 持有所有权，
    // getter 返回裸指针（与 table_heaps_ 一致的所有权约定）。
    std::unique_ptr<TableHeap> index_heap_;  // __sys_indexes__ 堆
    std::unordered_map<std::string, IndexInfo> indexes_;
    std::unordered_map<std::string, std::unique_ptr<BPlusTree>> index_trees_;

    // 40_txn_view_udf：视图 / UDF / 触发器字典。
    std::unordered_map<std::string, ViewDefinition> views_;
    std::unordered_map<std::string, FunctionDefinition> functions_;
    std::unordered_map<std::string, TriggerDefinition> triggers_;

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
};

}  // namespace sqlcompiler
