#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

#include "engine.hpp"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace {

// Parseia flags no formato --nome valor a partir de argv[start]. Não valida
// quais flags são esperadas aqui; cada subcomando checa o que precisa.
std::unordered_map<std::string, std::string> parse_flags(int argc, char** argv, int start) {
    std::unordered_map<std::string, std::string> flags;
    for (int i = start; i + 1 < argc; i += 2) {
        std::string name = argv[i];
        if (name.rfind("--", 0) == 0) {
            flags[name.substr(2)] = argv[i + 1];
        }
    }
    return flags;
}

std::string require_flag(const std::unordered_map<std::string, std::string>& flags,
                          const std::string& name) {
    auto it = flags.find(name);
    if (it == flags.end()) {
        throw std::runtime_error("faltando flag obrigatória --" + name);
    }
    return it->second;
}

// Constrói a resposta para uma única operação do workload. Erros de
// validação (campo faltando, tipo errado, op desconhecida) viram
// {"status": "error"} em vez de derrubar o processo inteiro: um workload
// com uma linha ruim não pode interromper o processamento das demais.
json handle_operation(ed2::Engine& engine, const json& req) {
    json resp;
    if (req.contains("id")) {
        resp["id"] = req["id"];
    }

    std::string op = req.value("op", "");

    try {
        if (op == "put") {
            uint64_t key = req.at("key").get<uint64_t>();
            std::string value = req.at("value").get<std::string>();
            engine.put(key, value);
            resp["status"] = "ok";
        } else if (op == "get") {
            uint64_t key = req.at("key").get<uint64_t>();
            auto value = engine.get(key);
            if (value.has_value()) {
                resp["status"] = "ok";
                resp["value"] = *value;
            } else {
                resp["status"] = "not_found";
            }
        } else if (op == "delete") {
            uint64_t key = req.at("key").get<uint64_t>();
            bool removed = engine.remove(key);
            resp["status"] = removed ? "ok" : "not_found";
        } else if (op == "scan") {
            uint64_t start = req.at("start").get<uint64_t>();
            uint64_t end = req.at("end").get<uint64_t>();
            auto records = engine.scan(start, end);
            json arr = json::array();
            for (const auto& [key, value] : records) {
                arr.push_back({{"key", key}, {"value", value}});
            }
            resp["status"] = "ok";
            resp["records"] = arr;
        } else {
            resp["status"] = "error";
            resp["message"] = "operação desconhecida: '" + op + "'";
        }
    } catch (const std::exception& e) {
        resp["status"] = "error";
        resp["message"] = e.what();
    }

    return resp;
}

int cmd_init(const std::unordered_map<std::string, std::string>& flags) {
    std::string data_dir = require_flag(flags, "data-dir");
    ed2::Engine engine(data_dir);
    std::cerr << "engine inicializado em " << data_dir << "\n";
    return 0;
}

int cmd_run(const std::unordered_map<std::string, std::string>& flags) {
    std::string data_dir = require_flag(flags, "data-dir");
    ed2::Engine engine(data_dir);

    std::ifstream input_file;
    std::istream* input = &std::cin;
    if (auto it = flags.find("input"); it != flags.end()) {
        input_file.open(it->second);
        if (!input_file) {
            throw std::runtime_error("não foi possível abrir --input " + it->second);
        }
        input = &input_file;
    }

    std::ofstream output_file;
    std::ostream* output = &std::cout;
    if (auto it = flags.find("output"); it != flags.end()) {
        output_file.open(it->second);
        if (!output_file) {
            throw std::runtime_error("não foi possível abrir --output " + it->second);
        }
        output = &output_file;
    }

    std::string line;
    while (std::getline(*input, line)) {
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;  // ignora linhas em branco
        }

        json req;
        try {
            req = json::parse(line);
        } catch (const std::exception& e) {
            std::cerr << "linha ignorada (JSON inválido): " << e.what() << "\n";
            continue;
        }

        json resp = handle_operation(engine, req);
        *output << resp.dump() << "\n";
    }

    return 0;
}

int cmd_verify(const std::unordered_map<std::string, std::string>& flags) {
    std::string data_dir = require_flag(flags, "data-dir");
    ed2::Engine engine(data_dir);  // recover() já roda aqui e corrige o log

    auto recovery = engine.recovery_stats();
    auto result = engine.verify();

    json out;
    out["status"] = result.ok ? "ok" : "corrupted";
    out["valid_records"] = result.valid_records;
    out["valid_bytes"] = result.valid_bytes;
    out["discarded_bytes_on_open"] = recovery.discarded_bytes;
    std::cout << out.dump() << "\n";

    return result.ok ? 0 : 1;
}

int cmd_describe(const std::unordered_map<std::string, std::string>&) {
    json out;
    out["engine"] = "ed2-adaptive-storage-engine";
    out["stage"] = "Entrega 1 - Persistent Storage Engine";
    out["architecture"] =
        "append-only log (data.log) com índice em memória (hash map chave->offset), "
        "no estilo Bitcask";
    out["key_type"] = "uint64";
    out["value_type"] = "bytes de tamanho variável";
    out["supported_ops"] = {"put", "get", "delete", "scan"};
    out["persistence"] = true;
    out["crash_recovery"] = true;
    out["integrity_check"] = "CRC32 por registro";
    out["notes"] =
        "scan nesta etapa é O(n log n) sobre um índice não ordenado; "
        "índice ordenado chega na Entrega 2";
    std::cout << out.dump(2) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "uso: engine <init|run|verify|describe> [--flag valor ...]\n";
        return 1;
    }

    std::string cmd = argv[1];
    auto flags = parse_flags(argc, argv, 2);

    try {
        if (cmd == "init") return cmd_init(flags);
        if (cmd == "run") return cmd_run(flags);
        if (cmd == "verify") return cmd_verify(flags);
        if (cmd == "describe") return cmd_describe(flags);

        std::cerr << "comando desconhecido: " << cmd << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "erro: " << e.what() << "\n";
        return 1;
    }
}
