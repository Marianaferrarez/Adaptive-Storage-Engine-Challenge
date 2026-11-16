#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#include "engine.hpp"

// Teste funcional do Persistent Storage Engine: valida PUT, GET, sobrescrita,
// DELETE, reinserção, valores de tamanhos variados, VERIFY e persistência
// após encerramento e reinicialização.
int main() {
    const std::string test_dir = "data/smoke_test";
    std::filesystem::remove_all(test_dir);

    {
        ed2::Engine engine(test_dir);

        // Inserção e leitura básica
        engine.put(91, "abc");
        assert(engine.get(91) == "abc");

        // Sobrescrita de valor existente
        engine.put(91, "abc-atualizado");
        assert(engine.get(91) == "abc-atualizado");

        // Remoção e tentativa de remoção duplicada
        assert(engine.remove(91));
        assert(!engine.get(91).has_value());
        assert(!engine.remove(91));

        // Reinserção de chave previamente removida
        engine.put(91, "reinserida");
        assert(engine.get(91) == "reinserida");

        // Valores de tamanhos diferentes e chave uint64 alta
        engine.put(7, "");
        assert(engine.get(7) == "");

        std::string valor_grande(4096, 'X');
        engine.put(18446744073709551615ULL, valor_grande);
        assert(engine.get(18446744073709551615ULL) == valor_grande);

        auto v = engine.verify();
        assert(v.ok);
        std::cout << "verify (antes de reabrir): ok=" << v.ok
                  << " registros_validos=" << v.valid_records << "\n";
    }

    // Reabre o engine simulando reinicialização do programa
    {
        ed2::Engine engine(test_dir);

        assert(engine.get(91) == "reinserida");
        assert(engine.get(7) == "");
        assert(engine.get(18446744073709551615ULL) == std::string(4096, 'X'));
        assert(!engine.get(999).has_value());

        auto v = engine.verify();
        assert(v.ok);
        std::cout << "verify (depois de reabrir): ok=" << v.ok
                  << " registros_validos=" << v.valid_records << "\n";
    }

    std::cout << "smoke test passou\n";
    return 0;
}