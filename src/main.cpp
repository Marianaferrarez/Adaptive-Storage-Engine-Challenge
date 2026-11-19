// CLI do storage engine.
//
//   engine run      --data-dir DIR [--input ARQ] [--output ARQ]
//   engine verify   --data-dir DIR
//   engine repair   --data-dir DIR
//   engine init     --data-dir DIR
//   engine describe
//
// O contrato da disciplina (./run.sh --data-dir --input --output) chama
// `engine run`. O protocolo é JSON Lines: uma operação por linha de entrada,
// exatamente uma resposta por linha de saída, na mesma ordem.

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>

#include "engine.hpp"
#include "third_party/nlohmann/json.hpp"

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;  // preserva a ordem id, status, ...

namespace {

using Flags = std::unordered_map<std::string, std::string>;

// Aceita "--nome valor" e "--nome=valor".
Flags parse_flags(int argc, char** argv, int start) {
    Flags flags;
    for (int i = start; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0) {
            throw std::runtime_error("argumento inesperado: '" + arg + "'");
        }
        std::string name = arg.substr(2);
        auto eq = name.find('=');
        if (eq != std::string::npos) {
            flags[name.substr(0, eq)] = name.substr(eq + 1);
        } else if (i + 1 < argc) {
            flags[name] = argv[++i];
        } else {
            throw std::runtime_error("flag --" + name + " sem valor");
        }
    }
    return flags;
}

std::string require_flag(const Flags& flags, const std::string& name) {
    auto it = flags.find(name);
    if (it == flags.end() || it->second.empty()) {
        throw std::runtime_error("faltando flag obrigatória --" + name);
    }
    return it->second;
}

// Erro de protocolo (requisição mal formada) — vira {"status":"error"}.
struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

uint64_t read_key(const json& req, const char* field = "key") {
    if (!req.contains(field)) {
        throw ProtocolError(std::string("campo '") + field + "' ausente");
    }
    const json& k = req[field];
    if (k.is_number_unsigned()) return k.get<uint64_t>();
    if (k.is_number_integer()) {
        throw ProtocolError(std::string("'") + field + "' deve ser uint64 (recebido valor negativo)");
    }
    if (k.is_number_float()) {
        throw ProtocolError(std::string("'") + field +
                            "' deve ser um inteiro uint64 (recebido número não inteiro ou fora do intervalo)");
    }
    throw ProtocolError(std::string("'") + field + "' deve ser um número inteiro uint64, recebido " +
                        k.type_name());
}

std::string read_value(const json& req) {
    if (!req.contains("value")) throw ProtocolError("campo 'value' ausente");
    const json& v = req["value"];
    if (!v.is_string()) {
        throw ProtocolError(std::string("'value' deve ser string, recebido ") + v.type_name());
    }
    return v.get<std::string>();
}

ojson error_response(const json& id, const std::string& message) {
    ojson r;
    r["id"] = id;
    r["status"] = "error";
    r["message"] = message;
    return r;
}

// Executa uma operação e monta a resposta. Nunca lança: qualquer falha vira
// {"status":"error","message":...} para que uma linha ruim não interrompa o
// processamento das demais.
ojson handle_operation(ed2::Engine& engine, const json& req) {
    if (!req.is_object()) return error_response(nullptr, "cada linha deve ser um objeto JSON");

    json id = req.contains("id") ? req["id"] : json(nullptr);
    if (!req.contains("id")) return error_response(id, "campo 'id' ausente");

    if (!req.contains("op") || !req["op"].is_string()) {
        return error_response(id, "campo 'op' ausente ou não é string");
    }
    std::string op = req["op"].get<std::string>();
    std::transform(op.begin(), op.end(), op.begin(), [](unsigned char c) { return std::tolower(c); });

    ojson resp;
    resp["id"] = id;
    try {
        if (op == "put") {
            uint64_t key = read_key(req);
            std::string value = read_value(req);
            engine.put(key, value);
            resp["status"] = "ok";
        } else if (op == "get") {
            auto value = engine.get(read_key(req));
            if (value) {
                resp["status"] = "ok";
                resp["value"] = *value;
            } else {
                resp["status"] = "not_found";
            }
        } else if (op == "delete") {
            resp["status"] = engine.remove(read_key(req)) ? "ok" : "not_found";
        } else if (op == "verify") {
            auto v = engine.verify();
            if (v.ok) {
                resp["status"] = "ok";
            } else {
                return error_response(id, "falha de integridade: " + v.message);
            }
        } else if (op == "scan") {
            uint64_t start = read_key(req, "start");
            uint64_t end = read_key(req, "end");
            ojson arr = ojson::array();
            for (const auto& [k, v] : engine.scan(start, end)) arr.push_back({{"key", k}, {"value", v}});
            resp["status"] = "ok";
            resp["records"] = arr;
        } else {
            return error_response(id, "operação desconhecida: '" + op + "'");
        }
    } catch (const std::exception& e) {
        return error_response(id, e.what());
    }
    return resp;
}

