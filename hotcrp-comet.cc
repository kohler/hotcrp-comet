// -*- mode: c++; c-basic-offset: 4 -*-
#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include "cotamer/http_fields.hh"
#include <charconv>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unordered_map>
#include <deque>
#include <format>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <climits>
#include <cmath>
#include <optional>
#include <cctype>
#include <sstream>
#include <pwd.h>
#include <grp.h>
#include <nlohmann/json.hpp>
#include "clp.h"
#if __linux__
# include <sys/inotify.h>
#endif

namespace cot = cotamer;
using namespace std::chrono_literals;
// `ordered_json` keeps object members in insertion order
using json = nlohmann::ordered_json;

#define LOG_ERROR 0
#define LOG_NORMAL 1
#define LOG_VERBOSE 2
#define LOG_DEBUG 3

#define MAX_LOGLEVEL LOG_DEBUG
#define MAX_NPOLLFDS 5

static constexpr cot::duration connection_timeout = 20s;
static constexpr cot::duration keepalive_timeout = 5s;
static constexpr cot::duration backend_timeout = 120s;
static constexpr unsigned site_validate_timeout = 120;
static constexpr unsigned site_error_validate_timeout = 1;
static constexpr double site_min_validate_interval = 1;
static constexpr double user_validate_timeout = 3600;
static double min_poll_timeout = 240;
static double max_poll_timeout = 300;
static int verify_port = 0;
static unsigned nconnections;
static unsigned connection_limit;
static unsigned npolls;
static int pidfd = -1;
static std::ostream* logs;
static std::ostream* logerrs;
static int loglevel;
static std::vector<std::string> startup_fds;
static std::string update_token;
static bool verbose;
static bool track_users;
static bool got_sigterm;
using eventid_t = uint64_t;

#define TIMESTAMP_FMT "%Y-%m-%d %H:%M:%S %z"

// Current time in seconds since the epoch
static double drecent() {
    return std::chrono::duration<double>(cot::now().time_since_epoch()).count();
}

static cot::duration seconds_duration(double s) {
    return std::chrono::duration_cast<cot::duration>(std::chrono::duration<double>(s));
}


// JSON helpers

// Parse `str`, returning null if it isn't valid JSON.
static json parse_json(std::string_view str) {
    json j = json::parse(str, nullptr, false);
    return j.is_discarded() ? json() : j;
}

// Return member `key` of `j`, or null if `j` isn't an object or lacks `key`.
static const json& member(const json& j, const char* key) {
    static const json null_json;
    if (j.is_object()) {
        auto it = j.find(key);
        if (it != j.end()) {
            return *it;
        }
    }
    return null_json;
}

// JavaScript truthiness: null, false, 0, NaN, and "" are falsy.
static bool truthy(const json& j) {
    switch (j.type()) {
    case json::value_t::boolean:
        return j.get<bool>();
    case json::value_t::number_integer:
        return j.get<int64_t>() != 0;
    case json::value_t::number_unsigned:
        return j.get<uint64_t>() != 0;
    case json::value_t::number_float: {
        double d = j.get<double>();
        return d == d && d != 0;
    }
    case json::value_t::string:
        return !j.get_ref<const std::string&>().empty();
    case json::value_t::object:
    case json::value_t::array:
    case json::value_t::binary:
        return true;
    default:
        return false;
    }
}

static std::string json_string(const json& j) {
    return j.is_string() ? j.get<std::string>() : std::string();
}

// Numbers that print like the old JSON library’s `%.12g`: integral values
// print as integers
static json json_number(double d) {
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        return json(int64_t(d));
    }
    return json(d);
}

// The current time, to 0.01 second
static json json_time() {
    return json_number(std::round(drecent() * 100) / 100);
}

// Strings from clients needn't be valid UTF-8; replace invalid bytes rather
// than throwing.
static std::string dump_json(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Unparse `j` with the outer `depth` levels of objects and arrays indented,
// and deeper levels on one line.
static void dump_json_indented(std::string& out, const json& j, int depth, int level = 0) {
    if (level >= depth || !j.is_structured() || j.empty()) {
        out += dump_json(j);
        return;
    }
    bool is_object = j.is_object();
    out += is_object ? "{\n" : "[\n";
    bool first = true;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!first) {
            out += ",\n";
        }
        first = false;
        out.append(2 * (level + 1), ' ');
        if (is_object) {
            out += dump_json(it.key());
            out += ": ";
        }
        dump_json_indented(out, *it, depth, level + 1);
    }
    out += '\n';
    out.append(2 * level, ' ');
    out += is_object ? '}' : ']';
}

// Return the event ID in `j`, if it is a nonnegative integer.
static std::optional<uint64_t> json_eventid(const json& j) {
    if (j.is_number_unsigned()) {
        return j.get<uint64_t>();
    } else if (j.is_number_integer() && j.get<int64_t>() >= 0) {
        return uint64_t(j.get<int64_t>());
    }
    return std::nullopt;
}

