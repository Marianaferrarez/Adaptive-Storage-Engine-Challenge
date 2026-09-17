#include "engine.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace ed2 {

namespace {

constexpr uint8_t kFlagPut = 0;
constexpr uint8_t kFlagDelete = 1;
constexpr size_t kHeaderSize = 4 + 1 + 8 + 4;  // checksum + flag + key + value_len

uint32_t crc32_table[256];
bool crc32_table_ready = false;

void init_crc32_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_ready = true;
}

uint32_t crc32(const uint8_t* data, size_t len) {
    if (!crc32_table_ready) init_crc32_table();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// Monta o buffer serializado de um registro (sem o checksum) e o retorna
// junto com o checksum já calculado, prontos para escrita em disco.
std::vector<uint8_t> build_record(uint8_t flag, uint64_t key, const std::string& value) {
    std::vector<uint8_t> buf(kHeaderSize + value.size());
    uint8_t* p = buf.data();

    // Os 4 primeiros bytes (checksum) ficam em branco por enquanto;
    // serão preenchidos depois de sabermos o conteúdo restante.
    p += 4;

    std::memcpy(p, &flag, 1);
    p += 1;
    std::memcpy(p, &key, 8);
    p += 8;
    uint32_t value_len = static_cast<uint32_t>(value.size());
    std::memcpy(p, &value_len, 4);
    p += 4;
    if (!value.empty()) {
        std::memcpy(p, value.data(), value.size());
    }

    uint32_t checksum = crc32(buf.data() + 4, buf.size() - 4);
    std::memcpy(buf.data(), &checksum, 4);

    return buf;
}

void pwrite_all(int fd, const void* buf, size_t count, off_t offset) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t written = 0;
    while (written < count) {
        ssize_t n = pwrite(fd, p + written, count - written, offset + written);
        if (n <= 0) {
            throw std::runtime_error("falha ao escrever no data.log");
        }
        written += static_cast<size_t>(n);
    }
}

// Lê exatamente `count` bytes a partir de `offset`. Retorna false se o
// arquivo acabar antes disso (registro truncado — fim do log válido).
// Equivalente a `mkdir -p`: cria cada componente do caminho que ainda não
// existir. Necessário porque --data-dir pode apontar para um caminho
// aninhado que ainda não existe (mkdir() sozinho só cria um nível).
void mkdir_p(const std::string& path) {
    std::string partial;
    size_t pos = 0;
    while (pos < path.size()) {
        size_t next = path.find('/', pos);
        if (next == std::string::npos) next = path.size();
        partial = path.substr(0, next);
        if (!partial.empty()) {
            mkdir(partial.c_str(), 0755);  // ignora erro se já existir
        }
        pos = next + 1;
    }
}

