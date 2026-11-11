// Focused tests of the Bug Report page's logic: which logs it offers, what the privacy filter leaves, and that the preview
// and the request are the same report. Synthetic logs in a temporary folder; no window, no network.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "game_profile.h"
#include "incident_report.h"
#include "localization/ui_text.h"
#include "log_scrubber.h"
#include "net/lobby_link.h"
#include "report_history.h"

namespace fs = std::filesystem;
using namespace aln;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

const std::string kGameSession = "[run] 2026-11-05T03:51:00\nLauncher: closed, starting the game\nALN: game build CUSA04430 1.00\n";
const std::string kGameplaySession = kGameSession + "Runtime: guest thread 'guest' created (stack=1048576, prio=700)\n";
const std::string kLauncherSession = "[run] 2026-11-05T03:51:00\nLauncher: shown\nLauncher: closed\n";
const std::string kLauncherCrash = "[run] 2026-11-05T03:51:00\nLauncher: shown\nLauncher: window 1689x840\n";

struct TempFolder {
    fs::path path;
    TempFolder() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("aln-incident-tests-" + std::to_string(tick));
        fs::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string LogName(int number) {
    char name[64];
    std::snprintf(name, sizeof(name), "alienation_%03d.log", number);
    return name;
}

fs::path WriteLog(const fs::path& folder, const std::string& name, const std::string& text, int minutes_ago) {
    const fs::path file = folder / name;
    std::ofstream(file, std::ios::binary) << text;
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::minutes(minutes_ago));
    return file;
}

std::vector<std::string> Names(const std::vector<GameLog>& logs) {
    std::vector<std::string> names;
    for (const GameLog& log : logs) names.push_back(log.name);
    return names;
}

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

const fs::path kSource(ALN_SOURCE_DIR);

PrivateFacts Facts() {
    PrivateFacts facts;
    facts.user_names = {"Zefa Tester", "zefa"};
    facts.machine_names = {"ZEFA-DESKTOP"};
    facts.profile_folders = {"C:\\Users\\Zefa Tester"};
    facts.install_folders = {"D:\\Jogos\\Zefa\\ALN"};
    facts.secrets = {std::string(64, 'a'), "7654321098765432"};
    return facts;
}

std::string Scrub(std::string_view text) { return ScrubText(text, Facts()); }

bool Has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

ReportFacts SampleFacts() { return {"0.9.3", "CUSA04430 1.00", "1.00", "CUSA04430", "Windows 11 (build 26200)"}; }

void TestNewestFiveFirst() {
    TempFolder temp;
    // Modification time decides, not the number in the name.
    const int ages[] = {50, 10, 40, 20, 60, 30, 70};
    for (int i = 0; i < 7; ++i) WriteLog(temp.path, LogName(i + 1), kGameSession, ages[i]);
    WriteLog(temp.path, "last_run.log", kGameSession, 1);
    WriteLog(temp.path, "alienation_abc.log", kGameSession, 1);
    WriteLog(temp.path, "alienation_012.log.bak", kGameSession, 1);
    WriteLog(temp.path, "alienation_7.log", kGameSession, 1);
    WriteLog(temp.path, "notes.txt", kGameSession, 1);
    fs::create_directories(temp.path / "alienation_099.log");
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    const std::vector<std::string> expected = {LogName(2), LogName(4), LogName(6), LogName(3), LogName(1)};
    CHECK(logs.size() == kMostGameLogs);
    CHECK(Names(logs) == expected);
    CHECK(logs[0].bytes == kGameSession.size() && logs[0].written > logs[1].written);
}

// tools\build.ps1 -Run puts its build section before the session in the same log; a long build must not hide the
// session, and a build that never ran the game is not a game log.
void TestBuildSectionBeforeTheSession() {
    TempFolder temp;
    std::string build = "[build]\nProject: alienation\nMode: incremental\n";
    while (build.size() < 300 * 1024) build += "[12/345] Building CXX object CMakeFiles/alienation.dir/src/loader.c.obj\n";
    build += "Result: SUCCESS\nExit code: 0\n";
    WriteLog(temp.path, LogName(1), build + kGameplaySession, 20);
    WriteLog(temp.path, LogName(2), "[build]\nProject: alienation\nResult: FAILED\nExit code: 1\n", 10);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    CHECK(Names(logs) == std::vector<std::string>{LogName(1)});
    CHECK(!logs.empty() && logs[0].activity == LogActivity::kGameplay);
}

void TestFewerThanFive() {
    TempFolder temp;
    CHECK(FindGameLogs(temp.path, {}).empty());
    CHECK(FindGameLogs(temp.path / "missing", {}).empty());
    WriteLog(temp.path, LogName(1), kGameSession, 30);
    WriteLog(temp.path, LogName(2), kGameSession, 20);
    const std::vector<std::string> expected = {LogName(2), LogName(1)};
    CHECK(Names(FindGameLogs(temp.path, {})) == expected);
}

void TestSessionsThatAreNotGameLogs() {
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameSession, 40);
    WriteLog(temp.path, LogName(2), kLauncherSession, 30);
    WriteLog(temp.path, LogName(3), kLauncherCrash, 20);
    WriteLog(temp.path, LogName(4), "", 15);
    const fs::path running = WriteLog(temp.path, LogName(5), kLauncherCrash, 0);
    const std::vector<std::string> expected = {LogName(5), LogName(3), LogName(1)};
    CHECK(Names(FindGameLogs(temp.path, running)) == expected);

    // Opening the launcher repeatedly must not push the game's logs off the list.
    TempFolder many;
    WriteLog(many.path, LogName(1), kGameSession, 100);
    for (int i = 2; i < 12; ++i) WriteLog(many.path, LogName(i), kLauncherSession, 100 - i);
    const std::vector<std::string> only_game = {LogName(1)};
    CHECK(Names(FindGameLogs(many.path, {})) == only_game);

    // The marks are what the launcher and the loader write.
    CHECK(Has(ReadAll(kSource / "src" / "loader.c"), "ALN: game build %s"));
    const std::string launcher = ReadAll(kSource / "launcher" / "launcher.cpp");
    CHECK(Has(launcher, "\"closed, starting the game\" : \"closed\""));
    CHECK(Has(launcher, "std::printf(\"Launcher: \");"));
    CHECK(Has(ReadAll(kSource / "src" / "runtime_thread.c"), "\"Runtime: guest thread '%s' created"));
}

std::vector<LogRole> Roles(const std::vector<GameLog>& logs) {
    std::vector<LogRole> roles;
    for (const GameLog& log : logs) roles.push_back(log.role);
    return roles;
}

void TestCurrentSessionAndLastGameplay() {
    using enum LogRole;
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameplaySession, 60);
    WriteLog(temp.path, LogName(2), kGameplaySession, 50);
    WriteLog(temp.path, LogName(3), kLauncherSession, 40);
    WriteLog(temp.path, LogName(4), kLauncherSession, 30);
    // First whatever its time says: the file is still open, so its time can lag.
    const fs::path running = WriteLog(temp.path, LogName(5), kLauncherCrash, 120);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, running);
    const std::vector<std::string> names = {LogName(5), LogName(2), LogName(1)};
    CHECK(Names(logs) == names);
    CHECK(Roles(logs) == (std::vector<LogRole>{kCurrentLauncher, kLastGameplay, kOtherGameplay}));
    CHECK(DefaultGameLog(logs) == 1);
    CHECK(logs[0].activity == LogActivity::kLauncher && logs[1].activity == LogActivity::kGameplay);
}

void TestLauncherSessionsAreNotGameplay() {
    using enum LogRole;
    // Sessions that never left the launcher earn no label, closed or crashed, however many there are.
    TempFolder closed;
    for (int i = 1; i < 8; ++i) WriteLog(closed.path, LogName(i), kLauncherSession, 100 - i);
    const fs::path running = WriteLog(closed.path, LogName(8), kLauncherCrash, 0);
    const std::vector<GameLog> alone = FindGameLogs(closed.path, running);
    CHECK(Roles(alone) == (std::vector<LogRole>{kCurrentLauncher}));
    CHECK(DefaultGameLog(alone) == 0);

    TempFolder crashed;
    WriteLog(crashed.path, LogName(1), kGameplaySession, 60);
    WriteLog(crashed.path, LogName(2), kLauncherCrash, 30);
    const fs::path open = WriteLog(crashed.path, LogName(3), kLauncherCrash, 0);
    const std::vector<GameLog> logs = FindGameLogs(crashed.path, open);
    CHECK(Roles(logs) == (std::vector<LogRole>{kCurrentLauncher, kOtherLauncher, kLastGameplay}));
    CHECK(DefaultGameLog(logs) == 2);

    // A crashed launcher is still worth reporting when no game has run.
    TempFolder only_crash;
    WriteLog(only_crash.path, LogName(1), kLauncherCrash, 30);
    const fs::path mine = WriteLog(only_crash.path, LogName(2), kLauncherCrash, 0);
    CHECK(DefaultGameLog(FindGameLogs(only_crash.path, mine)) == 1);
}