// Percent-encode everything but RFC 3986 unreserved characters.
static std::string encode_uri_component(std::string_view s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

class log_msg {
 public:
    log_msg(int msglevel = LOG_NORMAL)
        : msglevel_(msglevel) {
        if (logs && msglevel <= loglevel && msglevel <= MAX_LOGLEVEL) {
            logf_ = logs;
        } else if (msglevel == LOG_ERROR) {
            logf_ = logerrs;
        } else {
            logf_ = nullptr;
        }
        if (logf_) {
            add_prefix();
        }
    }
    ~log_msg() {
        if (logf_) {
            std::string str = stream_.str();
            if (logerrs && msglevel_ == LOG_ERROR) {
                *logerrs << str.substr(str.find(']') + 2) << std::endl;
            }
            if (logf_ && (msglevel_ != LOG_ERROR || logf_ != logerrs)) {
                *logf_ << str << std::endl;
            }
        }
    }
    template <typename T> log_msg& operator<<(T&& x) {
        if (logf_) {
            stream_ << x;
        }
        return *this;
    }
 private:
    int msglevel_;
    std::ostream* logf_;
    std::stringstream stream_;
    void add_prefix();
};

std::string timestamp_string(double at) {
    char buf[1024];
    time_t now = (time_t) at;
    struct tm* t = localtime(&now);
    strftime(buf, sizeof(buf), TIMESTAMP_FMT, t);
    return buf;
}

void log_msg::add_prefix() {
    char buf[1024];
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    strftime(buf, sizeof(buf), "[" TIMESTAMP_FMT "] ", t);
    stream_ << buf;
}


struct http_header {
    std::string name;
    std::string value;
};

// Is `url` a valid absolute URL with a host?
static bool valid_site_url(std::string_view url) {
    cot::strings::url_view u(url);
    return u.ok() && u.is_absolute() && !u.host().empty();
}


// Trigger every event in `v`.
static void trigger_all(std::vector<cot::event>& v) {
    std::vector<cot::event> w;
    w.swap(v);
    for (auto& e : w) {
        e.trigger();
    }
}

// Add `e` to `v`, occasionally dropping events no one waits for anymore.
static void add_waiter(std::vector<cot::event>& v, cot::event e) {
    if (v.size() >= 64 && (v.size() & (v.size() - 1)) == 0) {
        std::erase_if(v, [](const cot::event& x) { return x.empty(); });
    }
    v.push_back(std::move(e));
}


// backend_pool
//    Up to MAX_NPOLLFDS connections to the HotCRP back end. A lease holds
//    one slot and its connection (which may be closed). A lease returns its
//    slot when destroyed, but returns its connection only if `keep()` was
//    called, since an abandoned request leaves a connection mid-response.

class backend_pool {
  public:
    class lease {
      public:
        cot::fd fd;

        lease(backend_pool* pool, cot::fd f)
            : fd(std::move(f)), pool_(pool) {
        }
        lease(lease&& x)
            : fd(std::move(x.fd)), pool_(std::exchange(x.pool_, nullptr)),
              keep_(x.keep_) {
        }
        lease& operator=(lease&&) = delete;
        ~lease() {
            if (pool_) {
                pool_->release(keep_ ? std::move(fd) : cot::fd());
            }
        }
        void keep() {
            keep_ = true;
        }

      private:
        backend_pool* pool_;
        bool keep_ = false;
    };

    explicit backend_pool(size_t n)
        : free_(n) {
    }

    cot::task<lease> acquire();

  private:
    std::deque<cot::fd> free_;
    std::deque<cot::event> waiters_;

    void release(cot::fd f);
    void wake();
};

cot::task<backend_pool::lease> backend_pool::acquire() {
    while (free_.empty()) {
        cot::event e;
        waiters_.push_back(e);
        // If this coroutine is destroyed after `release` woke it, pass the
        // wakeup on.
        struct wake_guard {
            backend_pool* pool;
            cot::event& e;
            bool armed = true;
            ~wake_guard() {
                if (armed && e.triggered()) {
                    pool->wake();
                }
            }
        } guard{this, e};
        co_await e;
        guard.armed = false;
    }
    cot::fd f = std::move(free_.front());
    free_.pop_front();
    co_return lease(this, std::move(f));
}

void backend_pool::release(cot::fd f) {
    free_.push_front(std::move(f));
    wake();
}

void backend_pool::wake() {
    // skip waiters whose coroutines are gone
    while (!waiters_.empty()) {
        cot::event e = std::move(waiters_.front());
        waiters_.pop_front();
        if (!e.idle()) {
            e.trigger();
            break;
        }
    }
}

// Lives until exit (no static destructor runs after the driver is gone)
static backend_pool& pollfds = *new backend_pool(MAX_NPOLLFDS);


class Site {
  public:
    explicit Site(std::string url)
        : url_(std::move(url)), created_at_(drecent()) {
        if (url_.back() != '/') {
            url_ += '/';
        }
        cot::strings::url_view u(url_);
        host_ = u.host_port();
        port_ = 80;
        std::from_chars(u.port().data(), u.port().data() + u.port().size(), port_);
        path_ = u.path().empty() ? "/" : u.path();
    }

    inline const std::string& url() const {
        return url_;
    }
    inline const std::string& status() const {
        return status_;
    }
    inline eventid_t eventid() const {
        return eventid_;
    }
    inline bool is_valid() const;
    inline bool may_validate() const;
    inline bool failing() const {
        return validate_fail_ > 0;
    }

    enum source_type {
        source_validate = 0,
        source_update = 1,
        source_filesystem = 2
    };
    bool update(const json& j, source_type source, eventid_t old_eventid);

    std::string make_api_path(std::string fn, std::string rest = std::string());

    void validate(cot::event done);
    cot::task<std::pair<bool, std::string>> check_user(std::vector<http_header> cookies);

    void wait(eventid_t eventid, cot::event done) {
        if (is_valid()) {
            if (eventid != eventid_) {
                done.trigger();
            } else {
                add_waiter(status_change_, std::move(done));
            }
        } else if (may_validate()) {
            validate(std::move(done));
        } else {
            // backing off a failing site: wait for an update (or timeout)
            add_waiter(status_change_, std::move(done));
        }
    }

    void add_poller() {
        ++npoll_;
        ++npollers_;
    }
    void resolve_poller(bool blocked) {
        --npollers_;
        if (!blocked) {
            ++npoll_noblock_;
            ++recent_poll_noblock_;
        }
    }

    json status_json() const;

  private:
    std::string url_;
    std::string host_;
    uint16_t port_;
    std::string path_;
    std::string status_;
    eventid_t eventid_ = 0;
    double created_at_;
    double eventid_at_ = 0.0;
    double validate_at_ = 0.0;
    bool validating_ = false;
    int validate_fail_ = 0;
    std::vector<cot::event> validate_waiters_;
    std::vector<cot::event> status_change_;
    unsigned long long npoll_ = 0;
    unsigned long long npoll_noblock_ = 0;
    unsigned recent_poll_noblock_ = 0;
    unsigned long long nupdate_[3] = {0, 0, 0};
    unsigned long long nupfail_[3] = {0, 0, 0};
    unsigned long long nbadreq_ = 0;
    unsigned npollers_ = 0;

    cot::task<> run_validate();
    cot::task<json> send(std::string path,
                         std::vector<http_header> headers = {});
};

// Sites are never erased, so references to them stay valid.
typedef std::unordered_map<std::string, Site> site_map_type;
static site_map_type& sites = *new site_map_type;

Site& make_site(const std::string& url) {
    auto it = sites.find(url);
    if (it == sites.end()) {
        it = sites.insert(std::make_pair(url, Site(url))).first;
    }
    return it->second;
}

std::string Site::make_api_path(std::string fn, std::string rest) {
    std::string path = path_ + "api.php/" + fn;
    if (!update_token.empty()) {
        if (!rest.empty()) {
            rest += "&";
        }
        rest += "token=" + encode_uri_component(update_token);
    }
    if (!rest.empty()) {
        path += "?" + rest;
    }
    return path;
}

static void set_age_minutes(json& j, const char* name, double dt) {
    if (dt) {
        j[name] = json_number(round((drecent() - dt) / 6) / 10);
    }
}

json Site::status_json() const {
    json j = json::object();
    j["site"] = url_;
    j["eventid"] = eventid_;
    set_age_minutes(j, "age_min", created_at_);
    set_age_minutes(j, "event_age_min", eventid_at_);
    set_age_minutes(j, "validate_age_min", validate_at_);
    j["npoll"] = npoll_;
    j["nupdate"] = nupdate_[0] + nupdate_[1] + nupdate_[2];
    if (unsigned long long nupfail = nupfail_[0] + nupfail_[1] + nupfail_[2]) {
        j["nupdate_fail"] = nupfail;
    }
    if (nbadreq_ > 0) {
        j["nbad_request"] = nbadreq_;
    }
    j["npollers"] = npollers_;
    if (validating_) {
        j["validating"] = true;
    }
    return j;
}

inline bool Site::is_valid() const {
    if (!validate_at_ || validate_fail_ > 0) {
        return false;
    }
    double elapsed = drecent() - validate_at_;
    if (recent_poll_noblock_ > 20) {
        // pollers keep missing updates: revalidate, but not too often
        return elapsed < site_min_validate_interval;
    }
    return elapsed < site_validate_timeout;
}

// A failing site revalidates only after an exponential backoff, so that a
// broken back end isn't hammered by every poll.
inline bool Site::may_validate() const {
    return validating_
        || validate_fail_ == 0
        || drecent() - validate_at_
           >= (site_error_validate_timeout << (validate_fail_ - 1));
}

bool Site::update(const json& j, source_type source, eventid_t prev_eventid) {
    // Local update files are trusted (HotCRP doesn't write a token in them)
    auto jeventid = json_eventid(member(j, "tracker_eventid"));
    if (!j.is_object()
        || !truthy(member(j, "ok"))
        || (!update_token.empty()
            && source != source_filesystem
            && member(j, "token") != update_token)
        || !jeventid) {
        ++nupfail_[source];
        if (source == source_validate) {
            if (validate_fail_ == 0) {
                log_msg() << "! " << url_ << " validation failed";
            }
            validate_fail_ = std::min(validate_fail_ + 1, 7);
        }
        return false;
    }

    eventid_t eventid = *jeventid;
    if (eventid < eventid_
        && (source != source_validate || eventid_ != prev_eventid)) {
        return false;
    }
    validate_fail_ = 0;

    if (eventid != eventid_) {
        eventid_ = eventid;
        status_ = json_string(member(j, "tracker_status"));
        eventid_at_ = drecent();
        ++nupdate_[source];
        trigger_all(status_change_);
        log_msg() << "* " << url_ << " #" << eventid_;
    }

    recent_poll_noblock_ = 0;
    return true;
}

static std::string trim_body(const cot::http_message& msg) {
    std::string body = msg.body();
    if (body.empty()) {
        body = "<empty>";
    } else {
        body.erase(body.find_last_not_of(" \r\n\t\v\f") + 1);
    }
    return body;
}

cot::task<json> Site::send(std::string path, std::vector<http_header> headers) {
    // get a polling file descriptor
    auto lease = co_await pollfds.acquire();
    bool opened_pollfd = false;

    while (true) {
        if (!lease.fd) {
            opened_pollfd = true;
            try {
                lease.fd = co_await cot::tcp_connect(std::format(
                    "127.0.0.1:{}", verify_port > 0 ? verify_port : port_));
            } catch (const std::system_error& e) {
                log_msg() << host_ << path << ": error " << e.what();
                co_return json();
            }
            log_msg(LOG_DEBUG) << "fd " << lease.fd.fileno() << ": pollfd";
        }

        cot::http_parser hp(lease.fd, cot::http_parser::client);
        hp.set_receive_timeout(backend_timeout);
        cot::http_message res;
        try {
            cot::http_message req(HTTP_GET, path);
            req.header("Host", host_)
                .header("Connection", "keep-alive");
            for (auto& hdr : headers) {
                req.header(hdr.name, hdr.value);
            }
            auto ticket = co_await hp.send_request(std::move(req));
            res = co_await hp.receive(std::move(ticket));
        } catch (const std::invalid_argument& e) {
            // a bad header or URL in the request
            ++nbadreq_;
            log_msg() << host_ << path << ": bad request: " << e.what();
            lease.keep();
            co_return json();
        }

        // parse response
        json j;
        log_msg(LOG_VERBOSE) << "> GET http://" << host_ << path << " -> " << res.status_code();
        if (hp.ok() && res.ok()) {
            j = parse_json(res.body());
        }
        if (j.is_object() && !update_token.empty()) {
            j["token"] = update_token; // always trust response
        }
        if (!(j.is_object()
              && truthy(member(j, "ok"))
              && (update_token.empty() || member(j, "token") == update_token))) {
            int fdnum = lease.fd.fileno();
            lease.fd.close();
            log_msg(LOG_DEBUG) << "fd " << fdnum << ": close " << trim_body(res);
            if (!opened_pollfd) {
                // the pooled connection may have gone stale; retry once
                continue;
            }
            if (!hp.ok()) {
                log_msg() << host_ << path << ": error " << hp.error_name();
            } else {
                log_msg() << host_ << path << ": bad tracker status " << trim_body(res);
            }
        }
        if (!hp.should_keep_alive() && lease.fd) {
            int fdnum = lease.fd.fileno();
            lease.fd.close();
            log_msg(LOG_DEBUG) << "fd " << fdnum << ": close";
        }
        lease.keep();
        co_return j;
    }
}

void Site::validate(cot::event done) {
    // is status already being checked?
    validate_waiters_.push_back(std::move(done));
    if (!validating_) {
        validating_ = true;
        run_validate().detach();
    }
}

cot::task<> Site::run_validate() {
    eventid_t prev_eventid = eventid_;
    json j = co_await send(make_api_path("trackerstatus"));
    update(j, source_validate, prev_eventid);
    validate_at_ = drecent();
    validating_ = false;
    trigger_all(validate_waiters_);
}

cot::task<std::pair<bool, std::string>> Site::check_user(std::vector<http_header> cookies) {
    json j = co_await send(make_api_path("whoami"), std::move(cookies));
    if (truthy(member(j, "ok"))) {
        co_return {true, json_string(member(j, "email"))};
    } else {
        co_return {false, std::string()};
    }
}

bool check_conference(const std::string& arg) {
    return valid_site_url(arg);
}


struct user_map {
    double timestamp = 0.0;
    double used_at = 0.0;
    char firstch = 0;
    bool locked = true;
    unsigned long nused = 0;
    std::vector<http_header> cookies;
    std::string user;

    bool cookies_equal(const std::vector<http_header>& xcookies) const {
        if (cookies.size() != xcookies.size()) {
            return false;
        }
        auto i1 = cookies.begin();
        auto i2 = xcookies.begin();
        auto last = cookies.end();
        while (i1 != last && i1->name == i2->name && i1->value == i2->value) {
            ++i1;
            ++i2;
        }
        return i1 == last;
    }
};

static std::vector<user_map> users;
static constexpr size_t max_users_size = 2048;

cot::task<std::string> lookup_user(Site& site, std::vector<http_header> cookies) {
    // no cookies -> no user
    if (cookies.empty()) {
        co_return "";
    }

    // search for match
    char firstch = 0;
    size_t oldi = -1;
    double now = drecent();
    {
        size_t pos = cookies[0].value.find('=');
        if (pos < cookies[0].value.length()) {
            firstch = cookies[0].value[pos + 1];
        }

        double oldt = now;
        double expiry = now - user_validate_timeout;
        for (size_t i = 0; i != users.size(); ++i) {
            if (users[i].firstch == firstch
                && users[i].timestamp > expiry
                && users[i].cookies_equal(cookies)) {
                users[i].used_at = now;
                ++users[i].nused;
                co_return users[i].user;
            }
            if (users[i].used_at < oldt
                && !users[i].locked) {
                oldi = i;
                oldt = users[i].used_at;
            }
        }

        if (users.size() == max_users_size
            && oldi == (size_t) -1) {
            co_return "";
        }

        if (oldi == (size_t) -1
            || (users[oldi].used_at >= expiry && users.size() < max_users_size)) {
            users.emplace_back();
            oldi = users.size() - 1;
        }

        users[oldi].locked = true;
        users[oldi].timestamp = users[oldi].used_at = now;
        users[oldi].firstch = firstch;
        users[oldi].nused = 0;
        std::swap(users[oldi].cookies, cookies);
    }

    // `users` may grow while we wait, so refer to the entry by index. If this
    // coroutine is destroyed first, the entry must not keep a stale user.
    struct entry_guard {
        size_t i;
        bool done = false;
        ~entry_guard() {
            users[i].locked = false;
            if (!done) {
                users[i].user.clear();
                users[i].timestamp = 0;
            }
        }
    } guard{oldi};
    auto result = co_await site.check_user(users[oldi].cookies);
    users[oldi].user = result.second;
    guard.done = true;
    co_return std::move(result.second);
}


class Connection {
 public:
    static Connection* add(cot::fd cfd, bool allow_status);
    static std::vector<Connection*> all;
    static void clear_one(Connection* dont_clear);

    json status_json() const;
    double age() const {
        return drecent() - created_at_;
    }

  private:
    enum state { s_request, s_poll, s_response };

    cot::fd cfd_;
    int fdnum_;
    cot::http_parser hp_;
    cot::http_message req_;
    std::string recent_user_;
    std::string recent_site_;
    json resj_;
    int resj_indent_;
    bool allow_status_;
    double created_at_;
    cot::event poll_event_;
    cot::task<> user_task_;
    bool registered_ = false;   // in `all` and `nconnections`
    int state_;
    Connection* prev_;
    Connection* next_;

    static Connection* state_head[3];
    void set_state(int state);
    void unregister();

    Connection(cot::fd cfd, bool allow_status);
    ~Connection();
    cot::task<> handler();
    cot::task<> serve();
    cot::task<> poll_handler(double timeout);
    void update_handler();
    cot::task<> check_user();
};

std::vector<Connection*> Connection::all;
Connection* Connection::state_head[3];

Connection::Connection(cot::fd cfd, bool allow_status)
    : cfd_(std::move(cfd)), fdnum_(cfd_.fileno()),
      hp_(cfd_, cot::http_parser::server),
      allow_status_(allow_status), created_at_(drecent()), state_(-1) {
    assert(cfd_.valid());
    log_msg(LOG_DEBUG) << "fd " << fdnum_ << ": connection";
    if (all.size() <= (unsigned) fdnum_) {
        all.resize(fdnum_ + 1, nullptr);
    }
    assert(!all[fdnum_]);
    all[fdnum_] = this;
    ++nconnections;
    registered_ = true;
}

// Forget the connection's fd number, which a new connection may reuse once
// `cfd_` is closed.
void Connection::unregister() {
    if (registered_) {
        log_msg(LOG_DEBUG) << "fd " << fdnum_ << ": close";
        all[fdnum_] = nullptr;
        --nconnections;
        registered_ = false;
    }
}

Connection* Connection::add(cot::fd cfd, bool allow_status) {
    Connection* c = new Connection(std::move(cfd), allow_status);
    c->handler().detach();
    return c;
}

Connection::~Connection() {
    unregister();
    cfd_.close();
    set_state(-1);
}

void Connection::set_state(int state) {
    assert(state >= -1 && state < 3);
    if (state != state_ && state_ != -1) {
        prev_->next_ = next_;
        next_->prev_ = prev_;
        if (state_head[state_] == this && next_ == this) {
            state_head[state_] = nullptr;
        } else if (state_head[state_] == this) {
            state_head[state_] = next_;
        }
    }
    if (state != state_ && state != -1) {
        if (state_head[state]) {
            prev_ = state_head[state]->prev_;
            next_ = state_head[state];
        } else {
            prev_ = next_ = this;
        }
        prev_->next_ = next_->prev_ = state_head[state] = this;
    }
    state_ = state;
}

void Connection::clear_one(Connection* dont_clear) {
    Connection* c = nullptr;
    if (state_head[s_request]) {
        c = state_head[s_request]->prev_;
    }
    if ((!c || c == dont_clear || c->age() < 1)
        && state_head[s_poll]) {
        c = state_head[s_poll]->prev_;
    }
    if (!c || c == dont_clear) {
        // do nothing
    } else if (c->state_ == s_request) {
        // Closing the fd makes the handler's `receive()` fail, and the
        // handler then deletes the connection.
        log_msg() << "premature delete " << c->status_json();
        c->set_state(-1);
        c->unregister();
        c->cfd_.close();
    } else if (c->state_ == s_poll) {
        log_msg() << "premature close " << c->status_json();
        c->set_state(s_response);
        c->hp_.clear_should_keep_alive();
        c->poll_event_.trigger();
    }
}

static eventid_t parse_poll_status(std::string_view str) {
    eventid_t result = 0;
    const char* s = str.data();
    const char* ends = s + str.length();
    while (s != ends
           && isdigit((unsigned char) *s)
           && result < (eventid_t) 0x1000'0000'0000'0000ULL) {
        result = 10 * result + *s - '0';
        ++s;
    }
    return s == ends && !str.empty() ? result : (eventid_t) -1;
}

json Connection::status_json() const {
    static const char* stati[] = {"request", "poll", "response"};
    json j = json::object();
    j["fd"] = fdnum_;
    j["age"] = (unsigned long) (drecent() - created_at_);
    if (state_ >= 0) {
        j["state"] = stati[state_];
    }
    if (!recent_site_.empty()) {
        j["site"] = recent_site_;
    }
    if (state_ == s_poll && !req_.search_param("poll").empty()) {
        std::string poll_status(req_.search_param("poll"));
        auto ipoll = parse_poll_status(poll_status);
        if (ipoll != (eventid_t) -1) {
            j["eventid"] = ipoll;
        } else {
            j["eventid"] = poll_status;
        }
    }
    if (state_ == s_poll && !req_.search_param("uuid").empty()) {
        j["uuid"] = std::string(req_.search_param("uuid"));
    }
    if (!recent_user_.empty()) {
        j["user"] = recent_user_;
    }
    return j;
}

double poll_timeout(double timeout) {
    if (timeout <= 0 || timeout > min_poll_timeout) {
        double t = min_poll_timeout + drand48() * (max_poll_timeout - min_poll_timeout);
        if (timeout <= 0 || timeout > t) {
            timeout = t;
        }
    }
    return timeout;
}

cot::task<> Connection::poll_handler(double timeout) {
    Site& site = make_site(recent_site_);
    eventid_t poll_eventid = 0;
    auto deadline = cot::steady_now() + seconds_duration(poll_timeout(timeout));
    bool blocked = false;
    {
        std::string_view poll_status = req_.search_param("poll");
        eventid_t ipoll = parse_poll_status(poll_status);
        if (ipoll != (eventid_t) -1) {
            poll_eventid = ipoll;
        } else if (poll_status.empty() || site.status() == poll_status) {
            poll_eventid = site.eventid();
        } else {
            poll_eventid = site.eventid() - 1;
        }
    }
    site.add_poller();
    ++npolls;

    while (cfd_
           && cot::steady_now() < deadline
           && state_ == s_poll
           && (site.eventid() == poll_eventid || !site.is_valid())) {
        blocked = true;
        poll_event_ = cot::event();
        site.wait(poll_eventid, poll_event_);
        // stop early if the client goes away
        cot::event closed = cot::closed(cfd_);
        co_await cot::any(poll_event_, closed, cot::at(deadline));
        if (closed.triggered()) {
            break;
        }
    }
    poll_event_ = cot::event();

    if (!site.status().empty() && !site.failing()) {
        resj_["ok"] = true;
        resj_["at"] = json_time();
        resj_["tracker_eventid"] = site.eventid();
    } else {
        resj_["ok"] = false;
    }
    hp_.clear_should_keep_alive();
    site.resolve_poller(blocked);
    --npolls;
}

void Connection::update_handler() {
    json j = json::object();
    j["ok"] = true;
    if (!req_.search_param("tracker_status").empty()) {
        j["tracker_status"] = std::string(req_.search_param("tracker_status"));
    }
    if (!req_.search_param("tracker_eventid").empty()) {
        j["tracker_eventid"] = parse_json(req_.search_param("tracker_eventid"));
    }
    if (!req_.search_param("token").empty()) {
        j["token"] = std::string(req_.search_param("token"));
    }
    Site& site = make_site(recent_site_);
    if (site.update(j, Site::source_update, 0)) {
        resj_["ok"] = true;
    } else {
        resj_["ok"] = false;
        resj_["error"] = "invalid status update";
    }
    hp_.clear_should_keep_alive();
}

static size_t skip_semispace(const char* data, size_t n, size_t len) {
    while (n != len && (data[n] == ';' || isspace((unsigned char) data[n]))) {
        ++n;
    }
    return n;
}

static std::string strip_cookie_value(std::string value) {
    size_t n = 0, len = value.length();
    while (n != len) {
        size_t p = skip_semispace(value.data(), n, len);
        size_t semi = std::min(len, value.find(';', p));
        if ((semi - p > 13 && value.compare(p, 13, "hotlist-info-") == 0)
            || (semi - p > 13 && value.compare(p, 13, "hc-uredirect-") == 0)) {
            if (n == 0) {
                semi = skip_semispace(value.data(), semi, len);
            }
            value.erase(n, semi - n);
            len = value.length();
        } else {
            n = semi;
        }
    }
    return value;
}

cot::task<> Connection::check_user() {
    Site& site = make_site(recent_site_);
    std::vector<http_header> cookies;
    for (auto it = req_.header_begin(); it != req_.header_end(); ++it) {
        if (it.name_ieq("cookie")) {
            std::string value = strip_cookie_value(std::string(it.value()));
            if (!value.empty()) {
                cookies.push_back(http_header{"Cookie", std::move(value)});
            }
        }
    }
    recent_user_ = co_await lookup_user(site, std::move(cookies));
}

static double json_double(const json& j) {
    return j.is_number() ? j.get<double>() : 0.0;
}

json status_handler_user_cache() {
    std::vector<json> usersv;
    for (auto it = users.begin(); it != users.end(); ++it) {
        if (!it->locked) {
            json uj = json::object();
            uj["user"] = it->user;
            std::string cookie;
            for (auto cit = it->cookies.begin(); cit != it->cookies.end(); ++cit) {
                if (cookie.empty()) {
                    cookie = cit->value;
                } else {
                    cookie += "; ";
                    cookie += cit->value;
                }
            }
            uj["cookie"] = cookie;
            set_age_minutes(uj, "age_min", it->timestamp);
            if (it->nused > 0) {
                uj["nused"] = it->nused;
            }
            usersv.push_back(std::move(uj));
        }
    }
    std::sort(usersv.begin(), usersv.end(), [](const json& a, const json& b) {
        return json_double(member(a, "age_min")) < json_double(member(b, "age_min"));
    });
    return json(std::move(usersv));
}

void status_handler(json& resj) {
    resj["at"] = json_time();
    resj["at_time"] = timestamp_string(drecent());
    resj["nconnections"] = nconnections;
    resj["connection_limit"] = connection_limit;

    std::vector<json> sitesv;
    for (auto it = sites.begin(); it != sites.end(); ++it) {
        sitesv.push_back(it->second.status_json());
    }
    std::sort(sitesv.begin(), sitesv.end(), [](const json& a, const json& b) {
        const json& aage = member(a, "event_age_min");
        const json& bage = member(b, "event_age_min");
        bool anull = aage.is_null(), bnull = bage.is_null();
        if (anull && bnull) {
            return json_string(member(a, "site")) < json_string(member(b, "site"));
        } else if (anull || bnull) {
            return bnull;
        } else {
            return json_double(aage) < json_double(bage);
        }
    });
    resj["sites"] = std::move(sitesv);

    std::vector<json> connv;
    for (auto it = Connection::all.begin(); it != Connection::all.end(); ++it) {
        if (*it)
            connv.push_back((*it)->status_json());
    }
    std::sort(connv.begin(), connv.end(), [](const json& a, const json& b) {
        return json_double(member(a, "age")) > json_double(member(b, "age"));
    });
    resj["connections"] = std::move(connv);

    if (!users.empty() && false) {
        json usersj = status_handler_user_cache();
        if (!usersj.empty()) {
            resj["user_cache"] = std::move(usersj);
        }
    }
}

cot::task<> Connection::handler() {
    try {
        co_await serve();
    } catch (const std::exception& e) {
        log_msg() << "fd " << fdnum_ << ": " << e.what();
    }
    delete this;
}

cot::task<> Connection::serve() {
    cot::duration timeout = connection_timeout;
    while (cfd_) {
        set_state(s_request);
        hp_.set_receive_timeout(timeout);
        req_ = co_await hp_.receive();
        if (!hp_.ok() || !req_.ok()) {
            if (!hp_.ok() && hp_.error() != HPE_CLOSED_CONNECTION
                && hp_.error() != HPE_INVALID_EOF_STATE) {
                log_msg(LOG_DEBUG) << "fd " << fdnum_ << ": request fail " << hp_.error_name();
            }
            break;
        }
        if (loglevel >= LOG_VERBOSE) {
            log_msg(LOG_VERBOSE) << "< " << req_.method_name() << " " << req_.url() << " [" << npolls << "]";
        }
        cot::http_message res(200);
        res.header("Access-Control-Allow-Origin", "*")
            .header("Access-Control-Allow-Credentials", "true")
            .header("Access-Control-Allow-Headers", "Accept-Encoding")
            .header("Expires", "Mon, 26 Jul 1997 05:00:00 GMT");
        resj_ = json::object();
        resj_indent_ = 0;
        std::string url_path(req_.path());
        std::string conference(req_.search_param("conference"));
        if (url_path == "/status") {
            if (allow_status_) {
                status_handler(resj_);
            } else {
                resj_["ok"] = true;
            }
            resj_indent_ = 2;
        } else if (check_conference(conference)) {
            recent_site_ = conference;
            if (req_.has_header("cookie")
                && track_users) {
                user_task_ = check_user();
            }
            if (url_path.ends_with("/update")) {
                update_handler();
            } else if (url_path.ends_with("/poll")) {
                set_state(s_poll);
                json j = parse_json(req_.search_param("timeout"));
                co_await poll_handler(j.is_number() ? j.get<double>() / 1000. : 0);
            } else {
                resj_["ok"] = false;
                resj_["error"] = "bad request";
            }
        } else {
            resj_["ok"] = false;
            resj_["error"] = "bad request";
        }
        res.date_header("Date", time(nullptr))
            .header("Content-Type", "application/json");
        bool close = truthy(member(resj_, "ok")) || !hp_.should_keep_alive();
        if (close) {
            res.header("Connection", "close");
        }
        std::string body;
        dump_json_indented(body, resj_, resj_indent_);
        body += '\n';
        res.body(std::move(body));
        set_state(s_response);
        co_await hp_.send_response(std::move(res));
        if (close || !hp_.should_keep_alive()) {
            break;
        }
        timeout = keepalive_timeout;
    }
}

static void record_startup_fd(int fd, const char* msg) {
    std::ostringstream buf;
    buf << "fd " << fd << ": " << msg;
    startup_fds.push_back(buf.str());
}

cot::task<> listener(cot::fd sfd, bool allow_status) {
    while (sfd) {
        cot::fd cfd;
        bool failed = false;
        try {
            cfd = co_await cot::tcp_accept(sfd);
        } catch (const std::system_error& e) {
            log_msg() << "accept: " << e.what();
            failed = true;
        }
        if (failed) {
            co_await cot::after(1s);
            continue;
        }
        if (cfd) {
            Connection* c = Connection::add(std::move(cfd), allow_status);
            if (nconnections > connection_limit) {
                Connection::clear_one(c);
            }
        }
    }
}

#if __linux__
typedef struct inotify_event ievent;

struct directory_watch {
    int inotify_fd;
    int dirfd;
};

// Opened before forking, so that errors are reported to the caller.
static directory_watch open_directory_watch(const char* update_directory) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        log_msg(LOG_ERROR) << "inotify_init: " << strerror(errno);
        exit(1);
    }
    int dirfd = open(update_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) {
        log_msg(LOG_ERROR) << update_directory << ": " << strerror(errno);
        exit(1);
    }
    if (inotify_add_watch(fd, update_directory, IN_CLOSE_WRITE) == -1) {
        log_msg(LOG_ERROR) << update_directory << ": " << strerror(errno);
        exit(1);
    }
    record_startup_fd(fd, "inotify");
    record_startup_fd(dirfd, "watch directory");
    return {fd, dirfd};
}