bool pread_all(int fd, void* buf, size_t count, off_t offset) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t read_total = 0;
    while (read_total < count) {
        ssize_t n = pread(fd, p + read_total, count - read_total, offset + read_total);
        if (n < 0) {
            throw std::runtime_error("falha ao ler data.log");
        }
        if (n == 0) {
            return false;  // fim do arquivo antes do esperado
        }
        read_total += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

Engine::Engine(const std::string& data_dir) : data_dir_(data_dir) {
    mkdir_p(data_dir_);

    log_path_ = data_dir_ + "/data.log";
    log_fd_ = open(log_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (log_fd_ < 0) {
        throw std::runtime_error("não foi possível abrir " + log_path_);
    }

    recover();
}

Engine::~Engine() {
    if (log_fd_ >= 0) {
        fsync(log_fd_);
        close(log_fd_);
    }
}

void Engine::recover() {
    off_t offset = 0;
    index_.clear();
    off_t file_size = lseek(log_fd_, 0, SEEK_END);
    uint64_t valid_records = 0;

    for (;;) {
        uint8_t header[kHeaderSize];
        if (!pread_all(log_fd_, header, kHeaderSize, offset)) {
            break;  // sem registro completo aqui: fim do log válido
        }

        uint32_t stored_checksum;
        std::memcpy(&stored_checksum, header, 4);
        uint8_t flag = header[4];
        uint64_t key;
        std::memcpy(&key, header + 5, 8);
        uint32_t value_len;
        std::memcpy(&value_len, header + 13, 4);

        std::vector<uint8_t> value(value_len);
        if (value_len > 0 && !pread_all(log_fd_, value.data(), value_len, offset + kHeaderSize)) {
            break;  // valor truncado: registro incompleto, descarta
        }

        std::vector<uint8_t> check_buf(1 + 8 + 4 + value_len);
        check_buf[0] = flag;
        std::memcpy(check_buf.data() + 1, &key, 8);
        std::memcpy(check_buf.data() + 9, &value_len, 4);
        if (value_len > 0) {
            std::memcpy(check_buf.data() + 13, value.data(), value_len);
        }
        if (crc32(check_buf.data(), check_buf.size()) != stored_checksum) {
            break;  // registro corrompido: descarta daqui em diante
        }

        if (flag == kFlagPut) {
            index_[key] = IndexEntry{static_cast<uint64_t>(offset)};
        } else {
            index_.erase(key);
        }

        ++valid_records;
        offset += static_cast<off_t>(kHeaderSize + value_len);
    }

    recovery_stats_.valid_records = valid_records;
    recovery_stats_.discarded_bytes = static_cast<uint64_t>(file_size - offset);

    // Se sobrou lixo/registro incompleto no fim do arquivo (por causa de um
    // crash no meio de uma escrita anterior), descartamos fisicamente esses
    // bytes para não reprocessá-los à toa nas próximas aberturas.
    if (recovery_stats_.discarded_bytes > 0) {
        if (ftruncate(log_fd_, offset) != 0) {
            throw std::runtime_error("falha ao truncar data.log durante a recuperação");
        }
    }
}

void Engine::put(uint64_t key, const std::string& value) {
    std::vector<uint8_t> record = build_record(kFlagPut, key, value);

    off_t offset = lseek(log_fd_, 0, SEEK_END);
    pwrite_all(log_fd_, record.data(), record.size(), offset);
    // Sem fsync aqui de propósito: o write() já entrega os bytes ao page
    // cache do kernel, que sobrevive à morte do processo (só um
    // desligamento/reboot do SO perderia isso). Como a disciplina não exige
    // ACID completo (durabilidade contra falha de energia), trocamos essa
    // garantia mais forte por throughput bem maior — fsync por escrita
    // reduziu o PUT de ~1000 para ~145 ops/s no benchmark. fsync acontece
    // uma vez no destrutor, no encerramento normal.

    index_[key] = IndexEntry{static_cast<uint64_t>(offset)};
}

std::optional<std::string> Engine::get(uint64_t key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
        return std::nullopt;
    }

    off_t offset = static_cast<off_t>(it->second.offset);
    uint8_t header[kHeaderSize];
    if (!pread_all(log_fd_, header, kHeaderSize, offset)) {
        return std::nullopt;
    }
    uint32_t value_len;
    std::memcpy(&value_len, header + 13, 4);

    std::string value(value_len, '\0');
    if (value_len > 0 && !pread_all(log_fd_, value.data(), value_len, offset + kHeaderSize)) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::pair<uint64_t, std::string>> Engine::scan(uint64_t start, uint64_t end) {
    std::vector<uint64_t> keys;
    for (const auto& [key, entry] : index_) {
        (void)entry;
        if (key >= start && key <= end) {
            keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());

    std::vector<std::pair<uint64_t, std::string>> result;
    result.reserve(keys.size());
    for (uint64_t key : keys) {
        result.emplace_back(key, *get(key));
    }
    return result;
}

bool Engine::remove(uint64_t key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }

    std::vector<uint8_t> record = build_record(kFlagDelete, key, "");
    off_t offset = lseek(log_fd_, 0, SEEK_END);
    pwrite_all(log_fd_, record.data(), record.size(), offset);

    index_.erase(it);
    return true;
}

Engine::VerifyResult Engine::verify() {
    off_t offset = 0;
    uint64_t valid_records = 0;

    for (;;) {
        uint8_t header[kHeaderSize];
        if (!pread_all(log_fd_, header, kHeaderSize, offset)) {
            break;
        }

        uint32_t stored_checksum;
        std::memcpy(&stored_checksum, header, 4);
        uint8_t flag = header[4];
        uint64_t key;
        std::memcpy(&key, header + 5, 8);
        uint32_t value_len;
        std::memcpy(&value_len, header + 13, 4);

        std::vector<uint8_t> value(value_len);
        if (value_len > 0 && !pread_all(log_fd_, value.data(), value_len, offset + kHeaderSize)) {
            break;
        }

        std::vector<uint8_t> check_buf(1 + 8 + 4 + value_len);
        check_buf[0] = flag;
        std::memcpy(check_buf.data() + 1, &key, 8);
        std::memcpy(check_buf.data() + 9, &value_len, 4);
        if (value_len > 0) {
            std::memcpy(check_buf.data() + 13, value.data(), value_len);
        }
        if (crc32(check_buf.data(), check_buf.size()) != stored_checksum) {
            break;
        }

        ++valid_records;
        offset += static_cast<off_t>(kHeaderSize + value_len);
    }

    off_t total_size = lseek(log_fd_, 0, SEEK_END);
    bool ok = (offset == total_size);
    return VerifyResult{ok, valid_records, static_cast<uint64_t>(offset)};
}

}  // namespace ed2
