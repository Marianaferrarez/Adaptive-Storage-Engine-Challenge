#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <iostream>

#include "engine.hpp"

// Simula um crash no meio da escrita de um registro: grava alguns bytes de
// um registro incompleto diretamente no data.log (por fora do Engine, sem
// passar pelo write completo + fsync), imitando o que sobraria em disco se
// o processo morresse no meio de um pwrite(). Depois reabre o Engine e
// verifica se a recuperação descarta só esse lixo, sem perder nada válido.
int main() {
    const char* dir = "data/crash_test";
    const std::string log_path = std::string(dir) + "/data.log";

    {
        ed2::Engine engine(dir);
        engine.put(1, "primeiro");
        engine.put(2, "segundo");
        engine.put(3, "terceiro");
    }  // fecha o engine normalmente (todos os 3 registros estão íntegros em disco)

    // Injeta um registro "quebrado" no fim do arquivo, por fora do Engine.
    {
        int fd = open(log_path.c_str(), O_WRONLY | O_APPEND);
        assert(fd >= 0);
        const char garbage[] = "\x01\x02\x03\x04lixo-de-escrita-incompleta";
        ssize_t n = write(fd, garbage, sizeof(garbage) - 1);
        assert(n == static_cast<ssize_t>(sizeof(garbage) - 1));
        close(fd);
    }

    // Reabre o engine: recover() deve descartar o lixo do fim e manter as
    // 3 chaves gravadas antes do "crash" simulado.
    {
        ed2::Engine engine(dir);

        auto stats = engine.recovery_stats();
        std::cout << "registros_validos_apos_crash=" << stats.valid_records
                  << " bytes_descartados=" << stats.discarded_bytes << "\n";
        assert(stats.valid_records == 3);
        assert(stats.discarded_bytes > 0);

        assert(engine.get(1) == "primeiro");
        assert(engine.get(2) == "segundo");
        assert(engine.get(3) == "terceiro");

        auto v = engine.verify();
        assert(v.ok);
        assert(v.valid_records == 3);
    }

    std::cout << "teste de recuperacao apos crash passou\n";
    return 0;
}