void TestDefaultLog() {
    using enum LogRole;
    // A game that failed before its own code ran did not play, but it is the session a report is about.
    TempFolder failed;
    WriteLog(failed.path, LogName(1), kGameplaySession, 60);
    WriteLog(failed.path, LogName(2), kGameSession, 30);
    const fs::path running = WriteLog(failed.path, LogName(3), kLauncherCrash, 0);
    const std::vector<GameLog> logs = FindGameLogs(failed.path, running);
    CHECK(Roles(logs) == (std::vector<LogRole>{kCurrentLauncher, kNone, kLastGameplay}));
    CHECK(DefaultGameLog(logs) == 1);

    // The running log with gameplay is the current session, and nothing else claims to be the last one played.
    TempFolder current;
    WriteLog(current.path, LogName(1), kGameplaySession, 60);
    const fs::path played = WriteLog(current.path, LogName(2), kGameplaySession, 0);
    const std::vector<GameLog> both = FindGameLogs(current.path, played);
    CHECK(Roles(both) == (std::vector<LogRole>{kCurrentGameplay, kOtherGameplay}));
    CHECK(DefaultGameLog(both) == 0);

    // Without the running log the newest game session is simply the last one played.
    TempFolder finished;
    WriteLog(finished.path, LogName(1), kGameplaySession, 60);
    WriteLog(finished.path, LogName(2), kGameplaySession, 30);
    const std::vector<GameLog> closed = FindGameLogs(finished.path, {});
    CHECK(Roles(closed) == (std::vector<LogRole>{kLastGameplay, kOtherGameplay}));
    CHECK(DefaultGameLog(closed) == 0);

    // The running log may still be empty; it is offered, and a list of nothing has no selection to break.
    TempFolder empty;
    const fs::path fresh = WriteLog(empty.path, LogName(1), "", 0);
    CHECK(Roles(FindGameLogs(empty.path, fresh)) == (std::vector<LogRole>{kCurrentLauncher}));
    CHECK(DefaultGameLog({}) == 0);

    CHECK(LogRoleText(kNone) == nullptr);
    CHECK(std::string(LogRoleText(kCurrentLauncher)) == "Current launcher session");
    CHECK(std::string(LogRoleText(kCurrentGameplay)) == "Current session \xC2\xB7 Launcher + gameplay");
    CHECK(std::string(LogRoleText(kLastGameplay)) == "Last gameplay");
}

void TestOneChosenLogIsSent() {
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameSession + "Runtime: first\n", 30);
    WriteLog(temp.path, LogName(2), kGameSession + "Runtime: second\n", 20);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    CHECK(logs.size() == 2);
    const auto package = BuildIncidentPackage("It froze", logs[1], SampleFacts(), {});
    CHECK(package.has_value());
    if (!package) return;
    CHECK(package->Value("log_file") == LogName(1));
    CHECK(Has(package->Value("log_text"), "Runtime: first") && !Has(package->Value("log_text"), "Runtime: second"));
    int log_fields = 0;
    for (const IncidentField& field : package->fields) log_fields += field.key == "log_text";
    CHECK(log_fields == 1);
}

void TestAddresses() {
    CHECK(Scrub("Connection test: lobby answered; it sees 177.18.65.14") == "Connection test: lobby answered; it sees <ipv4>");
    CHECK(Scrub("member 0x20 is at 203.0.113.7:3658 now") == "member 0x20 is at <ipv4>:3658 now");
    CHECK(Scrub("UPnP router http://192.168.0.1:5000/x") == "UPnP router http://<ipv4>:5000/x");
    CHECK(Scrub("addr:10.1.2.3, peer=172.16.0.9.") == "addr:<ipv4>, peer=<ipv4>.");
    CHECK(Scrub("bind 0.0.0.0:3658 and 127.0.0.1") == "bind 0.0.0.0:3658 and 127.0.0.1");

    CHECK(Scrub("it sees 2001:db8:85a3::8a2e:370:7334") == "it sees <ipv6>");
    CHECK(Scrub("peer [2001:db8::1]:3658") == "peer [<ipv6>]:3658");
    CHECK(Scrub("lobby has 2804:14c:65:8ba2:1111:2222:3333:1122:3658)") == "lobby has <ipv6>:3658)");
    CHECK(Scrub("link fe80::1c2d:3eff:fe4a:5b6c%eth0 up") == "link <ipv6> up");
    CHECK(Scrub("full 2001:0db8:0000:0000:0000:ff00:0042:8329.") == "full <ipv6>.");
    CHECK(Scrub("mapped ::ffff:203.0.113.7 here") == "mapped <ipv6> here");
    CHECK(Scrub("loopback ::1 and any ::") == "loopback ::1 and any ::");
}

void TestDiagnosticsStay() {
    for (const char* line : {
             "Launcher: eboot.bin SHA-256 f9bff150c4c28507096e7e143a07492063ed89bf05ac6cb995a55a0946e3508a",
             "GPU: Vulkan device NVIDIA GeForce RTX 4070 (NVIDIA 621.1.0.0, Vulkan 1.4.357)",
             "GPU: Vulkan device NVIDIA GeForce (NVIDIA 96.1.0.0, Vulkan 1.1.0.0)",
             "Runtime: guest thread 'guest' created (stack=1048576, prio=700)",
             "[run] 2026-11-03T01:42:52",
             "Window: display 1 of 1 (Generic PnP Monitor), requested 1436x808",
             "GPU: experimental AMD water workaround off (vendor 0x10de, feedback loop layout)",
             "Trophies: NPWR03852_00 loaded, 49 trophies, 48 count toward Platinum",
             "Runtime: std::vector::at 00007FF6:0012:AB34 at 12:34:56",
             "Lobby: connected to wss://lobby.example.org",
             "Vulkan layers: path C:\\Program Files\\NVIDIA Corporation\\layer.json",
             "Vulkan layers: path C:\\ProgramData\\obs-studio-hook\\graphics-hook64.json",
             "Vulkan layers: path C:\\WINDOWS\\System32\\DriverStore\\nvoglv64.dll",
             "Matching2: join room 4096 as member 0x20 (error code 123456)",
         }) {
        CHECK(Scrub(line) == line);
    }
}

