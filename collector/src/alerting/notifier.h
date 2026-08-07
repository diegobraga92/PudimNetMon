#pragma once

#include <memory>
#include <string>

namespace pudimcollector::alerting {

// Immutable snapshot of an alert event handed to notifiers.
struct AlertNotification {
    std::string rule_id;
    std::string rule_name;
    std::string severity;
    std::string agent_id;
    std::string check_type;
    std::string target;
    std::string status;     // "firing" | "resolved"
    double value = 0.0;
    double threshold = 0.0;
    std::string detail;
    int64_t time_ms = 0;

    // Serializes this notification as JSON (no surrounding braces removed; a
    // complete JSON object string).
    std::string ToJson() const;
};

// Channel for alert notifications. Implementations must be thread-safe.
class Notifier {
public:
    virtual ~Notifier() = default;
    virtual void Notify(const AlertNotification &alert) = 0;
    virtual const char *Name() const = 0;
};

// Writes alerts as structured JSON log lines to stdout (always enabled).
class LogNotifier final : public Notifier {
public:
    void Notify(const AlertNotification &alert) override;
    const char *Name() const override { return "log"; }
};

// POSTs alert JSON to a webhook URL (Slack/Discord/mock incident service).
// Uses httplib::Client, which is already a collector dependency.
class WebhookNotifier final : public Notifier {
public:
    explicit WebhookNotifier(std::string url, int timeout_sec = 5);
    void Notify(const AlertNotification &alert) override;
    const char *Name() const override { return "webhook"; }

private:
    std::string m_url;
    int m_timeout_sec;
};

// Creates the appropriate notifier from a URL ("" or "log" → LogNotifier).
std::unique_ptr<Notifier> MakeNotifier(const std::string &url);

} // namespace pudimcollector::alerting
