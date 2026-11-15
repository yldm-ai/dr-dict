#include "dr/agent.h"
#include "dr/compress.h"
#include "dr/library.h"
#include "dr/mdict.h"
#include "dr/online.h"
#include "dr/ripemd128.h"
#include "dr/stardict.h"
#include "dr/text.h"
#include <cstdio>
#include <iostream>
#include <miniz.h>
#include <windows.h>

using namespace dr;
static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++failures;                                                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed\n";                                \
        }                                                                                                              \
    } while (0)

namespace
{
std::string be(uint64_t v, int bytes)
{
    std::string s(bytes, '\0');
    for (int i = bytes - 1; i >= 0; --i, v >>= 8)
        s[i] = static_cast<char>(v & 0xff);
    return s;
}
std::string le32(uint32_t v)
{
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i)
        s[i] = static_cast<char>((v >> (8 * i)) & 0xff);
    return s;
}
std::string utf16(std::string_view s)
{
    auto w = widen(s);
    return std::string(reinterpret_cast<const char *>(w.data()), w.size() * 2);
}
std::string zlib(std::string_view raw)
{
    mz_ulong size = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::string out(size, '\0');
    mz_compress(reinterpret_cast<unsigned char *>(out.data()), &size, reinterpret_cast<const unsigned char *>(raw.data()),
                static_cast<mz_ulong>(raw.size()));
    out.resize(size);
    return out;
}
std::string block(std::string_view raw)
{
    return std::string("\x02\0\0\0", 4) + be(adler32(raw), 4) + zlib(raw);
}
void encrypt_key_index(std::string &b)
{
    auto key = ripemd128(b.substr(4, 4) + std::string("\x95\x36\x00\x00", 4));
    uint8_t previous = 0x36;
    for (size_t i = 8; i < b.size(); ++i)
    {
        uint8_t plain = static_cast<uint8_t>(b[i]);
        uint8_t mixed = static_cast<uint8_t>(plain ^ previous ^ ((i - 8) & 0xff) ^ key[(i - 8) % 16]);
        uint8_t c = static_cast<uint8_t>((mixed >> 4) | (mixed << 4));
        b[i] = static_cast<char>(c);
        previous = c;
    }
}
// Writes a minimal MDict 2.0 file: one key block and one record block, zlib compressed.
void write_mdict(const fs::path &path, const std::vector<std::pair<std::string, std::string>> &entries, bool mdd,
                 bool encrypted, const std::string &extra = "")
{
    std::string header = std::string(mdd ? "<Library_Data" : "<Dictionary") +
                         " GeneratedByEngineVersion=\"2.0\" RequiredEngineVersion=\"2.0\" Encrypted=\"" +
                         (encrypted ? "2" : "0") + "\" Encoding=\"" + (mdd ? "" : "UTF-8") + "\" " + extra + "/>\r\n";
    auto header16 = utf16(header);
    std::string out = be(header16.size(), 4) + header16 + le32(adler32(header16));
    std::string records, keys;
    for (auto &[key, value] : entries)
    {
        keys += be(records.size(), 8);
        keys += mdd ? utf16(key) + std::string(2, '\0') : key + std::string(1, '\0');
        records += value;
        if (!mdd)
            records.push_back('\0');
    }
    auto key_block = block(keys);
    auto text = [&](const std::string &s) {
        auto units = mdd ? widen(s).size() : s.size();
        return be(units, 2) + (mdd ? utf16(s) + std::string(2, '\0') : s + std::string(1, '\0'));
    };
    std::string info_raw = be(entries.size(), 8) + text(entries.front().first) + text(entries.back().first) +
                           be(key_block.size(), 8) + be(keys.size(), 8);
    auto info = block(info_raw);
    if (encrypted)
        encrypt_key_index(info);
    std::string kh = be(1, 8) + be(entries.size(), 8) + be(info_raw.size(), 8) + be(info.size(), 8) +
                     be(key_block.size(), 8);
    out += kh + be(adler32(kh), 4) + info + key_block;
    auto record_block = block(records);
    out += be(1, 8) + be(entries.size(), 8) + be(16, 8) + be(record_block.size(), 8);
    out += be(record_block.size(), 8) + be(records.size(), 8) + record_block;
    write_file(path, out);
}
std::string gzip(std::string_view raw)
{
    size_t size = 0;
    void *data = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &size, TDEFL_DEFAULT_MAX_PROBES);
    std::string out = std::string("\x1f\x8b\x08\0\0\0\0\0\0\x03", 10) +
                      std::string(static_cast<const char *>(data), size) + le32(0) +
                      le32(static_cast<uint32_t>(raw.size()));
    mz_free(data);
    return out;
}
std::string hex(const std::array<uint8_t, 16> &d)
{
    std::string s;
    char buf[3];
    for (auto b : d)
    {
        snprintf(buf, sizeof buf, "%02x", b);
        s += buf;
    }
    return s;
}
void require(bool ok, const char *message)
{
    if (!ok)
    {
        ++failures;
        std::cerr << "require failed: " << message << "\n";
    }
}
template <class F> void rejects(F fn, const char *message)
{
    bool failed = false;
    try
    {
        fn();
    }
    catch (const std::exception &)
    {
        failed = true;
    }
    require(failed, message);
}
// From DR Reader's tests/engine_tests.cpp, which this agent module comes from.
// Event lines are trimmed copies of real output from codex-cli 0.167, Claude Code 2.1 and opencode 1.19.
void agent_checks()
{
    using dr::AgentEvent;
    using dr::AgentKind;
    auto texts = [](const std::vector<AgentEvent> &events) {
        std::string text;
        for (auto &event : events)
            if (event.type == AgentEvent::Type::Text) text += event.text;
        return text;
    };
    {
        dr::AgentStream claude(AgentKind::Claude);
        require(claude.line(R"({"type":"system","subtype":"init","tools":[],"mcp_servers":[]})").empty(), "claude init is silent");
        std::string answer;
        answer += texts(claude.line(R"({"type":"stream_event","event":{"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}},"parent_tool_use_id":null})"));
        answer += texts(claude.line(R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"书中"}},"parent_tool_use_id":null})"));
        answer += texts(claude.line(R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"x"}},"parent_tool_use_id":null})"));
        answer += texts(claude.line(R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"写道"}},"parent_tool_use_id":"tool_1"})"));
        answer += texts(claude.line(R"({"type":"assistant","message":{"content":[{"type":"text","text":"书中"}]}})"));
        auto retry = claude.line(R"({"type":"system","subtype":"api_retry","attempt":2,"max_retries":10})");
        require(retry.size() == 1 && retry[0].type == AgentEvent::Type::Status, "claude retry is progress");
        require(claude.line("not json at all").empty() && !claude.succeeded(), "claude ignores noise");
        claude.line(R"({"type":"result","subtype":"success","is_error":false,"result":"书中"})");
        require(answer == "书中" && claude.succeeded(), "claude streams text deltas only once");
        dr::AgentStream failing(AgentKind::Claude);
        auto failed = failing.line(R"({"type":"result","subtype":"success","is_error":true,"result":"API Error: 503"})");
        require(failing.failed() && failed.at(0).type == AgentEvent::Type::Error && failing.last_error() == "API Error: 503", "claude errors fail");
    }
    // Codex app-server and opencode ACP are conversations: each reply is one JSON-RPC line per request.
    auto requests = [](const std::string &reply) {
        std::vector<dr::Json> lines;
        for (size_t start = 0, end; (end = reply.find('\n', start)) != reply.npos; start = end + 1)
            lines.push_back(dr::Json::parse(reply.substr(start, end - start)));
        return lines;
    };
    {
        dr::AgentStream codex(AgentKind::Codex);
        require(codex.interactive(), "codex runs as a server");
        auto opening = requests(codex.start("问题", "gpt-6.1", "high", "C:/tmp/work"));
        require(opening.size() == 1 && opening[0]["method"] == "initialize" && opening[0]["id"] == 1, "codex initializes first");
        codex.line(R"({"id":1,"result":{"userAgent":"codex"}})");
        auto thread = requests(codex.take_reply());
        require(thread.size() == 2 && thread[0]["method"] == "initialized" && thread[1]["method"] == "thread/start" &&
                    thread[1]["params"]["model"] == "gpt-6.1" && thread[1]["params"]["sandbox"] == "read-only" &&
                    thread[1]["params"]["approvalPolicy"] == "never" && thread[1]["params"]["ephemeral"] == true,
                "codex starts a read-only, ephemeral thread with the chosen model");
        codex.line(R"({"id":2,"result":{"thread":{"id":"thr_1"},"model":"gpt-6.1"}})");
        auto turn = requests(codex.take_reply());
        require(turn.size() == 1 && turn[0]["method"] == "turn/start" && turn[0]["params"]["threadId"] == "thr_1" &&
                    turn[0]["params"]["effort"] == "high" && turn[0]["params"]["input"][0]["text"] == "问题",
                "codex asks the question with the effort");
        codex.line(R"({"id":3,"result":{"turn":{"id":"turn_1","items":[],"status":"inProgress"}}})");
        auto approval = requests((codex.line(R"({"id":0,"method":"item/commandExecution/requestApproval","params":{}})"), codex.take_reply()));
        require(approval.size() == 1 && approval[0]["id"] == 0 && approval[0].contains("error"), "codex server requests are refused");
        auto retry = codex.line(R"({"method":"error","params":{"error":{"message":"Reconnecting… 1/5"},"threadId":"thr_1","turnId":"turn_1","willRetry":true}})");
        require(retry.size() == 1 && retry[0].type == AgentEvent::Type::Status, "codex retries are progress");
        std::string answer;
        answer += texts(codex.line(R"({"method":"item/agentMessage/delta","params":{"delta":"第一","itemId":"msg_1","threadId":"thr_1","turnId":"turn_1"}})"));
        answer += texts(codex.line(R"({"method":"item/agentMessage/delta","params":{"delta":"段","itemId":"msg_1","threadId":"thr_1","turnId":"turn_1"}})"));
        answer += texts(codex.line(R"({"method":"item/completed","params":{"item":{"id":"msg_1","type":"agentMessage","text":"第一段"}}})"));
        answer += texts(codex.line(R"({"method":"item/agentMessage/delta","params":{"delta":"第二段","itemId":"msg_2","threadId":"thr_1","turnId":"turn_1"}})"));
        require(answer == "第一段\n\n第二段" && !codex.finished(), "codex streams deltas; messages become paragraphs");
        codex.line(R"({"method":"turn/completed","params":{"threadId":"thr_1","turn":{"id":"turn_1","items":[],"status":"completed"}}})");
        require(codex.succeeded() && codex.finished(), "codex turn completion succeeds");
        dr::AgentStream failing(AgentKind::Codex);
        failing.start("问题");
        failing.line(R"({"id":1,"result":{}})");
        require(!requests(failing.take_reply())[1]["params"].contains("model"), "codex keeps its default model");
        failing.line(R"({"id":2,"result":{"thread":{"id":"thr_2"}}})");
        require(!requests(failing.take_reply())[0]["params"].contains("effort"), "codex keeps its default effort");
        failing.line(R"({"method":"error","params":{"error":{"message":"The 'x' model is not supported"},"threadId":"thr_2","turnId":"t","willRetry":false}})");
        failing.line(R"({"method":"turn/completed","params":{"threadId":"thr_2","turn":{"id":"t","items":[],"status":"failed","error":null}}})");
        require(failing.failed() && failing.last_error() == "The 'x' model is not supported", "codex turn failure");
        dr::AgentStream rejected(AgentKind::Codex);
        rejected.start("问题", "gpt-6.1");
        rejected.line(R"({"id":1,"error":{"code":-32600,"message":"Not initialized"}})");
        require(rejected.failed() && rejected.last_error() == "Not initialized", "codex request errors fail");
    }
    {
        dr::AgentStream opencode(AgentKind::OpenCode);
        auto opening = requests(opencode.start("问题", "magpie/deepseek/deepseek-flash", "high", "C:/tmp/work"));
        require(opening.size() == 1 && opening[0]["method"] == "initialize" && opening[0]["params"]["protocolVersion"] == 1 &&
                    opening[0]["params"]["clientCapabilities"]["terminal"] == false,
                "opencode initializes ACP without client capabilities");
        opencode.line(R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1,"agentCapabilities":{}}})");
        auto session = requests(opencode.take_reply());
        require(session.size() == 1 && session[0]["method"] == "session/new" && session[0]["params"]["cwd"] == "C:/tmp/work" &&
                    session[0]["params"]["mcpServers"].empty(),
                "opencode opens a session in the working directory");
        opencode.line(R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"ses_1","configOptions":[]}})");
        auto model = requests(opencode.take_reply());
        require(model.size() == 1 && model[0]["method"] == "session/set_config_option" && model[0]["params"]["configId"] == "model" &&
                    model[0]["params"]["value"] == "magpie/deepseek/deepseek-flash" && model[0]["params"]["sessionId"] == "ses_1",
                "opencode selects the model");
        opencode.line(R"({"jsonrpc":"2.0","id":3,"result":{"configOptions":[]}})");
        auto effort = requests(opencode.take_reply());
        require(effort.size() == 1 && effort[0]["params"]["configId"] == "effort" && effort[0]["params"]["value"] == "high", "opencode selects the effort");
        opencode.line(R"({"jsonrpc":"2.0","id":4,"result":{"configOptions":[]}})");
        auto prompt = requests(opencode.take_reply());
        require(prompt.size() == 1 && prompt[0]["method"] == "session/prompt" && prompt[0]["params"]["prompt"][0]["text"] == "问题",
                "opencode asks the question");
        auto permission = requests((opencode.line(R"({"jsonrpc":"2.0","id":0,"method":"session/request_permission","params":{"sessionId":"ses_1"}})"), opencode.take_reply()));
        require(permission.size() == 1 && permission[0]["result"]["outcome"]["outcome"] == "cancelled", "opencode permissions are declined");
        std::string answer;
        answer += texts(opencode.line(R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"ses_1","update":{"sessionUpdate":"agent_thought_chunk","messageId":"prt_0","content":{"type":"text","text":"想"}}}})"));
        answer += texts(opencode.line(R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"ses_1","update":{"sessionUpdate":"agent_message_chunk","messageId":"msg_1","content":{"type":"text","text":"甲"}}}})"));
        answer += texts(opencode.line(R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"ses_1","update":{"sessionUpdate":"agent_message_chunk","messageId":"msg_1","content":{"type":"text","text":"乙"}}}})"));
        answer += texts(opencode.line(R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"ses_1","update":{"sessionUpdate":"agent_message_chunk","messageId":"msg_2","content":{"type":"text","text":"丙"}}}})"));
        require(answer == "甲乙\n\n丙" && !opencode.finished(), "opencode streams chunks, skips thoughts, and separates messages");
        opencode.line(R"({"jsonrpc":"2.0","id":5,"result":{"stopReason":"end_turn"}})");
        require(opencode.succeeded() && opencode.finished(), "opencode end of turn succeeds");
        dr::AgentStream plain(AgentKind::OpenCode);
        plain.start("问题", "", "", "C:/tmp/work");
        plain.line(R"({"jsonrpc":"2.0","id":1,"result":{}})");
        plain.take_reply();
        plain.line(R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"ses_2"}})");
        require(requests(plain.take_reply())[0]["method"] == "session/prompt", "opencode keeps its defaults without options");
        dr::AgentStream failing(AgentKind::OpenCode);
        failing.start("问题", "x/y", "", "C:/tmp/work");
        failing.line(R"({"jsonrpc":"2.0","id":1,"result":{}})");
        failing.line(R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"ses_3"}})");
        failing.line(R"({"jsonrpc":"2.0","id":3,"error":{"code":-32603,"message":"Internal error","data":{"message":"Model not found: x/y"}}})");
        require(failing.failed() && failing.last_error() == "Internal error：Model not found: x/y", "opencode errors explain themselves");
        dr::AgentStream refused(AgentKind::OpenCode);
        refused.start("问题", "", "", "C:/tmp/work");
        refused.line(R"({"jsonrpc":"2.0","id":5,"result":{"stopReason":"refusal"}})");
        require(refused.failed(), "opencode refusals fail");
    }
    // Arguments: tool-free, read-only, prompt on stdin, no cmd.exe metacharacters.
    auto codex_args = dr::agent_arguments(AgentKind::Codex, "gpt-6.1", "high", std::vector<std::string>{"node_repl", "bad name"});
    require(codex_args.front() == "app-server" && std::count(codex_args.begin(), codex_args.end(), "mcp_servers.node_repl.enabled=false") == 1 &&
                std::none_of(codex_args.begin(), codex_args.end(), [](auto &arg) { return arg.find("bad name") != arg.npos; }),
            "codex runs app-server and disables valid MCP names");
    auto effortful = dr::agent_arguments(AgentKind::Claude, "opus", "max");
    auto flag = std::find(effortful.begin(), effortful.end(), "--effort");
    require(flag != effortful.end() && flag + 1 != effortful.end() && flag[1] == "max", "claude effort flag");
    require(dr::agent_arguments(AgentKind::OpenCode, "opencode/beam-free", "xhigh") == std::vector<std::string>{"acp"}, "opencode runs ACP");
    for (auto kind : {AgentKind::Codex, AgentKind::Claude, AgentKind::OpenCode})
    {
        auto plain = dr::agent_arguments(kind, "");
        require(std::none_of(plain.begin(), plain.end(), [](auto &arg) { return arg == "--effort" || arg == "--variant" || arg.starts_with("model_reasoning_effort"); }),
                "no effort keeps the CLI default");
    }
    rejects([] { dr::agent_arguments(AgentKind::Claude, "", "high&calc"); }, "efforts cannot inject commands");
    rejects([] { dr::agent_arguments(AgentKind::Codex, "", "a=b"); }, "efforts cannot set other Codex options");
    auto claude_args = dr::agent_arguments(AgentKind::Claude, "");
    require(std::find(claude_args.begin(), claude_args.end(), "--no-session-persistence") != claude_args.end() &&
                std::find(claude_args.begin(), claude_args.end(), "--model") == claude_args.end(),
            "claude does not persist sessions and keeps its default model");
    for (auto kind : {AgentKind::Codex, AgentKind::Claude, AgentKind::OpenCode})
        for (auto &arg : dr::agent_arguments(kind, "anthropic/claude-sonnet-5", "high"))
            require(arg.find_first_of("&|<>^%!\"") == arg.npos, "agent arguments are cmd-safe");
    rejects([] { dr::agent_arguments(AgentKind::OpenCode, "x & calc"); }, "model names cannot inject commands");
    auto names = dr::codex_mcp_server_names("model = \"x\"\n[mcp_servers.node_repl]\n[mcp_servers.node_repl.env]\n  [mcp_servers.\"quoted-one\"]\n[mcp_servers.bad$name]\n[plugins.\"a@b\"]\n");
    require(names == std::vector<std::string>{"node_repl", "quoted-one"}, "codex MCP server names");
    // (DR Reader's own settings checks are replaced by the DR Dict ones in main.)

    // Model discovery: no model turn, only the CLIs' own listing protocols.
    {
        dr::AgentModelProbe codex(AgentKind::Codex);
        auto args = codex.arguments({"node_repl"});
        require(args.at(0) == "app-server" && std::count(args.begin(), args.end(), "mcp_servers.node_repl.enabled=false") == 1 &&
                    codex.interactive() && codex.start().find(R"("method":"initialize")") != std::string::npos,
                "codex lists models through app-server");
        require(codex.line(R"({"method":"remoteControl/status/changed","params":{"status":"disabled"}})").empty(), "codex notifications need no reply");
        auto refusal = codex.line(R"({"id":7,"method":"item/tool/requestUserInput","params":{}})");
        require(refusal.find(R"("id":7)") != std::string::npos && refusal.find("-32601") != std::string::npos, "codex server requests are refused");
        auto next = codex.line(R"({"id":1,"result":{"userAgent":"dr-reader/0.167.0","codexHome":"C:\\Users\\x\\.codex"}})");
        require(next.find(R"("method":"initialized")") != std::string::npos && next.find(R"("method":"model/list")") != std::string::npos,
                "codex asks for models once initialized");
        auto page = codex.line(R"({"id":2,"result":{"data":[{"id":"gpt-6.1-sol","model":"gpt-6.1-sol","displayName":"GPT-6.1-Sol","description":"Latest workhorse model.","hidden":false,"supportedReasoningEfforts":[{"reasoningEffort":"low","description":"Fast"},{"reasoningEffort":"high","description":"Deep"},{"reasoningEffort":"bad effort"}],"defaultReasoningEffort":"low","isDefault":true},{"id":"x","model":"internal-x","displayName":"X","hidden":true,"isDefault":false}],"nextCursor":"c2"}})");
        require(page.find(R"("cursor":"c2")") != std::string::npos && page.find(R"("id":3)") != std::string::npos && !codex.finished(),
                "codex follows the next page");
        codex.line(R"({"id":3,"result":{"data":[{"model":"gpt-6-astra","displayName":"GPT-6-Astra","isDefault":false},{"model":"gpt-6.1-sol","displayName":"dup"}],"nextCursor":null}})");
        auto models = codex.models();
        require(codex.finished() && !codex.failed() && models.size() == 2 && models[0].id == "gpt-6.1-sol" && models[0].is_default &&
                    models[0].name == "GPT-6.1-Sol" && models[1].id == "gpt-6-astra" &&
                    models[0].efforts == std::vector<std::string>{"low", "high"} && models[0].default_effort == "low" && models[1].efforts.empty(),
                "codex models: hidden and duplicate entries dropped");
        dr::AgentModelProbe rejected(AgentKind::Codex);
        rejected.line(R"({"id":1,"error":{"code":-32600,"message":"Not logged in"}})");
        require(rejected.failed() && rejected.error() == "Not logged in", "codex discovery error");
    }
    {
        dr::AgentModelProbe claude(AgentKind::Claude);
        auto args = claude.arguments();
        require(std::find(args.begin(), args.end(), "stream-json") != args.end() && std::find(args.begin(), args.end(), "--no-session-persistence") != args.end() &&
                    claude.start().find(R"("subtype":"initialize")") != std::string::npos,
                "claude lists models through the initialize control request");
        claude.line(R"({"type":"system","subtype":"init"})");
        require(!claude.finished(), "claude waits for its control response");
        claude.line(R"j({"type":"control_response","response":{"subtype":"success","request_id":"models","response":{"commands":[],"models":[{"value":"default","resolvedModel":"claude-opus-5-5","displayName":"Default (recommended)"},{"value":"opus","resolvedModel":"claude-opus-5-5","displayName":"Opus 5.5","description":"Custom Opus model","supportsEffort":true,"supportedEffortLevels":["low","medium","high","xhigh","max"]},{"value":"sonnet","resolvedModel":"claude-sonnet-5-5","displayName":"Sonnet 5.5"},{"value":"opus[1m]","resolvedModel":"claude-opus-5-5[1m]","displayName":"Opus (1M context)"},{"value":"bad model","displayName":"Bad"}]}}})j");
        auto models = claude.models();
        require(claude.finished() && models.size() == 3 && models[0].id == "opus" && models[0].is_default && !models[1].is_default &&
                    models[2].id == "opus[1m]" && models[0].efforts.size() == 5 && models[0].efforts.back() == "max" && models[1].efforts.empty(),
                "claude models: aliases kept, default marked, unsafe ids dropped");
        dr::AgentModelProbe rejected(AgentKind::Claude);
        rejected.line(R"({"type":"control_response","response":{"subtype":"error","request_id":"models","error":"Invalid API key"}})");
        require(rejected.failed() && rejected.error() == "Invalid API key", "claude discovery error");
    }
    {
        dr::AgentModelProbe opencode(AgentKind::OpenCode);
        require(opencode.arguments() == std::vector<std::string>{"models", "--verbose"} && !opencode.interactive() && opencode.start().empty(),
                "opencode lists models with `opencode models --verbose`");
        // Verbose output: the id line, then the model object; variants are its reasoning efforts.
        for (auto line : {"opencode/beam-free\r", "{", "  \"name\": \"Beam Free\",", "  \"limit\": {", "    \"context\": 1048576", "  },",
                          "  \"variants\": {", "    \"low\": { \"reasoningEffort\": \"low\" },", "    \"xhigh\": {}, \"max\": {}", "  }", "}\r",
                          "magpie/auto/gpt-5.6-luna", "{", "  \"name\": \"GPT-5.6 Luna\",", "  \"variants\": {}", "}",
                          "", "Warning: something", "opencode/beam-free", "plain/model-without-details"})
            opencode.line(line);
        opencode.end();
        auto models = opencode.models();
        require(models.size() == 3 && models[0].id == "opencode/beam-free" && models[0].name == "Beam Free" &&
                    models[0].efforts == std::vector<std::string>{"low", "xhigh", "max"} && models[1].name == "GPT-5.6 Luna" &&
                    models[1].efforts.empty() && models[2].name == "plain/model-without-details",
                "opencode models: names and variants in the CLI's order");
    }
}
} // namespace

