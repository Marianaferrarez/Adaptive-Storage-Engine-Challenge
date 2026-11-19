#include "engine.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ed2 {

namespace {

// ---------------------------------------------------------------------------
// Formato do arquivo data.log (todos os inteiros em little-endian):
//
//   Cabeçalho do arquivo (16 bytes):
//     [magic: 8 bytes "ED2KVLOG"][version: uint32 = 1][reserved: uint32 = 0]
//
//   Seguido de N registros:
//     [crc32: uint32][flag: uint8][key: uint64][value_len: uint32][value]
//     crc32 cobre flag + key + value_len + value.
//     flag 0 = PUT, flag 1 = DELETE (tombstone, value_len = 0).
// ---------------------------------------------------------------------------
constexpr char kMagic[8] = {'E', 'D', '2', 'K', 'V', 'L', 'O', 'G'};
constexpr uint32_t kVersion = 1;
constexpr size_t kFileHeaderSize = 16;

constexpr uint8_t kFlagPut = 0;
constexpr uint8_t kFlagDelete = 1;
constexpr size_t kRecordHeaderSize = 4 + 1 + 8 + 4;

// ------------------------------- CRC32 -------------------------------------
struct Crc32Table {
    uint32_t t[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
const Crc32Table kCrcTable;

uint32_t crc32_update(uint32_t c, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) c = kCrcTable.t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c;
}

uint32_t record_crc(const uint8_t* header_without_crc, const uint8_t* value, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    c = crc32_update(c, header_without_crc, kRecordHeaderSize - 4);
    c = crc32_update(c, value, len);
    return c ^ 0xFFFFFFFFu;
}

// ------------------------ codificação little-endian ------------------------
void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

struct RecordHeader {
    uint32_t crc;
    uint8_t flag;
    uint64_t key;
    uint32_t value_len;
};

RecordHeader decode_header(const uint8_t* h) {
    return RecordHeader{get_u32(h), h[4], get_u64(h + 5), get_u32(h + 13)};
}

std::vector<uint8_t> build_record(uint8_t flag, uint64_t key, const std::string& value) {
    std::vector<uint8_t> buf(kRecordHeaderSize + value.size());
    uint8_t* p = buf.data();
    p[4] = flag;
    put_u64(p + 5, key);
    put_u32(p + 13, static_cast<uint32_t>(value.size()));
    if (!value.empty()) std::memcpy(p + kRecordHeaderSize, value.data(), value.size());
    put_u32(p, record_crc(p + 4, p + kRecordHeaderSize, value.size()));
    return buf;
}

// ------------------------------ E/S auxiliar -------------------------------
std::string errno_text() { return std::strerror(errno); }

void pwrite_all(int fd, const uint8_t* p, size_t count, uint64_t offset) {
    size_t done = 0;
    while (done < count) {
        ssize_t n = pwrite(fd, p + done, count - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw StorageError("falha ao escrever no data.log: " + errno_text());
        done += static_cast<size_t>(n);
    }
}

// Lê exatamente `count` bytes; retorna false se o arquivo acabar antes.
bool pread_all(int fd, uint8_t* p, size_t count, uint64_t offset) {
    size_t done = 0;
    while (done < count) {
        ssize_t n = pread(fd, p + done, count - done, static_cast<off_t>(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw StorageError("falha ao ler data.log: " + errno_text());
        if (n == 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

uint64_t file_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) throw StorageError("falha em fstat(data.log): " + errno_text());
    return static_cast<uint64_t>(st.st_size);
}

// Equivalente a `mkdir -p`.
void mkdir_p(const std::string& path) {
    if (path.empty()) throw StorageError("--data-dir vazio");
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t next = path.find('/', pos);
        if (next == std::string::npos) next = path.size();
        std::string partial = path.substr(0, next);
        if (!partial.empty() && mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) {
            throw StorageError("não foi possível criar o diretório '" + partial + "': " + errno_text());
        }
        pos = next + 1;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        throw StorageError("--data-dir '" + path + "' existe mas não é um diretório");
    }
}

}  // namespace

// ============================================================================

Engine::Engine(const std::string& data_dir) : data_dir_(data_dir) {
    mkdir_p(data_dir_);
    log_path_ = data_dir_ + "/data.log";
    open_log();
}

Engine::~Engine() {
    if (log_fd_ >= 0) {
        fsync(log_fd_);  // durabilidade no encerramento normal
        close(log_fd_);  // também libera o flock
    }
}

void Engine::open_log() {
    log_fd_ = open(log_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (log_fd_ < 0) {
        throw StorageError("não foi possível abrir " + log_path_ + ": " + errno_text());
    }

    // Impede que dois processos escrevam no mesmo data.log ao mesmo tempo.
    if (flock(log_fd_, LOCK_EX | LOCK_NB) != 0) {
        close(log_fd_);
        log_fd_ = -1;
        throw StorageError(log_path_ + " já está em uso por outro processo do engine");
    }

    uint64_t size = file_size(log_fd_);
    if (size == 0) {
        // Arquivo novo: grava o cabeçalho.
        uint8_t header[kFileHeaderSize] = {};
        std::memcpy(header, kMagic, 8);
        put_u32(header + 8, kVersion);
        pwrite_all(log_fd_, header, kFileHeaderSize, 0);
        fsync(log_fd_);
        end_offset_ = kFileHeaderSize;
        return;
    }

    // Arquivo existente: só aceitamos se for reconhecidamente nosso. Nunca
    // alteramos um arquivo desconhecido.
    uint8_t header[kFileHeaderSize];
    if (size < kFileHeaderSize || !pread_all(log_fd_, header, kFileHeaderSize, 0) ||
        std::memcmp(header, kMagic, 8) != 0) {
        close(log_fd_);
        log_fd_ = -1;
        throw StorageError(log_path_ +
                           " não é um arquivo deste engine (cabeçalho inválido); "
                           "nenhum dado foi alterado");
    }
    uint32_t version = get_u32(header + 8);
    if (version != kVersion) {
        close(log_fd_);
        log_fd_ = -1;
        throw StorageError(log_path_ + " usa a versão de formato " + std::to_string(version) +
                           ", esperada " + std::to_string(kVersion));
    }

    ScanResult r = scan_log(true);
    end_offset_ = r.end_offset;
    if (!r.error.empty()) {
        corrupt_offset_ = r.end_offset;
        corruption_message_ = r.error;
    }
}

Engine::ScanResult Engine::scan_log(bool rebuild_index) {
    if (rebuild_index) index_.clear();

    const uint64_t size = file_size(log_fd_);
    ScanResult r{kFileHeaderSize, 0, ""};
    if (size < kFileHeaderSize) {
        r.end_offset = 0;
        r.error = "cabeçalho do data.log incompleto";
        return r;
    }

    std::vector<uint8_t> value;
    uint64_t offset = kFileHeaderSize;
    while (offset < size) {
        const std::string where = "offset " + std::to_string(offset);
        uint64_t remaining = size - offset;
        if (remaining < kRecordHeaderSize) {
            r.error = "registro truncado no " + where + " (" + std::to_string(remaining) +
                      " bytes restantes, cabeçalho exige " + std::to_string(kRecordHeaderSize) + ")";
            break;
        }

        uint8_t h[kRecordHeaderSize];
        if (!pread_all(log_fd_, h, kRecordHeaderSize, offset)) {
            r.error = "fim inesperado do arquivo no " + where;
            break;
        }
        RecordHeader rh = decode_header(h);

        // Valida o tamanho ANTES de alocar: um value_len corrompido não pode
        // causar uma alocação gigante.
        if (rh.value_len > remaining - kRecordHeaderSize) {
            r.error = "registro truncado no " + where + " (value_len=" +
                      std::to_string(rh.value_len) + " ultrapassa o fim do arquivo)";
            break;
        }
        value.resize(rh.value_len);
        if (rh.value_len > 0 && !pread_all(log_fd_, value.data(), rh.value_len, offset + kRecordHeaderSize)) {
            r.error = "fim inesperado do arquivo no " + where;
            break;
        }
        if (record_crc(h + 4, value.data(), rh.value_len) != rh.crc) {
            r.error = "checksum CRC32 inválido no " + where;
            break;
        }
        if (rh.flag != kFlagPut && rh.flag != kFlagDelete) {
            r.error = "flag desconhecida (" + std::to_string(rh.flag) + ") no " + where;
            break;
        }
        if (rh.flag == kFlagDelete && rh.value_len != 0) {
            r.error = "tombstone com value_len != 0 no " + where;
            break;
        }

        if (rebuild_index) {
            if (rh.flag == kFlagPut) index_[rh.key] = offset;
            else index_.erase(rh.key);
        }
        ++r.valid_records;
        offset += kRecordHeaderSize + rh.value_len;
    }
    r.end_offset = offset;
    return r;
}

void Engine::ensure_writable() const {
    if (corrupt_offset_) {
        throw StorageError("data.log corrompido (" + corruption_message_ +
                           "); escrita recusada para não agravar o dano. "
                           "Execute 'build/engine repair --data-dir " + data_dir_ + "'");
    }
}

void Engine::append(const std::vector<uint8_t>& record) {
    ensure_writable();
    try {
        pwrite_all(log_fd_, record.data(), record.size(), end_offset_);
    } catch (...) {
        // Escrita parcial: desfaz para não deixar um registro pela metade.
        if (ftruncate(log_fd_, static_cast<off_t>(end_offset_)) != 0) { /* melhor esforço */ }
        throw;
    }
    // Sem fsync por operação (ver docs/design.md): os bytes já estão no
    // page cache do kernel e sobrevivem ao fim do processo; o fsync é feito
    // uma única vez no encerramento.
    end_offset_ += record.size();
}

void Engine::put(uint64_t key, const std::string& value) {
    if (value.size() > std::numeric_limits<uint32_t>::max()) {
        throw StorageError("valor grande demais (máximo 4 GiB - 1)");
    }
    uint64_t offset = end_offset_;
    append(build_record(kFlagPut, key, value));
    index_[key] = offset;
}

std::optional<std::string> Engine::get(uint64_t key) {
    auto it = index_.find(key);
    if (it == index_.end()) return std::nullopt;

    const uint64_t offset = it->second;
    uint8_t h[kRecordHeaderSize];
    if (!pread_all(log_fd_, h, kRecordHeaderSize, offset)) {
        throw StorageError("registro da chave " + std::to_string(key) + " no offset " +
                           std::to_string(offset) + " está truncado");
    }
    RecordHeader rh = decode_header(h);
    if (rh.key != key || rh.flag != kFlagPut) {
        throw StorageError("índice inconsistente para a chave " + std::to_string(key));
    }
    std::string value(rh.value_len, '\0');
    if (rh.value_len > 0 &&
        !pread_all(log_fd_, reinterpret_cast<uint8_t*>(value.data()), rh.value_len, offset + kRecordHeaderSize)) {
        throw StorageError("valor da chave " + std::to_string(key) + " está truncado");
    }
    if (record_crc(h + 4, reinterpret_cast<const uint8_t*>(value.data()), rh.value_len) != rh.crc) {
        throw StorageError("checksum inválido no registro da chave " + std::to_string(key));
    }
    return value;
}

bool Engine::remove(uint64_t key) {
    auto it = index_.find(key);
    if (it == index_.end()) return false;
    append(build_record(kFlagDelete, key, ""));
    index_.erase(it);
    return true;
}

std::vector<std::pair<uint64_t, std::string>> Engine::scan(uint64_t start, uint64_t end) {
    std::vector<uint64_t> keys;
    for (const auto& kv : index_) {
        if (kv.first >= start && kv.first <= end) keys.push_back(kv.first);
    }
    std::sort(keys.begin(), keys.end());
    std::vector<std::pair<uint64_t, std::string>> out;
    out.reserve(keys.size());
    for (uint64_t k : keys) out.emplace_back(k, *get(k));
    return out;
}

Engine::VerifyResult Engine::verify() {
    VerifyResult v;
    v.file_bytes = file_size(log_fd_);
    v.live_keys = index_.size();

    // Varre o arquivo reconstruindo um índice temporário e compara com o
    // índice em memória: detecta tanto corrupção em disco quanto divergência
    // entre o que está em RAM e o que está persistido.
    auto saved = std::move(index_);
    ScanResult r = scan_log(true);
    auto disk_index = std::move(index_);
    index_ = std::move(saved);

    v.valid_records = r.valid_records;
    v.valid_bytes = r.end_offset;

    if (!r.error.empty()) {
        v.message = r.error;
    } else if (v.valid_bytes != v.file_bytes) {
        v.message = "tamanho do arquivo (" + std::to_string(v.file_bytes) +
                    ") difere do fim do último registro (" + std::to_string(v.valid_bytes) + ")";
    } else if (v.valid_bytes != end_offset_) {
        v.message = "fim do log em disco difere do esperado pelo engine";
    } else if (disk_index != index_) {
        v.message = "índice em memória diverge do conteúdo do data.log";
    } else {
        v.ok = true;
    }
    return v;
}

uint64_t Engine::repair() {
    ScanResult r = scan_log(false);
    const uint64_t size = file_size(log_fd_);
    if (r.error.empty() && r.end_offset == size) return 0;
    if (r.end_offset < kFileHeaderSize) {
        throw StorageError("cabeçalho do data.log inválido; reparo automático impossível");
    }

    // Copia os bytes inválidos para um arquivo de quarentena antes de
    // truncar, para que nada seja descartado sem deixar rastro.
    const uint64_t bad = size - r.end_offset;
    const std::string qpath = log_path_ + ".corrupted-" + std::to_string(r.end_offset);
    int qfd = open(qpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (qfd < 0) throw StorageError("não foi possível criar " + qpath + ": " + errno_text());
    std::vector<uint8_t> buf(1 << 20);
    uint64_t copied = 0;
    while (copied < bad) {
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(buf.size(), bad - copied));
        if (!pread_all(log_fd_, buf.data(), chunk, r.end_offset + copied)) break;
        pwrite_all(qfd, buf.data(), chunk, copied);
        copied += chunk;
    }
    fsync(qfd);
    close(qfd);

    if (ftruncate(log_fd_, static_cast<off_t>(r.end_offset)) != 0) {
        throw StorageError("falha ao truncar data.log: " + errno_text());
    }
    fsync(log_fd_);

    ScanResult again = scan_log(true);
    end_offset_ = again.end_offset;
    corrupt_offset_.reset();
    corruption_message_.clear();
    return bad;
}

}  // namespace ed2