void TestLocalPaths() {
    CHECK(Scrub("Settings: C:\\Users\\Zefa Tester\\AppData\\alienation.ini") == "Settings: <home>\\AppData\\alienation.ini");
    CHECK(Scrub("Settings: c:/users/zefa tester/AppData/alienation.ini") == "Settings: <home>/AppData/alienation.ini");
    CHECK(Scrub("{\"path\":\"C:\\\\Users\\\\Zefa Tester\\\\x.ini\"}") == "{\"path\":\"<home>\\\\x.ini\"}");
    CHECK(Scrub("{\"path\":\"D:\\\\Jogos\\\\Zefa\\\\ALN\\\\x.ini\"}") == "{\"path\":\"<install>\\\\x.ini\"}");
    CHECK(Scrub("{\"path\":\"\\\\\\\\FILESERVER\\\\ana\\\\a.log\"}") == "{\"path\":\"<network-path>\\\\a.log\"}");
    CHECK(Scrub("x>\\\\FILESERVER\\ana\\a.log") == "x><network-path>\\a.log");
    CHECK(Scrub("other C:\\Users\\Someone Else\\Documents\\a.txt") == "other <home>\\Documents\\a.txt");
    CHECK(Scrub("[run] \"D:\\Jogos\\Zefa\\ALN\\alienation.exe\"") == "[run] \"<install>\\alienation.exe\"");
    CHECK(Scrub("Launcher: game files in D:\\Jogos\\Zefa\\ALN\\game\\prepared\\v1.03: valid") ==
          "Launcher: game files in <install>\\game\\prepared\\v1.03: valid");
    CHECK(Scrub("package E:\\torrent_complete\\Zefas Stuff\\game.pkg") == "package <folder>\\game.pkg");
    CHECK(Scrub("Linux /home/ana/.local/share/aln/x.log") == "Linux <home>/.local/share/aln/x.log");
    CHECK(Scrub("Linux /run/media/ana/Games/aln") == "Linux <home>/Games/aln");
    CHECK(Scrub("share \\\\FILESERVER\\ana\\logs\\a.log") == "share <network-path>\\a.log");
    CHECK(Scrub("share \\\\NAS\\Games\\Jane Roe\\x.pkg: refused") == "share <network-path>\\x.pkg: refused");

    // Folders are named after people outside the profile too, with any punctuation.
    CHECK(Scrub("package /mnt/nas/Jane Roe/x.pkg") == "package <folder>/x.pkg");
    CHECK(Scrub("package \"/srv/Jane Roe/x.pkg\"") == "package \"<folder>/x.pkg\"");
    CHECK(Scrub("package D:\\Jane Roe (Backup)\\x.pkg") == "package <folder>\\x.pkg");
    CHECK(Scrub("package D:\\Roe, Jane\\x.pkg: refused") == "package <folder>\\x.pkg: refused");
    CHECK(Scrub("loaded C:\\Windows\\a.dll, D:\\Jane Roe\\x.pkg") == "loaded C:\\Windows\\a.dll, <folder>\\x.pkg");
    CHECK(Scrub("loaded /usr/lib/libvulkan.so.1 (found)") == "loaded /usr/lib/libvulkan.so.1 (found)");
    CHECK(Scrub("guest open /app0/data/stream.dat") == "guest open /app0/data/stream.dat");
    CHECK(Scrub("Render 1920/1080 and/or 21:9") == "Render 1920/1080 and/or 21:9");

    // An encoded name that is not UTF-8 goes with its path, and the rest of the text stays valid.
    CHECK(Scrub("Settings: C:\\Users\\Jo\xE3o\\alienation.ini") == "Settings: <home>\\alienation.ini");
    CHECK(Scrub("bad byte \xE3 here") == "bad byte ? here");
}

void TestNames() {
    CHECK(Scrub("Signed in on ZEFA-DESKTOP as zefa") == "Signed in on <computer> as <user>");
    CHECK(Scrub("zefa-desktop answered") == "<computer> answered");
    CHECK(Scrub("zefaland stays, azefa stays") == "zefaland stays, azefa stays");
    PrivateFacts generic;
    generic.user_names = {"deck"};
    generic.machine_names = {"steamdeck"};
    CHECK(ScrubText("Settings: Windows, Steam Deck deck", generic) == "Settings: Windows, Steam Deck deck");
    PrivateFacts collision;
    collision.user_names = {"home"};
    CHECK(ScrubText("/home/ana/x", collision) == "<home>/x");
}

void TestSecrets() {
    const std::string secret(64, 'a');
    CHECK(Scrub("online secret " + secret + " here") == "online secret <secret> here");
    CHECK(Scrub("Online: on as Ana (account 7654321098765432), build") == "Online: on as Ana (account <secret>), build");
    CHECK(Scrub("secret=abc123") == "secret=<secret>");
    CHECK(Scrub("password: hunter2 next") == "password: <secret> next");
    CHECK(Scrub("{\"token\":\"tok-1\",\"x\":1}") == "{\"token\":\"<secret>\",\"x\":1}");
    CHECK(Scrub("url?api_key=k1&page=2") == "url?api_key=<secret>&page=2");
    CHECK(Scrub("Authorization: Bearer abc def") == "Authorization: <secret>");
    CHECK(Scrub("header Bearer abc.def") == "header Bearer <secret>");
    CHECK(Scrub("Friend code: ABCD-EF23") == "Friend code: <secret>");
    CHECK(Scrub("friend_code=ABCD-EF23") == "friend_code=<secret>");
    CHECK(Scrub("jwt eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.abcdefgh end") == "jwt <secret> end");
}

void TestOtherPlayers() {
    CHECK(Scrub("Chat: Bob: meet me at 203.0.113.7") == "Chat: <message removed>");
    CHECK(Scrub("P2P: peer 1234567890123 at 203.0.113.7:3658") == "P2P: peer <player> at <ipv4>:3658");
    CHECK(Scrub("P2P: link 2 to peer 1234567890123 connected") == "P2P: link 2 to peer <player> connected");
    CHECK(Scrub("Rudp: context 1 -> peer 987654321, link 2") == "Rudp: context 1 -> peer <player>, link 2");
    CHECK(Scrub("Online: player account 5550001112 joined") == "Online: player account <player> joined");
    CHECK(Scrub("Matching2: search for the room with code ABC234") == "Matching2: search for the room with code <room>");
    CHECK(Scrub("Matching2: created room 4096 as member 0x10 (owner 0x10, 1 member(s), code ZX9K2M)") ==
          "Matching2: created room 4096 as member 0x10 (owner 0x10, 1 member(s), code <room>)");
    CHECK(Scrub("Matching2: peer 12 left") == "Matching2: peer 12 left");
}

void TestTextIsClean() {
    CHECK(Scrub("one\r\ntwo\r\n") == "one\ntwo\n");
    CHECK(Scrub(std::string_view("a\0b\x01" "c\td\x7F", 8)) == "abc\td");
    CHECK(Scrub("caf\xC3\xA9 \xF0\x9F\x90\x89") == "caf\xC3\xA9 \xF0\x9F\x90\x89");
    CHECK(Scrub("c1 \xC2\x85 gone") == "c1  gone");
    CHECK(Scrub("").empty());
}

void TestSizeLimit() {
    TempFolder temp;
    std::string big = kGameSession;
    for (int i = 0; big.size() < 512 * 1024; ++i) big += "Runtime: line " + std::to_string(i) + " at 0x10000\n";
    big += "Error: the last line\n";
    WriteLog(temp.path, LogName(1), big, 5);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    CHECK(logs.size() == 1);
    const auto package = BuildIncidentPackage("It froze", logs[0], SampleFacts(), {});
    CHECK(package.has_value());
    if (!package) return;
    const std::string& text = package->Value("log_text");
    CHECK(text.size() <= kMostLogBytes && text.size() > kMostLogBytes - 256);
    CHECK(text.starts_with("Runtime: line ") && text.ends_with("Error: the last line\n"));
    CHECK(package->Value("log_clipped") == "yes");

    WriteLog(temp.path, LogName(2), kGameSession, 1);
    const auto small = BuildIncidentPackage("It froze", FindGameLogs(temp.path, {})[0], SampleFacts(), {});
    CHECK(small && small->Value("log_clipped") == "no" && small->Value("log_text") == kGameSession);

    CHECK(LastBytes("abc", 10) == "abc");
    CHECK(LastBytes("one\ntwo\nthree\n", 9) == "three\n");

    // The whole limit of line breaks, the worst case for JSON escaping, fits the lobby's frame.
    IncidentPackage worst = *small;
    for (IncidentField& field : worst.fields) {
        if (field.key == "log_text") field.value.assign(kMostLogBytes, '\n');
        if (field.key == "description") field.value.assign(kMostDescriptionCharacters, '"');
    }
    CHECK(IncidentRequestBody(worst).dump().size() < 160 * 1024 - 64);
}

size_t CountOf(const std::string& text, std::string_view part) {
    size_t count = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++count;
    return count;
}

