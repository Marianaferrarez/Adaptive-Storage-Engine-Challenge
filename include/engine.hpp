#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ed2 {

class Engine {
public:
    explicit Engine(const std::string& data_dir);
    ~Engine();

    void put(uint64_t key, const std::string& value);
    std::optional<std::string> get(uint64_t key);
    bool remove(uint64_t key);

    // Implementação ingênua (O(chaves) + sort) só para completar o
    // protocolo na Entrega 1. Entrega 2 troca isso por um índice ordenado
    // de verdade (B+Tree/LSM/arquivo ordenado).
    std::vector<std::pair<uint64_t, std::string>> scan(uint64_t start, uint64_t end);

    struct VerifyResult {
        bool ok;
        uint64_t valid_records;
        uint64_t valid_bytes;
    };
    VerifyResult verify();

    // Estado da recuperação feita ao abrir o engine (construtor): quantos
    // registros válidos foram indexados e quantos bytes no fim do log
    // tiveram que ser descartados por estarem incompletos/corrompidos.
    struct RecoveryStats {
        uint64_t valid_records = 0;
        uint64_t discarded_bytes = 0;
    };
    const RecoveryStats& recovery_stats() const { return recovery_stats_; }

private:
    struct IndexEntry {
        uint64_t offset;
    };

    void recover();

    std::string data_dir_;
    std::string log_path_;
    int log_fd_ = -1;
    std::unordered_map<uint64_t, IndexEntry> index_;
    RecoveryStats recovery_stats_;
};

}  // namespace ed2
