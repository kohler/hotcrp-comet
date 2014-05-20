// -*- mode: c++ -*-
#include <tamer/tamer.hh>
#include <tamer/http.hh>
#include <unordered_set>
#include "json.hh"

static double connection_timeout = 20;
static double site_validate_timeout = 120;
static double site_error_validate_timeout = 5;
static tamer::fd pollfd;
static unsigned counter;

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
    }

    if (status_at_
        && (status_.empty()
            ? tamer::drecent() - status_at_ < site_error_validate_timeout
            : tamer::drecent() - status_at_ < site_validate_timeout)) {
        done();
        return;
    } else if (status_check_) {
        status_check_ += std::move(done);
        return;
    }

    status_check_ = std::move(done);
    if (path_.empty()) {
        req.url(url_);
        host_ = req.url_host_port();
        path_ = req.url_path() + "deadlines.php?checktracker=1&ajax=1";
    }
    std::cerr<< "connecting\n";
    if (!pollfd || pollfd.socket_error())
        twait { tamer::tcp_connect(80, tamer::make_event(pollfd)); }
    std::cerr<< "connected\n";
    req.method(HTTP_GET)
        .url(path_)
        .header("Host", host_)
        .header("Connection", "keep-alive");
    twait { tamer::http_parser::send_request(pollfd, req,
                                             tamer::make_event()); }
    twait { hp.receive(pollfd, tamer::make_event(res)); }
    if (hp.ok() && res.ok()
        && (j = Json::parse(res.body()))
        && j["ok"] && j["tracker_status"].is_s())
        set_status(j["tracker_status"].to_s());
    else
        std::cerr << "bad status " << j << "\n";
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
        std::cerr << c << "/" << cfd.value() << ": validated\n";
        if (req.query("poll") != site.status())
            break;
        twait { site.wait(req.query("poll"), tamer::make_event()); }
        std::cerr << c << "/" << cfd.value() << ": polled\n";
    }
    buf << "{\"tracker_status\":\"" << site.status() << "\",\"ok\":true}";
    res.error(HPE_OK)
        .date_header("Date", tamer::recent().tv_sec)
        .header("Content-Type", "text/plain")
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
    std::cerr << c << "/" << cfd.value() << ": start\n";
    res.http_minor(0).error(HPE_PAUSED)
        .header("Connection", "close")
        .header("Access-Control-Allow-Origin", "*")
        .header("Access-Control-Allow-Credentials", "true")
        .header("Access-Control-Allow-Headers", "Accept-Encoding");
    twait { hp.receive(cfd, tamer::add_timeout(connection_timeout,
                                               tamer::make_event(req))); }
    std::cerr << c << "/" << cfd.value() << ": " << req.url() << "\n";
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
    std::cerr << c << "/" << cfd.value() << ": " << res.status_code() << " " << res.body() << "\n";
    twait { tamer::http_parser::send_response(cfd, res, tamer::make_event()); }
    std::cerr << c << "/" << cfd.value() << ": close\n";
    cfd.close();
}

tamed void listener(int port) {
    tvars {
        tamer::fd sfd = tamer::tcp_listen(port);
        tamer::fd cfd;
    }
    while (sfd) {
        twait { sfd.accept(tamer::make_event(cfd)); }
        connection(std::move(cfd));
    }
}

int main(int, char**) {
    tamer::initialize();
    listener(20444);
    tamer::loop();
    tamer::cleanup();
}
