#pragma once

#include <memory>
#include <string>
#include <unordered_map>

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

    bool HasTable(const std::string& table_name) const;
    const TableInfo* GetTable(const std::string& table_name) const;

    // 获取某张表对应的数据存储堆，供执行引擎读写记录；表不存在返回nullptr
    TableHeap* GetTableHeap(const std::string& table_name);

    // 提供内存态元数据视图，供语义分析/计划生成阶段复用（避免与编译器模块重复实现）
    SymbolTable& GetSymbolTable();

private:
    BufferPoolManager* buffer_pool_manager_;
    SymbolTable symbol_table_;  // 内存态元数据缓存

    page_id_t sys_tables_first_page_id_;  // 系统目录自身存储表的首页

    // 各用户表对应的数据堆，key为表名
    std::unordered_map<std::string, std::unique_ptr<TableHeap>> table_heaps_;

    // 将一条表的元数据（表名、列定义列表）编码为记录，追加写入sys_tables堆表
    bool PersistTableMetadata(const TableInfo& table_info);

    // 将sys_tables堆表中的一条记录解码为TableInfo
    TableInfo DecodeTableMetadata(const Tuple& tuple) const;
};

}  // namespace sqlcompiler
