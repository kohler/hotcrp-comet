// -*- mode: c++; c-basic-offset: 4 -*-
#include <tamer/tamer.hh>
#include <tamer/http.hh>
#include <tamer/channel.hh>
#include <fcntl.h>
#include <unordered_map>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <pwd.h>
#include <grp.h>
#include "clp.h"
#include "json.hh"
#if __linux__
# include <sys/inotify.h>
#endif

#define LOG_ERROR 0
#define LOG_NORMAL 1
#define LOG_DEBUG 2

#define MAX_LOGLEVEL LOG_DEBUG
#define MAX_NPOLLFDS 5

static double connection_timeout = 20;
static double site_validate_timeout = 120;
static double site_error_validate_timeout = 5;
static double min_poll_timeout = 240;
static double max_poll_timeout = 300;
static tamer::channel<tamer::fd> pollfds;
static tamer::fd serverfd;
static tamer::fd watchfd;
static unsigned nconnections;
static unsigned connection_limit;
static const char* pid_file = nullptr;
static std::ostream* logs;
static std::ostream* logerrs;
static int loglevel;
static std::vector<std::string> startup_fds;
static String update_token;

#define TIMESTAMP_FMT "%Y-%m-%d %H:%M:%S %z"

class log_msg {
 public:
    log_msg(int msglevel = LOG_NORMAL)
        : msglevel_(msglevel) {
        if (logs && msglevel <= loglevel && msglevel <= MAX_LOGLEVEL)
            logf_ = logs;
        else if (msglevel == LOG_ERROR)
            logf_ = logerrs;
        else
            logf_ = nullptr;
        if (logf_)
            add_prefix();
    }
    ~log_msg() {
        if (logf_) {
            std::string str = stream_.str();
            if (logerrs && msglevel_ == LOG_ERROR)
                *logerrs << str.substr(str.find(']') + 2) << std::endl;
            if (logf_ && (msglevel_ != LOG_ERROR || logf_ != logerrs))
                *logf_ << str << std::endl;
        }
    }
    template <typename T> log_msg& operator<<(T&& x) {
        if (logf_)
            stream_ << x;
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
        : url_(std::move(url)), create_at_(tamer::drecent()),
          status_at_(0), status_change_at_(0), status_seq_(0),
          pulse_at_(0),
          npoll_(0), nupdate_(0), npollers_(0) {
        if (url_.back() != '/')
            url_ += '/';
        tamer::http_message req;
        req.url(url_);
        host_ = req.url_host_port();
        path_ = req.url_path() + "api.php?fn=trackerstatus";
        if (update_token)
            path_ += "&token=" + std::string(update_token.encode_uri_component());
    }

    inline const std::string& url() const {
        return url_;
    }
    inline const std::string& status() const {
        return status_;
    }
    inline double status_seq() const {
        return status_seq_;
    }
    inline bool is_valid() const;

    static bool status_update_valid(const Json& j);
    bool set_status(const Json& j, bool is_update);

    inline double pulse_at() const {
        return pulse_at_;
    }
    inline void pulse() {
        pulse_at_ = tamer::drecent();
        status_change_();
    }

    tamed void validate(tamer::event<> done);

    void wait(const std::string& status, double status_seq,
              tamer::event<> done) {
        if (is_valid() && status_ == status)
            status_change_ += std::move(done);
        else if (is_valid() && status_seq_ >= status_seq)
            done();
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

  private:
    std::string url_;
    std::string host_;
    std::string path_;
    std::string status_;
    double create_at_;
    double status_at_;
    double status_change_at_;
    double status_seq_;
    double pulse_at_;
    tamer::event<> status_check_;
    tamer::event<> status_change_;
    unsigned long long npoll_;
    unsigned long long nupdate_;
    unsigned npollers_;
};

typedef std::unordered_map<std::string, Site> site_map_type;
site_map_type sites;

Site& make_site(const std::string& url) {
    auto it = sites.find(url);
    if (it == sites.end())
        it = sites.insert(std::make_pair(url, Site(url))).first;
    return it->second;
}

Json Site::status_json() const {
    Json j = Json().set("site", url_)
        .set("status", status_)
        .set("age", (unsigned long) (tamer::drecent() - create_at_))
        .set("status_age", (unsigned long) (tamer::drecent() - status_change_at_))
        .set("check_age", (unsigned long) (tamer::drecent() - status_at_))
        .set("npoll", npoll_)
        .set("nupdate", nupdate_)
        .set("npollers", npollers_);
    if (status_check_)
        j.set("status_check", true);
    return j;
}

inline bool Site::is_valid() const {
    double to = status_.empty() ? site_error_validate_timeout : site_validate_timeout;
    return status_at_ && tamer::drecent() - status_at_ < to;
}

bool Site::status_update_valid(const Json& j) {
    return j.is_o()
        && j["ok"]
        && (update_token.empty() || j["token"] == update_token)
        && j["tracker_status"].is_s()
        && (!j["tracker_status_at"] || j["tracker_status_at"].is_number());
}

bool Site::set_status(const Json& j, bool is_update) {
    String status1 = j["tracker_status"].to_s();
    std::string status(status1.data(), status1.length());
    double status_seq = j["tracker_status_at"].to_d();
    if (is_update)
        add_update();
    if (status_ != status
        && (!status_seq || status_seq > status_seq_)) {
        status_ = status;
        status_change_at_ = tamer::drecent();
        status_seq_ = status_seq;
        status_change_();
        log_msg() << url_ << ": tracker " << (is_update ? "update " : "status ") << status;
        return true;
    } else if (j["pulse"]) {
        pulse();
        return false;
    } else
        return false;
}

tamed void Site::validate(tamer::event<> done) {
    tvars {
        tamer::http_parser hp(HTTP_RESPONSE);
        tamer::http_message req, res;
        tamer::fd cfd;
        Json j;
        bool opened_pollfd = false;
    }

    // is status already being checked?
    {
        bool checking = !!status_check_;
        status_check_ += std::move(done);
        if (checking)
            return;
    }

    // get a polling file descriptor
    twait { pollfds.pop_front(tamer::make_event(cfd)); }

 reopen_pollfd:
    if (!cfd || cfd.socket_error()) {
        opened_pollfd = true;
        twait { tamer::tcp_connect(80, tamer::make_event(cfd)); }
        log_msg(LOG_DEBUG) << "fd " << cfd.recent_fdnum() << ": pollfd";
    }
    req.method(HTTP_GET)
        .url(path_)
        .header("Host", host_)
        .header("Connection", "keep-alive");
    twait { tamer::http_parser::send_request(cfd, req,
                                             tamer::make_event()); }

    // parse response
    twait { hp.receive(cfd, tamer::add_timeout(120, tamer::make_event(res))); }
    j = Json();
    if (hp.ok() && res.ok())
        j = Json::parse(res.body());
    if (j.is_o() && update_token)
        j.set("token", update_token); // always trust response
    if (status_update_valid(j))
        set_status(j, false);
    else {
        cfd.close();
        log_msg(LOG_DEBUG) << "fd " << cfd.recent_fdnum() << ": close";
        if (!opened_pollfd) {
            hp.clear();
            goto reopen_pollfd;
        }
        if (!hp.ok())
            log_msg() << "read " << url_ << ": error " << http_errno_name(hp.error());
        else
            log_msg() << "read " << url_ << ": bad tracker status " << res.body();
        set_status(Json(), false);
    }
    if (!hp.should_keep_alive() && cfd) {
        cfd.close();
        log_msg(LOG_DEBUG) << "fd " << cfd.recent_fdnum() << ": close";
    }
    status_at_ = tamer::drecent();
    status_check_();
    pollfds.push_front(std::move(cfd));
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
    Json resj_;
    int resj_indent_;
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
      created_at_(tamer::drecent()), state_(-1) {
    assert(cfd_.valid());
    log_msg(LOG_DEBUG) << "fd " << cfd_.fdnum() << ": connection";
    if (all.size() <= (unsigned) cfd_.fdnum())
        all.resize(cfd_.fdnum() + 1, nullptr);
    assert(!all[cfd_.fdnum()]);
    all[cfd_.fdnum()] = this;
    ++nconnections;
}

Connection* Connection::add(tamer::fd cfd) {
    Connection* c = new Connection(std::move(cfd));
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
    Json j = Json().set("fd", cfd_.fdnum())
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
        std::string poll_status = req_.query("poll");
        double start_at = tamer::drecent();
        double timeout_at = start_at + poll_timeout(timeout);
        double status_seq = 1;
    }
    if (!req_.query("tracker_status_at").empty()) {
        Json j = Json::parse(req_.query("tracker_status_at"));
        if (j.is_number())
            status_seq = j.to_d();
    }
    if (poll_status.empty())
        poll_status = site.status();
    site.add_poller();
    while (cfd_ && tamer::drecent() < timeout_at && state_ == s_poll
           && (site.status() == poll_status || site.status_seq() < status_seq)
           && site.pulse_at() < start_at)
        twait {
            poll_event_ = tamer::add_timeout(timeout_at - tamer::drecent(),
                                             tamer::make_event());
            site.wait(poll_status, status_seq, poll_event_);
            status_seq = 0;
        }
    if (!site.status().empty())
        resj_.set("ok", true).set("tracker_status", site.status())
            .set("tracker_status_at", site.status_seq());
    else
        resj_.set("ok", false);
    hp_.clear_should_keep_alive();
    site.resolve_poller();
    done();
}

void Connection::update_handler() {
    Json j = Json().set("ok", true)
        .set("tracker_status", req_.query("tracker_status"));
    if (!req_.query("tracker_status_at").empty())
        j.set("tracker_status_at", Json::parse(req_.query("tracker_status_at")));
    if (!req_.query("pulse").empty())
        j.set("pulse", Json::parse(req_.query("pulse")));
    if (!req_.query("token").empty())
        j.set("token", req_.query("token"));
    Site& site = make_site(req_.query("conference"));
    if (Site::status_update_valid(j)) {
        site.set_status(j, true);
        resj_.set("ok", true);
    } else
        resj_.set("ok", false).set("error", "invalid status update");
    hp_.clear_should_keep_alive();
}

void hotcrp_comet_status_handler(Json& resj) {
    resj.set("at", tamer::drecent())
        .set("at_time", timestamp_string(tamer::drecent()))
        .set("nconnections", nconnections)
        .set("connection_limit", connection_limit);

    Json sitesj = Json();
    for (auto it = sites.begin(); it != sites.end(); ++it)
        sitesj[it->first] = it->second.status_json();
    resj.set("sites", sitesj);

    Json connj = Json();
    for (auto it = Connection::all.begin(); it != Connection::all.end(); ++it)
        if (*it)
            connj.push_back((*it)->status_json());
    std::sort(connj.abegin(), connj.aend(), [](const Json& a, const Json& b) {
            return a.get("age").as_d() > b.get("age").as_d();
        });
    resj.set("connections", connj);
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
        if (!hp_.ok() || !req_.ok()) {
            if (!hp_.ok() && hp_.error() != HPE_INVALID_EOF_STATE)
                log_msg(LOG_DEBUG) << "fd " << cfd_.recent_fdnum() << ": request fail " << http_errno_name(hp_.error());
            break;
        }
        res_.error(HPE_PAUSED)
            .header("Access-Control-Allow-Origin", "*")
            .header("Access-Control-Allow-Credentials", "true")
            .header("Access-Control-Allow-Headers", "Accept-Encoding")
            .header("Expires", "Mon, 26 Jul 1997 05:00:00 GMT");
        resj_.clear();
        resj_indent_ = 0;
        if (req_.url_path() == "/status") {
            hotcrp_comet_status_handler(resj_);
            resj_indent_ = 2;
        } else if (check_conference(req_.query("conference"), confurl)) {
            if (!req_.query("poll").empty()) {
                set_state(s_poll);
                twait volatile {
                    Json j = Json::parse(req_.query("timeout"));
                    poll_handler(j.is_number() ? j.to_d() / 1000. : 0, tamer::make_event());
                }
            } else if (!req_.query("tracker_status").empty())
                update_handler();
        } else
            resj_.set("ok", false).set("error", "missing `conference`");
        res_.error(HPE_OK).date_header("Date", tamer::recent().tv_sec)
            .header("Content-Type", "application/json");
        if (resj_["ok"] || !hp_.should_keep_alive())
            res_.header("Connection", "close");
        // if (!res_.ok())
        //    res_.status_code(503);
        res_.body(resj_.unparse(Json::indent_depth(resj_indent_)));
        set_state(s_response);
        twait { hp_.send_response(cfd_, res_, tamer::make_event()); }
        if (!hp_.should_keep_alive())
            break;
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

tamed void listener() {
    tvars { tamer::fd cfd; }
    record_startup_fd(serverfd.fdnum(), "listen");
    while (serverfd) {
        twait { serverfd.accept(tamer::make_event(cfd)); }
        if (cfd) {
            Connection* c = Connection::add(std::move(cfd));
            if (nconnections > connection_limit)
                Connection::clear_one(c);
        }
    }
}

#if __linux__
typedef struct inotify_event ievent;

tamed void directory_watcher(const char* update_directory) {
    tvars {
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
        if (e != 0)
            log_msg() << update_directory << ": " << strerror(-e);
        for (size_t off = 0; off < r; ) {
            ievent* x = reinterpret_cast<ievent*>(&buf[off]);
            int ffd = openat(dirfd, x->name, O_RDONLY);
            if (ffd >= 0) {
                StringAccum sa;
                ssize_t r;
                while ((r = read(ffd, sa.reserve(8192), 8192)) > 0)
                    sa.adjust_length(r);
                Json j = Json::parse(sa.take_string());
                if (j && j["conference"].is_s() && Site::status_update_valid(j)) {
                    Site& site = make_site(j["conference"].to_s());
                    site.set_status(j, true);
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
    { "token", 't', 0, Clp_ValString, 0 },
    { "update-directory", 0, 0, Clp_ValString, 0 },
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
            log_msg(LOG_ERROR) << "fork: " << strerror(errno);
            exit(1);
        } else if (pid > 0)
            return pid;
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
    } else
        user = group = userarg;
    struct passwd* pwent = nullptr;
    struct group* grent = nullptr;
    if (user && !(pwent = getpwnam(user.c_str())))
        log_msg(LOG_ERROR) << "user " << user << " does not exist";
    if (group && !(grent = getgrnam(group.c_str())))
        log_msg(LOG_ERROR) << "group " << group << " does not exist";
    if (grent && setgid(grent->gr_gid) != 0) {
        log_msg(LOG_ERROR) << "setgid: " << strerror(errno);
        grent = nullptr;
    }
    if (pwent && setuid(pwent->pw_uid) != 0) {
        log_msg(LOG_ERROR) << "setuid: " << strerror(errno);
        pwent = nullptr;
    }
    if ((user && !pwent) || (group && !grent))
        exit(1);
}

static void create_pid_file(pid_t pid, const char* pid_filename) {
    int pidfd = open(pid_filename, O_WRONLY | O_CREAT | O_TRUNC, 0660);
    if (pidfd < 0) {
        log_msg(LOG_ERROR) << pid_filename << ": " << strerror(errno);
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
    watchfd.close();
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
    int nfiles = 0;
    bool nfiles_set = false;
    const char* pid_filename = nullptr;
    const char* log_filename = nullptr;
    const char* update_directory = nullptr;
    String userarg;
    Clp_Parser* clp = Clp_NewParser(argc, argv, sizeof(options)/sizeof(options[0]), options);
    while (1) {
        int opt = Clp_Next(clp);
        if (Clp_IsLong(clp, "fg"))
            fg = true;
        else if (Clp_IsLong(clp, "nfiles")) {
            nfiles = clp->val.i;
            nfiles_set = true;
        } else if (Clp_IsLong(clp, "port"))
            port = clp->val.i;
        else if (Clp_IsLong(clp, "pid-file"))
            pid_filename = clp->val.s;
        else if (Clp_IsLong(clp, "log-file") || Clp_IsLong(clp, "log"))
            log_filename = clp->val.s;
        else if (Clp_IsLong(clp, "log-level"))
            loglevel = std::max(0, std::min(MAX_LOGLEVEL, clp->val.i));
        else if (Clp_IsLong(clp, "token"))
            update_token = clp->val.s;
        else if (Clp_IsLong(clp, "update-directory"))
            update_directory = clp->val.s;
        else if (Clp_IsLong(clp, "user"))
            userarg = clp->val.s;
        else if (opt != Clp_Done)
            usage();
        else
            break;
    }

    tamer::initialize();
    tamer::driver::main->set_error_handler(driver_error_handler);
    atexit(exiter);
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
    if (logs != &std::cerr)
        logerrs = &std::cerr;

    serverfd = tamer::tcp_listen(port);
    if (!serverfd) {
        log_msg(LOG_ERROR) << "listen: " << strerror(-serverfd.error());
        exit(1);
    }
    listener();

    if (update_directory)
        directory_watcher(update_directory);

    if (userarg)
        set_userarg(userarg);

    if (nfiles_set) {
        int r = tamer::fd::open_limit(nfiles <= 0 ? INT_MAX : nfiles);
        if (r < nfiles)
            log_msg(LOG_ERROR) << "hotcrp-comet: limited to " << r << " open files";
    }
    connection_limit = tamer::fd::open_limit();
    if (connection_limit > 100)
        connection_limit -= 20;
    else
        connection_limit = 0;

    pid_t pid = maybe_fork(!fg);

    if (pid != getpid())
        exit(0);
    if (pid_filename)
        create_pid_file(pid, pid_filename);
    log_msg() << "hotcrp-comet started, pid " << pid;
    for (auto m : startup_fds)
        log_msg(LOG_DEBUG) << m;
    while (pollfds.size() != MAX_NPOLLFDS)
        pollfds.push_back(tamer::fd());

    tamer::loop();
    tamer::cleanup();
}
