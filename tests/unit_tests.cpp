// Testes unitários do Engine (sem passar pelo CLI/JSON).
// Compilar e rodar: make unit
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "engine.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::cerr << "  FALHOU " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        }                                                                             \
    } while (0)

template <class F>
bool throws(F&& f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

std::string g_root;

std::string fresh_dir(const std::string& name) {
    std::string d = g_root + "/" + name;
    std::string cmd = "rm -rf '" + d + "'";
    if (std::system(cmd.c_str()) != 0) std::abort();
    return d;
}

uint64_t file_size(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

void write_file(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << data;
}

void flip_byte(const std::string& path, uint64_t offset) {
    int fd = open(path.c_str(), O_RDWR);
    unsigned char b;
    if (pread(fd, &b, 1, offset) != 1) std::abort();
    b ^= 0xFF;
    if (pwrite(fd, &b, 1, offset) != 1) std::abort();
    close(fd);
}

// ---------------------------------------------------------------------------

void test_put_get_delete() {
    auto d = fresh_dir("basico");
    ed2::Engine e(d);
    CHECK(!e.get(1).has_value());
    e.put(1, "um");
    CHECK(e.get(1) == "um");
    CHECK(e.remove(1));
    CHECK(!e.get(1).has_value());
    CHECK(!e.remove(1));  // remover de novo -> not_found
    CHECK(e.verify().ok);
}

void test_overwrite_and_reinsert() {
    auto d = fresh_dir("sobrescrita");
    ed2::Engine e(d);
    e.put(7, "a");
    e.put(7, std::string(5000, 'b'));
    CHECK(e.get(7) == std::string(5000, 'b'));
    e.put(7, "");
    CHECK(e.get(7) == "");
    CHECK(e.remove(7));
    e.put(7, "de volta");
    CHECK(e.get(7) == "de volta");
    CHECK(e.size() == 1);
}

void test_binary_and_limits() {
    auto d = fresh_dir("binario");
    std::string bin("a\0b\0\xff\xfe", 6);
    {
        ed2::Engine e(d);
        e.put(0, bin);
        e.put(UINT64_MAX, "max");
        e.put(UINT64_MAX - 1, "max-1");
        e.put(1ull << 63, "2^63");
    }
    ed2::Engine e(d);
    CHECK(e.get(0) == bin);
    CHECK(e.get(UINT64_MAX) == "max");
    CHECK(e.get(UINT64_MAX - 1) == "max-1");
    CHECK(e.get(1ull << 63) == "2^63");
}

void test_persistence_across_reopen() {
    auto d = fresh_dir("persistencia");
    {
        ed2::Engine e(d);
        for (uint64_t k = 0; k < 1000; ++k) e.put(k, "v" + std::to_string(k));
        for (uint64_t k = 0; k < 1000; k += 3) e.remove(k);
        e.put(1, "sobrescrito");
    }
    {
        ed2::Engine e(d);
        CHECK(e.get(1) == "sobrescrito");
        CHECK(!e.get(0).has_value());
        CHECK(e.get(2) == "v2");
        CHECK(e.size() == 1000 - 334);
        CHECK(e.verify().ok);
        e.put(0, "reinserido");
    }
    ed2::Engine e(d);
    CHECK(e.get(0) == "reinserido");
}

void test_reopen_without_writes_keeps_file() {
    auto d = fresh_dir("sem_escrita");
    { ed2::Engine e(d); e.put(1, "x"); }
    std::string before = read_file(d + "/data.log");
    { ed2::Engine e(d); (void)e.get(1); (void)e.verify(); }
    CHECK(read_file(d + "/data.log") == before);
}

void test_nested_data_dir() {
    auto d = fresh_dir("aninhado") + "/a/b/c";
    ed2::Engine e(d);
    e.put(1, "ok");
    CHECK(file_size(d + "/data.log") > 0);
}

void test_detects_bit_flip() {
    auto d = fresh_dir("bitflip");
    { ed2::Engine e(d); e.put(1, "primeiro"); e.put(2, "segundo"); e.put(3, "terceiro"); }
    // Cabeçalho do arquivo: 16 bytes; registro: 17 + len. Corrompe o valor do 2º.
    uint64_t off2 = 16 + 17 + 8;
    flip_byte(d + "/data.log", off2 + 17 + 2);
    std::string before = read_file(d + "/data.log");

    ed2::Engine e(d);
    CHECK(e.corrupted());
    CHECK(!e.verify().ok);
    CHECK(e.get(1) == "primeiro");          // parte íntegra continua legível
    CHECK(throws([&] { e.put(9, "x"); }));  // escrita recusada
    CHECK(throws([&] { e.remove(1); }));
    CHECK(read_file(d + "/data.log") == before);  // nada foi alterado
}

void test_truncated_tail_and_repair() {
    auto d = fresh_dir("truncado");
    { ed2::Engine e(d); e.put(1, "a"); e.put(2, "b"); }
    uint64_t good = file_size(d + "/data.log");
    {   // simula uma escrita interrompida: meio registro no fim
        std::ofstream f(d + "/data.log", std::ios::binary | std::ios::app);
        f << std::string("\x01\x02\x03\x04\x00\x05", 6);
    }
    {
        ed2::Engine e(d);
        CHECK(e.corrupted());
        CHECK(!e.verify().ok);
        CHECK(e.repair() == 6);
        CHECK(!e.corrupted());
        CHECK(e.verify().ok);
        CHECK(e.get(1) == "a");
        CHECK(e.get(2) == "b");
        e.put(3, "c");  // escrita volta a funciona
    }
    CHECK(file_size(d + "/data.log.corrupted-" + std::to_string(good)) == 6);  // quarentena
    ed2::Engine e(d);
    CHECK(e.verify().ok);
    CHECK(e.get(3) == "c");
}

void test_huge_value_len_does_not_crash() {
    auto d = fresh_dir("value_len_gigante");
    { ed2::Engine e(d); e.put(1, "a"); }
    // Registro forjado com value_len = 0xFFFFFFFF: não pode tentar alocar 4 GiB.
    std::string rec(17, '\0');
    rec[4] = 0;
    for (int i = 13; i < 17; ++i) rec[i] = '\xff';
    std::ofstream(d + "/data.log", std::ios::binary | std::ios::app) << rec;
    ed2::Engine e(d);
    CHECK(e.corrupted());
    CHECK(e.get(1) == "a");
}

void test_rejects_foreign_file() {
    auto d = fresh_dir("estranho");
    std::string cmd = "mkdir -p '" + d + "'";
    if (std::system(cmd.c_str()) != 0) std::abort();
    write_file(d + "/data.log", "conteúdo que não é do engine\n");
    std::string before = read_file(d + "/data.log");
    CHECK(throws([&] { ed2::Engine e(d); }));
    CHECK(read_file(d + "/data.log") == before);  // arquivo intocado
}

void test_lock_prevents_second_instance() {
    auto d = fresh_dir("lock");
    ed2::Engine a(d);
    CHECK(throws([&] { ed2::Engine b(d); }));
}

// Cenário funcional completo (vindo do smoke test da Lethicia, branch
// feature/entrega-3): PUT, sobrescrita, DELETE, reinserção, valores de
// tamanhos variados, VERIFY e persistência após reiniciar.
void test_functional_scenario() {
    auto d = fresh_dir("cenario_funcional");
    const uint64_t kMax = 18446744073709551615ULL;
    const std::string grande(4096, 'X');
    {
        ed2::Engine e(d);
        e.put(91, "abc");
        CHECK(e.get(91) == "abc");
        e.put(91, "abc-atualizado");
        CHECK(e.get(91) == "abc-atualizado");
        CHECK(e.remove(91));
        CHECK(!e.get(91).has_value());
        CHECK(!e.remove(91));
        e.put(91, "reinserida");
        CHECK(e.get(91) == "reinserida");
        e.put(7, "");
        CHECK(e.get(7) == "");
        e.put(kMax, grande);
        CHECK(e.get(kMax) == grande);
        CHECK(e.verify().ok);
    }
    ed2::Engine e(d);
    CHECK(e.get(91) == "reinserida");
    CHECK(e.get(7) == "");
    CHECK(e.get(kMax) == grande);
    CHECK(!e.get(999).has_value());
    CHECK(e.verify().ok);
}

// Teste diferencial: 20 000 operações aleatórias comparadas com std::map,
// reabrindo o engine a cada 2 000 operações.
void test_randomized_against_model() {
    auto d = fresh_dir("aleatorio");
    std::map<uint64_t, std::string> model;
    std::mt19937_64 rng(12345);
    auto engine = std::make_unique<ed2::Engine>(d);
    for (int i = 0; i < 20000; ++i) {
        uint64_t k = rng() % 500;
        int r = rng() % 10;
        if (r < 5) {
            std::string v(rng() % 200, static_cast<char>('a' + rng() % 26));
            engine->put(k, v);
            model[k] = v;
        } else if (r < 8) {
            auto got = engine->get(k);
            auto it = model.find(k);
            CHECK(got.has_value() == (it != model.end()));
            if (got && it != model.end()) CHECK(*got == it->second);
        } else {
            CHECK(engine->remove(k) == (model.erase(k) == 1));
        }
        if (i % 2000 == 1999) {
            engine.reset();
            engine = std::make_unique<ed2::Engine>(d);
            CHECK(engine->size() == model.size());
        }
    }
    for (auto& [k, v] : model) CHECK(engine->get(k) == v);
    CHECK(engine->verify().ok);
}

}  // namespace

int main() {
    char tmpl[] = "/tmp/ed2-unit-XXXXXX";
    char* root = mkdtemp(tmpl);
    if (!root) return 1;
    g_root = root;

    std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"put/get/delete", test_put_get_delete},
        {"sobrescrita e reinserção", test_overwrite_and_reinsert},
        {"valores binários e chaves limite", test_binary_and_limits},
        {"persistência ao reabrir", test_persistence_across_reopen},
        {"reabrir sem escrita não altera o arquivo", test_reopen_without_writes_keeps_file},
        {"data-dir aninhado", test_nested_data_dir},
        {"detecta byte corrompido", test_detects_bit_flip},
        {"cauda truncada + repair", test_truncated_tail_and_repair},
        {"value_len gigante não derruba", test_huge_value_len_does_not_crash},
        {"recusa arquivo desconhecido", test_rejects_foreign_file},
        {"lock impede segunda instância", test_lock_prevents_second_instance},
        {"cenário funcional completo", test_functional_scenario},
        {"aleatório vs modelo (20k ops)", test_randomized_against_model},
    };
    for (auto& [name, fn] : tests) {
        int before = g_failures;
        fn();
        std::cout << (g_failures == before ? "  ok    " : "  FALHA ") << name << "\n";
    }
    std::string cmd = std::string("rm -rf '") + root + "'";
    if (std::system(cmd.c_str()) != 0) { /* ignora */ }

    std::cout << g_checks << " verificações, " << g_failures << " falhas\n";
    return g_failures == 0 ? 0 : 1;
}