void TestPreviewIsTheRequest() {
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameSession + "Lobby sees 203.0.113.7\n", 5);
    const auto logs = FindGameLogs(temp.path, {});
    const auto package = BuildIncidentPackage("  It froze at 203.0.113.9\nTwice.  ", logs[0], SampleFacts(), Facts());
    CHECK(package.has_value());
    if (!package) return;
    // Sending without a preview builds the same report the preview would have shown.
    const auto direct = BuildIncidentPackage("  It froze at 203.0.113.9\nTwice.  ", logs[0], SampleFacts(), Facts());
    CHECK(direct && IncidentRequestBody(*direct).dump() == IncidentRequestBody(*package).dump());
    const net::Json body = IncidentRequestBody(*package);
    const std::string preview = IncidentPreviewText(*package);
    CHECK(body.members().size() == package->fields.size());
    for (const IncidentField& field : package->fields) {
        CHECK(body[field.key].as_string() == field.value);
        CHECK(field.value.empty() || Has(preview, field.value));
        CHECK(Has(preview, field.label));
    }
    CHECK(package->Value("description") == "It froze at <ipv4>\nTwice.");
    CHECK(Has(package->Value("log_text"), "Lobby sees <ipv4>"));
    CHECK(!Has(preview, "203.0.113"));
    CHECK(!Has(body.dump(), "203.0.113"));
    const char* const wanted[] = {"description", "launcher_version", "game_build", "game_version", "cusa", "operating_system", "log_file", "log_clipped", "log_text"};
    CHECK(body.members().size() == std::size(wanted));
    for (const char* key : wanted) CHECK(body.has(key));

    // One way to build a report: an open preview is sent as shown, and otherwise Send builds it the same way.
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    const size_t build = page.find("BuildIncidentPackage(");
    CHECK(build != std::string::npos && page.find("BuildIncidentPackage(", build + 1) == std::string::npos);
    CHECK(Has(page, "previewed_ = BuildPackage(facts);"));
    CHECK(Has(page, "preview_text_ = IncidentPreviewText(*previewed_);"));
    CHECK(Has(page, "package = previewed_ ? previewed_ : BuildPackage(facts);"));
    CHECK(Has(page, "FileIncident(config, user_dir, net::kOnlineGame, *package)"));
    CHECK(page.find("IncidentRequestBody") == std::string::npos);
    // Only a running delivery holds Send back; a bad draft is answered when it is pressed.
    CHECK(CountOf(page, "ImGui::BeginDisabled(sending);") == 4 && !Has(page, "BeginDisabled(!previewed_"));
    CHECK(!Has(page, "problem != DraftProblem::kNone ||"));
    CHECK(CountOf(page, "if (!AcceptDraft()) return;") == 2);
}

void TestPreviewButton() {
    CHECK(std::string(PreviewButtonText(false)) == "Open preview");
    CHECK(std::string(PreviewButtonText(true)) == "Close preview");
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    CHECK(Has(page, "PreviewButtonText(previewed_ != nullptr)"));
    CHECK(Has(page, "if (previewed_) {\n        ForgetPreview();\n        return;\n    }"));
}

void TestPageStackPairs() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    for (const auto& [open, close] : {std::pair{"ImGui::PushID(", "ImGui::PopID()"}, std::pair{"ImGui::PushFont(", "ImGui::PopFont()"},
                                      std::pair{"ImGui::PushStyleColor(", "ImGui::PopStyleColor()"},
                                      std::pair{"ImGui::BeginDisabled(", "ImGui::EndDisabled()"}}) {
        CHECK(CountOf(page, open) == CountOf(page, close));
    }
    // A table opens inside the first ui.Row and closes in ui.EndRows, and it must find its own ID on top of the stack then, so a
    // row's control pushes its ID only after ui.Row has run.
    const size_t logs = page.find("void IncidentPage::DrawLogs(");
    const size_t loop = page.find("for (size_t i = 0; i < logs_.size(); ++i) {", logs);
    const size_t row = page.find("ui.RowLeft(", loop), control = page.find("ui.RowControl();", loop);
    const size_t push = page.find("ImGui::PushID(", loop), pop = page.find("ImGui::PopID();", loop);
    const size_t end_rows = page.find("ui.EndRows();", loop);
    CHECK(logs != std::string::npos && loop != std::string::npos);
    CHECK(row < control && control < push && push < pop && pop < end_rows);
    CHECK(page.find("return;", loop) == std::string::npos || page.find("return;", loop) > end_rows);
}

void TestDraftChecks() {
    CHECK(kFewestDescriptionCharacters == 10);
    CHECK(CheckDraft("", true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft("  \n\t ", true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft(std::string(40, ' '), true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft("123456789", true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft(" \n 123456789 \t\n ", true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft("1234567890", true) == DraftProblem::kNone);
    CHECK(CheckDraft("  1234567890  ", true) == DraftProblem::kNone);
    CHECK(CheckDraft("It froze, twice", false) == DraftProblem::kNoLog);
    CHECK(CheckDraft("123456789", false) == DraftProblem::kDescriptionTooShort);
    std::string accents;
    for (int i = 0; i < 9; ++i) accents += "\xC3\xA9";
    CHECK(CheckDraft(accents, true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft(accents + "\xC3\xA9", true) == DraftProblem::kNone);
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    CHECK(kFewestDescriptionCharacters == 10);
    CHECK(Has(page, "\"Please describe the problem, minimum 10 characters\""));
    CHECK(!Has(page, "Describe the problem") && !Has(page, "Preview the report"));
    CHECK(Has(page, "\"1. Report the problem\""));
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameSession, 5);
    const GameLog log = FindGameLogs(temp.path, {})[0];
    CHECK(!BuildIncidentPackage(" \n", log, SampleFacts(), {}).has_value());
    GameLog gone = log;
    gone.path = temp.path / "removed.log";
    CHECK(!BuildIncidentPackage("It froze", gone, SampleFacts(), {}).has_value());
}

void TestDescriptionLimit() {
    CHECK(BytesOfCharacters("abc", 2) == 2 && BytesOfCharacters("abc", 5) == 3 && BytesOfCharacters("", 5) == 0);
    CHECK(BytesOfCharacters("\xC3\xA9\xC3\xA9", 1) == 2);
    CHECK(BytesOfCharacters("\xF0\x9F\x90\x89x", 1) == 4);
    CHECK(CountCharacters("\xC3\xA9\xF0\x9F\x90\x89x") == 3);
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameSession, 5);
    const GameLog log = FindGameLogs(temp.path, {})[0];
    std::string text;
    for (size_t i = 0; i < kMostDescriptionCharacters + 20; ++i) text += "\xC3\xA9";
    const auto package = BuildIncidentPackage(text, log, SampleFacts(), {});
    CHECK(package && CountCharacters(package->Value("description")) == kMostDescriptionCharacters);
}

void TestLogListText() {
    std::tm date{};
    date.tm_year = 2026 - 1900;
    date.tm_mon = 10;
    date.tm_mday = 5;
    date.tm_hour = 3;
    date.tm_min = 51;
    date.tm_isdst = -1;
    const std::time_t written = std::mktime(&date);
    CHECK(LogDateText(written) == "Nov 5, 2026 03:51");
    CHECK(LogSizeText(184 * 1024) == "184 KB");
    CHECK(LogSizeText(10) == "1 KB");
    CHECK(LogSizeText(1536 * 1024) == "1.5 MB");
}

void TestOutcomes() {
    CHECK(IncidentOutcomeOf("ok") == IncidentOutcome::kSent);
    CHECK(IncidentOutcomeOf("incident_hourly_limit") == IncidentOutcome::kRecentLimit);
    CHECK(IncidentOutcomeOf("incident_daily_limit") == IncidentOutcome::kDailyLimit);
    CHECK(IncidentOutcomeOf("limit") == IncidentOutcome::kRecentLimit);
    CHECK(std::string(IncidentLimitTitle(IncidentOutcome::kRecentLimit)) == "Too many reports were sent recently");
    CHECK(std::string(IncidentLimitTitle(IncidentOutcome::kDailyLimit)) == "Daily report limit reached");
    CHECK(IncidentLimitTitle(IncidentOutcome::kSent) == nullptr);
    CHECK(IncidentOutcomeOf("bad_request") == IncidentOutcome::kRefused);
    CHECK(IncidentOutcomeOf("too_large") == IncidentOutcome::kRefused);
    CHECK(IncidentOutcomeOf(net::kUnreachable) == IncidentOutcome::kUnreachable);
    CHECK(IncidentOutcomeOf(net::kLinkLost) == IncidentOutcome::kUnreachable);
    CHECK(IncidentOutcomeOf("timeout") == IncidentOutcome::kUnreachable);
    CHECK(IncidentOutcomeOf("internal") == IncidentOutcome::kServiceError);
    CHECK(IncidentOutcomeOf("unauthorized") == IncidentOutcome::kServiceError);
}

void TestLimitsKeepTheDraft() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    const size_t collect = page.find("void IncidentPage::CollectDelivery()");
    const size_t forget = page.find("void IncidentPage::ForgetPreview()");
    CHECK(collect != std::string::npos && collect < forget);
    const std::string collected = page.substr(collect, forget - collect);
    const size_t sent = collected.find("if (result_->outcome == IncidentOutcome::kSent) {");
    const size_t cleared = collected.find("description_[0] = ");
    CHECK(sent != std::string::npos && sent < cleared && cleared != std::string::npos);
    CHECK(CountOf(page, "description_[0] = ") == 1);
    CHECK(!Has(collected, "selected_") && !Has(collected, "FindLogs"));
    CHECK(Has(page, "IncidentLimitTitle(result_->outcome), \"Please try again later\""));
}

void TestResultComesIntoView() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    CHECK(CountOf(page, "bring_result_into_view_ = true;") == 2);
    const size_t collect = page.find("void IncidentPage::CollectDelivery()");
    const size_t send = page.find("void IncidentPage::Send(");
    const size_t draw = page.find("void IncidentPage::DrawSend(");
    CHECK(page.find("bring_result_into_view_ = true;", collect) < page.find("void IncidentPage::ForgetPreview()"));
    CHECK(page.find("bring_result_into_view_ = true;", send) < draw);
    const size_t result = page.find("DrawResult(ui);", draw);
    const size_t scroll = page.find("if (std::exchange(bring_result_into_view_, false)) ImGui::SetScrollHereY(1.0f);", draw);
    CHECK(result != std::string::npos && scroll != std::string::npos && result < scroll);
}

void TestNoQuotaOnThePage() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    for (const char* text : {"daily", "online account", "your address", "Online Play", "remaining", "per day", "per hour"}) {
        CHECK(!Has(page, text));
    }
}

const std::string kReceipt(64, 'a');

IncidentPackage SamplePackage() {
    IncidentPackage package;
    package.fields = {{"description", "Description", "It froze"}, {"launcher_version", "Launcher version", "0.9.3"},
                      {"game_build", "Game build", "CUSA04430 1.00"}, {"game_version", "Game version", "1.00"},
                      {"cusa", "CUSA", "CUSA04430"}, {"operating_system", "Operating system", "Windows 11 (build 26200)"},
                      {"log_file", "Selected log", "alienation_186.log"}, {"log_clipped", "Log shortened to its end", "no"},
                      {"log_text", "Log after privacy filtering", "Launcher: shown\n"}};
    return package;
}

TrackedReport Tracked(const std::string& ticket, int64_t submitted_at) {
    return TrackedReportOf(SamplePackage(), {IncidentOutcome::kSent, ticket, kReceipt, submitted_at});
}

ReportStatus StatusOf(const std::string& ticket, const std::string& status, int64_t updated_at, const std::string& version = {},
                      const std::string& message = {}) {
    return {ticket, status, updated_at, version, message};
}

void TestSentReportIsRemembered() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    {
        ReportHistory history(file);
        history.Load();
        CHECK(history.reports().empty());
        CHECK(history.Add(Tracked("ALN-AAAAAA", 1000)));
        CHECK(history.Add(Tracked("ALN-BBBBBB", 3000)));
        CHECK(history.Add(Tracked("ALN-CCCCCC", 2000)));
    }
    ReportHistory restarted(file);
    restarted.Load();
    CHECK(restarted.reports().size() == 3);
    if (restarted.reports().size() != 3) return;
    CHECK(restarted.reports()[0].ticket == "ALN-BBBBBB" && restarted.reports()[1].ticket == "ALN-CCCCCC");
    const TrackedReport& report = restarted.reports()[2];
    CHECK(report.ticket == "ALN-AAAAAA" && report.receipt == kReceipt && report.submitted_at == 1000);
    CHECK(report.description == "It froze" && report.log_file == "alienation_186.log");
    CHECK(report.launcher_version == "0.9.3" && report.game_build == "CUSA04430 1.00" && report.cusa == "CUSA04430");
    CHECK(report.operating_system == "Windows 11 (build 26200)");
    CHECK(report.status == "new" && report.status_updated_at == 1000 && !report.unseen);

    const std::string saved = ReadAll(file);
    CHECK(Has(saved, kReceipt) && !Has(saved, "Launcher: shown") && !Has(saved, "account") && !Has(saved, "secret"));
    CHECK(!fs::exists(fs::path(file).concat(".new")));
}