cot::task<> directory_watcher(const char* update_directory, directory_watch dw) {
    cot::fd watchfd(dw.inotify_fd);
    alignas(ievent) char buf[sizeof(ievent) + 1 + NAME_MAX];
    while (watchfd) {
        auto r = co_await cot::read_once(watchfd, buf, sizeof(buf));
        if (!r) {
            log_msg() << update_directory << ": " << r.error().message();
            break;
        }
        for (size_t off = 0; off < *r; ) {
            ievent* x = reinterpret_cast<ievent*>(&buf[off]);
            int ffd = openat(dw.dirfd, x->name, O_RDONLY | O_CLOEXEC);
            if (ffd >= 0) {
                std::string text;
                char rbuf[8192];
                ssize_t nr;
                while ((nr = read(ffd, rbuf, sizeof(rbuf))) > 0) {
                    text.append(rbuf, nr);
                }
                json j = parse_json(text);
                if (member(j, "conference").is_string()) {
                    Site& site = make_site(json_string(member(j, "conference")));
                    site.update(j, Site::source_filesystem, 0);
                    unlinkat(dw.dirfd, x->name, 0);
                }
                close(ffd);
            }
            off += sizeof(ievent) + x->len;
        }
    }
    close(dw.dirfd);
    log_msg(LOG_DEBUG) << "fd " << dw.dirfd << ": close";
}
#endif

