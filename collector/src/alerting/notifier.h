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

    // Serializes the notification as a JSON object.
    std::string ToJson() const;
};

// Channel for alert notifications. Implementations must be thread-safe.
class Notifier {
public:
    virtual ~Notifier() = default;
    virtual void Notify(const AlertNotification &alert) = 0;
    virtual const char *Name() const = 0;
};

// Writes alerts to stdout as structured JSON log lines.
class LogNotifier final : public Notifier {
public:
    void Notify(const AlertNotification &alert) override;
    const char *Name() const override { return "log"; }
};

// Posts alert JSON to a webhook URL.
class WebhookNotifier final : public Notifier {
public:
    explicit WebhookNotifier(std::string url, int timeout_sec = 5);
    void Notify(const AlertNotification &alert) override;
    const char *Name() const override { return "webhook"; }

private:
    std::string m_url;
    int m_timeout_sec;
};

// Creates a notifier. An empty string or "log" selects LogNotifier.
std::unique_ptr<Notifier> MakeNotifier(const std::string &url);

} // namespace pudimcollector::alerting
