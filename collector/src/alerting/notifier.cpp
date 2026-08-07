#include <chrono>
#include <iostream>

#include "httplib.h"

#include "notifier.h"

namespace pudimcollector::alerting {

namespace {

// Produces a compact decimal string for a double ("12.5", not "12.500000").
std::string FormatDouble(double v) {
    if (v == static_cast<long long>(v)) {
        return std::to_string(static_cast<long long>(v));
    }
    std::string s = std::to_string(v);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string EscapeJson(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

void WriteField(std::string &json, const char *name, const std::string &value,
                bool first) {
    if (!first) json += ",";
    json += "\"";
    json += name;
    json += "\":\"";
    json += EscapeJson(value);
    json += "\"";
}

} // anonymous namespace

std::string AlertNotification::ToJson() const {
    std::string json = "{";
    WriteField(json, "rule_id", rule_id, true);
    WriteField(json, "rule_name", rule_name, false);
    WriteField(json, "severity", severity, false);
    WriteField(json, "agent_id", agent_id, false);
    WriteField(json, "check_type", check_type, false);
    WriteField(json, "target", target, false);
    WriteField(json, "status", status, false);
    WriteField(json, "detail", detail, false);
    json += ",\"value\":" + FormatDouble(value);
    json += ",\"threshold\":" + FormatDouble(threshold);
    json += ",\"time_ms\":" + std::to_string(time_ms);
    json += "}";
    return json;
}

void LogNotifier::Notify(const AlertNotification &alert) {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    // Structured log line with an "alert" level so it stands out from
    // info/warn/error in log pipelines.
    std::cout << "{\"timestamp\":" << now
              << ",\"level\":\"alert\""
              << ",\"component\":\"collector\""
              << ",\"message\":\"alert notification\""
              << ",\"payload\":" << alert.ToJson() << "}" << std::endl;
}

WebhookNotifier::WebhookNotifier(std::string url, int timeout_sec)
    : m_url(std::move(url)), m_timeout_sec(timeout_sec) {}

void WebhookNotifier::Notify(const AlertNotification &alert) {
    std::string body = alert.ToJson();

    // Split scheme://host[:port]/path so we can use httplib's client.
    std::string scheme = "http";
    std::string host_and_path = m_url;
    auto scheme_pos = m_url.find("://");
    if (scheme_pos != std::string::npos) {
        scheme = m_url.substr(0, scheme_pos);
        host_and_path = m_url.substr(scheme_pos + 3);
    }

    auto slash = host_and_path.find('/');
    std::string host = host_and_path;
    std::string path = "/";
    if (slash != std::string::npos) {
        host = host_and_path.substr(0, slash);
        path = host_and_path.substr(slash);
    }

    httplib::Client client(host);
    client.set_connection_timeout(m_timeout_sec, 0);
    client.set_read_timeout(m_timeout_sec, 0);

    httplib::Headers headers = {{"Content-Type", "application/json"},
                                {"User-Agent", "pudim-collector/0.1"}};

    httplib::Result res;
    if (scheme == "https") {
        res = client.Post(path, headers, body, "application/json");
    } else {
        res = client.Post(path, headers, body, "application/json");
    }

    if (!res) {
        std::cerr << "Webhook notify failed for " << m_url << ": "
                  << httplib::to_string(res.error()) << "\n";
        return;
    }
    if (res->status < 200 || res->status >= 300) {
        std::cerr << "Webhook notify returned HTTP " << res->status
                  << " for " << m_url << "\n";
    }
}

std::unique_ptr<Notifier> MakeNotifier(const std::string &url) {
    if (url.empty() || url == "log") {
        return std::make_unique<LogNotifier>();
    }
    return std::make_unique<WebhookNotifier>(url);
}

} // namespace pudimcollector::alerting
