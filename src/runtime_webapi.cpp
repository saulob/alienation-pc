// NpWebApi, the Invitation dialog and session invitations while online play is on (docs/ONLINE.md "Sessions and
// invitations"). Every room the game creates gets an NP session (POST /v1/sessions) and every join goes through one
// (POST members, then GET sessionData, whose first 8 bytes are the Matching2 room id), so these requests are answered by
// the lobby; userProfile reads come from the lobby's friends. An accepted invitation, or a Join picked in the launcher,
// reaches the game as SCE_SYSTEM_SERVICE_EVENT_SESSION_INVITATION, the event its own join path waits for.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "alngpu.h"
#include "net/json.h"
#include "net/lobby_link.h"
#include "runtime.h"
#include "runtime_online.h"

#define ALN_SYSV __attribute__((sysv_abi))

namespace aln::online {
namespace {

using net::Json;

constexpr int32_t kErrInvalid = int32_t(0x80552902), kErrNoRequest = int32_t(0x80552906);
constexpr int32_t kDialogNotInitialized = int32_t(0x80B80003), kDialogBusy = int32_t(0x80B80004);
constexpr int32_t kDialogNotFinished = int32_t(0x80B80005), kDialogNotRunning = int32_t(0x80B8000B);
constexpr int kStatusNone = 0, kStatusInitialized = 1, kStatusRunning = 2, kStatusFinished = 3;
constexpr uint32_t kSessionInvitationEvent = 0x10000002;
constexpr int32_t kInitialUser = 1;
constexpr size_t kNpIdSize = 0x24, kOnlineIdChars = 16, kSessionIdChars = 0x2f, kInvitationIdChars = 0x3f;
constexpr int64_t kPendingJoinSeconds = 600;

template <class T> T Rd(const void* p, size_t offset) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(v));
    return v;
}
template <class T> void Wr(void* p, size_t offset, T v) { std::memcpy(static_cast<uint8_t*>(p) + offset, &v, sizeof(v)); }

std::string Text(const char* p, size_t max) { return p ? std::string(p, strnlen(p, max)) : std::string(); }

// ---- sessions, invitations and friends over the lobby ----
struct Reply {
    int status = 404;
    std::string body;
};

int HttpStatus(const std::string& code) {
    if (code == "ok") return 200;
    if (code == "not_found") return 404;
    if (code == "forbidden" || code == "incompatible" || code == "not_friends" || code == "full" || code == "not_owner") return 403;
    if (code == "limit") return 429;
    if (code == net::kUnreachable || code == net::kLinkLost || code == "timeout") return 503;
    return 400;
}

