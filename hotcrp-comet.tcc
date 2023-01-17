// -*- mode: c++; c-basic-offset: 4 -*-
#include <tamer/tamer.hh>
#include <tamer/http.hh>
#include <tamer/channel.hh>
#include <fcntl.h>
#include <sys/file.h>
#include <unordered_map>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <pwd.h>
#include <grp.h>
#include "clp.h"
#include "json.hh"
#if __linux__
# include <sys/inotify.h>
#endif

#define LOG_ERROR 0
#define LOG_NORMAL 1
#define LOG_VERBOSE 2
#define LOG_DEBUG 3

#define MAX_LOGLEVEL LOG_DEBUG
#define MAX_NPOLLFDS 5

static constexpr double connection_timeout = 20;
static constexpr unsigned site_validate_timeout = 120;
static constexpr unsigned site_error_validate_timeout = 1;
static constexpr double user_validate_timeout = 3600;
static double min_poll_timeout = 240;
static double max_poll_timeout = 300;
static int verify_port = 0;
static tamer::channel<tamer::fd> pollfds;
static tamer::fd serverfd;
static tamer::fd statusserverfd;
static tamer::fd watchfd;
static unsigned nconnections;
static unsigned connection_limit;
static unsigned npolls;
static int pidfd = -1;
static std::ostream* logs;
static std::ostream* logerrs;
static int loglevel;
static std::vector<std::string> startup_fds;
static String update_token;
static bool verbose;

#define TIMESTAMP_FMT "%Y-%m-%d %H:%M:%S %z"

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


class Site : public tamer::tamed_class {
  public:
    explicit Site(std::string url)
        : url_(std::move(url)), created_at_(tamer::drecent()) {
        if (url_.back() != '/') {
            url_ += '/';
        }
        tamer::http_message req;
        req.url(url_);
        host_ = req.url_host_port();
        port_ = req.url_port() ? : 80;
        path_ = req.url_path();
    }

    inline const std::string& url() const {
        return url_;
    }
    inline const std::string& status() const {
        return status_;
    }
    inline constexpr double status_at() const {
        return status_at_;
    }
    inline uint64_t eventid() const {
        return eventid_;
    }
    inline bool is_valid() const;

    enum source_type {
        source_validate = 0,
        source_update = 1,
        source_filesystem = 2
    };
    bool update(const Json& j, source_type source, uint64_t old_eventid);

    std::string make_api_path(std::string fn, std::string rest = std::string());

    tamed void validate(tamer::event<> done);
    tamed void check_user(std::vector<tamer::http_header> cookies,
                          tamer::event<bool, std::string> done);

