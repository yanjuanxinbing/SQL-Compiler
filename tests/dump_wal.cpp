// Quick WAL dumper for debugging.
// Build: g++ -std=c++17 -I../include dump_wal.cpp -o dump_wal
// Run:   ./dump_wal <wal_file>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include "txn/LogRecord.h"

using namespace sqlcompiler;

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: dump_wal <wal_file>\n"; return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { std::cerr << "cannot open " << argv[1] << "\n"; return 1; }
    std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    size_t pos = 0;
    int n = 0;
    while (pos + sizeof(uint32_t) <= raw.size()) {
        uint32_t len; std::memcpy(&len, raw.data() + pos, 4); pos += 4;
        if (pos + len > raw.size()) break;
        LogRecord rec;
        if (!DeserializeLogRecord(raw.data() + pos, len, &rec)) break;
        const char* tn = "?";
        switch (rec.type_) {
            case LogRecordType::BEGIN: tn = "BEGIN"; break;
            case LogRecordType::COMMIT: tn = "COMMIT"; break;
            case LogRecordType::ABORT: tn = "ABORT"; break;
            case LogRecordType::UPDATE: tn = "UPDATE"; break;
            case LogRecordType::CHECKPOINT: tn = "CHECKPOINT"; break;
        }
        std::cout << "#" << ++n
                  << " lsn=" << rec.lsn_
                  << " prev=" << rec.prev_lsn_
                  << " txn=" << rec.txn_id_
                  << " type=" << tn
                  << " page=" << rec.page_id_
                  << " before=" << rec.before_image_.size() << "B"
                  << " after=" << rec.after_image_.size() << "B"
                  << "\n";
        pos += len;
    }
    return 0;
}