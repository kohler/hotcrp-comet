// -*- mode: c++; c-basic-offset: 4 -*-
#include <tamer/tamer.hh>
#include <tamer/http.hh>
#include <fcntl.h>
#include <unordered_set>
#include <fstream>
#include <algorithm>
#include <pwd.h>
#include <grp.h>
#include "clp.h"
#include "json.hh"

#define LOG_ALWAYS 0
#define LOG_DEBUG 1

#define MAX_LOGLEVEL LOG_ALWAYS

static double connection_timeout = 20;
static double site_validate_timeout = 120;
static double site_error_validate_timeout = 5;
static double min_poll_timeout = 240;
static double max_poll_timeout = 300;
static tamer::fd pollfd;
static tamer::fd serverfd;
static unsigned nconnections;
static unsigned connection_limit;
static const char* pid_file = nullptr;
static std::ostream* logs;
static int loglevel;

class log_msg {
 public:
    log_msg(int msglevel = 0)
        : logf_(logs && msglevel <= loglevel && msglevel <= MAX_LOGLEVEL ? logs : 0) {
        if (logf_)
            add_prefix();
    }
    ~log_msg() {
        if (logf_)
            *logf_ << stream_.str() << std::endl;
    }
    template <typename T> log_msg& operator<<(T&& x) {
        if (logf_)
            stream_ << x;
        return *this;
    }
 private:
    std::ostream* logf_;
    std::stringstream stream_;
    void add_prefix();
};

#define TIMESTAMP_FMT "%m/%d/%Y %H:%M:%S %z"

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
        : url_(std::move(url)), create_at_(tamer::drecent()),
          status_at_(0), status_change_at_(0),
          npoll_(0), nupdate_(0), npollers_(0) {
        if (url_.back() != '/')
            url_ += '/';
        tamer::http_message req;
        req.url(url_);
        host_ = req.url_host_port();
        path_ = req.url_path() + "api.php?checktracker=1";
    }

    inline const std::string& url() const {
        return url_;
    }
    inline const std::string& status() const {
        return status_;
    }
    inline bool is_valid() const;
    inline bool status_may_equal(const std::string& status) const;
    void set_status(const std::string& status) {
        if (status_ != status) {
            status_ = status;
            status_change_at_ = tamer::drecent();
            status_change_();
        }
    }
    inline void set_status(const String& status) {
        set_status(std::string(status.data(), status.length()));
    }

    tamed void validate(tamer::event<> done);

    void wait(const std::string& status, tamer::event<> done) {
        if (is_valid() && !status_may_equal(status))
            done();
        else if (is_valid())
            status_change_ += std::move(done);
        else
            validate(std::move(done));
    }

    void add_poller() {
        ++npoll_;
        ++npollers_;
    }
    void resolve_poller() {
        --npollers_;
    }
    void add_update() {
        ++nupdate_;
    }

    Json status_json() const;

    class hasher {
      public:
        size_t operator()(const Site& s) const {
            return std::hash<std::string>()(s.url());
        }
    };
    class comparator {
      public:
        bool operator()(const Site& a, const Site& b) const {
            return a.url_ == b.url_;
        }
    };

  private:
    std::string url_;
    std::string host_;
    std::string path_;
    std::string status_;
    double create_at_;
    double status_at_;
    double status_change_at_;
    tamer::event<> status_check_;
    tamer::event<> status_change_;
    unsigned long long npoll_;
    unsigned long long nupdate_;
    unsigned npollers_;
};

std::unordered_set<Site, Site::hasher, Site::comparator> sites;

Site& make_site(const std::string& url) {
    return const_cast<Site&>(*sites.emplace(url).first);
}

Json Site::status_json() const {
    return Json().set("site", url_)
        .set("status", status_)
        .set("age", (unsigned long) (tamer::drecent() - create_at_))
        .set("status_age", (unsigned long) (tamer::drecent() - status_change_at_))
        .set("npoll", npoll_)
        .set("nupdate", nupdate_)
        .set("npollers", npollers_);
}

inline bool Site::is_valid() const {
    double to = status_.empty() ? site_error_validate_timeout : site_validate_timeout;
    return status_at_ && tamer::drecent() - status_at_ < to;
}

inline bool Site::status_may_equal(const std::string& status) const {
    return !is_valid() || (!status.empty() && status_ == status);
}

