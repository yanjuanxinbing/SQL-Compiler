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
    };
    struct FunctionDefinition {
        std::string function_name;
        std::vector<FunctionParameter> parameters;
        std::string return_type;
        int32_t return_char_length = -1;
        ExprPtr body_expr;
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

    bool CreateFunction(const FunctionDefinition& def);
    bool DropFunction(const std::string& function_name);
    bool HasFunction(const std::string& function_name) const;
    const FunctionDefinition* GetFunction(const std::string& function_name) const;

    bool CreateTrigger(const TriggerDefinition& def);
    bool DropTrigger(const std::string& trigger_name);
    bool HasTrigger(const std::string& trigger_name) const;

private:
    BufferPoolManager* buffer_pool_manager_;
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