std::vector<std::string> Segments(const std::string& path) {
    std::vector<std::string> parts;
    const std::string bare = path.substr(0, path.find('?'));
    size_t at = 0;
    while (at <= bare.size()) {
        const size_t slash = bare.find('/', at);
        const std::string part = bare.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (!part.empty()) parts.push_back(part);
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    return parts;
}

int QueryInt(const std::string& path, const std::string& key) {
    const size_t q = path.find('?');
    if (q == std::string::npos) return 0;
    const std::string query = "&" + path.substr(q + 1);
    const size_t at = query.find("&" + key + "=");
    return at == std::string::npos ? 0 : std::atoi(query.c_str() + at + key.size() + 2);
}

// The parts of the POST /v1/sessions multipart body: the session-request JSON and the 16 bytes of session data.
struct SessionRequest {
    std::optional<Json> json;
    std::string data;
};

SessionRequest ReadSessionRequest(const std::string& body, const std::string& content_type) {
    SessionRequest out;
    const size_t b = content_type.find("boundary=");
    const std::string boundary = "--" + (b == std::string::npos ? std::string("xyz123xyz123xyz123") : content_type.substr(b + 9));
    size_t at = body.find(boundary);
    while (at != std::string::npos) {
        const size_t start = at + boundary.size();
        const size_t next = body.find(boundary, start);
        const std::string part = body.substr(start, next == std::string::npos ? std::string::npos : next - start);
        size_t head_end = part.find("\n\n");
        size_t skip = 2;
        if (const size_t crlf = part.find("\r\n\r\n"); crlf != std::string::npos && crlf < head_end) head_end = crlf, skip = 4;
        if (head_end != std::string::npos) {
            const std::string head = part.substr(0, head_end);
            const std::string content = part.substr(head_end + skip);
            const size_t length_at = head.find("Content-Length:");
            const size_t length = length_at == std::string::npos ? content.size() : size_t(std::atoi(head.c_str() + length_at + 15));
            if (head.find("session-request") != std::string::npos) out.json = Json::parse(content.substr(0, std::min(length, content.size())));
            if (head.find("session-data") != std::string::npos) out.data = content.substr(0, std::min(length, content.size()));
        }
        at = next;
    }
    return out;
}

Reply CreateSession(const std::string& body, const std::string& content_type) {
    const SessionRequest request = ReadSessionRequest(body, content_type);
    if (!request.json || request.data.size() != 16) {
        std::printf("Session: POST /v1/sessions without a session request or 16 bytes of session data\n");
        return {400, ""};
    }
    const bool is_private = (*request.json)["sessionPrivacy"].as_string() == "private";
    const int64_t max = std::clamp<int64_t>((*request.json)["sessionMaxUser"].as_int(4), 1, 8);
    Json call = Json::Object();
    call.set("private", is_private);
    call.set("max", max);
    call.set("data", net::hex_encode(request.data.data(), request.data.size()));
    Json reply;
    std::string code;
    if (!lobby_call("session.create", call, &reply, &code)) {
        std::printf("Session: create refused (%s)\n", code.c_str());
        return {HttpStatus(code), ""};
    }
    uint64_t room = 0;
    std::memcpy(&room, request.data.data(), sizeof(room));
    std::printf("Session: created %s for room %llu (%s, %lld seats)\n", reply["session"].as_string().c_str(),
                (unsigned long long)room, is_private ? "private" : "public", (long long)max);
    Json answer = Json::Object();
    answer.set("sessionId", reply["session"].as_string());
    return {200, answer.dump()};
}

Reply SessionCall(const char* op, const std::string& session, Json body = Json::Object()) {
    body.set("session", session);
    Json reply;
    std::string code;
    if (!lobby_call(op, body, &reply, &code)) {
        std::printf("Session: %s %s refused (%s)\n", op, session.c_str(), code.c_str());
        return {HttpStatus(code), ""};
    }
    if (std::strcmp(op, "session.data") != 0) return {204, ""};
    std::vector<uint8_t> data;
    net::hex_decode(reply["data"].as_string(), data);
    uint64_t room = 0;
    if (data.size() >= 8) std::memcpy(&room, data.data(), sizeof(room));
    std::printf("Session: %s leads to room %llu\n", session.c_str(), (unsigned long long)room);
    return {200, std::string(data.begin(), data.end())};
}

// The friends the lobby lists, read again after a friend event or a few seconds (presence and friendList pages ask often).
std::mutex g_friends_mutex;
Json g_friends = Json::Array();
uint64_t g_friends_ms = 0;

bool Friends(Json& out) {
    {
        std::lock_guard<std::mutex> lock(g_friends_mutex);
        if (g_friends_ms && net::now_ms() - g_friends_ms < 10000) {
            out = g_friends;
            return true;
        }
    }
    Json reply;
    std::string code;
    if (!lobby_call("friends.list", Json::Object(), &reply, &code, 5000)) return false;
    std::lock_guard<std::mutex> lock(g_friends_mutex);
    g_friends = reply["friends"];
    g_friends_ms = net::now_ms();
    out = g_friends;
    return true;
}

// The friends as the Invitation dialog sees them: the ones it lists (online, same build) and why the others are not.
struct FriendCounts {
    std::vector<Json> listed;
    size_t total = 0, other_build = 0, offline = 0;
};

FriendCounts CountFriends(const Json& friends, const std::string& build) {
    FriendCounts c;
    for (const Json& f : friends.items()) {
        ++c.total;
        if (!f["online"].as_bool()) ++c.offline;
        else if (f["build"].as_string() != build) ++c.other_build;
        else c.listed.push_back(f);
    }
    return c;
}

std::string Plural(size_t n, const char* one) { return std::to_string(n) + " " + one + (n == 1 ? "" : "s"); }

std::string FriendSummary(const FriendCounts& c) {
    if (!c.total) return "no friends";
    return Plural(c.total, "friend") + ": " + std::to_string(c.listed.size()) + " online with your game version, " +
           std::to_string(c.other_build) + " on another version, " + std::to_string(c.offline) + " offline";
}

// The one row of an empty Invitation dialog; `requests` is whether a friend request waits on either side.
std::string NoFriendToInvite(const FriendCounts& c, bool requests) {
    if (!c.total && requests) return "No friends yet: a friend request is waiting in the launcher's Friends tab";
    if (!c.total) return "No friends added: add one by friend code in the launcher's Friends tab";
    if (c.other_build) return "Friends are online on another game version";
    return "No friend is online right now";
}

bool FriendRequestsWaiting() {
    Json reply;
    std::string code;
    if (!lobby_call("friends.requests", Json::Object(), &reply, &code, 5000)) return false;
    return reply["incoming"].size() || reply["outgoing"].size();
}

std::mutex g_summary_mutex;
std::string g_logged_summary;

void LogFriendSummary(const FriendCounts& c) {
    const std::string summary = FriendSummary(c);
    std::lock_guard<std::mutex> lock(g_summary_mutex);
    if (summary == g_logged_summary) return;
    g_logged_summary = summary;
    std::printf("Friend: list for the game: %s\n", summary.c_str());
}

// SceNpId of a friend: online id, terminator, padding, the account in opt (as the Matching2 layer writes members).
std::string NpIdText(const std::string& name, uint64_t account) {
    uint8_t id[kNpIdSize] = {};
    std::memcpy(id, name.data(), std::min(name.size(), kOnlineIdChars));
    Wr<uint64_t>(id, 0x14, account);
    return net::hex_encode(id, sizeof(id));
}

Reply FriendList(const std::string& path) {
    Json friends;
    if (!Friends(friends)) {
        std::printf("Friend: the game's friend list is unavailable (the lobby did not answer)\n");
        return {503, ""};
    }
    LogFriendSummary(CountFriends(friends, own_build()));
    const int offset = std::max(0, QueryInt(path, "offset"));
    Json list = Json::Array();
    for (size_t i = size_t(offset); i < friends.items().size(); ++i) {
        const Json& f = friends.items()[i];
        Json row = Json::Object();
        row.set("npId", NpIdText(f["name"].as_string(), f["account"].as_u64()));
        row.set("onlineId", f["name"].as_string());
        Json presence = Json::Object();
        presence.set("onlineStatus", f["online"].as_bool() ? "online" : "offline");
        row.set("presence", std::move(presence));
        list.push(std::move(row));
    }
    Json body = Json::Object();
    body.set("start", int64_t(offset));
    body.set("size", int64_t(list.size()));
    body.set("totalResults", int64_t(friends.items().size()));
    body.set("friendList", std::move(list));
    return {200, body.dump()};
}

Reply Presence(const std::string& who) {
    bool online = who == settings().online_id;
    Json friends;
    if (!online && Friends(friends)) {
        for (const Json& f : friends.items())
            if (f["name"].as_string() == who) online = f["online"].as_bool();
    }
    Json presence = Json::Object();
    presence.set("onlineStatus", online ? "online" : "offline");
    Json body = Json::Object();
    body.set("presence", std::move(presence));
    return {200, body.dump()};
}

std::mutex g_unknown_mutex;
std::set<std::string> g_unknown;

Reply Route(const std::string& group, int method, const std::string& path, const std::string& body,
            const std::string& content_type) {
    enum { kGet, kPost, kPut, kDelete };
    const std::vector<std::string> p = Segments(path);
    const auto is = [&](std::initializer_list<const char*> want) {
        if (p.size() != want.size()) return false;
        size_t i = 0;
        for (const char* w : want) {
            if (*w && p[i] != w) return false;
            ++i;
        }
        return true;
    };
    if (group == "sessionInvitation") {
        if (method == kPost && is({"v1", "sessions"})) return CreateSession(body, content_type);
        if (method == kPost && is({"v1", "sessions", "", "members"})) {
            std::printf("Session: joining %s\n", p[2].c_str());
            return SessionCall("session.join", p[2]);
        }
        if (method == kGet && is({"v1", "sessions", "", "sessionData"})) return SessionCall("session.data", p[2]);
        if (method == kPut && is({"v1", "sessions", ""})) {
            const std::optional<Json> change = Json::parse(body);
            if (!change || !(*change)["sessionPrivacy"].is_string()) return {204, ""};
            Json update = Json::Object();
            update.set("private", (*change)["sessionPrivacy"].as_string() == "private");
            return SessionCall("session.update", p[2], std::move(update));
        }
        if (method == kDelete && (is({"v1", "sessions", ""}) || is({"v1", "sessions", "", "members", ""}))) {
            return SessionCall("session.leave", p[2]);
        }
        if (method == kPut && is({"v1", "users", "", "invitations", ""})) {
            Json used = Json::Object();
            used.set("invitation", p[4]);
            lobby_send("invite.used", std::move(used));
            return {204, ""};
        }
    } else if (group == "userProfile" && p.size() >= 4 && p[0] == "v1" && p[1] == "users") {
        if (method == kGet && p[3] == "friendList") return FriendList(path);
        if (method == kGet && p[3] == "blockList") return {200, "{\"start\":0,\"size\":0,\"totalResults\":0,\"blockList\":[]}"};
        if (method == kGet && p[3] == "presence") return Presence(p[2]);
        if (method == kGet && p[3] == "profile") return {200, "{\"region\":\"\",\"personalDetail\":{},\"isOfficiallyVerified\":false}"};
        if (method == kPut && p[3] == "presence") return {204, ""};
    }
    const std::string what = group + " " + std::to_string(method) + " " + (p.size() > 3 ? p[0] + "/" + p[1] + "/*/" + p[3] : path);
    std::lock_guard<std::mutex> lock(g_unknown_mutex);
    if (g_unknown.insert(what).second) std::printf("WebApi: %s is not provided\n", what.c_str());
    return {404, ""};
}

// ---- NpWebApi ----
struct Request {
    std::string group, path, content_type;
    int method = 0;
    Reply reply;
    size_t read = 0;
};

std::mutex g_web;
std::map<int64_t, Request> g_requests;
int64_t g_next_request = 1;
int32_t g_next_id = 1;

int32_t NewId() {
    std::lock_guard<std::mutex> lock(g_web);
    return g_next_id++;
}

ALN_SYSV int32_t web_initialize(int32_t http_ctx, uint64_t pool) { (void)http_ctx, (void)pool; return NewId(); }
ALN_SYSV int32_t web_ok_id(int32_t id) { (void)id; return 0; }
ALN_SYSV int32_t web_ok_pair(int32_t a, int32_t b) { (void)a, (void)b; return 0; }
ALN_SYSV int32_t web_create_context(int32_t lib_ctx, const void* online_id) { (void)lib_ctx, (void)online_id; return NewId(); }
ALN_SYSV int32_t web_create_handle(int32_t lib_ctx) { (void)lib_ctx; return NewId(); }
ALN_SYSV int32_t web_push_filter(int32_t lib_ctx, const void* param, uint64_t count) { (void)lib_ctx, (void)param, (void)count; return NewId(); }
ALN_SYSV int32_t web_service_push_filter(int32_t lib_ctx, int32_t handle, const char* service, int32_t label, const void* param,
                                         uint64_t count) {
    (void)lib_ctx, (void)handle, (void)service, (void)label, (void)param, (void)count;
    return NewId();
}
// Push callbacks are registered and never called: invitations reach the game as a system event (the game only counts
// pushed invitations, and its scripts never read that count).
ALN_SYSV int32_t web_register_callback(int32_t user_ctx, int32_t filter, void* callback, void* arg) {
    (void)user_ctx, (void)filter, (void)callback, (void)arg;
    return NewId();
}

// (user context, apiGroup, path, method 0 GET / 1 POST / 2 PUT / 3 DELETE, {u64 length, const char* type} or null, id*)
ALN_SYSV int32_t web_create_request(int32_t user_ctx, const char* group, const char* path, int32_t method, const void* content,
                                    int64_t* id) {
    (void)user_ctx;
    if (!group || !path || !id || method < 0 || method > 3) return kErrInvalid;
    Request r;
    r.group = Text(group, 64);
    r.path = Text(path, 1024);
    r.method = method;
    if (content) r.content_type = Text(Rd<const char*>(content, 8), 256);
    std::lock_guard<std::mutex> lock(g_web);
    *id = g_next_request++;
    g_requests[*id] = std::move(r);
    return 0;
}

// The answer comes from the lobby on the game's WebApi thread; only the HTTP status @0 of the response information is
// written.
ALN_SYSV int32_t web_send_request(int64_t id, const void* data, uint64_t size, void* response) {
    std::string group, path, type;
    int method = 0;
    {
        std::lock_guard<std::mutex> lock(g_web);
        const auto it = g_requests.find(id);
        if (it == g_requests.end()) return kErrNoRequest;
        group = it->second.group, path = it->second.path, type = it->second.content_type, method = it->second.method;
    }
    const std::string body = data && size ? std::string(static_cast<const char*>(data), size_t(std::min<uint64_t>(size, 4 << 20))) : "";
    const Reply reply = Route(group, method, path, body, type);
    {
        std::lock_guard<std::mutex> lock(g_web);
        const auto it = g_requests.find(id);
        if (it == g_requests.end()) return kErrNoRequest;
        it->second.reply = reply;
        it->second.read = 0;
    }
    if (response) Wr<int32_t>(response, 0, reply.status);
    return 0;
}

ALN_SYSV int32_t web_read_data(int64_t id, void* buffer, uint64_t size) {
    std::lock_guard<std::mutex> lock(g_web);
    const auto it = g_requests.find(id);
    if (it == g_requests.end()) return kErrNoRequest;
    if (!buffer) return kErrInvalid;
    Request& r = it->second;
    const size_t n = std::min<size_t>(size_t(size), r.reply.body.size() - r.read);
    std::memcpy(buffer, r.reply.body.data() + r.read, n);
    r.read += n;
    return int32_t(n);
}

ALN_SYSV int32_t web_delete_request(int64_t id) {
    std::lock_guard<std::mutex> lock(g_web);
    return g_requests.erase(id) ? 0 : kErrNoRequest;
}

ALN_SYSV int32_t web_abort_request(int64_t id) {
    std::lock_guard<std::mutex> lock(g_web);
    return g_requests.count(id) ? 0 : kErrNoRequest;
}

// The npId of friendList rows: the 36-byte SceNpId in hex as written above; a plain online id is accepted too.
ALN_SYSV int32_t web_parse_np_id(const char* text, uint8_t* np_id) {
    if (!text || !np_id) return kErrInvalid;
    std::memset(np_id, 0, kNpIdSize);
    const std::string value = Text(text, 128);
    std::vector<uint8_t> bytes;
    if (value.size() == kNpIdSize * 2 && net::hex_decode(value, bytes) && bytes.size() == kNpIdSize) {
        std::memcpy(np_id, bytes.data(), kNpIdSize);
        np_id[kOnlineIdChars] = 0;
        return 0;
    }
    std::memcpy(np_id, value.data(), std::min(value.size(), kOnlineIdChars));
    return 0;
}

// ---- session invitation events ----
void PushSessionEvent(const std::string& session, const std::string& invitation) {
    uint8_t event[0xc0] = {};
    Wr<uint32_t>(event, 0, kSessionInvitationEvent);
    std::memcpy(event + 4, session.data(), std::min(session.size(), kSessionIdChars));
    if (!invitation.empty()) {
        std::memcpy(event + 0x34, invitation.data(), std::min(invitation.size(), kInvitationIdChars));
        Wr<uint32_t>(event, 0x74, 1);
    }
    Wr<int32_t>(event, 0x90, kInitialUser);
    if (!runtime_system_event_push(event, sizeof(event))) std::printf("Online: the system event queue is full\n");
}

struct Invitation {
    std::string invitation, session, name, build;
};

// ---- the Invitation dialog and invitation prompts (one overlay list at a time) ----
struct Dialog {
    int status = kStatusNone;
    int mode = 0;
    std::string session, message;
    std::vector<Json> choices; // friends (send) or invitations (receive), in the order shown
    bool showing = false, cancelled = false;
    int32_t error = 0;
    uint32_t sent = 0;
};

std::mutex g_dialog_mutex;
Dialog g_dialog;
std::deque<Invitation> g_incoming;
std::optional<Invitation> g_prompt;
std::optional<Invitation> g_pending_join;

std::string FriendLabel(const Json& f) {
    const std::string name = f["display_name"].as_string().empty() ? f["name"].as_string() : f["display_name"].as_string();
    return name + (f["room"].as_u64() ? " \xC2\xB7 in a squad" : " \xC2\xB7 online");
}

// Begins the overlay list; false (and nothing shown) without a window.
bool ShowChoice(const std::string& title, const std::vector<std::string>& labels) {
    std::vector<const char*> items;
    for (const std::string& l : labels) items.push_back(l.c_str());
    return alngpu_choice_begin(title.c_str(), items.data(), nullptr, int(items.size()), 0) != 0;
}

void FinishDialog(Dialog& d, bool cancelled, int32_t error = 0, uint32_t sent = 0) {
    d.status = kStatusFinished;
    d.showing = false;
    d.cancelled = cancelled;
    d.error = error;
    d.sent = sent;
}

ALN_SYSV int32_t invitation_initialize() {
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    if (g_dialog.status != kStatusNone) return kDialogBusy;
    g_dialog = Dialog{};
    g_dialog.status = kStatusInitialized;
    return 0;
}

ALN_SYSV int32_t invitation_terminate() {
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    if (g_dialog.status == kStatusNone) return kDialogNotInitialized;
    g_dialog = Dialog{};
    return 0;
}

// Open param (0x90): mode u32 @0x34 (1 send, 2 the received list), data @0x48: send {message @0, session id buffer @8,
// max invitations u32 @0x18}. The lobby is read without the dialog lock, so the pump thread never waits behind it.
ALN_SYSV int32_t invitation_open(const uint8_t* param) {
    if (!param) return kErrInvalid;
    int mode = 0;
    std::string session, message;
    {
        std::lock_guard<std::mutex> lock(g_dialog_mutex);
        Dialog& d = g_dialog;
        if (d.status == kStatusNone) return kDialogNotInitialized;
        if (d.status == kStatusRunning || g_prompt) return kDialogBusy;
        d = Dialog{};
        d.status = kStatusRunning;
        d.mode = mode = int(Rd<uint32_t>(param, 0x34));
        if (mode == 1) {
            const uint8_t* data = Rd<const uint8_t*>(param, 0x48);
            d.session = session = data ? Text(Rd<const char*>(data, 8), kSessionIdChars) : "";
            d.message = message = data ? Text(Rd<const char*>(data, 0), 512) : "";
        }
    }
    std::vector<Json> choices;
    std::vector<std::string> labels;
    std::string title;
    bool failed = false;
    if (mode == 1) {
        Json friends;
        failed = session.empty() || !Friends(friends);
        const FriendCounts counts = CountFriends(friends, own_build());
        choices = counts.listed;
        for (const Json& f : choices) labels.push_back(FriendLabel(f));
        title = "Invite a friend to your squad\n" + FriendSummary(counts);
        if (labels.empty() && !failed) labels.push_back(NoFriendToInvite(counts, !counts.total && FriendRequestsWaiting()));
        if (!failed) {
            std::string names;
            for (const Json& f : choices) names += (names.empty() ? "" : ", ") + f["name"].as_string();
            std::printf("Friend: Invite dialog: %s; %s %s\n", FriendSummary(counts).c_str(), names.empty() ? "shows" : "lists",
                        names.empty() ? labels[0].c_str() : names.c_str());
        }
    } else {
        Json reply;
        std::string code;
        failed = !lobby_call("invite.list", Json::Object(), &reply, &code, 5000);
        for (const Json& i : reply["invitations"].items()) {
            if (!i["joinable"].as_bool()) continue;
            choices.push_back(i);
            labels.push_back(i["name"].as_string() + " \xC2\xB7 " + std::to_string(i["players"].as_int()) + "/" +
                             std::to_string(i["slots"].as_int()) + " players");
        }
        title = "Squad invitations";
        if (labels.empty()) labels.push_back("No squad invitations");
    }
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    Dialog& d = g_dialog;
    if (d.status != kStatusRunning) return 0;
    if (failed) {
        std::printf("Friend: the Invitation dialog has no list from the online service\n");
        FinishDialog(d, false, kErrInvalid);
        return 0;
    }
    d.choices = std::move(choices);
    d.showing = ShowChoice(title, labels);
    if (!d.showing) FinishDialog(d, true);
    return 0;
}

struct Outcome {
    bool cancelled = false;
    int32_t error = 0;
    uint32_t sent = 0;
};

Outcome Picked(int mode, const std::string& session, const std::string& message, const Json& choice) {
    if (mode == 1) {
        Json send = Json::Object();
        send.set("session", session);
        Json to = Json::Array();
        to.push(choice["account"].as_string());
        send.set("to", std::move(to));
        if (!message.empty()) send.set("message", message);
        std::string code;
        const bool ok = lobby_call("invite.send", send, nullptr, &code, 5000);
        if (ok) std::printf("Friend: invite sent to %s\n", choice["name"].as_string().c_str());
        else std::printf("Friend: invite to %s refused (%s)\n", choice["name"].as_string().c_str(), code.c_str());
        return {false, ok ? 0 : kErrInvalid, ok ? 1u : 0u};
    }
    Json answer = Json::Object();
    answer.set("invitation", choice["invitation"].as_string());
    answer.set("accept", true);
    Json reply;
    std::string code;
    if (!lobby_call("invite.answer", answer, &reply, &code, 5000)) {
        std::printf("Friend: invitation could not be accepted (%s)\n", code.c_str());
        return {false, kErrInvalid, 0};
    }
    std::printf("Friend: invite accepted (from %s)\n", choice["name"].as_string().c_str());
    PushSessionEvent(reply["session"].as_string(), choice["invitation"].as_string());
    return {};
}

ALN_SYSV int32_t invitation_update_status() {
    int mode = 0;
    std::string session, message;
    Json choice;
    {
        std::lock_guard<std::mutex> lock(g_dialog_mutex);
        Dialog& d = g_dialog;
        if (d.status != kStatusRunning || !d.showing) return d.status;
        const int picked = alngpu_choice_poll();
        if (picked == -1) return d.status;
        if (picked < 0 || size_t(picked) >= d.choices.size()) {
            FinishDialog(d, true);
            return d.status;
        }
        d.showing = false;
        mode = d.mode, session = d.session, message = d.message, choice = d.choices[size_t(picked)];
    }
    const Outcome outcome = Picked(mode, session, message, choice);
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    if (g_dialog.status == kStatusRunning) FinishDialog(g_dialog, outcome.cancelled, outcome.error, outcome.sent);
    return g_dialog.status;
}

// Result: error u32 @8 (0 ok), cancelled u32 @0xc, and through the pointer @0x10 the number of invitations sent.
ALN_SYSV int32_t invitation_get_result(uint8_t* result) {
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    const Dialog& d = g_dialog;
    if (d.status != kStatusFinished) return kDialogNotFinished;
    if (!result) return kErrInvalid;
    Wr<uint32_t>(result, 8, d.error ? 1u : 0u);
    Wr<uint32_t>(result, 0xc, d.cancelled ? 1u : 0u);
    if (uint8_t* count = Rd<uint8_t*>(result, 0x10)) Wr<uint32_t>(count, 0, d.sent);
    return 0;
}

ALN_SYSV int32_t invitation_close() {
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    if (g_dialog.status != kStatusRunning) return kDialogNotRunning;
    FinishDialog(g_dialog, true);
    return 0;
}

// An invitation that arrived while playing: one prompt at a time, never over another overlay list or the menu.
void PromptNext() {
    if (g_prompt || g_incoming.empty() || g_dialog.status == kStatusRunning || alngpu_overlay_captures_input()) return;
    Invitation next = g_incoming.front();
    g_incoming.pop_front();
    if (!ShowChoice(next.name + " invites you to their squad", {"Join squad", "Decline"})) {
        std::printf("Friend: invitation from %s left for the launcher (no window)\n", next.name.c_str());
        return;
    }
    g_prompt = std::move(next);
}

void AnswerPrompt(const Invitation& invitation, int choice) {
    if (choice == -2) return; // left pending, as "not now"
    Json answer = Json::Object();
    answer.set("invitation", invitation.invitation);
    answer.set("accept", choice == 0);
    if (choice != 0) {
        lobby_send("invite.answer", std::move(answer));
        std::printf("Friend: invitation from %s declined\n", invitation.name.c_str());
        return;
    }
    Json reply;
    std::string code;
    if (!lobby_call("invite.answer", answer, &reply, &code, 5000)) {
        std::printf("Friend: invitation from %s could not be accepted (%s)\n", invitation.name.c_str(), code.c_str());
        return;
    }
    std::printf("Friend: invite accepted (from %s); the game joins the squad\n", invitation.name.c_str());
    PushSessionEvent(invitation.session, invitation.invitation);
}

#define WEB(name, fn) {name, reinterpret_cast<void*>(&fn)}
const struct {
    const char* name;
    void* function;
} kExports[] = {
    WEB("sceNpWebApiInitialize", web_initialize),
    WEB("sceNpWebApiTerminate", web_ok_id),
    WEB("sceNpWebApiCreateContext", web_create_context),
    WEB("sceNpWebApiDeleteContext", web_ok_id),
    WEB("sceNpWebApiCreateRequest", web_create_request),
    WEB("sceNpWebApiSendRequest2", web_send_request),
    WEB("sceNpWebApiReadData", web_read_data),
    WEB("sceNpWebApiDeleteRequest", web_delete_request),
    WEB("sceNpWebApiAbortRequest", web_abort_request),
    WEB("sceNpWebApiCreateHandle", web_create_handle),
    WEB("sceNpWebApiDeleteHandle", web_ok_pair),
    WEB("sceNpWebApiCreatePushEventFilter", web_push_filter),
    WEB("sceNpWebApiDeletePushEventFilter", web_ok_pair),
    WEB("sceNpWebApiCreateServicePushEventFilter", web_service_push_filter),
    WEB("sceNpWebApiDeleteServicePushEventFilter", web_ok_pair),
    WEB("sceNpWebApiRegisterPushEventCallback", web_register_callback),
    WEB("sceNpWebApiUnregisterPushEventCallback", web_ok_pair),
    WEB("sceNpWebApiRegisterServicePushEventCallback", web_register_callback),
    WEB("sceNpWebApiUnregisterServicePushEventCallback", web_ok_pair),
    WEB("sceNpWebApiUtilityParseNpId", web_parse_np_id),
    WEB("sceInvitationDialogInitialize", invitation_initialize),
    WEB("sceInvitationDialogTerminate", invitation_terminate),
    WEB("sceInvitationDialogOpen", invitation_open),
    WEB("sceInvitationDialogUpdateStatus", invitation_update_status),
    WEB("sceInvitationDialogGetResult", invitation_get_result),
    WEB("sceInvitationDialogClose", invitation_close),
};
#undef WEB

} // namespace