int main()
{
    // RIPEMD-128 reference vectors from the algorithm's specification.
    CHECK(hex(ripemd128("")) == "cdf26213a150dc3ecb610f18f6b38b46");
    CHECK(hex(ripemd128("abc")) == "c14a12199c66e4ba84636b0f69144c77");
    CHECK(hex(ripemd128("message digest")) == "9e327b3d6e523062afc1132d7df9d1b8");

    // LZO1X: a literal run, an M2 back-reference and the end-of-stream marker.
    CHECK(lzo1x_decompress(std::string("\x16" "abcde" "\x11\0\0", 9), 5) == "abcde");
    CHECK(lzo1x_decompress(std::string("\x16" "abcde" "\x50\x00" "\x11\0\0", 11), 8) == "abcdeabc");

    CHECK(fold_key("Café") == fold_key("cafe"));
    CHECK(fold_key("Ice-Cream") == fold_key("icecream"));
    CHECK(fold_key("  Hello  ") == "hello");
    CHECK(fold_key("が") != fold_key("か"));
    CHECK(fold_key("+") == "+");
    CHECK(to_utf8(std::string("\xc4\xe3\xba\xc3", 4), "GBK") == "你好");
    CHECK(apply_mdict_styles("`1`bold`2`plain", {{1, {"<b>", "</b>"}}}) == "<b>bold</b>plain");
    CHECK(decode_entities("&lt;a&gt; &amp; &#x27;q&#39;") == "<a> & 'q'");
    auto forms = english_base_forms("studies");
    CHECK(std::find(forms.begin(), forms.end(), "study") != forms.end());
    forms = english_base_forms("running");
    CHECK(std::find(forms.begin(), forms.end(), "run") != forms.end());

    agent_checks();

    // Online sources, with a fake network.
    CHECK(md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(code_points("a苹果") == 3);
    CHECK(translation_direction("今天天气很好。").from == "zh");
    CHECK(translation_direction("我喜欢 Python 编程").to == "en");
    CHECK(translation_direction("The weather is nice today.").from == "en");
    CHECK(translation_direction("I visited 北京 last year").to == "zh");
    // authStr: "key=value" pairs in key order, apikey included, empty values and authStr left out.
    CHECK(NiuTrans::auth_str({{"to", "zh"}, {"from", "en"}, {"empty", ""}, {"authStr", "x"}}, "k") ==
          md5_hex("apikey=k&from=en&to=zh"));
    {
        std::vector<HttpRequest> sent;
        NiuTrans niu(
            [&](const HttpRequest &request) {
                sent.push_back(request);
                return HttpResponse{200, R"({"from":"en","to":"zh","tgtText":" 今天天气很好。 "})"};
            },
            [] { return 1700000000000LL; });
        bool threw = false;
        try
        {
            niu.translate("Hello");
        }
        catch (const std::runtime_error &)
        {
            threw = true;
        }
        CHECK(threw && sent.empty()); // no credentials, no request
        niu.set_credentials("app", "key");
        auto result = niu.translate("  The weather is nice today.  ");
        CHECK(result["translation"] == "今天天气很好。");
        CHECK(result["from"] == "en" && result["to"] == "zh");
        CHECK(sent.size() == 1);
        if (!sent.empty())
        {
            CHECK(sent[0].method == "POST" && sent[0].url == "https://api.niutrans.com/v2/text/translate");
            auto body = Json::parse(sent[0].body);
            CHECK(body["srcText"] == "The weather is nice today." && body["appId"] == "app");
            CHECK(body["timestamp"] == "1700000000000");
            std::map<std::string, std::string> params;
            for (auto &[k, v] : body.items())
                params[k] = v.get<std::string>();
            CHECK(body["authStr"] == NiuTrans::auth_str(params, "key"));
        }
        niu.translate("The weather is nice today.");
        CHECK(sent.size() == 1); // remembered
        niu.translate("The weather is nice today.", false);
        CHECK(sent.size() == 2);
        NiuTrans denied([](const HttpRequest &) { return HttpResponse{200, R"({"errorCode":"20001","errorMsg":"auth"})"}; });
        denied.set_credentials("app", "bad");
        std::string message;
        try
        {
            denied.translate("你好");
        }
        catch (const std::runtime_error &e)
        {
            message = e.what();
        }
        CHECK(message.find("鉴权失败") != std::string::npos);
    }
    {
        const auto &en = *find_wiki_site("en");
        CHECK(find_wiki_site("xx") == nullptr);
        CHECK(wiki_parse_url(en, "ice cream").find("&page=ice%20cream") != std::string::npos);
        const std::string wrapper = "<div class=\"mw-content-ltr mw-parser-output\" lang=\"en\" dir=\"ltr\">";
        const std::string page =
            wrapper + "<div class=\"disambig-see-also\">See also</div>"
                      "<div class=\"mw-heading mw-heading2\"><h2 id=\"English\">English</h2></div><p>en</p>"
                      "<div class=\"mw-heading mw-heading2\"><h2 id=\"French\">French</h2></div><p>fr</p>"
                      "<div class=\"mw-heading mw-heading2\"><h2 id=\"Chinese\">Chinese</h2></div><p>zh</p>"
                      "<div class=\"mw-heading mw-heading2\"><h2 id=\"Dutch\">Dutch</h2></div><p>nl</p></div>";
        auto kept = keep_wiki_languages(page, en.languages);
        CHECK(kept.find("<p>en</p>") != std::string::npos && kept.find("<p>zh</p>") != std::string::npos);
        CHECK(kept.find("<p>fr</p>") == std::string::npos && kept.find("<p>nl</p>") == std::string::npos);
        CHECK(kept.find("See also") != std::string::npos);
        CHECK(kept.size() >= 6 && kept.substr(kept.size() - 6) == "</div>"); // the wrapper stays closed
        CHECK(keep_wiki_languages(page, {"Latin"}) == page);                  // nothing matched: whole page

        auto html = rewrite_wiki_html(
            en, "<a href=\"/wiki/ice_cream#English\" title=\"x\">i</a><a href=\"/wiki/Appendix:Glossary#dated\">g</a>"
                "<a href=\"/w/index.php?title=x&amp;action=edit\">e</a><a href=\"#Noun\">n</a>"
                "<a href=\"https://en.wikipedia.org/wiki/hello\">w</a><img src=\"//upload.wikimedia.org/a.png\" "
                "srcset=\"//upload.wikimedia.org/b.png 1.5x, //upload.wikimedia.org/c.png 2x\">"
                "<span style=\"background:url(//upload.wikimedia.org/d.svg)\"></span>"
                "<script>alert(1)</script><SCRIPT src=\"x\"></SCRIPT><p>x=\"y\" stays</p>");
        CHECK(html.find("href=\"entry://ice%20cream\"") != std::string::npos);
        CHECK(html.find("href=\"https://en.wiktionary.org/wiki/Appendix:Glossary#dated\"") != std::string::npos);
        CHECK(html.find("href=\"https://en.wiktionary.org/w/index.php?title=x&amp;action=edit\"") != std::string::npos);
        CHECK(html.find("href=\"#Noun\"") != std::string::npos);
        CHECK(html.find("href=\"https://en.wikipedia.org/wiki/hello\"") != std::string::npos);
        CHECK(html.find("src=\"https://upload.wikimedia.org/a.png\"") != std::string::npos);
        CHECK(html.find("srcset=\"https://upload.wikimedia.org/b.png 1.5x, https://upload.wikimedia.org/c.png 2x\"") !=
              std::string::npos);
        CHECK(html.find("url(https://upload.wikimedia.org/d.svg)") != std::string::npos);
        CHECK(html.find("alert") == std::string::npos && lower_ascii(html).find("<script") == std::string::npos);
        CHECK(html.find("<p>x=\"y\" stays</p>") != std::string::npos);

        CHECK(!parse_wiki_reply(en, R"({"error":{"code":"missingtitle"}})"));
        auto article = parse_wiki_reply(en, Json{{"parse", {{"title", "hello"}, {"pageid", 1}, {"text", page}}}}.dump());
        CHECK(article && article->title == "hello" && article->html.find("<p>fr</p>") == std::string::npos);
        if (article)
        {
            auto frame = wiki_page(en, *article, "dark");
            CHECK(frame.find("data-dr-theme=\"dark\"") != std::string::npos);
            CHECK(frame.find("Content-Security-Policy") != std::string::npos);
            CHECK(frame.find("https://en.wiktionary.org/wiki/hello") != std::string::npos);
        }

        // "Hello" has no page of its own: the lowercase title is tried, and both are remembered.
        std::vector<std::string> asked;
        Wiktionary wiki([&](const HttpRequest &request) {
            asked.push_back(request.url);
            if (request.url.find("&page=hello") != std::string::npos)
                return HttpResponse{200, Json{{"parse", {{"title", "hello"}, {"text", "<p>hi</p>"}}}}.dump()};
            return HttpResponse{200, R"({"error":{"code":"missingtitle"}})"};
        });
        auto found = wiki.lookup(en, "Hello");
        CHECK(found && found->title == "hello" && asked.size() == 2);
        CHECK(wiki.lookup(en, "Hello") && wiki.lookup(en, "hello") && asked.size() == 2);
        CHECK(!wiki.lookup(en, "zzqq") && asked.size() == 4); // zzqq, then Zzqq
        CHECK(!wiki.lookup(en, "zzqq") && asked.size() == 4); // misses are remembered too
        CHECK(!wiki.lookup(en, std::string(81, 'a')) && asked.size() == 4);
    }

    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    auto root = fs::path(temp) / ("dr-dict-tests-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    auto dicts = root / "dicts";
    fs::create_directories(dicts / "sd" / "res");

    write_mdict(dicts / "demo.mdx",
                {{"apple", "<link rel=\"stylesheet\" href=\"demo.css\"><b>apple</b> 苹果 `1`fruit`0`"},
                 {"Banana", "<i>banana</i> 香蕉"},
                 {"pomme", "@@@LINK=apple"}},
                false, true, "Title=\"Demo &amp; Test\" Description=\"&lt;p&gt;About&lt;/p&gt;\" StyleSheet=\"1\n<em>\n</em>\n\"");
    // OALDPE names example audio like "_take#_gbs_1.mp3" (linked as sound://_take#_gbs_1.mp3).
    write_mdict(dicts / "demo.mdd",
                {{"\\demo.css", "b{color:red}"}, {"\\img\\A.png", "PNG"}, {"\\_take#_gbs_1.mp3", "MP3"}}, true, false);
    {
        MdictDictionary d(dicts / "demo.mdx");
        CHECK(d.title() == "Demo & Test");
        CHECK(d.description() == "<p>About</p>");
        std::vector<std::tuple<std::string, uint64_t, uint64_t>> words;
        IndexSink sink;
        sink.word = [&](std::string_view w, uint64_t a, uint64_t b) { words.emplace_back(std::string(w), a, b); };
        sink.resource = [&](std::string_view p, int, uint64_t, uint64_t) { words.emplace_back("res:" + std::string(p), 0, 0); };
        d.index(sink);
        CHECK(words.size() == 6);
        CHECK(std::get<0>(words[0]) == "apple");
        CHECK(d.article(std::get<1>(words[0]), std::get<2>(words[0])).find("<em>fruit</em>") != std::string::npos);
        CHECK(d.article(std::get<1>(words[2]), std::get<2>(words[2])) == "@@@LINK=apple");
        CHECK(std::get<0>(words[4]) == "res:img/A.png");
    }

    // StarDict with a .syn file and a gzip-compressed .dict.dz without a random-access field.
    std::string dict_data = std::string("hello world\nsecond line") + "goodbye";
    std::string idx = std::string("goodbye") + '\0' + be(23, 4) + be(7, 4) + "hello" + '\0' + be(0, 4) + be(23, 4);
    write_file(dicts / "sd" / "test.ifo", "StarDict's dict ifo file\nversion=2.4.2\nwordcount=2\nidxfilesize=" +
                                               std::to_string(idx.size()) + "\nbookname=Star Test\nsametypesequence=m\n");
    write_file(dicts / "sd" / "test.idx", idx);
    write_file(dicts / "sd" / "test.dict.dz", gzip(dict_data));
    write_file(dicts / "sd" / "test.syn", std::string("hi") + '\0' + be(1, 4));
    write_file(dicts / "sd" / "res" / "pic.png", "IMG");
    CHECK(stardict_to_html(std::string("thalo\0mmeaning", 14), "") ==
          "<div class=\"sd-t\">[halo]</div><div class=\"sd-m\">meaning</div>");

    {
        Library library(root / "data");
        auto result = library.add({dicts}, {});
        CHECK(result["added"] == 2);
        CHECK(result["errors"].empty());
        auto again = library.add({dicts / "demo.mdx"}, {});
        CHECK(again["skipped"] == 1);
        auto list = library.list();
        CHECK(list.size() == 2);
        std::string mdx_id, sd_id;
        for (auto &d : list)
            (d["format"] == "mdict" ? mdx_id : sd_id) = d["id"];
        CHECK(list[0]["status"] == "ready");

        auto suggestions = library.suggest("ba", 10);
        CHECK(suggestions.size() == 1 && suggestions[0] == "Banana");
        auto hit = library.lookup("APPLE");
        CHECK(hit["results"].size() == 1);
        auto plural = library.lookup("apples");
        CHECK(plural["word"] == "apples" || plural["word"] == "apple");
        CHECK(plural["results"].size() == 1);
        auto syn = library.lookup("hi");
        CHECK(syn["results"].size() == 1);
        // A search can be limited to some dictionaries.
        CHECK(library.lookup("apple", {sd_id})["results"].empty());
        CHECK(library.lookup("apple", {mdx_id})["results"].size() == 1);
        CHECK(library.suggest("ba", 10, {sd_id}).empty() && library.suggest("ba", 10, {mdx_id}).size() == 1);

        auto page = library.article_page(mdx_id, "pomme", "light");
        CHECK(page.find("苹果") != std::string::npos);
        CHECK(page.find("frame.js") != std::string::npos);
        auto star = library.article_page(sd_id, "hello", "dark");
        CHECK(star.find("hello world<br>second line") != std::string::npos);
        CHECK(library.article_page(sd_id, "goodbye", "light").find("goodbye") != std::string::npos);

        auto css = library.resource(mdx_id, "demo.css");
        CHECK(css && css->data == "b{color:red}" && css->mime == "text/css");
        auto img = library.resource(mdx_id, "IMG/a.png?x=1");
        CHECK(img && img->data == "PNG");
        CHECK(!library.resource(mdx_id, "../secret.txt"));
        // frame.js percent-encodes sound:// names, so a '#' inside the name arrives as %23; a raw '#'
        // is a URL fragment and must not reach the name.
        auto voice = library.resource(mdx_id, "_take%23_gbs_1.mp3");
        CHECK(voice && voice->data == "MP3" && voice->mime == "audio/mpeg");
        CHECK(!library.resource(mdx_id, "_take#_gbs_1.mp3"));
        auto pic = library.resource(sd_id, "pic.png");
        CHECK(pic && pic->data == "IMG");

        library.update(mdx_id, {{"enabled", false}});
        CHECK(library.lookup("apple")["results"].empty());
        library.update(mdx_id, {{"enabled", true}});
        auto order = library.reorder(Json::array({sd_id, mdx_id}));
        CHECK(order[0]["id"] == sd_id);
        CHECK(library.reindex(mdx_id, {}).size() == 2);
        CHECK(library.lookup("banana")["results"].size() == 1);
        library.remove(mdx_id);
        auto after = library.list();
        CHECK(after.size() == 2); // the removed dictionary is still listed …
        for (auto &d : after)
            if (d["id"] == mdx_id)
                CHECK(d["status"] == "removed" && d["enabled"] == false && d["fileMissing"] == false);
        CHECK(library.lookup("banana")["results"].empty()); // … but not looked up
        CHECK(library.suggest("ban", 10).empty());
        bool rejected = false;
        try
        {
            library.update(mdx_id, {{"enabled", true}});
        }
        catch (const std::exception &)
        {
            rejected = true;
        }
        CHECK(rejected);
        rejected = false;
        try
        {
            library.forget(mdx_id); // its file still exists
        }
        catch (const std::exception &)
        {
            rejected = true;
        }
        CHECK(rejected);
        library.restore(mdx_id, {});
        CHECK(library.lookup("banana")["results"].size() == 1);
        library.remove(mdx_id);

        library.remember("apple");
        library.remember("pear");
        auto history = library.remember("apple");
        CHECK(history.size() == 2 && history[0]["word"] == "apple");
        history = library.forget_word("apple");
        CHECK(history.size() == 1 && history[0]["word"] == "pear" && library.history().size() == 1);
        CHECK(library.favorites().empty());
        library.toggle_favorite("apple");
        auto stars = library.toggle_favorite("pear");
        CHECK(stars.size() == 2 && stars[0]["word"] == "pear");
        stars = library.toggle_favorite("apple"); // starring again unstars
        CHECK(stars.size() == 1 && library.favorites()[0]["word"] == "pear");
        CHECK(library.save_settings({{"volume", 3}})["volume"] == 1.0);
        CHECK(library.save_settings({{"volume", 0.5}})["volume"] == 0.5);
        auto scoped = library.save_settings({{"scope", Json::array({mdx_id, 3})}});
        CHECK(scoped["scope"] == Json::array({mdx_id}));
        auto settings = library.save_settings({{"theme", "dark"}, {"shell", "bogus"}});
        CHECK(settings["theme"] == "dark" && settings["shell"] == "mica");

        // Appearance: built-in schemes, custom colours, background image.
        CHECK(library.save_settings({{"theme", "night"}})["theme"] == "night");
        CHECK(library.save_settings({{"theme", "neon"}})["theme"] == "night");
        CHECK(theme_mode("night") == "dark" && theme_mode("paper") == "light" && theme_mode("system") == "system");
        auto colors = library.save_settings({{"backgroundColor", "#1A2b3c"}, {"accentColor", "red"}});
        CHECK(colors["backgroundColor"] == "#1A2b3c" && colors["accentColor"] == "");
        CHECK(library.save_settings({{"backgroundColor", ""}})["backgroundColor"] == "");
        auto look = library.save_settings({{"backgroundOpacity", 2}, {"backgroundBlur", 99}, {"backgroundImage", "../x"}});
        CHECK(look["backgroundOpacity"] == 1.0 && look["backgroundBlur"] == 40 && look["backgroundImage"] == "");
        // Ctrl+C+C popup: on by default; its size is clamped.
        CHECK(Library::default_settings()["scanPopup"] == true);
        auto popup = library.save_settings({{"scanPopup", false}, {"popupWidth", 10}, {"popupHeight", 9999}});
        CHECK(popup["scanPopup"] == false && popup["popupWidth"] == 280 && popup["popupHeight"] == 1400);
        library.save_settings({{"backgroundBlur", 12}});
        CHECK(library.settings()["backgroundBlur"] == 12); // read back from settings.json
        write_file(dicts / "wall.PNG", "PNGDATA");
        write_file(dicts / "notes.txt", "x");
        bool bad_image = false;
        try
        {
            library.set_background_image(dicts / "notes.txt");
        }
        catch (const std::exception &)
        {
            bad_image = true;
        }
        CHECK(bad_image);
        auto first = library.set_background_image(dicts / "wall.PNG")["backgroundImage"].get<std::string>();
        CHECK(first.ends_with(".png") && fs::exists(root / "data" / "backgrounds" / first));
        auto image = library.background_image(first);
        CHECK(image && image->data == "PNGDATA" && image->mime == "image/png");
        CHECK(!library.background_image("../settings.json") && !library.background_image("other.png"));
        auto second = library.set_background_image(dicts / "wall.PNG")["backgroundImage"].get<std::string>();
        CHECK(second != first && !fs::exists(root / "data" / "backgrounds" / first)); // old copy removed
        CHECK(library.clear_background_image()["backgroundImage"] == "");
        CHECK(!fs::exists(root / "data" / "backgrounds" / second) && !library.background_image(second));
        CHECK(fs::exists(dicts / "wall.PNG")); // the original is untouched
    }

    // Remembered folders: a rescan picks up new dictionaries; removed ones stay in the removed group.
    {
        auto shelf = dicts / "shelf";
        write_mdict(shelf / "one.mdx", {{"alpha", "<p>a</p>"}}, false, false);
        Library lib(root / "shelf-data");
        CHECK(lib.add({shelf}, {})["added"] == 1);
        CHECK(lib.folders().size() == 1 && lib.folders()[0]["exists"] == true);
        write_mdict(shelf / "deeper" / "two.mdx", {{"beta", "<p>b</p>"}}, false, false);
        auto rescanned = lib.rescan({});
        CHECK(rescanned["added"] == 1 && rescanned["folders"] == 1 && rescanned["errors"].empty());
        CHECK(lib.lookup("beta")["results"].size() == 1);
        CHECK(lib.rescan({})["added"] == 0); // nothing new

        std::string two_id;
        for (auto &d : lib.list())
            if (d["path"].get<std::string>().find("two.mdx") != std::string::npos)
                two_id = d["id"];
        auto removed_count = [&] {
            int n = 0;
            for (auto &d : lib.list())
                n += d["status"] == "removed";
            return n;
        };
        lib.remove(two_id);
        CHECK(lib.rescan({})["added"] == 0 && removed_count() == 1); // removed stays removed
        CHECK(lib.add({shelf}, {})["added"] == 0 && removed_count() == 1); // even when its folder is added again
        CHECK(lib.lookup("beta")["results"].empty());
        CHECK(lib.add({shelf / "deeper" / "two.mdx"}, {})["added"] == 1); // picking its file restores it
        CHECK(removed_count() == 0 && lib.lookup("beta")["results"].size() == 1);

        // Once the file of a removed dictionary is gone, its entry can be cleared.
        lib.remove(two_id);
        fs::remove_all(shelf / "deeper");
        CHECK(lib.forget(two_id).size() == 1 && removed_count() == 0);
        CHECK(lib.list().size() == 1);

        fs::create_directories(shelf / "deeper");
        lib.add({shelf / "deeper"}, {}); // inside a remembered folder: nothing new to remember
        CHECK(lib.folders().size() == 1);
        lib.add({dicts}, {}); // a parent replaces the folders inside it
        CHECK(lib.folders().size() == 1 && lib.folders()[0]["path"].get<std::string>().find("shelf") == std::string::npos);
        CHECK(lib.remove_folder(lib.folders()[0]["path"]).empty());
        CHECK(lib.rescan({})["folders"] == 0);
    }

    // Simplified/traditional lookups through OpenCC, whose data the build copies beside this binary.
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        auto opencc = fs::path(exe).parent_path() / "opencc";
        auto has = [](const std::vector<std::string> &list, const std::string &value) {
            return std::find(list.begin(), list.end(), value) != list.end();
        };
        ChineseVariants cc(opencc);
        CHECK(cc.available());
        CHECK(has(cc.expand("软件", "t"), "軟件") && !has(cc.expand("软件", "t"), "軟體"));
        CHECK(has(cc.expand("软件", "twp"), "軟體"));
        CHECK(has(cc.expand("软件", "all"), "軟件") && has(cc.expand("软件", "all"), "軟體"));
        CHECK(has(cc.expand("軟體", "all"), "软件")); // traditional input reaches simplified too
        CHECK(has(cc.expand("里面", "t"), "裏面") && has(cc.expand("里面", "tw"), "裡面"));
        CHECK(cc.expand("apple", "all").empty() && cc.expand("软件", "bogus").empty());
        CHECK(has_han("A 字") && !has_han("abc") && !has_han("かな"));
        // Article text switches script; markup, resource names, scripts and styles do not.
        auto html = cc.convert_html(
            "<p class=\"長\" title='a>長'><img src=\"長.png\">長度 a<b <!--長--><script>var s='長';</script>"
            "<STYLE>.長{}</STYLE><b>长</b></p>",
            "s", "t");
        CHECK(html == "<p class=\"長\" title='a>長'><img src=\"長.png\">长度 a<b <!--長--><script>var s='長';</script>"
                      "<STYLE>.長{}</STYLE><b>长</b></p>");
        CHECK(cc.convert_html("<i>软件</i>", "t", "twp") == "<i>軟體</i>");
        CHECK(cc.convert_html("<i>软件</i>", "t", "t") == "<i>軟件</i>");
        CHECK(cc.convert_html("<i>軟</i>", "x", "t") == "<i>軟</i>");

        // A dictionary written only in Taiwan forms.
        write_mdict(dicts / "zh" / "zh.mdx", {{"軟體", "<p>software</p>"}, {"裏面", "<p>inside</p>"}}, false, false);
        Library zh(root / "zh-data");
        zh.set_opencc_dir(opencc);
        CHECK(zh.chinese_available());
        CHECK(zh.add({dicts / "zh" / "zh.mdx"}, {})["added"] == 1);
        CHECK(zh.lookup("软件")["results"].empty()); // off by default
        zh.save_settings({{"chineseConversion", true}, {"chineseVariant", "tw"}});
        CHECK(zh.lookup("软件")["results"].empty()); // Taiwan characters alone give 軟件
        CHECK(zh.save_settings({{"chineseVariant", "bogus"}})["chineseVariant"] == "tw");
        zh.save_settings({{"chineseVariant", "twp"}});
        auto hit = zh.lookup("软件");
        CHECK(hit["results"].size() == 1 && hit["variants"] == Json::array({"軟體"}));
        if (!hit["results"].empty())
        {
            auto zh_id = hit["results"][0]["id"].get<std::string>();
            CHECK(zh.article_page(zh_id, "软件", "light").find("software") != std::string::npos);
        }
        write_mdict(dicts / "zh2" / "zh2.mdx", {{"長", "<p>長度</p>"}}, false, false);
        CHECK(zh.add({dicts / "zh2" / "zh2.mdx"}, {})["added"] == 1);
        auto long_hit = zh.lookup("長");
        CHECK(!long_hit["results"].empty());
        if (!long_hit["results"].empty())
        {
            auto id = long_hit["results"][0]["id"].get<std::string>();
            CHECK(zh.article_page(id, "長", "light", "s").find("<p>长度</p>") != std::string::npos);
            CHECK(zh.article_page(id, "長", "light").find("<p>長度</p>") != std::string::npos);
        }
        // 繁: the standard chosen in the header, else the "traditionalVariant" setting.
        write_mdict(dicts / "zh3" / "zh3.mdx", {{"软", "<p>软件</p>"}}, false, false);
        CHECK(zh.add({dicts / "zh3" / "zh3.mdx"}, {})["added"] == 1);
        CHECK(Library::default_settings()["traditionalVariant"] == "t");
        CHECK(zh.save_settings({{"traditionalVariant", "all"}})["traditionalVariant"] == "t");
        auto soft = zh.lookup("软");
        CHECK(!soft["results"].empty());
        if (!soft["results"].empty())
        {
            auto id = soft["results"][0]["id"].get<std::string>();
            CHECK(zh.article_page(id, "软", "light", "t").find("<p>軟件</p>") != std::string::npos);
            CHECK(zh.article_page(id, "软", "light", "t", "twp").find("<p>軟體</p>") != std::string::npos);
            zh.save_settings({{"traditionalVariant", "twp"}});
            CHECK(zh.article_page(id, "软", "light", "t").find("<p>軟體</p>") != std::string::npos);
            CHECK(zh.article_page(id, "软", "light", "t", "t").find("<p>軟件</p>") != std::string::npos);
        }
        // Online settings: known Wiktionary editions only, once each, in a fixed order.
        CHECK(zh.settings()["wiktionary"] == Json::array({"en"}));
        CHECK(zh.save_settings({{"wiktionary", {"zh", "xx", "en", "zh"}}})["wiktionary"] == Json::array({"en", "zh"}));
        CHECK(zh.save_settings({{"wiktionary", Json::array()}, {"translateSentences", false}})["wiktionary"].empty());
        CHECK(zh.settings()["translateSentences"] == false);
        // AI 助查 is off until turned on; tool, model and effort names are checked like DR Reader's.
        CHECK(zh.settings()["agentEnabled"] == false && zh.settings()["agentButton"] == true &&
              zh.settings()["agentPinned"] == false && zh.settings()["agent"] == "auto");
        auto agent = zh.save_settings({{"agentEnabled", true},
                                       {"agent", "claude"},
                                       {"agentModels", {{"claude", "opus[1m]"}, {"codex", "a;b"}}},
                                       {"agentEfforts", {{"claude", "max"}, {"opencode", "high max"}}}});
        CHECK(agent["agentEnabled"] == true && agent["agent"] == "claude");
        CHECK(agent["agentModels"]["claude"] == "opus[1m]" && agent["agentModels"]["codex"] == "");
        CHECK(agent["agentEfforts"]["claude"] == "max" && agent["agentEfforts"]["opencode"] == "");
        CHECK(zh.save_settings({{"agent", "gemini"}})["agent"] == "claude");
        // Hiding the button is independent of enabling the feature; pin and visibility persist.
        zh.save_settings({{"agentButton", false}, {"agentPinned", true}});
        CHECK(zh.settings()["agentEnabled"] == true && zh.settings()["agentButton"] == false &&
              zh.settings()["agentPinned"] == true);
        CHECK(zh.save_settings({{"agentButton", "true"}, {"agentPinned", 0}})["agentButton"] == false);
        zh.save_settings({{"agentEnabled", false}});
        CHECK(zh.settings()["agentEnabled"] == false && zh.settings()["agentButton"] == false &&
              zh.settings()["agentPinned"] == true);
        zh.save_settings({{"chineseVariant", "t"}});
        CHECK(zh.lookup("里面")["results"].size() == 1);
        zh.save_settings({{"chineseVariant", "all"}});
        auto suggestions = zh.suggest("软", 10);
        CHECK(std::find(suggestions.begin(), suggestions.end(), "軟體") != suggestions.end());
    }
    fs::remove_all(root);
    if (failures)
    {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "engine tests passed\n";
    return 0;
}