// On SIGTERM, cancel everything; main() then exits with status 1. (The wait
// is weak: the listeners, not this, keep the server running.)
cot::task<> catch_sigterm() {
    co_await cot::at_signal_weak(SIGTERM);
    got_sigterm = true;
    cot::clear();
}

static const Clp_Option options[] = {
    { "fg", 0, 0, 0, 0 },
    { "log", 0, 0, Clp_ValString, 0 },
    { "log-file", 0, 0, Clp_ValString, 0 },
    { "log-stderr", 0, 0, Clp_ValString, 0 },
    { "log-level", 0, 0, Clp_ValInt, 0 },
    { "nfiles", 'n', 0, Clp_ValInt, 0 },
    { "pid-file", 0, 0, Clp_ValString, 0 },
    { "port", 'p', 0, Clp_ValInt, 0 },
    { "status-port", 0, 0, Clp_ValInt, 0 },
    { "token", 't', 0, Clp_ValString, 0 },
    { "track-users", 0, 0, 0, Clp_Negate },
    { "update-directory", 0, 0, Clp_ValString, 0 },
    { "user", 'u', 0, Clp_ValString, 0 },
    { "verbose", 'V', 0, 0, Clp_Negate },
    { "verify-port", 'P', 0, Clp_ValInt, 0 }
};

