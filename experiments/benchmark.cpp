#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "engine.hpp"

// Benchmark simples e reproduzível (seed fixa) para a análise experimental
// da Entrega 1: throughput de PUT/GET e um teste de valor grande.
// Uso: ./benchmark [num_operacoes] [tamanho_valor_bytes] [seed]
int main(int argc, char** argv) {
    uint64_t num_ops = argc > 1 ? std::stoull(argv[1]) : 50000;
    size_t value_size = argc > 2 ? std::stoul(argv[2]) : 100;
    uint64_t seed = argc > 3 ? std::stoull(argv[3]) : 42;

    std::cout << "num_ops=" << num_ops << " value_size=" << value_size
              << " seed=" << seed << "\n";

    std::mt19937_64 rng(seed);
    std::string value(value_size, 'x');

    ed2::Engine engine("data/benchmark");

    std::vector<uint64_t> keys(num_ops);
    for (uint64_t i = 0; i < num_ops; ++i) {
        keys[i] = rng();
    }

    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t k : keys) {
        engine.put(k, value);
    }
    auto t1 = std::chrono::steady_clock::now();
    double put_secs = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "PUT: " << num_ops << " ops em " << put_secs << "s -> "
              << (num_ops / put_secs) << " ops/s\n";

    std::shuffle(keys.begin(), keys.end(), rng);

    auto t2 = std::chrono::steady_clock::now();
    uint64_t hits = 0;
    for (uint64_t k : keys) {
        if (engine.get(k).has_value()) ++hits;
    }
    auto t3 = std::chrono::steady_clock::now();
    double get_secs = std::chrono::duration<double>(t3 - t2).count();
    std::cout << "GET: " << num_ops << " ops (" << hits << " hits) em "
              << get_secs << "s -> " << (num_ops / get_secs) << " ops/s\n";

    // Teste de valor grande (fora do laço principal, não entra no throughput acima).
    size_t big_size = 4ull * 1024 * 1024;
    std::string big_value(big_size, 'y');
    uint64_t big_key = rng();

    auto t4 = std::chrono::steady_clock::now();
    engine.put(big_key, big_value);
    auto retrieved = engine.get(big_key);
    auto t5 = std::chrono::steady_clock::now();

    bool big_ok = retrieved.has_value() && *retrieved == big_value;
    double big_secs = std::chrono::duration<double>(t5 - t4).count();
    std::cout << "valor grande (" << big_size / (1024 * 1024)
              << " MiB): correto=" << big_ok << " tempo=" << big_secs << "s\n";

    return 0;
}
