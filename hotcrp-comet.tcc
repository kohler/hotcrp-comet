// -*- mode: c++ -*-
#include <tamer/tamer.hh>
#include <tamer/http.hh>
#include <fcntl.h>
#include <unordered_set>
#include <pwd.h>
#include <grp.h>
#include "clp.h"
#include "json.hh"

static double connection_timeout = 20;
static double site_validate_timeout = 120;
static double site_error_validate_timeout = 5;
static tamer::fd pollfd;
static tamer::fd serverfd;
static unsigned counter;
static const char* pid_file = nullptr;

class Site : public tamer::tamed_class {
  public:
    explicit Site(std::string url)
        : url_(std::move(url)), status_at_(0) {
    }

    inline const std::string& url() const {
        return url_;
    }
    inline const std::string& status() const {
        return status_;
    }
    void set_status(const std::string& status) {
        if (status_ != status) {
            status_ = status;
            status_change_();
        }
    }
    inline void set_status(const String& status) {
        set_status(std::string(status.data(), status.length()));
    }

    tamed void validate(tamer::event<> done);

    void wait(const std::string& status, tamer::event<> done) {
        if (status_ == status)
            status_change_ += std::move(done);
        else
            done();
    }

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
    double status_at_;
    tamer::event<> status_check_;
    tamer::event<> status_change_;
};

std::unordered_set<Site, Site::hasher, Site::comparator> sites;

Site& make_site(const std::string& url) {
    return const_cast<Site&>(*sites.emplace(url).first);
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
    if (status_at_
        && (status_.empty()
            ? tamer::drecent() - status_at_ < site_error_validate_timeout
            : tamer::drecent() - status_at_ < site_validate_timeout)) {
        done();
        return;
    } else {
        bool checking = !!status_check_;
        status_check_ += std::move(done);
        if (checking)
            return;
    }

    // send request
    if (path_.empty()) {
        req.url(url_);
        host_ = req.url_host_port();
        path_ = req.url_path() + "deadlines.php?checktracker=1&ajax=1";
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
        std::cerr << url_ << ": status " << status() << "\n";
    } else {
        pollfd.close();
        if (!opened_pollfd) {
            hp.clear();
            goto reopen_pollfd;
        }
        std::cerr << url_ << ": bad status " << res.body() << "\n";
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

void update_handler(tamer::http_message& req, tamer::http_message& res,
                    unsigned) {
    Site& site = make_site(req.query("conference"));
    site.set_status(req.query("update"));
    res.error(HPE_OK)
        .date_header("Date", tamer::recent().tv_sec)
        .header("Content-Type", "application/json")
        .header("Connection", "close")
        .body("{\"ok\":true}");
}

tamed void poll_handler(tamer::http_message& req, tamer::http_message& res,
                        tamer::fd cfd, unsigned c, tamer::event<> done) {
    tvars {
        Site& site = make_site(req.query("conference"));
        tamer::destroy_guard guard(&site);
        std::ostringstream buf;
    }
    while (cfd) {
        twait { site.validate(tamer::make_event()); }
        if (req.query("poll") != site.status())
            break;
        twait { site.wait(req.query("poll"), tamer::make_event()); }
    }
    if (!site.status().empty())
        buf << "{\"tracker_status\":\"" << site.status() << "\",\"ok\":true}";
    else
        buf << "{\"ok\":false}";
    res.error(HPE_OK)
        .date_header("Date", tamer::recent().tv_sec)
        .header("Content-Type", "application/json")
        .header("Connection", "close")
        .body(buf.str());
    done();
}

tamed void connection(tamer::fd cfd) {
    tvars {
        tamer::http_parser hp(HTTP_REQUEST);
        tamer::http_message req, res, confurl;
        unsigned c = ++counter;
    }
    res.http_minor(0).error(HPE_PAUSED)
        .header("Connection", "close")
        .header("Access-Control-Allow-Origin", "*")
        .header("Access-Control-Allow-Credentials", "true")
        .header("Access-Control-Allow-Headers", "Accept-Encoding");
    twait { hp.receive(cfd, tamer::add_timeout(connection_timeout,
                                               tamer::make_event(req))); }
    if (hp.ok()
        && req.ok()
        && check_conference(req.query("conference"), confurl)) {
        if (!req.query("poll").empty())
            twait volatile { poll_handler(req, res, cfd, c, tamer::make_event()); }
        else if (!req.query("update").empty())
            update_handler(req, res, c);
    }
    if (!res.ok())
        res.status_code(503);
    twait { tamer::http_parser::send_response(cfd, res, tamer::make_event()); }
    cfd.close();
}

tamed void listener() {
    tvars { tamer::fd cfd; }
    while (serverfd) {
        twait { serverfd.accept(tamer::make_event(cfd)); }
        connection(std::move(cfd));
    }
}

tamed void catch_sigterm() {
    twait volatile { tamer::at_signal(SIGTERM, tamer::make_event()); }
    exit(1);
}

static const Clp_Option options[] = {
    { "fg", 0, 0, 0, 0 },
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

int main(int argc, char** argv) {
    bool fg = false;
    int port = 20444;
    const char* pid_filename = nullptr;
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
        else if (Clp_IsLong(clp, "user"))
            userarg = clp->val.s;
        else if (opt != Clp_Done)
            usage();
        else
            break;
    }

    tamer::initialize();
    serverfd = tamer::tcp_listen(port);
    if (!serverfd) {
        std::cerr << "listen: " << strerror(-serverfd.error()) << "\n";
        exit(1);
    }

    atexit(exiter);
    listener();
    catch_sigterm();
    if (userarg)
        set_userarg(userarg);
    pid_t pid = maybe_fork(!fg);
    if (pid_filename)
        create_pid_file(pid, pid_filename);
    if (pid != getpid())
        exit(0);
    tamer::loop();
    tamer::cleanup();
}