tamed void Site::validate(tamer::event<> done) {
    tvars {
        tamer::http_parser hp(HTTP_RESPONSE);
        tamer::http_message req, res;
        tamer::fd cfd;
        Json j;
        bool opened_pollfd = false;
    }

    // is status already available?
    if (is_valid()) {
        done();
        return;
    }

    // is status already being checked?
    {
        bool checking = !!status_check_;
        status_check_ += std::move(done);
        if (checking)
            return;
    }

 reopen_pollfd:
    if (!pollfd || pollfd.socket_error()) {
        opened_pollfd = true;
        twait { tamer::tcp_connect(80, tamer::make_event(pollfd)); }
    }
    req.method(HTTP_GET)
        .url(path_)
        .header("Host", host_)
        .header("Connection", "keep-alive");
    twait { tamer::http_parser::send_request(pollfd, req,
                                             tamer::make_event()); }

    // parse response
    twait { hp.receive(pollfd, tamer::make_event(res)); }
    if (hp.ok() && res.ok()
        && (j = Json::parse(res.body()))
        && j["ok"]
        && j["tracker_status"].is_s()) {
        set_status(j["tracker_status"].to_s());
        log_msg() << url_ << ": status " << status();
    } else {
        pollfd.close();
        if (!opened_pollfd) {
            hp.clear();
            goto reopen_pollfd;
        }
        log_msg() << url_ << ": bad status " << res.body();
    }
    if (!hp.should_keep_alive())
        pollfd.close();
    status_at_ = tamer::drecent();
    status_check_();
}

bool check_conference(std::string arg, tamer::http_message& conf_m) {
    if (arg.empty())
        return false;
    conf_m.url(arg);
    if (conf_m.url_schema().empty()
        || conf_m.url_host().empty())
        return false;
    return true;
}



class Connection : public tamer::tamed_class {
 public:
    static Connection* add(tamer::fd cfd);
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
    int cfd_value_;
    double created_at_;
    tamer::event<> poll_event_;
    int state_;
    Connection* prev_;
    Connection* next_;

    static Connection* state_head[3];
    void set_state(int state);

    Connection(tamer::fd cfd);
    ~Connection();
    tamed void poll_handler(double timeout, tamer::event<> done);
    void update_handler();
    tamed void handler();
};

std::vector<Connection*> Connection::all;
Connection* Connection::state_head[3];

Connection::Connection(tamer::fd cfd)
    : cfd_(std::move(cfd)), hp_(HTTP_REQUEST),
      cfd_value_(cfd_.value()), created_at_(tamer::drecent()), state_(-1) {
    assert(cfd_.value() >= 0);
    if (all.size() <= (unsigned) cfd_.value())
        all.resize(cfd_.value() + 1, nullptr);
    assert(!all[cfd_.value()]);
    all[cfd_.value()] = this;
    ++nconnections;
}

Connection* Connection::add(tamer::fd cfd) {
    Connection* c = new Connection(std::move(cfd));
    c->handler();
    return c;
}

Connection::~Connection() {
    cfd_.close();
    all[cfd_value_] = nullptr;
    --nconnections;
    set_state(-1);
}

void Connection::set_state(int state) {
    assert(state >= -1 && state < 3);
    if (state != state_ && state_ != -1) {
        prev_->next_ = next_;
        next_->prev_ = prev_;
        if (state_head[state_] == this && next_ == this)
            state_head[state_] = nullptr;
        else if (state_head[state_] == this)
            state_head[state_] = next_;
    }
    if (state != state_ && state != -1) {
        if (state_head[state]) {
            prev_ = state_head[state]->prev_;
            next_ = state_head[state];
        } else
            prev_ = next_ = this;
        prev_->next_ = next_->prev_ = state_head[state] = this;
    }
    state_ = state;
}