void TestHistoryIsBounded() {
    TempFolder temp;
    ReportHistory history(temp.path / kReportHistoryFile);
    for (int i = 0; i < int(kMostTrackedReports) + 5; ++i) history.Add(Tracked("ALN-" + std::to_string(100000 + i), i));
    CHECK(history.reports().size() == kMostTrackedReports);
    CHECK(history.reports().front().submitted_at == int64_t(kMostTrackedReports) + 4 && history.reports().back().submitted_at == 5);
    const net::Json request = IncidentStatusRequest(history.reports());
    CHECK(request["reports"].size() == kMostTrackedReports);
    CHECK(Has(ReadAll(kSource / "server" / "lobby" / "src" / "incidents.js"), "statusBatch: " + std::to_string(kMostTrackedReports) + ","));
}

void TestHistorySaveFailureKeepsThePrevious() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    ReportHistory history(file);
    CHECK(history.Add(Tracked("ALN-AAAAAA", 1000)));
    const std::string before = ReadAll(file);
    fs::create_directory(fs::path(file).concat(".new"));
    CHECK(!history.Add(Tracked("ALN-BBBBBB", 2000)));
    CHECK(history.reports().size() == 2);
    CHECK(ReadAll(file) == before);

    ReportHistory unwritable(temp.path / kReportHistoryFile / "nested" / kReportHistoryFile);
    CHECK(!unwritable.Add(Tracked("ALN-CCCCCC", 3000)));
    CHECK(unwritable.reports().size() == 1);
}

void TestDamagedHistoryIsSetAside() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    for (const char* text : {"{\"reports\": [{\"ticket\"", "not json", "[]", "{}"}) {
        std::ofstream(file, std::ios::binary) << text;
        ReportHistory history(file);
        history.Load();
        CHECK(history.reports().empty());
        CHECK(!fs::exists(file));
        CHECK(ReadAll(fs::path(file).concat(".damaged")) == text);
        CHECK(history.Add(Tracked("ALN-AAAAAA", 1000)));
        fs::remove(file);
    }
    std::ofstream(file, std::ios::binary) << "{\"reports\": [{\"ticket\": \"ALN-AAAAAA\"}, {\"receipt\": \"x\"}]}";
    ReportHistory partial(file);
    partial.Load();
    CHECK(partial.reports().empty());
}

void TestStatusChangesMarkAReportUpdated() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    ReportHistory history(file);
    history.Add(Tracked("ALN-AAAAAA", 1000));

    CHECK(!history.Apply({StatusOf("ALN-AAAAAA", "new", 1000)}));
    CHECK(!history.HasUnseen());

    CHECK(history.Apply({StatusOf("ALN-AAAAAA", "investigating", 2000, "", "Looking at it")}));
    CHECK(history.HasUnseen());
    CHECK(history.reports()[0].status == "investigating" && history.reports()[0].player_message == "Looking at it");
    CHECK(history.reports()[0].status_updated_at == 2000);
    CHECK(history.MarkSeen("ALN-AAAAAA"));
    CHECK(!history.HasUnseen() && !history.MarkSeen("ALN-AAAAAA"));
    CHECK(!history.Apply({StatusOf("ALN-AAAAAA", "investigating", 2000, "", "Looking at it")}));

    CHECK(history.Apply({StatusOf("ALN-AAAAAA", "fixed", 3000, "0.9.3")}));
    CHECK(history.HasUnseen() && history.reports()[0].fixed_version == "0.9.3" && history.reports()[0].player_message.empty());
    CHECK(history.Save());

    ReportHistory restarted(file);
    restarted.Load();
    CHECK(restarted.HasUnseen());
    restarted.MarkSeen("ALN-AAAAAA");
    CHECK(restarted.Save());
    ReportHistory again(file);
    again.Load();
    CHECK(!again.HasUnseen() && again.reports()[0].status == "fixed");

    CHECK(!history.Apply({}));
    CHECK(!history.Apply({StatusOf("ALN-ZZZZZZ", "fixed", 9000)}));
    CHECK(!history.Apply({StatusOf("ALN-AAAAAA", "something_new", 9000)}));
    CHECK(history.reports()[0].status == "fixed" && history.reports()[0].status_updated_at == 3000);
}

