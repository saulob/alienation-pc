// DEBUG_SAULO: TEMPORARY. Runs tools\debug-crash-report-fixture.ps1 in a temporary folder and asks the launcher's own log
// discovery (FindGameLogs, CheckCrashDump, BuildIncidentPackage) whether it sees a crash with an attachable dump. Delete with
// the script once the manual test is confirmed.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "incident_report.h"

namespace fs = std::filesystem;
using namespace aln;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

const fs::path kSource(ALN_SOURCE_DIR);

struct TempFolder {
    fs::path path;
    TempFolder() {
        path = fs::temp_directory_path() / ("aln-debug-saulo-fixture-" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path / "logs");
        fs::create_directories(path / "out");
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path logs() const { return path / "logs"; }
};

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void Write(const fs::path& file, const std::string& text) { std::ofstream(file, std::ios::binary) << text; }

// Everything in a folder by name, with its content.
std::map<std::string, std::string> Snapshot(const fs::path& folder) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::directory_iterator(folder)) files[entry.path().filename().string()] = ReadAll(entry.path());
    return files;
}

bool RunScript(const TempFolder& temp, const std::wstring& arguments) {
    const fs::path script = kSource / "tools" / "debug-crash-report-fixture.ps1";
    const std::wstring command = L"\"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -LogDir \"" +
                                 temp.logs().wstring() + L"\" " + arguments + L" > \"" + (temp.path / "out" / "script.txt").wstring() + L"\" 2>&1\"";
    const int code = _wsystem(command.c_str());
    if (code != 0) std::printf("script output:\n%s\n", ReadAll(temp.path / "out" / "script.txt").c_str());
    return code == 0;
}

const GameLog* Find(const std::vector<GameLog>& logs, const std::string& name) {
    for (const GameLog& log : logs) {
        if (log.name == name) return &log;
    }
    return nullptr;
}

} // namespace

int main() {
    TempFolder temp;
    const std::string session = "[run] 2026-11-05T03:51:00\nLauncher: closed, starting the game\nRuntime: guest thread 'guest' created\n";
    // Ordinary sessions that must survive: one crashed with its own dump, one plain, and a second dump of session 3.
    Write(temp.logs() / "alienation_001.log", session + "CRASH: a real one\n");
    Write(temp.logs() / "alienation_001.dmp", "MDMP-of-a-real-crash-xxxxxxxxxxxxxxxxxxxxxxxx");
    Write(temp.logs() / "alienation_002.log", session);
    Write(temp.logs() / "alienation_003_2.dmp", "MDMP-second-dump-xxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    const std::map<std::string, std::string> ordinary = Snapshot(temp.logs());

    CHECK(RunScript(temp, L""));
    const std::map<std::string, std::string> first = Snapshot(temp.logs());
    // The next unused number counts the dump of session 3 as taken; nothing that existed was touched.
    CHECK(first.count("alienation_004.log") && first.count("alienation_004.dmp") && first.count("alienation_004.debug_saulo_fixture"));
    for (const auto& [name, content] : ordinary) CHECK(first.count(name) && first.at(name) == content);
    CHECK(first.size() == ordinary.size() + 3);

    // The launcher's own discovery sees a crash with a dump it will attach by default.
    const std::vector<GameLog> logs = FindGameLogs(temp.logs(), {});
    const GameLog* fixture = Find(logs, "alienation_004.log");
    CHECK(fixture && fixture->Crashed() && fixture->CanAttachCrash() && DefaultAttachCrashReport(*fixture));
    if (!fixture) return 1;
    CHECK(fixture->crash->name == "alienation_004.dmp" && fixture->crash->path == temp.logs() / "alienation_004.dmp");
    CHECK(CheckCrashDump(*fixture, fixture->crash->path) == DumpProblem::kNone);
    CHECK(fixture->crash->bytes >= kFewestDumpBytes && fixture->crash->bytes <= kMostDumpBytes);
    CHECK(first.at("alienation_004.log").find("DEBUG_SAULO crash report fixture for manual testing") != std::string::npos);
    const GameLog* real = Find(logs, "alienation_001.log");
    CHECK(real && real->Crashed() && real->crash_problem == DumpProblem::kNone);
    const GameLog* plain = Find(logs, "alienation_002.log");
    CHECK(plain && !plain->Crashed());
    // A report built from it carries that dump.
    const ReportFacts facts{"0.2.0", "CUSA00062 1.06", "1.06", "CUSA00062", "Windows"};
    const auto package = BuildIncidentPackage("The fixture crash", *fixture, facts, {}, true);
    CHECK(package && package->crash_dump && package->crash_dump->name == "alienation_004.dmp");
    CHECK(package && package->crash_dump && package->crash_dump->bytes == fixture->crash->bytes);

    // A second run takes the next number and leaves the first fixture alone.
    CHECK(RunScript(temp, L""));
    const std::map<std::string, std::string> second = Snapshot(temp.logs());
    CHECK(second.count("alienation_005.log") && second.count("alienation_005.dmp"));
    for (const auto& [name, content] : first) CHECK(second.count(name) && second.at(name) == content);

    // A fixture file that was changed since it was made is not deleted; the rest is.
    Write(temp.logs() / "alienation_005.log", second.at("alienation_005.log") + "edited\n");
    CHECK(RunScript(temp, L"-Remove"));
    const std::map<std::string, std::string> after = Snapshot(temp.logs());
    CHECK(!after.count("alienation_004.log") && !after.count("alienation_004.dmp") && !after.count("alienation_004.debug_saulo_fixture"));
    CHECK(after.count("alienation_005.log") && !after.count("alienation_005.dmp") && after.count("alienation_005.debug_saulo_fixture"));
    for (const auto& [name, content] : ordinary) CHECK(after.count(name) && after.at(name) == content);

    // Put the edited fixture log back as made: now everything it created goes, and only that.
    Write(temp.logs() / "alienation_005.log", second.at("alienation_005.log"));
    CHECK(RunScript(temp, L"-Remove"));
    CHECK(Snapshot(temp.logs()) == ordinary);
    CHECK(RunScript(temp, L"-Remove"));
    CHECK(Snapshot(temp.logs()) == ordinary);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