uintptr_t webapi_export(const std::string& symbol) {
    for (const auto& e : kExports)
        if (symbol == e.name) return reinterpret_cast<uintptr_t>(e.function);
    return 0;
}

void sessions_event(const std::string& event, const Json& body) {
    if (event.rfind("friend.", 0) == 0) {
        {
            std::lock_guard<std::mutex> lock(g_friends_mutex);
            g_friends_ms = 0;
        }
        const std::string who = body["friend"]["name"].as_string().empty() ? body["name"].as_string() : body["friend"]["name"].as_string();
        if (event == "friend.request") std::printf("Friend: friend request from %s (accept it in the launcher's Friends tab)\n", who.c_str());
        else if (event == "friend.added") std::printf("Friend: %s is now your friend\n", who.c_str());
        else if (event == "friend.removed") std::printf("Friend: a friend removed you\n");
        else if (event == "friend.online" && !who.empty()) std::printf("Friend: %s is online\n", who.c_str());
        return;
    }
    const std::string name = body["display_name"].as_string().empty() ? body["name"].as_string() : body["display_name"].as_string();
    if (event == "invite.answered") {
        std::printf("Friend: %s %s your invitation\n", name.c_str(), body["accept"].as_bool() ? "accepted" : "declined");
        return;
    }
    if (event != "invite.received") return;
    Invitation invitation{body["invitation"].as_string(), body["session"].as_string(), name, body["build"].as_string()};
    if (invitation.build != own_build()) {
        std::printf("Friend: invitation from %s to a squad of game build %s ignored (this game is %s)\n", name.c_str(),
                    invitation.build.c_str(), own_build().c_str());
        return;
    }
    std::printf("Friend: %s invited you to their squad\n", name.c_str());
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    g_incoming.push_back(std::move(invitation));
}