static void usage() {
    std::cerr << "Usage: hotcrp-comet [-p PORT] [--fg] [--pid-file PIDFILE]\n";
    exit(1);
}

static pid_t maybe_fork(bool dofork) {
    if (dofork) {
        pid_t pid = fork();
        if (pid == -1) {
            log_msg(LOG_ERROR) << "fork: " << strerror(errno);
            exit(1);
        } else if (pid > 0) {
            return pid;
        }
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        if (logs != &std::cerr) {
            close(STDERR_FILENO);
            logerrs = nullptr;
        }
        setpgid(0, 0);
        signal(SIGHUP, SIG_IGN);
    }
    return getpid();
}

static void set_userarg(const std::string& userarg) {
    size_t colon = userarg.find(':');
    std::string user, group;
    if (colon != std::string::npos) {
        user = userarg.substr(0, colon);
        group = userarg.substr(colon + 1);
    } else {
        user = group = userarg;
    }
    struct passwd* pwent = nullptr;
    struct group* grent = nullptr;
    if (!user.empty() && !(pwent = getpwnam(user.c_str()))) {
        log_msg(LOG_ERROR) << "user " << user << " does not exist";
    }
    if (!group.empty() && !(grent = getgrnam(group.c_str()))) {
        log_msg(LOG_ERROR) << "group " << group << " does not exist";
    }
    if (grent && setgid(grent->gr_gid) != 0) {
        log_msg(LOG_ERROR) << "setgid: " << strerror(errno);
        grent = nullptr;
    }
    if (pwent && setuid(pwent->pw_uid) != 0) {
        log_msg(LOG_ERROR) << "setuid: " << strerror(errno);
        pwent = nullptr;
    }
    if ((!user.empty() && !pwent) || (!group.empty() && !grent)) {
        exit(1);
    }
}

