#include <cassert>
#include <iostream>

#include "engine.hpp"

// Teste manual de fumaça (smoke test) para a Entrega 1: valida put/get/delete
// e, principalmente, que os dados sobrevivem a reabrir o engine (persistência).
int main() {
    {
        ed2::Engine engine("data/smoke_test");

        engine.put(91, "abc");
        assert(engine.get(91) == "abc");

        engine.put(91, "abc-atualizado");
        assert(engine.get(91) == "abc-atualizado");

        engine.put(7, "sete");
        assert(engine.get(7) == "sete");

        bool removed = engine.remove(91);
        assert(removed);
        assert(!engine.get(91).has_value());

        bool removed_again = engine.remove(91);
        assert(!removed_again);

        auto v = engine.verify();
        std::cout << "verify (antes de reabrir): ok=" << v.ok
                  << " registros_validos=" << v.valid_records << "\n";
    }

    // Reabre o engine simulando reinicialização do programa: se a
    // persistência estiver certa, a chave 7 continua existindo e a 91 não.
    {
        ed2::Engine engine("data/smoke_test");

        assert(engine.get(7) == "sete");
        assert(!engine.get(91).has_value());

        auto v = engine.verify();
        std::cout << "verify (depois de reabrir): ok=" << v.ok
                  << " registros_validos=" << v.valid_records << "\n";
    }

    std::cout << "smoke test passou\n";
    return 0;
}