void sessions_pump() {
    std::optional<Invitation> join, answered;
    int choice = -1;
    {
        std::lock_guard<std::mutex> lock(g_dialog_mutex);
        join.swap(g_pending_join);
        if (g_prompt && (choice = alngpu_choice_poll()) != -1) answered.swap(g_prompt);
    }
    if (join) {
        std::printf("Online: joining the squad chosen in the launcher (%s)\n",
                    join->invitation.empty() ? "Join" : "invitation accepted");
        PushSessionEvent(join->session, join->invitation);
    }
    if (answered) AnswerPrompt(*answered, choice);
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    PromptNext();
}

void load_pending_join(const std::string& user_dir) {
    const std::filesystem::path file = std::filesystem::path(user_dir) / net::kPendingJoinFile;
    std::ifstream in(file);
    if (!in) return;
    Invitation join;
    int64_t written = 0;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "session") join.session = value.substr(0, kSessionIdChars);
        if (key == "invitation") join.invitation = value.substr(0, kInvitationIdChars);
        if (key == "time") written = std::atoll(value.c_str());
    }
    in.close();
    std::error_code ec;
    std::filesystem::remove(file, ec);
    const int64_t age = int64_t(std::time(nullptr)) - written;
    if (join.session.empty() || age < 0 || age > kPendingJoinSeconds) {
        std::printf("Online: a Join from the launcher was too old and is dropped\n");
        return;
    }
    std::lock_guard<std::mutex> lock(g_dialog_mutex);
    g_pending_join = std::move(join);
}

} // namespace aln::online