void Connection::clear_one(Connection* dont_clear) {
    Connection* c = nullptr;
    if (state_head[s_request])
        c = state_head[s_request]->prev_;
    if ((!c || c == dont_clear || c->age() < 1)
        && state_head[s_poll])
        c = state_head[s_poll]->prev_;
    if (!c || c == dont_clear)
        /* do nothing */;
    else if (c->state_ == s_request) {
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
    Json j = Json().set("fd", cfd_.value())
        .set("age", (unsigned long) (tamer::drecent() - created_at_));
    if (state_ >= 0)
        j.set("state", stati[state_]);
    return j;
}

double poll_timeout(double timeout) {
    if (timeout <= 0 || timeout > min_poll_timeout) {
        double t = min_poll_timeout + drand48() * (max_poll_timeout - min_poll_timeout);
        if (timeout <= 0 || timeout > t)
            timeout = t;
    }
    return timeout;
}

tamed void Connection::poll_handler(double timeout, tamer::event<> done) {
    tvars {
        Site& site = make_site(req_.query("conference"));
        tamer::destroy_guard guard(&site);
        std::ostringstream buf;
        double timeout_at = tamer::drecent() + poll_timeout(timeout);
    }
    site.add_poller();
    while (cfd_ && tamer::drecent() < timeout_at && state_ == s_poll
           && site.status_may_equal(req_.query("poll")))
        twait {
            poll_event_ = tamer::add_timeout(timeout_at - tamer::drecent(),
                                             tamer::make_event());
            site.wait(req_.query("poll"), poll_event_);
        }
    if (!site.status().empty())
        buf << "{\"tracker_status\":\"" << site.status() << "\",\"ok\":true}";
    else
        buf << "{\"ok\":false}";
    res_.error(HPE_OK)
        .date_header("Date", tamer::recent().tv_sec)
        .header("Content-Type", "application/json")
        .header("Connection", "close")
        .body(buf.str());
    site.resolve_poller();
    done();
}

void Connection::update_handler() {
    Site& site = make_site(req_.query("conference"));
    site.add_update();
    site.set_status(req_.query("update"));
    res_.error(HPE_OK)
        .date_header("Date", tamer::recent().tv_sec)
        .header("Content-Type", "application/json")
        .header("Connection", "close")
        .body("{\"ok\":true}");
}

void hotcrp_comet_status_handler(tamer::http_message&, tamer::http_message& res) {
    Json j = Json().set("at", tamer::drecent())
        .set("at_time", timestamp_string(tamer::drecent()))
        .set("nconnections", nconnections)
        .set("connection_limit", connection_limit);

    Json sitesj = Json();
    for (auto it = sites.begin(); it != sites.end(); ++it)
        sitesj[it->url()] = it->status_json();
    j.set("sites", sitesj);

    Json connj = Json();
    for (auto it = Connection::all.begin(); it != Connection::all.end(); ++it)
        if (*it)
            connj.push_back((*it)->status_json());
    std::sort(connj.abegin(), connj.aend(), [](const Json& a, const Json& b) {
            return a.get("age").as_d() > b.get("age").as_d();
        });
    j.set("connections", connj);

    res.error(HPE_OK)
        .date_header("Date", tamer::recent().tv_sec)
        .header("Content-Type", "application/json")
        .body(j.unparse(Json::indent_depth(2)));
}

tamed void Connection::handler() {
    tvars {
        tamer::http_message confurl;
        double timeout = connection_timeout;
    }
    // log_msg() << "new connection " << status_json();
    while (cfd_) {
        req_.error(HPE_PAUSED);
        set_state(s_request);
        twait { hp_.receive(cfd_, tamer::add_timeout(timeout, tamer::make_event(req_))); }
        if (!hp_.ok() || !req_.ok())
            break;
        res_.error(HPE_PAUSED)
            .header("Access-Control-Allow-Origin", "*")
            .header("Access-Control-Allow-Credentials", "true")
            .header("Access-Control-Allow-Headers", "Accept-Encoding");
        if (req_.url_path() == "/hotcrp_comet_status")
            hotcrp_comet_status_handler(req_, res_);
        else if (check_conference(req_.query("conference"), confurl)) {
            if (!req_.query("poll").empty()) {
                set_state(s_poll);
                twait volatile {
                    Json j = Json::parse(req_.query("timeout"));
                    poll_handler(j.is_number() ? j.to_d() / 1000. : 0, tamer::make_event());
                }
            } else if (!req_.query("update").empty())
                update_handler();
        }
        if (!res_.ok() || !hp_.should_keep_alive())
            res_.header("Connection", "close");
        if (!res_.ok())
            res_.status_code(503);
        set_state(s_response);
        twait { hp_.send_response(cfd_, res_, tamer::make_event()); }
        if (!hp_.should_keep_alive())
            break;
        res_.clear();
        timeout = 5; // keep-alive timeout drops to 5sec
    }
    delete this;
}

tamed void listener() {
    tvars { tamer::fd cfd; }
    while (serverfd) {
        twait { serverfd.accept(tamer::make_event(cfd)); }
        if (cfd) {
            Connection* c = Connection::add(std::move(cfd));
            if (nconnections > connection_limit)
                Connection::clear_one(c);
        }
    }
}

tamed void catch_sigterm() {
    twait volatile { tamer::at_signal(SIGTERM, tamer::make_event()); }
    exit(1);
}

static const Clp_Option options[] = {
    { "fg", 0, 0, 0, 0 },
    { "log-file", 0, 0, Clp_ValString, 0 },
    { "pid-file", 0, 0, Clp_ValString, 0 },
    { "port", 'p', 0, Clp_ValInt, 0 },
    { "user", 'u', 0, Clp_ValString, 0 }
};

static void usage() {
    std::cerr << "Usage: hotcrp-comet [-p PORT] [--fg] [--pid-file PIDFILE]\n";
    exit(1);
}

static pid_t maybe_fork(bool dofork) {
    if (dofork) {
        pid_t pid = fork();
        if (pid == -1) {
            std::cerr << "fork: " << strerror(errno) << "\n";
            exit(1);
        } else if (pid > 0)
            return pid;
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        setpgid(0, 0);
        signal(SIGHUP, SIG_IGN);
    }
    return getpid();
}

static void set_userarg(const String& userarg) {
    int colon = userarg.find_left(':');
    String user, group;
    if (colon >= 0) {
        user = userarg.substr(0, colon);
        group = userarg.substr(colon + 1);
    } else
        user = group = userarg;
    struct passwd* pwent = nullptr;
    struct group* grent = nullptr;
    if (user && !(pwent = getpwnam(user.c_str())))
        std::cerr << "user " << user << " does not exist\n";
    if (group && !(grent = getgrnam(group.c_str())))
        std::cerr << "group " << group << " does not exist\n";
    if (grent && setgid(grent->gr_gid) != 0) {
        std::cerr << "setgid: " << strerror(errno) << "\n";
        grent = nullptr;
    }
    if (pwent && setuid(pwent->pw_uid) != 0) {
        std::cerr << "setuid: " << strerror(errno) << "\n";
        pwent = nullptr;
    }
    if ((user && !pwent) || (group && !grent))
        exit(1);
}

static void create_pid_file(pid_t pid, const char* pid_filename) {
    int pidfd = open(pid_filename, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    if (pidfd < 0) {
        std::cerr << pid_filename << ": " << strerror(errno) << "\n";
        exit(1);
    }
    pid_file = pid_filename;
    char buf[100];
    int buflen = sprintf(buf, "%ld\n", (long) pid);
    ssize_t nw = write(pidfd, buf, buflen);
    assert(nw == buflen);
    close(pidfd);
}

extern "C" {
static void exiter() {
    serverfd.close();
    if (pid_file)
        unlink(pid_file);
}
}

static void driver_error_handler(int, int err, std::string msg) {
    log_msg() << "tamer error: " << strerror(err) << ", " << msg;
}

int main(int argc, char** argv) {
    bool fg = false;
    int port = 20444;
    const char* pid_filename = nullptr;
    const char* log_filename = nullptr;
    String userarg;
    Clp_Parser* clp = Clp_NewParser(argc, argv, sizeof(options)/sizeof(options[0]), options);
    while (1) {
        int opt = Clp_Next(clp);
        if (Clp_IsLong(clp, "fg"))
            fg = true;
        else if (Clp_IsLong(clp, "port"))
            port = clp->val.i;
        else if (Clp_IsLong(clp, "pid-file"))
            pid_filename = clp->val.s;
        else if (Clp_IsLong(clp, "log-file"))
            log_filename = clp->val.s;
        else if (Clp_IsLong(clp, "user"))
            userarg = clp->val.s;
        else if (opt != Clp_Done)
            usage();
        else
            break;
    }

    tamer::initialize();
    tamer::driver::main->set_error_handler(driver_error_handler);
    serverfd = tamer::tcp_listen(port);
    if (!serverfd) {
        std::cerr << "listen: " << strerror(-serverfd.error()) << "\n";
        exit(1);
    }

    atexit(exiter);
    listener();
    catch_sigterm();

    std::ofstream logf_stream;
    if (log_filename) {
        logf_stream.open(log_filename, std::ofstream::app);
        if (logf_stream.bad()) {
            std::cerr << log_filename << ": " << strerror(errno) << "\n";
            exit(1);
        }
        logs = &logf_stream;
    } else if (fg)
        logs = &std::cerr;

    if (userarg)
        set_userarg(userarg);
    connection_limit = tamer::fd::open_limit();
    if (connection_limit > 100)
        connection_limit -= 20;
    else
        connection_limit = 0;

    pid_t pid = maybe_fork(!fg);

    if (pid_filename)
        create_pid_file(pid, pid_filename);
    if (pid != getpid())
        exit(0);

    tamer::loop();
    tamer::cleanup();
}