std::string Body(const std::string& text, std::string_view signature) {
    const size_t start = text.find(signature);
    if (start == std::string::npos) return {};
    const size_t end = text.find("\n}\n", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

void TestRemoveIsLocal() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    ReportHistory history(file);
    history.Add(Tracked("ALN-AAAAAA", 1000));
    history.Add(Tracked("ALN-BBBBBB", 2000));
    CHECK(history.Remove("ALN-AAAAAA") && !history.Remove("ALN-AAAAAA"));
    CHECK(history.Save());
    CHECK(!Has(ReadAll(file), "ALN-AAAAAA") && Has(ReadAll(file), "ALN-BBBBBB"));
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string reports = Body(page, "void MyReports::DrawReports(");
    CHECK(!reports.empty());
    CHECK(Has(reports, "if (removed == open_ticket_) Collapse();") && Has(reports, "history_.Remove(removed);") && Has(reports, "SaveHistory();"));
    CHECK(reports.find("for (size_t i = 0;") < reports.find("history_.Remove(removed);"));
    CHECK(!Has(reports, "Fetch") && !Has(reports, "Refresh"));
    CHECK(!Has(ReadAll(kSource / "launcher" / "incident_report.cpp"), "incident.delete"));
}

void TestStatusProtocol() {
    TempFolder temp;
    ReportHistory history(temp.path / kReportHistoryFile);
    history.Add(Tracked("ALN-AAAAAA", 1000));
    const net::Json request = IncidentStatusRequest(history.reports());
    CHECK(request.size() == 1 && request["reports"].size() == 1);
    CHECK(request["reports"].items()[0]["ticket"].as_string() == "ALN-AAAAAA");
    CHECK(request["reports"].items()[0]["receipt"].as_string() == kReceipt);
    CHECK(request["reports"].items()[0].size() == 2);

    const auto answer = net::Json::parse(R"({"reports":[{"ticket":"ALN-AAAAAA","status":"fixed","status_updated_at":1793891198608,
        "fixed_version":"0.9.3","player_message":"Done"},{"ticket":"ALN-BBBBBB","status":"closed","status_updated_at":5}]})");
    CHECK(answer.has_value());
    if (!answer) return;
    const std::vector<ReportStatus> statuses = ReadReportStatuses(*answer);
    CHECK(statuses.size() == 2);
    if (statuses.size() != 2) return;
    CHECK(statuses[0].updated_at == 1793891198608 && statuses[0].fixed_version == "0.9.3" && statuses[0].player_message == "Done");
    CHECK(statuses[1].status == "closed" && statuses[1].fixed_version.empty());
    CHECK(ReadReportStatuses(net::Json::Object()).empty());

    const IncidentPackage package = SamplePackage();
    CHECK(IncidentPreviewText(SentReportOf(IncidentRequestBody(package))) == IncidentPreviewText(package));

    const std::string desk = ReadAll(kSource / "server" / "lobby" / "src" / "lobby.js");
    CHECK(Has(desk, std::string("'") + kIncidentStatusOperation + "'(att, msg)") && Has(desk, std::string("'") + kIncidentReadOperation + "'(att, msg)"));
}

void TestStatusTexts() {
    for (const char* status : {"new", "investigating", "fixed", "cannot_reproduce", "duplicate", "not_a_bug", "closed"}) {
        CHECK(ReportStatusText(status) != nullptr);
        CHECK(Has(ReadAll(kSource / "server" / "lobby" / "src" / "incidents.js"), std::string("'") + status + "'"));
    }
    CHECK(ReportStatusText("something_new") == nullptr);
    CHECK(std::string(ReportStatusText("cannot_reproduce")) == "Cannot reproduce");
    TrackedReport report = Tracked("ALN-AAAAAA", 1000);
    report.status = "closed";
    report.fixed_version = "0.9.3";
    CHECK(ReportStatusLine(report) == "Closed");
    report.status = "fixed";
    CHECK(ReportStatusLine(report) == "Fixed in version 0.9.3");
    report.fixed_version.clear();
    CHECK(ReportStatusLine(report) == "Fixed");
    CHECK(ReportPreview("Short") == "Short");
    CHECK(ReportPreview("two\nlines") == "two lines");
    CHECK(ReportPreview(std::string(80, 'x')) == std::string(60, 'x') + "...");
}

void TestReceiptsStayOutOfLogsAndSettings() {
    for (const char* name : {"incident_report.cpp", "my_reports.cpp", "report_history.cpp", "incident_page.cpp"}) {
        const std::string text = ReadAll(kSource / "launcher" / name);
        CHECK(!text.empty());
        for (size_t at = text.find("printf("); at != std::string::npos; at = text.find("printf(", at + 1)) {
            const std::string line = text.substr(at, text.find('\n', at) - at);
            CHECK(!Has(line, "receipt") && !Has(line, "credential"));
        }
    }
    CHECK(!Has(ReadAll(kSource / "launcher" / "settings.cpp"), "receipt"));
    CHECK(!Has(ReadAll(kSource / "launcher" / "online_settings.cpp"), "receipt"));
    CHECK(std::string_view(kReportHistoryFile) != "alienation.ini");
    const std::string online = ReadAll(kSource / "docs" / "ONLINE.md");
    CHECK(Has(online, "incident.status") && Has(online, "receipt"));
}

void TestLookupsTakeNoAccountAndNeverBlock() {
    const std::string report = ReadAll(kSource / "launcher" / "incident_report.cpp");
    const size_t fetch = report.find("FetchReportStatuses(const");
    CHECK(fetch != std::string::npos);
    const std::string lookups = report.substr(fetch);
    CHECK(CountOf(lookups, "session.connect(config.lobby, &error)") == 2);
    CHECK(!Has(lookups, "load_identity") && !Has(lookups, "hello_body") && !Has(lookups, "session.open"));
    CHECK(!Has(lookups, "printf"));

    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    CHECK(CountOf(page, "std::thread(") == 2 && CountOf(page, ".detach();") == 2);
    CHECK(CountOf(page, "FetchReportStatuses(") == 1 && CountOf(page, "FetchSentReport(") == 1);
    const std::string launcher = ReadAll(kSource / "launcher" / "launcher.cpp");
    CHECK(Has(launcher, "incident_page_->CheckReports(online);"));
    CHECK(Has(launcher, "[this] { return incident_page_->HasReportUpdates(); }, nullptr};"));
    CHECK(!Has(launcher, "OpenPopup(\"Report"));
}

void TestNewStatusesCollectOnTheUiThread() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const size_t collect = page.find("void MyReports::Collect()");
    const size_t save = page.find("void MyReports::SaveHistory()");
    CHECK(collect != std::string::npos && collect < save);
    const std::string collected = page.substr(collect, save - collect);
    CHECK(Has(collected, "check_->statuses && history_.Apply(*check_->statuses)"));
    CHECK(Has(collected, "check_failed_ = asked_by_player_ && !check_->statuses;"));
    const std::string thread = page.substr(page.find("check->statuses = FetchReportStatuses"), 90);
    CHECK(Has(thread, "check->done = true;") && !Has(thread, "history_"));
}

void TestMyReportsPageStack() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    for (const auto& [open, close] : {std::pair{"ImGui::PushID(", "ImGui::PopID()"}, std::pair{"ImGui::PushFont(", "ImGui::PopFont()"},
                                      std::pair{"ImGui::PushStyleColor(", "ImGui::PopStyleColor()"},
                                      std::pair{"ImGui::PushStyleVar(", "ImGui::PopStyleVar()"},
                                      std::pair{"ImGui::BeginDisabled(", "ImGui::EndDisabled()"},
                                      std::pair{"ImGui::PushTextWrapPos(", "ImGui::PopTextWrapPos()"},
                                      std::pair{"ImGui::BeginTable(", "ImGui::EndTable()"}, std::pair{"ImGui::Indent(", "ImGui::Unindent("},
                                      std::pair{"ImGui::BeginPopupModal(", "ImGui::EndPopup()"}, std::pair{"layers.Split(", "layers.Merge("}}) {
        CHECK(CountOf(page, open) == CountOf(page, close));
        CHECK(CountOf(page, open) > 0);
    }
    CHECK(!Has(page, "BeginChild"));

    // A collapsed row opens its table with RowLeft before it pushes its ID, which is popped before the next row.
    const std::string row = Body(page, "void MyReports::DrawRow(");
    const size_t left = row.find("ui.RowLeft("), control = row.find("ui.RowControl();");
    const size_t push = row.find("ImGui::PushID("), pop = row.find("ImGui::PopID();");
    CHECK(!row.empty() && left < control && control < push && push < pop);
    CHECK(!Has(row, "return;") && !Has(row, "Dummy"));

    // An expanded report closes the rows before it, and its tables open and close between its own PushID and PopID.
    const std::string expanded = Body(page, "void MyReports::DrawExpanded(");
    const size_t close_rows = expanded.find("ui.EndRows();"), push_id = expanded.find("ImGui::PushID(");
    const size_t summary = expanded.find("DrawSummary("), facts = expanded.find("DrawFacts("), actions = expanded.find("DrawActions(");
    const size_t merged = expanded.find("layers.Merge(draw);"), pop_id = expanded.find("ImGui::PopID();");
    CHECK(!expanded.empty() && close_rows < push_id && push_id < summary && summary < facts && facts < actions && actions < merged &&
          merged < pop_id);
    CHECK(!Has(expanded, "return;") && CountOf(expanded, "ImGui::PushID(") == 1 && CountOf(expanded, "ImGui::PopID();") == 1);
    for (const char* table : {"void MyReports::DrawSummary(", "void DrawSummary(", "void DrawFacts("}) {
        const std::string body = Body(page, table);
        if (body.empty()) continue;
        CHECK(Has(body, "if (!ImGui::BeginTable(") && Has(body, "ImGui::EndTable();"));
        CHECK(!Has(body, "ImGui::PushID(") && !Has(body, "ImGui::PopID("));
    }
    const std::string confirm = Body(page, "bool MyReports::ConfirmRemoval(");
    CHECK(confirm.find("ImGui::EndPopup();") != std::string::npos && confirm.find("if (!open) return false;") < confirm.find("ImGui::EndPopup();"));
    CHECK(confirm.find("ImGui::PopStyleVar();") < confirm.find("if (!open) return false;"));
}