    void wait(uint64_t eventid, tamer::event<> done) {
        if (!is_valid()) {
            validate(std::move(done));
        } else if (eventid != eventid_) {
            done();
        } else {
            status_change_ += std::move(done);
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

    Json status_json() const;

  private:
    std::string url_;
    std::string host_;
    uint16_t port_;
    std::string path_;
    std::string status_;
    uint64_t eventid_ = 0;
    double status_at_ = 0.0;
    double created_at_;
    double eventid_at_ = 0.0;
    double validate_at_ = 0.0;
    bool validating_ = false;
    int validate_fail_ = 0;
    tamer::event<> validate_event_;
    tamer::event<> status_change_;
    unsigned long long npoll_ = 0;
    unsigned long long npoll_noblock_ = 0;
    unsigned recent_poll_noblock_ = 0;
    unsigned long long nupdate_[3] = {0, 0, 0};
    unsigned long long nupfail_[3] = {0, 0, 0};
    unsigned long long nbadreq_ = 0;
    unsigned npollers_ = 0;

    tamed void send(std::string path,
                    std::vector<tamer::http_header> headers,
                    tamer::event<Json> done);
    void send(std::string path, tamer::event<Json> done);
};

typedef std::unordered_map<std::string, Site> site_map_type;
site_map_type sites;

Site& make_site(const std::string& url) {
    auto it = sites.find(url);
    if (it == sites.end()) {
        it = sites.insert(std::make_pair(url, Site(url))).first;
    }
    return it->second;
}

std::string Site::make_api_path(std::string fn, std::string rest) {
    std::string path = path_ + "api.php?fn=" + fn;
    if (!rest.empty()) {
        path += "&" + rest;
    }
    if (update_token) {
        path += "&token=" + std::string(update_token.encode_uri_component());
    }
    return path;
}

static void set_age_minutes(Json& j, const char* name, double dt) {
    if (dt) {
        j.set(name, round((tamer::drecent() - dt) / 6) / 10);
    }
}

Json Site::status_json() const {
    Json j = Json().set("site", url_).set("eventid", eventid_);
    set_age_minutes(j, "age_min", created_at_);
    set_age_minutes(j, "event_age_min", eventid_at_);
    set_age_minutes(j, "validate_age_min", validate_at_);
    j.set("npoll", npoll_)
        .set("nupdate", nupdate_[0] + nupdate_[1] + nupdate_[2]);
    if (unsigned long long nupfail = nupfail_[0] + nupfail_[1] + nupfail_[2]) {
        j.set("nupdate_fail", nupfail);
    }
    if (nbadreq_ > 0) {
        j.set("nbad_request", nbadreq_);
    }
    j.set("npollers", npollers_);
    if (validating_) {
        j.set("validating", true);
    }
    return j;
}

inline bool Site::is_valid() const {
    if (!validate_at_
        || recent_poll_noblock_ > 20) {
        return false;
    }
    double elapsed = tamer::drecent() - validate_at_;
    if (validate_fail_ > 0) {
        return elapsed < (site_error_validate_timeout << (validate_fail_ - 1));
    } else {
        return elapsed < site_validate_timeout;
    }
}

bool Site::update(const Json& j, source_type source, uint64_t prev_eventid) {
    if (!j.is_o()
        || !j["ok"]
        || (!update_token.empty() && j["token"] != update_token)
        || !j["tracker_eventid"].is_u()) {
        ++nupfail_[source];
        if (source == source_validate) {
            validate_fail_ = std::min(validate_fail_ + 1, 7);
        }
        return false;
    }

    uint64_t eventid = j["tracker_eventid"].to_u();
    if (eventid < eventid_
        && (source != source_validate || eventid_ != prev_eventid)) {
        return false;
    }
    validate_fail_ = 0;

    if (eventid != eventid_) {
        eventid_ = eventid;
        if (j["tracker_status"].is_s()) {
            status_ = j["tracker_status"].to_s();
        } else {
            status_ = std::string();
        }
        if (j["tracker_status_at"].is_d()) {
            status_at_ = j["tracker_status_at"].to_d();
        } else {
            status_at_ = 0;
        }
        eventid_at_ = tamer::drecent();
        ++nupdate_[source];
        status_change_();
        log_msg() << "* " << url_ << " #" << eventid_;
    }

    recent_poll_noblock_ = 0;
    return true;
}

static std::string trim_body(tamer::http_message& msg) {
    std::string body = msg.body();
    if (body.empty()) {
        body = "<empty>";
    } else {
        body.erase(body.find_last_not_of(" \r\n\t\v\f") + 1);
    }
    return body;
}

tamed void Site::send(std::string path,
                      std::vector<tamer::http_header> headers,
                      tamer::event<Json> done) {
    tamed {
        tamer::http_parser hp(HTTP_RESPONSE);
        tamer::http_message req, res;
        tamer::fd cfd;
        Json j;
        bool opened_pollfd = false;
    }

    // get a polling file descriptor
    twait {
        pollfds.pop_front(tamer::make_event(cfd));
    }

 reopen_pollfd:
    if (!cfd || cfd.socket_error()) {
        opened_pollfd = true;
        twait {
            tamer::tcp_connect(verify_port > 0 ? verify_port : port_, tamer::make_event(cfd));
        }
        log_msg(LOG_DEBUG) << "fd " << cfd.recent_fdnum() << ": pollfd";
    }
    req.method(HTTP_GET)
        .url(path)
        .header("Host", host_)
        .header("Connection", "keep-alive");
    for (auto& hdr : headers) {
        req.header(hdr.name, hdr.value);
    }
    twait {
        tamer::http_parser::send_request(cfd, req, tamer::make_event());
    }

    // parse response
    twait {
        hp.receive(cfd, tamer::add_timeout(120, tamer::make_event(res)));
    }
    j = Json();
    log_msg(LOG_VERBOSE) << "> GET http://" << host_ << path << " -> " << res.status_code();
    if (hp.ok() && res.ok()) {
        j = Json::parse(res.body());
    }
    if (j.is_o() && update_token) {
        j.set("token", update_token); // always trust response
    }
    if (!(j.is_o()
          && j["ok"]
          && (update_token.empty() || j["token"] == update_token))) {
        cfd.close();
        log_msg(LOG_DEBUG) << "fd " << cfd.recent_fdnum() << ": close " << trim_body(res);
        if (!opened_pollfd) {
            hp.clear();
            req.clear();
            goto reopen_pollfd;
        }
        if (!hp.ok()) {
            log_msg() << host_ << path << ": error " << http_errno_name(hp.error());
        } else {
            log_msg() << host_ << path << ": bad tracker status " << trim_body(res);
        }
    }
    if (!hp.should_keep_alive() && cfd) {
        cfd.close();
        log_msg(LOG_DEBUG) << "fd " << cfd.recent_fdnum() << ": close";
    }
    pollfds.push_front(std::move(cfd));
    done(j);
}

void Site::send(std::string path, tamer::event<Json> done) {
    send(path, std::vector<tamer::http_header>(), done);
}

tamed void Site::validate(tamer::event<> done) {
    tamed {
        uint64_t prev_eventid = eventid_;
        Json j;
    }

    // is status already being checked?
    validate_event_ += std::move(done);
    if (validating_) {
        return;
    }
    validating_ = true;

    twait {
        send(make_api_path("trackerstatus"), make_event(j));
    }

    update(j, source_validate, prev_eventid);
    validate_at_ = tamer::drecent();
    validating_ = false;
    validate_event_();
}

tamed void Site::check_user(std::vector<tamer::http_header> cookies,
                            tamer::event<bool, std::string> done) {
    tamed { Json j; }
    twait {
        send(make_api_path("whoami", ""), cookies, make_event(j));
    }
    if (j.is_o() && j["ok"]) {
        done(true, j["email"].to_s());
    } else {
        done(false, std::string());
    }
}

bool check_conference(std::string arg, tamer::http_message& conf_m) {
    if (arg.empty()) {
        return false;
    }
    conf_m.url(arg);
    if (conf_m.url_schema().empty()
        || conf_m.url_host().empty()) {
        return false;
    }
    return true;
}


struct user_map {
    double timestamp = 0.0;
    double used_at = 0.0;
    char firstch = 0;
    bool locked = true;
    unsigned long nused = 0;
    std::vector<tamer::http_header> cookies;
    std::string user;

    bool cookies_equal(const std::vector<tamer::http_header>& xcookies) const {
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

tamed void lookup_user(Site& site, std::vector<tamer::http_header> cookies,
                       tamer::event<std::string> done) {
    tamed {
        tamer::destroy_guard guard(&site);
        char firstch = 0;
        size_t oldi = -1;
        bool ok;
        std::string user;
        double now = tamer::drecent();
    }

    // no cookies -> no user
    if (cookies.empty()) {
        done("");
        return;
    }

    // search for match
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
                done(users[i].user);
                return;
            }
            if (users[i].used_at < oldt
                && !users[i].locked) {
                oldi = i;
                oldt = users[i].used_at;
            }
        }

        if (users.size() == max_users_size
            && oldi == (size_t) -1) {
            done("");
            return;
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

    twait {
        site.check_user(users[oldi].cookies, tamer::make_event(ok, user));
    }

    users[oldi].locked = false;
    users[oldi].user = user;
    done(user);
}


class Connection : public tamer::tamed_class {
 public:
    static Connection* add(tamer::fd cfd, bool allow_status);
    static std::vector<Connection*> all;
    static void clear_one(Connection* dont_clear);

    Json status_json() const;
    double age() const {
        return tamer::drecent() - created_at_;
    }

  private:
    enum state { s_request, s_poll, s_response };

    tamer::fd cfd_;
    tamer::http_parser hp_;
    tamer::http_message req_;
    tamer::http_message res_;
    std::string recent_user_;
    std::string recent_site_;
    Json resj_;
    int resj_indent_;
    bool allow_status_;
    double created_at_;
    tamer::event<> poll_event_;
    int state_;
    Connection* prev_;
    Connection* next_;

    static Connection* state_head[3];
    void set_state(int state);

    Connection(tamer::fd cfd, bool allow_status);
    ~Connection();
    tamed void poll_handler(double timeout, tamer::event<> done);
    void update_handler();
    tamed void handler();
    tamed void check_user();
};

std::vector<Connection*> Connection::all;
Connection* Connection::state_head[3];

Connection::Connection(tamer::fd cfd, bool allow_status)
    : cfd_(std::move(cfd)), hp_(HTTP_REQUEST),
      allow_status_(allow_status), created_at_(tamer::drecent()), state_(-1) {
    assert(cfd_.valid());
    log_msg(LOG_DEBUG) << "fd " << cfd_.fdnum() << ": connection";
    if (all.size() <= (unsigned) cfd_.fdnum()) {
        all.resize(cfd_.fdnum() + 1, nullptr);
    }
    assert(!all[cfd_.fdnum()]);
    all[cfd_.fdnum()] = this;
    ++nconnections;
}

Connection* Connection::add(tamer::fd cfd, bool allow_status) {
    Connection* c = new Connection(std::move(cfd), allow_status);
    c->handler();
    return c;
}

Connection::~Connection() {
    cfd_.close();
    log_msg(LOG_DEBUG) << "fd " << cfd_.recent_fdnum() << ": close";
    all[cfd_.recent_fdnum()] = nullptr;
    --nconnections;
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
        log_msg() << "premature delete " << c->status_json();
        delete c;
    } else if (c->state_ == s_poll) {
        Connection* c = state_head[s_poll]->prev_;
        log_msg() << "premature close " << c->status_json();
        c->set_state(s_response);
        c->hp_.clear_should_keep_alive();
        c->poll_event_();
    }
}

Json Connection::status_json() const {
    static const char* stati[] = {"request", "poll", "response"};
    Json j = Json().set("fd", cfd_.fdnum())
        .set("age", (unsigned long) (tamer::drecent() - created_at_));
    if (state_ >= 0) {
        j.set("state", stati[state_]);
    }
    if (!recent_site_.empty()) {
        j.set("site", recent_site_);
    }
    if (!recent_user_.empty()) {
        j.set("user", recent_user_);
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

tamed void Connection::poll_handler(double timeout, tamer::event<> done) {
    tamed {
        Site& site = make_site(recent_site_);
        tamer::destroy_guard guard(&site);
        uint64_t poll_eventid = 0;
        double start_at = tamer::drecent();
        double timeout_at = start_at + poll_timeout(timeout);
        bool blocked = false;
    }
    {
        std::string poll_status = req_.query("poll");
        const char* ps = poll_status.c_str();
        const char* ps_end = ps + poll_status.length();
        char* ptr = nullptr;
        uint64_t result = 0;
        if (ps != ps_end && isdigit((unsigned char) *ps)) {
            result = strtoul(ps, &ptr, 10);
        }
        if (ptr == ps_end) {
            poll_eventid = result;
        } else if (poll_status.empty() || site.status() == poll_status) {
            poll_eventid = site.eventid();
        } else {
            poll_eventid = site.eventid() - 1;
        }
    }
    site.add_poller();
    ++npolls;

    while (!cfd_.write_closed()
           && tamer::drecent() < timeout_at
           && state_ == s_poll
           && (site.eventid() == poll_eventid || !site.is_valid())) {
        blocked = true;
        twait {
            poll_event_ = tamer::add_timeout(timeout_at - tamer::drecent(),
                                             tamer::make_event());
            tamer::at_fd_write(cfd_.fdnum(), poll_event_);
            site.wait(poll_eventid, poll_event_);
        }
    }

    if (!site.status().empty()) {
        resj_.set("ok", true)
            .set("at", tamer::drecent())
            .set("tracker_status", site.status())
            .set("tracker_status_at", site.status_at())
            .set("tracker_eventid", site.eventid());
    } else {
        resj_.set("ok", false);
    }
    hp_.clear_should_keep_alive();
    site.resolve_poller(blocked);
    --npolls;
    done();
}

void Connection::update_handler() {
    Json j = Json().set("ok", true);
    if (!req_.query("tracker_status").empty()) {
        j.set("tracker_status", req_.query("tracker_status"));
    }
    if (!req_.query("tracker_status_at").empty()) {
        j.set("tracker_status_at", Json::parse(req_.query("tracker_status_at")));
    }
    if (!req_.query("tracker_eventid").empty()) {
        j.set("tracker_eventid", Json::parse(req_.query("tracker_eventid")));
    }
    if (!req_.query("token").empty()) {
        j.set("token", req_.query("token"));
    }
    Site& site = make_site(recent_site_);
    if (site.update(j, Site::source_update, 0)) {
        resj_.set("ok", true);
    } else {
        resj_.set("ok", false).set("error", "invalid status update");
    }
    hp_.clear_should_keep_alive();
}

tamed void Connection::check_user() {
    twait {
        Site& site = make_site(recent_site_);
        std::vector<tamer::http_header> cookies;
        for (auto it = req_.header_begin(); it != req_.header_end(); ++it) {
            if (it->is_canonical("cookie"))
                cookies.push_back(*it);
        }
        lookup_user(site, cookies, tamer::make_event(recent_user_));
    }
}

void hotcrp_comet_status_handler(Json& resj) {
    resj.set("at", tamer::drecent())
        .set("at_time", timestamp_string(tamer::drecent()))
        .set("nconnections", nconnections)
        .set("connection_limit", connection_limit);

    Json sitesj = Json::make_array();
    for (auto it = sites.begin(); it != sites.end(); ++it) {
        sitesj.push_back(it->second.status_json());
    }
    std::sort(sitesj.abegin(), sitesj.aend(), [](const Json& a, const Json& b) {
        Json aage = a.get("event_age_min"), bage = b.get("event_age_min");
        bool anull = aage.is_null(), bnull = bage.is_null();
        if (anull && bnull) {
            return a.get("site").as_s() < b.get("site").as_s();
        } else if (anull || bnull) {
            return bnull;
        } else {
            return aage.as_d() < bage.as_d();
        }
    });
    resj.set("sites", sitesj);

    Json connj = Json();
    for (auto it = Connection::all.begin(); it != Connection::all.end(); ++it) {
        if (*it)
            connj.push_back((*it)->status_json());
    }
    std::sort(connj.abegin(), connj.aend(), [](const Json& a, const Json& b) {
        return a.get("age").as_d() > b.get("age").as_d();
    });
    resj.set("connections", connj);

    if (!users.empty()) {
        Json usersj = Json();
        for (auto it = users.begin(); it != users.end(); ++it) {
            if (!it->locked) {
                Json uj = Json().set("user", it->user);
                std::string cookie;
                for (auto cit = it->cookies.begin(); cit != it->cookies.end(); ++cit) {
                    if (cookie.empty()) {
                        cookie = cit->value;
                    } else {
                        cookie += "; ";
                        cookie += cit->value;
                    }
                }
                uj.set("cookie", cookie);
                set_age_minutes(uj, "age_min", it->timestamp);
                if (it->nused > 0) {
                    uj.set("nused", it->nused);
                }
                usersj.push_back(uj);
            }
        }
        std::sort(usersj.abegin(), usersj.aend(), [](const Json& a, const Json& b) {
            return a.get("age_min").as_d() < b.get("age_min").as_d();
        });
        if (!usersj.empty()) {
            resj.set("user_cache", usersj);
        }
    }
}

tamed void Connection::handler() {
    tamed {
        tamer::http_message confurl;
        double timeout = connection_timeout;
        std::string url_path;
    }
    // log_msg() << "new connection " << status_json();
    while (cfd_) {
        req_.error(HPE_PAUSED);
        set_state(s_request);
        twait {
            hp_.receive(cfd_, tamer::add_timeout(timeout, tamer::make_event(req_)));
        }
        if (!hp_.ok() || !req_.ok()) {
            if (!hp_.ok() && hp_.error() != HPE_INVALID_EOF_STATE) {
                log_msg(LOG_DEBUG) << "fd " << cfd_.recent_fdnum() << ": request fail " << http_errno_name(hp_.error());
            }
            break;
        }
        if (loglevel >= LOG_VERBOSE) {
            log_msg(LOG_VERBOSE) << "< " << http_method_str(req_.method()) << " " << req_.url() << " [" << npolls << "]";
        }
        res_.error(HPE_PAUSED)
            .header("Access-Control-Allow-Origin", "*")
            .header("Access-Control-Allow-Credentials", "true")
            .header("Access-Control-Allow-Headers", "Accept-Encoding")
            .header("Expires", "Mon, 26 Jul 1997 05:00:00 GMT");
        resj_.clear();
        resj_indent_ = 0;
        url_path = req_.url_path();
        if (url_path == "/status" && allow_status_) {
            hotcrp_comet_status_handler(resj_);
            resj_indent_ = 2;
        } else if (check_conference(req_.query("conference"), confurl)) {
            recent_site_ = req_.query("conference");
            if (req_.has_canonical_header("cookie")
                && verbose) {
                check_user();
            }
            if (url_path.length() >= 7
                && url_path.compare(url_path.length() - 7, 7, "/update") == 0) {
                update_handler();
            } else if (url_path.length() >= 5
                       && url_path.compare(url_path.length() - 5, 5, "/poll") == 0) {
                set_state(s_poll);
                twait volatile {
                    Json j = Json::parse(req_.query("timeout"));
                    poll_handler(j.is_number() ? j.to_d() / 1000. : 0, tamer::make_event());
                }
            } else {
                resj_.set("ok", false).set("error", "bad request");
            }
        } else {
            resj_.set("ok", false).set("error", "bad request");
        }
        res_.error(HPE_OK).date_header("Date", tamer::recent().tv_sec)
            .header("Content-Type", "application/json");
        if (resj_["ok"] || !hp_.should_keep_alive()) {
            res_.header("Connection", "close");
        }
        // if (!res_.ok())
        //    res_.status_code(503);
        res_.body(resj_.unparse(Json::indent_depth(resj_indent_).newline_terminator(true)));
        set_state(s_response);
        twait { hp_.send_response(cfd_, res_, tamer::make_event()); }
        if (!hp_.should_keep_alive()) {
            break;
        }
        res_.clear();
        timeout = 5; // keep-alive timeout drops to 5sec
    }
    delete this;
}

static void record_startup_fd(int fd, const char* msg) {
    std::ostringstream buf;
    buf << "fd " << fd << ": " << msg;
    startup_fds.push_back(buf.str());
}

tamed void listener(tamer::fd sfd, bool allow_status) {
    tamed { tamer::fd cfd; }
    record_startup_fd(sfd.fdnum(), "listen");
    while (sfd) {
        twait { sfd.accept(tamer::make_event(cfd)); }
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

tamed void directory_watcher(const char* update_directory) {
    tamed {
        char buf[sizeof(struct inotify_event) + 1 + NAME_MAX];
        int dirfd, e;
        size_t r;
    }
    {
        int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd < 0) {
            log_msg(LOG_ERROR) << "inotify_init: " << strerror(errno);
            exit(1);
        }
        dirfd = open(update_directory, O_RDONLY | O_DIRECTORY);
        if (dirfd < 0) {
            log_msg(LOG_ERROR) << update_directory << ": " << strerror(errno);
            exit(1);
        }
        if (inotify_add_watch(fd, update_directory, IN_CLOSE_WRITE) == -1) {
            log_msg(LOG_ERROR) << update_directory << ": " << strerror(errno);
            exit(1);
        }
        watchfd = tamer::fd(fd);
    }
    record_startup_fd(watchfd.fdnum(), "inotify");
    record_startup_fd(dirfd, "watch directory");
    while (watchfd) {
        twait { watchfd.read_once(buf, sizeof(buf), r, tamer::make_event(e)); }
        if (e != 0) {
            log_msg() << update_directory << ": " << strerror(-e);
        }
        for (size_t off = 0; off < r; ) {
            ievent* x = reinterpret_cast<ievent*>(&buf[off]);
            int ffd = openat(dirfd, x->name, O_RDONLY);
            if (ffd >= 0) {
                StringAccum sa;
                ssize_t r;
                while ((r = read(ffd, sa.reserve(8192), 8192)) > 0) {
                    sa.adjust_length(r);
                }
                Json j = Json::parse(sa.take_string());
                if (j && j["conference"].is_s()) {
                    Site& site = make_site(j["conference"].to_s());
                    site.update(j, Site::source_filesystem, 0);
                    unlinkat(dirfd, x->name, 0);
                }
                close(ffd);
            }
            off += sizeof(struct inotify_event) + x->len;
        }
    }
    close(dirfd);
    log_msg(LOG_DEBUG) << "fd " << dirfd << ": close";
}
#else
void directory_watcher(const char*) {
    log_msg(LOG_ERROR) << "--update-directory is only supported on Linux.";
    exit(1);
}
#endif

tamed void catch_sigterm() {
    twait volatile { tamer::at_signal(SIGTERM, tamer::make_event()); }
    exit(1);
}

static const Clp_Option options[] = {
    { "fg", 0, 0, 0, 0 },
    { "log", 0, 0, Clp_ValString, 0 },
    { "log-file", 0, 0, Clp_ValString, 0 },
    { "log-level", 0, 0, Clp_ValInt, 0 },
    { "nfiles", 'n', 0, Clp_ValInt, 0 },
    { "pid-file", 0, 0, Clp_ValString, 0 },
    { "port", 'p', 0, Clp_ValInt, 0 },
    { "status-port", 0, 0, Clp_ValInt, 0 },
    { "token", 't', 0, Clp_ValString, 0 },
    { "update-directory", 0, 0, Clp_ValString, 0 },
    { "user", 'u', 0, Clp_ValString, 0 },
    { "verbose", 'V', 0, 0, 0 },
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
        close(STDERR_FILENO);
        setpgid(0, 0);
        signal(SIGHUP, SIG_IGN);
        logerrs = nullptr;
    }
    return getpid();
}

static void set_userarg(const String& userarg) {
    int colon = userarg.find_left(':');
    String user, group;
    if (colon >= 0) {
        user = userarg.substr(0, colon);
        group = userarg.substr(colon + 1);
    } else {
        user = group = userarg;
    }
    struct passwd* pwent = nullptr;
    struct group* grent = nullptr;
    if (user && !(pwent = getpwnam(user.c_str()))) {
        log_msg(LOG_ERROR) << "user " << user << " does not exist";
    }
    if (group && !(grent = getgrnam(group.c_str()))) {
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
    if ((user && !pwent) || (group && !grent)) {
        exit(1);
    }
}

static void write_pid_file(pid_t pid) {
    char buf[100];
    int buflen = snprintf(buf, sizeof(buf), "%ld\n", (long) pid);
    ssize_t nw = write(pidfd, buf, buflen);
    assert(nw == buflen);
}

extern "C" {
static void exiter() {
    serverfd.close();
    statusserverfd.close();
    watchfd.close();
    if (pidfd >= 0) {
        lseek(pidfd, 0, SEEK_SET);
        write(pidfd, "0\n", 2);
        ftruncate(pidfd, 2);
    }
}
}

static void driver_error_handler(int, int err, std::string msg) {
    log_msg() << "tamer error: " << strerror(err) << ", " << msg;
}

int main(int argc, char** argv) {
    bool fg = false;
    int port = 20444;
    int status_port = 0;
    int nfiles = 0;
    bool nfiles_set = false;
    const char* pid_filename = nullptr;
    const char* log_filename = nullptr;
    const char* update_directory = nullptr;
    String userarg;
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
        } else if (Clp_IsLong(clp, "log-file") || Clp_IsLong(clp, "log")) {
            log_filename = clp->val.s;
        } else if (Clp_IsLong(clp, "log-level")) {
            loglevel = std::max(0, std::min(MAX_LOGLEVEL, clp->val.i));
        } else if (Clp_IsLong(clp, "token")) {
            update_token = clp->val.s;
        } else if (Clp_IsLong(clp, "update-directory")) {
            update_directory = clp->val.s;
        } else if (Clp_IsLong(clp, "user")) {
            userarg = clp->val.s;
        } else if (Clp_IsLong(clp, "verbose")) {
            verbose = true;
            loglevel = std::max(loglevel, LOG_VERBOSE);
        } else if (opt != Clp_Done) {
            usage();
        } else {
            break;
        }
    }

    tamer::initialize();
    tamer::driver::main->set_error_handler(driver_error_handler);
    atexit(exiter);
    catch_sigterm();

    // open log stream
    std::ofstream logf_stream;
    if (log_filename) {
        logf_stream.open(log_filename, std::ofstream::app);
        if (logf_stream.bad()) {
            std::cerr << log_filename << ": " << strerror(errno) << "\n";
            exit(1);
        }
        logs = &logf_stream;
    } else if (fg) {
        logs = &std::cerr;
    }
    if (logs != &std::cerr) {
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
        pidfd = open(pid_filename, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0666);
        umask(old_mask);
        if (pidfd < 0) {
            log_msg(LOG_ERROR) << pid_filename << ": " << strerror(errno);
            exit(1);
        }
    }

    serverfd = tamer::tcp_listen(port);
    if (!serverfd) {
        log_msg(LOG_ERROR) << "listen: " << strerror(-serverfd.error());
        exit(1);
    }
    log_msg(LOG_VERBOSE) << "listen http://localhost:" << port;
    listener(serverfd, status_port == 0);

    if (status_port > 0) {
        statusserverfd = tamer::tcp_listen(status_port);
        if (!statusserverfd) {
            log_msg(LOG_ERROR) << "listen: " << strerror(-serverfd.error());
            exit(1);
        }
        log_msg(LOG_VERBOSE) << "listen http://localhost:" << port;
        listener(statusserverfd, true);
    }

    if (update_directory) {
        directory_watcher(update_directory);
    }

    if (userarg) {
        set_userarg(userarg);
    }

    if (nfiles_set) {
        int r = tamer::fd::open_limit(nfiles <= 0 ? INT_MAX : nfiles);
        if (r < nfiles) {
            log_msg(LOG_ERROR) << "hotcrp-comet: limited to " << r << " open files";
        }
    }
    connection_limit = tamer::fd::open_limit();
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
    log_msg() << "hotcrp-comet started, pid " << pid;
    for (auto m : startup_fds) {
        log_msg(LOG_DEBUG) << m;
    }
    while (pollfds.size() != MAX_NPOLLFDS) {
        pollfds.push_back(tamer::fd());
    }

    tamer::loop();
    tamer::cleanup();
}
