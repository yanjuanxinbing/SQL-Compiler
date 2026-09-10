// 存储引擎测试：覆盖 Value 类型与序列化、Tuple 序列化往返、
// TableHeap 槽位页的插入/读取/墓碑删除/原地更新/多页扫描。
#include "test_framework.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#include "storage/BufferPoolManager.h"
#include "storage/DiskManager.h"
#include "storage_engine/TableHeap.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

using namespace sqlcompiler;

namespace {

constexpr const char* kHeapDbFile = "test_heap.db";

// 测试数据三列结构: (INT id, VARCHAR name, FLOAT score)
const std::vector<ValueType> kColumnTypes = {
    ValueType::INTEGER, ValueType::VARCHAR, ValueType::FLOAT};

Tuple MakeRow(int32_t id, const std::string& name, double score) {
    return Tuple({Value::MakeInt(id), Value::MakeVarchar(name), Value::MakeFloat(score)});
}

// 打开一个全新的数据库文件并返回三件套（自动清理）
struct HeapEnv {
    std::string file;
    std::unique_ptr<DiskManager> dm;
    std::unique_ptr<BufferPoolManager> bpm;
    std::unique_ptr<TableHeap> heap;

    static HeapEnv Create(const std::string& file, size_t pool_pages = 16) {
        std::filesystem::remove(file);
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f.close();
        HeapEnv env;
        env.file = file;
        env.dm = std::make_unique<DiskManager>(file);
        env.bpm = std::make_unique<BufferPoolManager>(pool_pages, env.dm.get());
        env.heap = std::unique_ptr<TableHeap>(TableHeap::Create(env.bpm.get()));
        return env;
    }
    ~HeapEnv() { std::filesystem::remove(file); }
};

size_t CountRows(TableHeap* heap) {
    size_t n = 0;
    for (auto it = heap->Begin(); it.HasNext(); ) {
        it.Next(kColumnTypes);
        ++n;
    }
    return n;
}

// ---- Value ----

void TestValueTypeFromString() {
    CHECK(ValueTypeFromString("INT") == ValueType::INTEGER);
    CHECK(ValueTypeFromString("VARCHAR") == ValueType::VARCHAR);
    CHECK(ValueTypeFromString("FLOAT") == ValueType::FLOAT);
}

void TestValueFactories() {
    Value i = Value::MakeInt(42);
    CHECK(!i.IsNull());
    CHECK(i.GetType() == ValueType::INTEGER);
    CHECK_EQ(i.AsInt(), 42);

    Value f = Value::MakeFloat(2.5);
    CHECK(f.GetType() == ValueType::FLOAT);
    CHECK_EQ(f.AsFloat(), 2.5);

    Value s = Value::MakeVarchar("hello");
    CHECK(s.GetType() == ValueType::VARCHAR);
    CHECK_EQ(s.AsVarchar(), std::string("hello"));

    Value n = Value::MakeNull();
    CHECK(n.IsNull());
    CHECK(n.GetType() == ValueType::NULL_TYPE);
}

void TestValueToString() {
    CHECK_EQ(Value::MakeInt(123).ToString(), std::string("123"));
    CHECK_EQ(Value::MakeVarchar("abc").ToString(), std::string("abc"));
}

void TestValueCompare() {
    CHECK(Value::Compare(Value::MakeInt(1), Value::MakeInt(2)) < 0);
    CHECK(Value::Compare(Value::MakeInt(2), Value::MakeInt(1)) > 0);
    CHECK(Value::Compare(Value::MakeInt(7), Value::MakeInt(7)) == 0);
    CHECK(Value::Compare(Value::MakeVarchar("abc"), Value::MakeVarchar("abd")) < 0);
    CHECK(Value::Compare(Value::MakeVarchar("b"), Value::MakeVarchar("a")) > 0);
    CHECK(Value::Compare(Value::MakeFloat(1.5), Value::MakeFloat(1.5)) == 0);
}

void TestValueSerializeRoundtrip() {
    // INT
    {
        Value v = Value::MakeInt(-98765);
        std::vector<char> buf(v.SerializedSize());
        CHECK_EQ(v.SerializeTo(buf.data()), v.SerializedSize());
        Value out;
        Value::DeserializeFrom(buf.data(), ValueType::INTEGER, &out);
        CHECK_EQ(Value::Compare(v, out), 0);
    }
    // FLOAT
    {
        Value v = Value::MakeFloat(3.14159);
        std::vector<char> buf(v.SerializedSize());
        v.SerializeTo(buf.data());
        Value out;
        Value::DeserializeFrom(buf.data(), ValueType::FLOAT, &out);
        CHECK_EQ(Value::Compare(v, out), 0);
    }
    // VARCHAR（含中文长度场景）
    {
        Value v = Value::MakeVarchar("hello world");
        std::vector<char> buf(v.SerializedSize());
        v.SerializeTo(buf.data());
        Value out;
        Value::DeserializeFrom(buf.data(), ValueType::VARCHAR, &out);
        CHECK_EQ(Value::Compare(v, out), 0);
    }
    // NULL
    {
        Value v = Value::MakeNull();
        std::vector<char> buf(v.SerializedSize());
        v.SerializeTo(buf.data());
        Value out = Value::MakeInt(1);  // 预置非NULL，验证被覆盖
        Value::DeserializeFrom(buf.data(), ValueType::NULL_TYPE, &out);
        CHECK(out.IsNull());
    }
}

// ---- Tuple ----

void TestTupleBasics() {
    Tuple t = MakeRow(1, "Alice", 90.5);
    CHECK_EQ(t.ColumnCount(), static_cast<size_t>(3));
    CHECK_EQ(t.GetValue(0).AsInt(), 1);
    CHECK_EQ(t.GetValue(1).AsVarchar(), std::string("Alice"));
    CHECK_EQ(t.GetValue(2).AsFloat(), 90.5);

    RID rid;
    CHECK(!rid.IsValid());
    rid.page_id = 3;
    rid.slot_num = 5;
    CHECK(rid.IsValid());
    t.SetRid(rid);
    CHECK(t.GetRid() == rid);
}

void TestTupleSerializeRoundtrip() {
    Tuple t = MakeRow(7, "Bob", 88.25);
    std::vector<char> data = t.Serialize();
    CHECK(!data.empty());

    Tuple out = Tuple::Deserialize(data.data(), kColumnTypes);
    CHECK_EQ(out.ColumnCount(), static_cast<size_t>(3));
    CHECK_EQ(out.GetValue(0).AsInt(), 7);
    CHECK_EQ(out.GetValue(1).AsVarchar(), std::string("Bob"));
    CHECK_EQ(out.GetValue(2).AsFloat(), 88.25);
}

void TestTupleRoundtripWithNull() {
    Tuple t({Value::MakeInt(10), Value::MakeNull(), Value::MakeFloat(1.0)});
    std::vector<char> data = t.Serialize();
    Tuple out = Tuple::Deserialize(data.data(), kColumnTypes);
    CHECK_EQ(out.GetValue(0).AsInt(), 10);
    CHECK(out.GetValue(1).IsNull());
}

// ---- TableHeap ----

void TestTableHeapInsertAndScan() {
    HeapEnv env = HeapEnv::Create(kHeapDbFile);
    CHECK(env.heap != nullptr);
    if (!env.heap) return;
    CHECK(env.heap->GetFirstPageId() >= 0);

    // 插入 300 行：约 300 * 32B = 9.6KB，必然跨越多个页
    std::vector<RID> rids;
    for (int i = 1; i <= 300; ++i) {
        RID rid;
        bool ok = env.heap->InsertTuple(MakeRow(i, "name_" + std::to_string(i), i * 0.5), &rid);
        CHECK(ok);
        rids.push_back(rid);
    }

    // 顺序扫描应读到全部 300 行，且顺序与插入一致
    CHECK_EQ(CountRows(env.heap.get()), static_cast<size_t>(300));
    auto it = env.heap->Begin();
    Tuple first = it.Next(kColumnTypes);
    CHECK_EQ(first.GetValue(0).AsInt(), 1);
}

void TestTableHeapGetTuple() {
    HeapEnv env = HeapEnv::Create(kHeapDbFile);
    if (!env.heap) return;

    RID rid_first, rid_last;
    for (int i = 1; i <= 100; ++i) {
        RID rid;
        CHECK(env.heap->InsertTuple(MakeRow(i, "row" + std::to_string(i), i), &rid));
        if (i == 1) rid_first = rid;
        if (i == 100) rid_last = rid;
    }

    Tuple t1;
    CHECK(env.heap->GetTuple(rid_first, &t1, kColumnTypes));
    CHECK_EQ(t1.GetValue(0).AsInt(), 1);
    CHECK_EQ(t1.GetValue(1).AsVarchar(), std::string("row1"));

    Tuple t2;
    CHECK(env.heap->GetTuple(rid_last, &t2, kColumnTypes));
    CHECK_EQ(t2.GetValue(0).AsInt(), 100);
    CHECK_EQ(t2.GetValue(2).AsFloat(), 100.0);
}

void TestTableHeapDeleteTombstone() {
    HeapEnv env = HeapEnv::Create(kHeapDbFile);
    if (!env.heap) return;

    std::vector<RID> rids;
    for (int i = 1; i <= 50; ++i) {
        RID rid;
        CHECK(env.heap->InsertTuple(MakeRow(i, "x", 0.0), &rid));
        rids.push_back(rid);
    }

    // 墓碑删除第 5 行（下标 4）
    CHECK(env.heap->DeleteTuple(rids[4]));
    Tuple out;
    CHECK(!env.heap->GetTuple(rids[4], &out, kColumnTypes));  // 已删记录不可读
    CHECK_EQ(CountRows(env.heap.get()), static_cast<size_t>(49));

    // 重复删除同一记录应失败
    CHECK(!env.heap->DeleteTuple(rids[4]));
}

void TestTableHeapUpdate() {
    HeapEnv env = HeapEnv::Create(kHeapDbFile);
    if (!env.heap) return;

    RID rid;
    CHECK(env.heap->InsertTuple(MakeRow(1, "old", 1.0), &rid));

    Tuple new_row = MakeRow(99, "updated", 9.9);
    CHECK(env.heap->UpdateTuple(rid, new_row));

    Tuple out;
    CHECK(env.heap->GetTuple(rid, &out, kColumnTypes));
    CHECK_EQ(out.GetValue(0).AsInt(), 99);
    CHECK_EQ(out.GetValue(1).AsVarchar(), std::string("updated"));
    CHECK_EQ(out.GetValue(2).AsFloat(), 9.9);
}

void TestTableHeapUpdateDeletedRecordFails() {
    HeapEnv env = HeapEnv::Create(kHeapDbFile);
    if (!env.heap) return;

    RID rid;
    CHECK(env.heap->InsertTuple(MakeRow(1, "a", 1.0), &rid));
    CHECK(env.heap->DeleteTuple(rid));
    // 对已删除记录的更新应失败
    CHECK(!env.heap->UpdateTuple(rid, MakeRow(2, "b", 2.0)));
}

void TestTableHeapScanSkipsDeletedAcrossPages() {
    // 足够多的行跨多个页，删除散布在不同页中的记录后扫描应跳过它们
    HeapEnv env = HeapEnv::Create(kHeapDbFile, 8);
    if (!env.heap) return;

    std::vector<RID> rids;
    for (int i = 1; i <= 400; ++i) {
        RID rid;
        CHECK(env.heap->InsertTuple(MakeRow(i, "n", 0.0), &rid));
        rids.push_back(rid);
    }
    // 删除 50/150/250/350 号（跨页分布）
    CHECK(env.heap->DeleteTuple(rids[49]));
    CHECK(env.heap->DeleteTuple(rids[149]));
    CHECK(env.heap->DeleteTuple(rids[249]));
    CHECK(env.heap->DeleteTuple(rids[349]));
    CHECK_EQ(CountRows(env.heap.get()), static_cast<size_t>(396));
}

}  // namespace