// Para linhas que nem são JSON válido, tenta ao menos recuperar o id para
// que a resposta de erro possa ser associada à operação.
json salvage_id(const std::string& line) {
    static const std::regex re(R"re("id"\s*:\s*(-?\d+|"(?:[^"\\]|\\.)*"))re");
    std::smatch m;
    if (std::regex_search(line, m, re)) {
        try {
            return json::parse(m[1].str());
        } catch (...) {
        }
    }
    return nullptr;
}

std::string dump(const ojson& j) {
    // replace: nunca lança por causa de UTF-8 inválido.
    return j.dump(-1, ' ', false, ojson::error_handler_t::replace);
}

int cmd_run(const Flags& flags) {
    const std::string data_dir = require_flag(flags, "data-dir");

    std::ifstream input_file;
    std::istream* input = &std::cin;
    if (auto it = flags.find("input"); it != flags.end()) {
        input_file.open(it->second, std::ios::binary);
        if (!input_file) throw std::runtime_error("não foi possível abrir --input '" + it->second + "'");
        input = &input_file;
    }

    std::ofstream output_file;
    std::ostream* output = &std::cout;
    if (auto it = flags.find("output"); it != flags.end()) {
        output_file.open(it->second, std::ios::binary | std::ios::trunc);
        if (!output_file) throw std::runtime_error("não foi possível abrir --output '" + it->second + "'");
        output = &output_file;
    }

    // Se o diretório de dados não puder ser aberto, continuamos lendo a
    // entrada e respondemos erro a cada operação (uma resposta por linha).
    std::unique_ptr<ed2::Engine> engine;
    std::string open_error;
    try {
        engine = std::make_unique<ed2::Engine>(data_dir);
        if (engine->corrupted()) {
            std::cerr << "aviso: " << engine->log_path() << " contém dados corrompidos ("
                      << engine->corruption_message()
                      << "). Leituras seguem sobre a parte íntegra; escritas serão recusadas.\n";
        }
    } catch (const std::exception& e) {
        open_error = std::string("não foi possível abrir o engine: ") + e.what();
        std::cerr << "erro: " << open_error << "\n";
    }

    std::string line;
    uint64_t line_no = 0;
    while (std::getline(*input, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line_no == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // BOM
        if (line.find_first_not_of(" \t") == std::string::npos) continue;  // linha em branco

        ojson resp;
        json req;
        bool parsed = true;
        try {
            req = json::parse(line);
        } catch (const std::exception& e) {
            parsed = false;
            resp = error_response(salvage_id(line),
                                  "JSON inválido na linha " + std::to_string(line_no) + ": " + e.what());
        }
        if (parsed) {
            if (!engine) {
                json id = (req.is_object() && req.contains("id")) ? req["id"] : json(nullptr);
                resp = error_response(id, open_error);
            } else {
                resp = handle_operation(*engine, req);
            }
        }
        *output << dump(resp) << '\n';
    }

    output->flush();
    if (!*output) throw std::runtime_error("falha ao gravar o arquivo de saída");
    return engine ? 0 : 1;
}

int cmd_verify(const Flags& flags) {
    ed2::Engine engine(require_flag(flags, "data-dir"));
    auto v = engine.verify();
    ojson out;
    out["status"] = v.ok ? "ok" : "error";
    if (!v.ok) out["message"] = v.message;
    out["valid_records"] = v.valid_records;
    out["valid_bytes"] = v.valid_bytes;
    out["file_bytes"] = v.file_bytes;
    out["live_keys"] = v.live_keys;
    std::cout << dump(out) << "\n";
    return v.ok ? 0 : 1;
}

int cmd_repair(const Flags& flags) {
    ed2::Engine engine(require_flag(flags, "data-dir"));
    uint64_t moved = engine.repair();
    ojson out;
    out["status"] = "ok";
    out["quarantined_bytes"] = moved;
    out["live_keys"] = engine.size();
    std::cout << dump(out) << "\n";
    return 0;
}

int cmd_init(const Flags& flags) {
    std::string dir = require_flag(flags, "data-dir");
    ed2::Engine engine(dir);
    std::cerr << "engine inicializado em " << dir << "\n";
    return 0;
}

int cmd_describe() {
    ojson out;
    out["engine"] = "ed2-persistent-storage-engine";
    out["architecture"] = "log append-only (data.log) + índice hash em memória chave->offset (estilo Bitcask)";
    out["key_type"] = "uint64";
    out["value_type"] = "string de tamanho variável (até 4 GiB - 1 bytes)";
    out["supported_ops"] = {"put", "get", "delete", "verify", "scan"};
    out["integrity"] = "cabeçalho com magic/versão + CRC32 por registro";
    std::cout << out.dump(2) << "\n";
    return 0;
}

void usage() {
    std::cerr << "uso: engine <run|verify|repair|init|describe> [--data-dir DIR] [--input ARQ] [--output ARQ]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];
    try {
        Flags flags = parse_flags(argc, argv, 2);
        if (cmd == "run") return cmd_run(flags);
        if (cmd == "verify") return cmd_verify(flags);
        if (cmd == "repair") return cmd_repair(flags);
        if (cmd == "init") return cmd_init(flags);
        if (cmd == "describe") return cmd_describe();
        std::cerr << "comando desconhecido: " << cmd << "\n";
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "erro: " << e.what() << "\n";
        return 1;
    }
}