// Return the limit on open files, after trying to set it to `n` if `n >= 0`.
static int open_limit(int n = -1) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        return -errno;
    }
    if (n >= 0) {
        if (rl.rlim_max < (rlim_t) n && geteuid() == 0) {
            rl.rlim_max = n;
        }
        rl.rlim_cur = std::min(rl.rlim_max, (rlim_t) n);
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
#ifdef OPEN_MAX
            // macOS refuses limits above OPEN_MAX
            rl.rlim_cur = std::min(rl.rlim_cur, (rlim_t) OPEN_MAX);
            setrlimit(RLIMIT_NOFILE, &rl);
#endif
        }
        getrlimit(RLIMIT_NOFILE, &rl);
    }
    return rl.rlim_cur > (rlim_t) INT_MAX ? INT_MAX : (int) rl.rlim_cur;
}

static void write_pid_file(pid_t pid) {
    char buf[100];
    int buflen = snprintf(buf, sizeof(buf), "%ld\n", (long) pid);
    ssize_t nw = write(pidfd, buf, buflen);
    assert(nw == buflen);
    (void) nw;
}

extern "C" {
static void exiter() {
    if (pidfd >= 0) {
        lseek(pidfd, 0, SEEK_SET);
        ssize_t nw = write(pidfd, "0\n", 2);
        (void) nw;
        int r = ftruncate(pidfd, 2);
        (void) r;
    }
}
}