void TestRefreshLivesInTheHeader() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string draw = Body(page, "void MyReports::Draw(");
    const std::string header = Body(page, "void MyReports::DrawHeader(");
    CHECK(!draw.empty() && draw.find("DrawHeader(ui);") < draw.find("DrawReports(ui);"));
    CHECK(header.find("ImGui::TextUnformatted(title);") < header.find("ImGui::Button(Tr(check_ ? \"Checking...\" : \"Refresh\")"));
    CHECK(Has(header, "ImGui::SameLine(ImGui::GetCursorPosX() + room - width)") && Has(header, "if (room >= title_width"));
    CHECK(header.find("ImGui::Button(") < header.find("ui.Help(\"Only reports sent from this PC are listed here.\");"));
    CHECK(Has(header, "ImGui::BeginDisabled(check_ != nullptr);") && Has(header, "check_failed_"));

    CHECK(!Has(page, "ButtonRow(\"Refresh\"") && !Has(page, "Checks these reports"));
    CHECK(CountOf(page, "Refresh(LobbyConfig(ui), true)") == 1 && CountOf(page, "void MyReports::Refresh(") == 1);
    CHECK(CountOf(page, "std::make_shared<StatusCheck>()") == 1 && CountOf(page, "FetchReportStatuses(") == 1);
    CHECK(Has(ReadAll(kSource / "launcher" / "incident_page.h"), "my_reports_.Refresh(config);"));
    CHECK(Has(ReadAll(kSource / "launcher" / "incident_page.cpp"), "my_reports_.RefreshIfStale(config);"));
    CHECK(Has(ReadAll(kSource / "launcher" / "launcher.cpp"), "incident_page_->CheckReports(online);"));
}

void TestDetailsExpandInPlace() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string header = ReadAll(kSource / "launcher" / "my_reports.h");
    const std::string reports = Body(page, "void MyReports::DrawReports(");
    const size_t expanded = reports.find("DrawExpanded(ui, reports[i], i, toggled, removed)");
    const size_t collapsed = reports.find("DrawRow(ui, reports[i], i, toggled)");
    CHECK(reports.find("for (size_t i = 0; i < reports.size(); ++i) {") < expanded && expanded < collapsed);
    CHECK(Has(reports, "if (reports[i].ticket == open_ticket_) DrawExpanded"));
    CHECK(collapsed < reports.find("if (!toggled.empty()) Open(toggled);"));
    CHECK(!Has(page, "DrawDetail") && !Has(page, "DrawRows") && CountOf(page, "DrawExpanded(ui") == 1);
    const std::string draw = Body(page, "void MyReports::Draw(");
    CHECK(draw.rfind("DrawReports(ui);") != std::string::npos && draw.find("DrawReports(ui);") > draw.find("DrawHeader(ui);"));

    // One open report: a second View details replaces it, and the same one closes it.
    CHECK(Has(header, "std::string open_ticket_;") && !Has(header, "open_tickets") && !Has(header, "std::set<std::string>"));
    const std::string open = Body(page, "void MyReports::Open(");
    CHECK(Has(open, "if (open_ticket_ == ticket) {") && Has(open, "Collapse();") && Has(open, "open_ticket_ = ticket;"));
    CHECK(open.find("Collapse();") < open.find("open_ticket_ = ticket;"));
    const std::string collapse = Body(page, "void MyReports::Collapse(");
    CHECK(Has(collapse, "open_ticket_.clear();") && Has(collapse, "ForgetSentReport();"));
    const std::string summary = Body(page, "void DrawSummary(");
    CHECK(Has(summary, "ImGui::Button(Tr(\"Hide details\")") && Has(summary, "toggled = report.ticket;"));
    const std::string row = Body(page, "void MyReports::DrawRow(");
    CHECK(Has(row, "ImGui::Button(Tr(\"View details\")") && Has(row, "toggled = report.ticket;"));
    CHECK(Has(row, "ReportPreview(report.description)") && Has(row, "StatusText(report)") && Has(row, "DateText(report.submitted_at)"));
    CHECK(Has(Body(page, "std::string StatusText("), "Tr(\"Updated\")"));
}

void TestExpandedReportIsOneFramedItem() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string expanded = Body(page, "void MyReports::DrawExpanded(");
    const char* order[] = {"history_.MarkSeen(report.ticket)", "layers.Split(draw, 2);", "layers.SetCurrentChannel(draw, 1);", "DrawSummary(",
                           "DrawFacts(", "DrawActions(", "DrawSentReport(ui);", "layers.SetCurrentChannel(draw, 0);", "DrawFrame(",
                           "layers.Merge(draw);"};
    size_t at = 0;
    for (const char* part : order) {
        const size_t found = expanded.find(part);
        CHECK(found != std::string::npos && found >= at);
        at = found == std::string::npos ? at : found;
    }
    CHECK(Has(expanded, "if (sent_ticket_ == report.ticket) DrawSentReport(ui);"));

    // Summary and details share one frame, with no row line between them.
    for (const char* name : {"void DrawSummary(", "void DrawFacts(", "void MyReports::DrawActions(", "void MyReports::DrawSentReport(",
                             "void MyReports::DrawExpanded("}) {
        const std::string body = Body(page, name);
        CHECK(!body.empty());
        for (const char* row : {"ui.Row(", "ui.RowLeft(", "ui.RowControl(", "ButtonRow(", "ConfirmRow(", "InfoGrid(", "Separator"}) {
            CHECK(!Has(body, row));
        }
    }
    const std::string frame = Body(page, "void DrawFrame(");
    CHECK(Has(frame, "border_panel") && Has(frame, "theme.panel") && Has(frame, "theme.frame") && !Has(frame, "AddRect("));
    CHECK(CountOf(frame, "AddRectFilled(") == 3 && Has(frame, "PanelBorderWidth(scale)"));
    CHECK(Has(Body(page, "void DrawFacts("), "ImGui::BeginTable(\"##facts\"") && Has(Body(page, "void DrawSummary("), "ImGui::BeginTable(\"##summary\""));

    const std::string facts = Body(page, "void DrawFacts(");
    for (const char* label : {"Report ID", "Submitted", "Description", "Selected log", "Game build", "Launcher version", "Status",
                              "Status updated", "Fixed in version", "Message from developer"}) {
        CHECK(Has(facts, std::string("\"") + label + "\""));
    }
    CHECK(Has(facts, "report.status == \"fixed\" && !report.fixed_version.empty()") && Has(facts, "!report.player_message.empty()"));
    CHECK(!Has(page, "BeginChild") && !Has(page, "SetScrollHereY(1.0f)"));
    CHECK(Has(Body(page, "void KeepInView("), "top < window->InnerRect.Min.y"));
}

