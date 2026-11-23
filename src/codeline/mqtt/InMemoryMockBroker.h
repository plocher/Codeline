// A synchronous in-memory MQTT broker for scenarios: exact-topic
// subscriptions (the bindings use no wildcards), retained messages, clean
// sessions, last will, link loss, an unreachable client, and a broker
// restart with or without a persistent retained store. No network. Delivery happens inside
// publish(), so a scenario sees the whole exchange before publish() returns.
// Heap use is fine here; this is test equipment.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "codeline/mqtt/IMqttClient.h"

namespace codeline::mqtt {

class InMemoryMockBroker {
public:
  struct Message {
    std::string topic;
    std::string payload;
    bool retained;
  };

  class Client final : public IMqttClient {
  public:
    explicit Client(InMemoryMockBroker& broker) : broker_(broker) { broker_.clients_.push_back(this); }
    ~Client() override {
      broker_.endSession(*this, false);
      for (size_t i = 0; i < broker_.clients_.size(); ++i) {
        if (broker_.clients_[i] == this) broker_.clients_.erase(broker_.clients_.begin() + static_cast<long>(i));
      }
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Fails (connected() stays false) while the broker is down or this
    // client cannot reach it.
    void connect(std::string_view client_id, const MqttWill* will) override {
      broker_.endSession(*this, false);
      ++connect_attempts_;
      if (!broker_.up_ || !reachable_) return;
      id_ = std::string(client_id);
      has_will_ = will != nullptr;
      if (will) will_ = Message{std::string(will->topic), std::string(will->payload), will->retained};
      connected_ = true;
    }
    void disconnect() override { broker_.endSession(*this, false); }
    bool connected() const override { return connected_; }
    void publish(std::string_view topic, std::string_view payload, bool retained) override {
      if (connected_) broker_.publish(topic, payload, retained);
    }
    void subscribe(std::string_view topic, IMqttHandler* handler) override {
      if (connected_) broker_.subscribe(*this, topic, handler);
    }
    void setLinkObserver(IMqttLinkObserver* observer) override { link_observer_ = observer; }
    // Test observation: how many times connect() was called.
    uint32_t connectAttempts() const { return connect_attempts_; }

  private:
    friend class InMemoryMockBroker;
    InMemoryMockBroker& broker_;
    std::string id_;
    bool connected_ = false;
    bool reachable_ = true;
    uint32_t connect_attempts_ = 0;
    bool has_will_ = false;
    Message will_;
    IMqttLinkObserver* link_observer_ = nullptr;
  };

  // Test control: the client vanishes without DISCONNECT (power loss, Wi-Fi
  // drop). The broker publishes its will; the client learns its link broke.
  void dropConnection(Client& c) {
    if (!c.connected_) return;
    endSession(c, true);
    if (c.link_observer_) c.link_observer_->onConnectionLost();
  }

  // Test control: whether `c` can reach the broker. An unreachable client's
  // connect() fails; combine with dropConnection() for an outage.
  void setReachable(Client& c, bool reachable) { c.reachable_ = reachable; }

  // Test control: the broker process dies. Every session ends without its
  // will (the broker that would publish it is gone), every client learns its
  // link broke, and connect() fails until comeUp(). keep_retained = false is
  // a broker without persistence: the retained store is empty afterwards.
  void goDown(bool keep_retained) {
    up_ = false;
    const std::vector<Client*> clients = clients_;
    for (Client* c : clients) {
      if (!c->connected_) continue;
      endSession(*c, false);
      if (c->link_observer_) c->link_observer_->onConnectionLost();
    }
    if (!keep_retained) retained_.clear();
  }
  void comeUp() { up_ = true; }

  // Test control: the retained store is lost (a broker restarted without
  // persistence while this client was away).
  void clearRetained() { retained_.clear(); }

  const std::string* retained(std::string_view topic) const {
    for (const Message& m : retained_) {
      if (m.topic == topic) return &m.payload;
    }
    return nullptr;
  }

  const std::vector<Message>& log() const { return log_; }

private:
  struct Subscription {
    Client* client;
    std::string topic;
    IMqttHandler* handler;
  };

  std::vector<Client*> clients_;
  bool up_ = true;
  std::vector<Subscription> subs_;
  std::vector<Message> retained_;
  std::vector<Message> log_;

  void publish(std::string_view topic, std::string_view payload, bool retained) {
    log_.push_back(Message{std::string(topic), std::string(payload), retained});
    if (retained) storeRetained(topic, payload);
    // Copy first: a handler may publish (nested delivery) while we iterate.
    const std::string t(topic), p(payload);
    for (size_t i = 0; i < subs_.size(); ++i) {
      if (subs_[i].topic == t) subs_[i].handler->onMessage(t, p, retained);
    }
  }

  void storeRetained(std::string_view topic, std::string_view payload) {
    for (size_t i = 0; i < retained_.size(); ++i) {
      if (retained_[i].topic == topic) {
        if (payload.empty()) {
          retained_.erase(retained_.begin() + static_cast<long>(i));
        } else {
          retained_[i].payload = std::string(payload);
        }
        return;
      }
    }
    if (!payload.empty()) retained_.push_back(Message{std::string(topic), std::string(payload), true});
  }

  void subscribe(Client& c, std::string_view topic, IMqttHandler* handler) {
    subs_.push_back(Subscription{&c, std::string(topic), handler});
    const std::vector<Message> snapshot = retained_;
    for (const Message& m : snapshot) {
      if (m.topic == topic) handler->onMessage(m.topic, m.payload, true);
    }
  }

  void endSession(Client& c, bool publish_will) {
    if (!c.connected_) return;
    c.connected_ = false;
    for (size_t i = subs_.size(); i-- > 0;) {
      if (subs_[i].client == &c) subs_.erase(subs_.begin() + static_cast<long>(i));
    }
    if (publish_will && c.has_will_) publish(c.will_.topic, c.will_.payload, c.will_.retained);
  }
};

}  // namespace codeline::mqtt