int main() {
    testfw::Run("存储引擎: 类型名映射", TestValueTypeFromString);
    testfw::Run("存储引擎: Value工厂与访问器", TestValueFactories);
    testfw::Run("存储引擎: Value转字符串", TestValueToString);
    testfw::Run("存储引擎: Value比较", TestValueCompare);
    testfw::Run("存储引擎: Value序列化往返", TestValueSerializeRoundtrip);
    testfw::Run("存储引擎: Tuple基础操作", TestTupleBasics);
    testfw::Run("存储引擎: Tuple序列化往返", TestTupleSerializeRoundtrip);
    testfw::Run("存储引擎: Tuple含NULL往返", TestTupleRoundtripWithNull);
    testfw::Run("存储引擎: 插入300行跨页扫描", TestTableHeapInsertAndScan);
    testfw::Run("存储引擎: RID随机读取", TestTableHeapGetTuple);
    testfw::Run("存储引擎: 墓碑删除", TestTableHeapDeleteTombstone);
    testfw::Run("存储引擎: 记录更新", TestTableHeapUpdate);
    testfw::Run("存储引擎: 更新已删记录失败", TestTableHeapUpdateDeletedRecordFails);
    testfw::Run("存储引擎: 跨页删除后扫描", TestTableHeapScanSkipsDeletedAcrossPages);
    return testfw::Summary("storage_engine_test");
}