void TestWhatWasSentStaysInTheReport() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    CHECK(CountOf(page, "\"##sent\"") == 1 && CountOf(page, "DrawSentReport(ui)") == 1);
    const std::string sent = Body(page, "void MyReports::DrawSentReport(");
    CHECK(Has(sent, "\"##sent\"") && Has(sent, "sent_unavailable_") && Has(sent, "ImGuiInputTextFlags_ReadOnly"));
    const std::string actions = Body(page, "void MyReports::DrawActions(");
    CHECK(Has(actions, "\"What was sent\"") && Has(actions, "\"Hide report\"") && Has(actions, "ReadSentReport(report, LobbyConfig(ui));"));
    CHECK(Has(actions, "ConfirmRemoval(ui)") && Has(actions, "removed = report.ticket;"));
    const std::string confirm = Body(page, "bool MyReports::ConfirmRemoval(");
    CHECK(Has(confirm, "Remove this report from this PC?") && Has(confirm, "ui.AccentButton(\"Remove\"") && Has(confirm, "Cancel"));
    CHECK(!Has(confirm, "Fetch") && !Has(actions, "incident.delete"));
    const std::string forget = Body(page, "void MyReports::ForgetSentReport(");
    CHECK(Has(forget, "sent_ticket_.clear();") && Has(forget, "sent_text_.clear();") && Has(forget, "sent_unavailable_ = false;"));
    const std::string collect = Body(page, "void MyReports::Collect(");
    CHECK(Has(collect, "reading_->ticket == sent_ticket_"));
}

void TestSelectedLogButtonSitsInItsFacts() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    const std::string facts = Body(page, "bool DrawLogFacts(");
    CHECK(!facts.empty());
    CHECK(CountOf(page, "\"View selected log\"") == 1 && CountOf(facts, "ImGui::Button(Tr(\"View selected log\")") == 1);
    CHECK(CountOf(page, "trg::OpenUrl(") == 1 && Has(page, "if (DrawLogFacts(ui, log)) trg::OpenUrl(Utf8(log.path));"));
    CHECK(!Has(page, "ButtonRow(\"Selected log\"") && !Has(page, "Opens the selected log") && !Has(page, "InfoGrid("));
    CHECK(!Has(facts, "BeginDisabled") && !Has(facts, "OpenUrl") && !Has(facts, "ui.Row") && !Has(facts, "ButtonRow"));

    // The button is the last cell of the grid's second row, as wide as the page's other buttons and flush with their right edge.
    const size_t last_written = facts.find("FactLabel(ui, \"Last written\");"), size = facts.find("FactLabel(ui, \"Size\");");
    const size_t place = facts.find("ImGui::SetCursorScreenPos(ImVec2(left + room - width, cell.y));");
    const size_t button = facts.find("ImGui::Button(Tr(\"View selected log\")"), end_table = facts.find("ImGui::EndTable();");
    CHECK(last_written < size && size < place && place < button && button < end_table);
    CHECK(Has(facts, "ControlColumnWidth(room, scale), left + room - cell.x)") && Has(facts, "ImVec2(width, 0)"));
    CHECK(CountOf(facts, "ImGui::SetCursorScreenPos(") == 1);
    CHECK(Has(facts, "ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoClip") && CountOf(facts, "ImGui::BeginTable(") == 1);

    // Nothing follows the grid: the next section starts right after it.
    const std::string logs = Body(page, "void IncidentPage::DrawLogs(");
    const size_t grid = logs.find("DrawLogFacts(ui, log)");
    CHECK(grid != std::string::npos);
    const std::string after = logs.substr(grid);
    for (const char* extra : {"ui.Row", "Spacer", "Dummy", "ButtonRow", "Separator", "EndRows", "Paragraph", "Help("}) CHECK(!Has(after, extra));
    CHECK(Has(page, "DrawLogs(ui, sending);\n    ui.Heading(\"3. Preview what will be sent\");"));

    // Stack: the cell padding and the table close together, and nothing returns between them.
    CHECK(CountOf(facts, "ImGui::PushStyleVar(") == 1 && CountOf(facts, "ImGui::PopStyleVar();") == 2);
    CHECK(facts.find("ImGui::PopStyleVar();") < facts.find("return false;") && facts.find("ImGui::EndTable();") < facts.rfind("ImGui::PopStyleVar();"));
    CHECK(CountOf(facts, "return ") == 2 && !Has(facts, "ImGui::PushID(") && !Has(facts, "ImGui::PopID("));
    for (const char* helper : {"void FactLabel(", "void FactValue(", "float SemiboldWidth("}) {
        const std::string body = Body(page, helper);
        CHECK(CountOf(body, "ImGui::PushFont(") == CountOf(body, "ImGui::PopFont()"));
        CHECK(CountOf(body, "ImGui::PushStyleColor(") == CountOf(body, "ImGui::PopStyleColor()"));
        CHECK(CountOf(body, "ImGui::PushTextWrapPos(") == CountOf(body, "ImGui::PopTextWrapPos()"));
    }
}

void TestLobbyAgrees() {
    const std::string desk = ReadAll(kSource / "server" / "lobby" / "src" / "incidents.js");
    const std::string lobby = ReadAll(kSource / "server" / "lobby" / "src" / "lobby.js");
    CHECK(Has(lobby, std::string("'") + kIncidentOperation + "'(att, msg)"));
    CHECK(Has(desk, "incident_hourly_limit:") && Has(desk, "incident_daily_limit:") && Has(lobby, "fail(refusal, 'report limit');"));
    CHECK(Has(desk, "descriptionChars: " + std::to_string(kMostDescriptionCharacters) + ","));
    CHECK(Has(desk, "logBytes: " + std::to_string(kMostLogBytes / 1024) + " * 1024,"));
    CHECK(Has(desk, "alienation_[0-9]{3,10}"));
    CHECK(Has(desk, "frameChars: 160 * 1024,"));
    // A report is stored as received or refused: the Worker has no second filter that could change what the player saw.
    CHECK(!Has(desk, ".replace(") && Has(desk, "value !== value.trim()"));
}

void TestProductionLobbyOnly() {
    // The page reaches a report service only the way the Online page does; by default there is none.
    for (const char* file : {"incident_page.cpp", "incident_page.h", "incident_report.cpp", "incident_report.h"}) {
        const std::string text = ReadAll(kSource / "launcher" / file);
        CHECK(!text.empty() && !Has(text, "workers.dev") && !Has(text, "wss://") && !Has(text, "ws://") && !Has(text, "localhost") &&
              !Has(text, "127.0.0.1"));
    }
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    CHECK(Has(page, "net::OnlineConfig config = OnlineConfigOf(ui.settings());") && Has(page, "net::apply_environment(config);"));
    const std::string settings = ReadAll(kSource / "src" / "net" / "online_config.h");
    CHECK(Has(settings, "kDefaultLobbyUrl[] = \"\";"));
    CHECK(Has(settings, "std::string lobby = kDefaultLobbyUrl;"));
}

} // namespace

int main() {
    TestNewestFiveFirst();
    TestBuildSectionBeforeTheSession();
    TestFewerThanFive();
    TestSessionsThatAreNotGameLogs();
    TestCurrentSessionAndLastGameplay();
    TestLauncherSessionsAreNotGameplay();
    TestDefaultLog();
    TestOneChosenLogIsSent();
    TestAddresses();
    TestDiagnosticsStay();
    TestLocalPaths();
    TestNames();
    TestSecrets();
    TestOtherPlayers();
    TestTextIsClean();
    TestSizeLimit();
    TestPreviewIsTheRequest();
    TestPreviewButton();
    TestPageStackPairs();
    TestDraftChecks();
    TestDescriptionLimit();
    TestLogListText();
    TestOutcomes();
    TestLimitsKeepTheDraft();
    TestResultComesIntoView();
    TestNoQuotaOnThePage();
    TestSentReportIsRemembered();
    TestHistoryIsBounded();
    TestHistorySaveFailureKeepsThePrevious();
    TestDamagedHistoryIsSetAside();
    TestStatusChangesMarkAReportUpdated();
    TestRemoveIsLocal();
    TestStatusProtocol();
    TestStatusTexts();
    TestReceiptsStayOutOfLogsAndSettings();
    TestLookupsTakeNoAccountAndNeverBlock();
    TestNewStatusesCollectOnTheUiThread();
    TestMyReportsPageStack();
    TestRefreshLivesInTheHeader();
    TestDetailsExpandInPlace();
    TestExpandedReportIsOneFramedItem();
    TestWhatWasSentStaysInTheReport();
    TestSelectedLogButtonSitsInItsFacts();
    TestLobbyAgrees();
    TestProductionLobbyOnly();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