// Listen on `port` (all address families), or exit.
static std::vector<cot::fd> listen_or_exit(int port) {
    std::vector<cot::fd> fds;
    std::string err;
    auto t = [](int port, std::vector<cot::fd>& fds, std::string& err) -> cot::task<> {
        try {
            // (not `for (... : co_await ...)`, which crashes g++ 15.2:
            // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=125495)
            auto lfds = co_await cot::tcp_listen_all(std::format(":{}", port));
            for (auto& f : lfds) {
                fds.push_back(std::move(f));
            }
        } catch (const std::system_error& e) {
            err = e.what();
        }
    }(port, fds, err);
    cot::loop();
    if (fds.empty()) {
        log_msg(LOG_ERROR) << "listen: " << (err.empty() ? std::string("failed") : err);
        exit(1);
    }
    return fds;
}

int main(int argc, char** argv) {
    bool fg = false;
    int port = 20444;
    int status_port = 0;
    int nfiles = 0;
    bool nfiles_set = false;
    const char* pid_filename = nullptr;
    const char* log_filename = nullptr;
    bool log_stderr = false;
    bool have_loglevel = false;
    bool have_track_users = false;
    const char* update_directory = nullptr;
    std::string userarg;
    Clp_Parser* clp = Clp_NewParser(argc, argv, sizeof(options)/sizeof(options[0]), options);
    while (true) {
        int opt = Clp_Next(clp);
        if (Clp_IsLong(clp, "fg")) {
            fg = true;
        } else if (Clp_IsLong(clp, "nfiles")) {
            nfiles = clp->val.i;
            nfiles_set = true;
        } else if (Clp_IsLong(clp, "port")) {
            port = clp->val.i;
        } else if (Clp_IsLong(clp, "status-port")) {
            status_port = clp->val.i;
        } else if (Clp_IsLong(clp, "verify-port")) {
            verify_port = clp->val.i;
        } else if (Clp_IsLong(clp, "pid-file")) {
            pid_filename = clp->val.s;
        } else if (Clp_IsLong(clp, "log-stderr")) {
            log_filename = clp->val.s;
            log_stderr = true;
        } else if (Clp_IsLong(clp, "log-file") || Clp_IsLong(clp, "log")) {
            log_filename = clp->val.s;
        } else if (Clp_IsLong(clp, "log-level")) {
            loglevel = std::max(0, std::min(MAX_LOGLEVEL, clp->val.i));
            have_loglevel = true;
        } else if (Clp_IsLong(clp, "track-users")) {
            track_users = !clp->negated;
            have_track_users = true;
        } else if (Clp_IsLong(clp, "token")) {
            update_token = clp->val.s;
        } else if (Clp_IsLong(clp, "update-directory")) {
            update_directory = clp->val.s;
        } else if (Clp_IsLong(clp, "user")) {
            userarg = clp->val.s;
        } else if (Clp_IsLong(clp, "verbose")) {
            verbose = !clp->negated;
        } else if (opt != Clp_Done) {
            usage();
        } else {
            break;
        }
    }
    if (verbose && !have_loglevel) {
        loglevel = std::max(loglevel, LOG_VERBOSE);
    }
    if (verbose && !have_track_users) {
        track_users = true;
    }

    cot::set_clock(cot::clock::real_time);
    atexit(exiter);

    // open log stream
    std::ofstream logf_stream;
    if (log_stderr) {
        if (!freopen(log_filename, "a", stderr)) {
            std::cerr << log_filename << ": " << strerror(errno) << "\n";
            exit(1);
        }
        logs = &std::cerr;
    } else if (log_filename) {
        logf_stream.open(log_filename, std::ofstream::app);
        if (logf_stream.bad()) {
            std::cerr << log_filename << ": " << strerror(errno) << "\n";
            exit(1);
        }
        logs = &logf_stream;
    } else if (fg) {
        logs = &std::cerr;
    }
    if (logs != &std::cerr || log_stderr) {
        logerrs = &std::cerr;
    }

    // open PID file
    if (pid_filename) {
        int rdfd = open(pid_filename, O_RDONLY);
        if (rdfd >= 0) {
            int r = flock(rdfd, LOCK_EX);
            if (r != 0) {
                log_msg(LOG_ERROR) << pid_filename << ": Could not obtain lock";
                exit(1);
            }
            char buf[1024];
            ssize_t nr = read(rdfd, buf, 1024);
            if (nr != 2 || buf[0] != '0' || buf[1] != '\n') {
                log_msg(LOG_ERROR) << pid_filename << ": File exists";
                exit(1);
            }
            unlink(pid_filename);
            close(rdfd);
        }

        auto old_mask = umask(022);
        pidfd = open(pid_filename, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL | O_CLOEXEC, 0666);
        umask(old_mask);
        if (pidfd < 0) {
            log_msg(LOG_ERROR) << pid_filename << ": " << strerror(errno);
            exit(1);
        }
    }

    // Bind before forking, to report errors, and before changing user, to
    // allow privileged ports.
    std::vector<cot::fd> serverfds = listen_or_exit(port);
    for (auto& f : serverfds) {
        record_startup_fd(f.fileno(), "listen");
    }
    log_msg(LOG_VERBOSE) << "listen http://localhost:" << port;

    std::vector<cot::fd> statusserverfds;
    if (status_port > 0) {
        statusserverfds = listen_or_exit(status_port);
        for (auto& f : statusserverfds) {
            record_startup_fd(f.fileno(), "listen");
        }
        log_msg(LOG_VERBOSE) << "listen http://localhost:" << status_port << " for status";
    }

#if __linux__
    directory_watch dirwatch{-1, -1};
    if (update_directory) {
        dirwatch = open_directory_watch(update_directory);
    }
#else
    if (update_directory) {
        log_msg(LOG_ERROR) << "--update-directory is only supported on Linux.";
        exit(1);
    }
#endif

    if (!userarg.empty()) {
        set_userarg(userarg);
    }

    if (nfiles_set) {
        int r = open_limit(nfiles <= 0 ? INT_MAX : nfiles);
        if (r < nfiles) {
            log_msg(LOG_ERROR) << "hotcrp-comet: limited to " << r << " open files";
        }
    }
    connection_limit = open_limit();
    if (connection_limit > 100) {
        connection_limit -= 20;
    } else {
        connection_limit = 0;
    }

    pid_t pid = maybe_fork(!fg);

    if (pidfd >= 0 && (fg || pid != getpid())) {
        write_pid_file(pid);
    }
    if (pid != getpid()) {
        pidfd = -1;
        exit(0);
    }
    if (!fg) {
        // The child gets a fresh driver: a kqueue doesn't survive fork().
        cot::reset();
    }
    log_msg() << "hotcrp-comet started, pid " << pid;
    for (auto m : startup_fds) {
        log_msg(LOG_DEBUG) << m;
    }

    catch_sigterm().detach();
    for (auto& f : serverfds) {
        listener(std::move(f), status_port == 0).detach();
    }
    for (auto& f : statusserverfds) {
        listener(std::move(f), true).detach();
    }
    serverfds.clear();
    statusserverfds.clear();
#if __linux__
    if (update_directory) {
        directory_watcher(update_directory, dirwatch).detach();
    }
#endif

    cot::loop();
    exit(got_sigterm ? 1 : 0);
}
