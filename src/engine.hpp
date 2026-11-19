#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace ed2 {

// Erro lançado quando o engine detecta um problema nos arquivos persistentes
// (arquivo não reconhecido, registro corrompido etc.). É separado de
// std::runtime_error genérico para que o CLI possa diferenciá-lo nas
// mensagens, mas qualquer um dos dois vira {"status": "error"} no protocolo.
class StorageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Storage engine chave-valor persistente no estilo Bitcask:
//   - um único arquivo de log append-only (data.log) guarda todos os PUTs e
//     DELETEs (tombstones), cada registro protegido por CRC32;
//   - um índice em RAM mapeia chave -> offset do registro mais recente.
// Os VALORES nunca ficam em memória: o GET lê o registro do disco com pread.
// Ver docs/design.md para o formato completo.
class Engine {
public:
    explicit Engine(const std::string& data_dir);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void put(uint64_t key, const std::string& value);
    std::optional<std::string> get(uint64_t key);
    bool remove(uint64_t key);

    struct VerifyResult {
        bool ok = false;
        uint64_t valid_records = 0;   // registros íntegros encontrados
        uint64_t valid_bytes = 0;     // bytes íntegros (cabeçalho + registros)
        uint64_t file_bytes = 0;      // tamanho real do data.log
        uint64_t live_keys = 0;       // chaves presentes no índice
        std::string message;          // descrição do problema (vazio se ok)
    };
    // Relê o data.log inteiro do disco, revalidando cabeçalho, CRC de cada
    // registro, flags, tamanho do arquivo e a consistência do índice em RAM.
    VerifyResult verify();

    // Move os bytes inválidos do fim do log para um arquivo de quarentena
    // (data.log.corrupted-<offset>) e trunca o log no último registro íntegro.
    // Retorna quantos bytes foram movidos (0 se o arquivo já estava íntegro).
    uint64_t repair();

    // true quando a abertura encontrou bytes inválidos no data.log. Nesse
    // estado leituras funcionam (sobre a parte íntegra), mas escritas são
    // recusadas para não misturar dados novos com lixo.
    bool corrupted() const { return corrupt_offset_.has_value(); }
    const std::string& corruption_message() const { return corruption_message_; }

    size_t size() const { return index_.size(); }
    const std::string& log_path() const { return log_path_; }

private:
    struct ScanResult {
        uint64_t end_offset;        // offset logo após o último registro íntegro
        uint64_t valid_records;
        std::string error;          // vazio se o arquivo inteiro é válido
    };

    // Percorre o log a partir do cabeçalho. Se `rebuild_index` for true,
    // reconstrói index_ conforme avança.
    ScanResult scan_log(bool rebuild_index);
    void open_log();
    void ensure_writable() const;
    void append(const std::vector<uint8_t>& record);

    std::string data_dir_;
    std::string log_path_;
    int log_fd_ = -1;
    uint64_t end_offset_ = 0;  // onde o próximo registro será gravado
    std::unordered_map<uint64_t, uint64_t> index_;  // chave -> offset
    std::optional<uint64_t> corrupt_offset_;
    std::string corruption_message_;
};

}  // namespace ed2
