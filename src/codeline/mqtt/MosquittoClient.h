// IMqttClient over libmosquitto, for the host on a real broker. Link with
// -lmosquitto. Single-threaded: the owner calls pump() often (the host loop
// does, every tick). Nothing here runs on a thread of its own.
//
//   - MQTT 5, clean start, session expiry 0: the broker keeps nothing for a
//     client that is away (CodeLineMQTT.md 5 rule 1 "Clean start").
//   - Subscriptions use retain-as-published, so a handler's `retained` is the
//     publisher's flag for a live message and true for a stored one, as
//     IMqttClient.h promises.
//   - QoS of a publish (IMqttClient carries none, and the appliance binding
//     states none): a retained publish goes at QoS 1 (indications, appliance
//     /state), an unretained one at QoS 0 (controls, /cmd, /maint, /event).
//     subscribe() asks for QoS 1 (a QoS 0 publish is still delivered at QoS 0);
//     subscribeAtQos() asks for the QoS a binding states (the code line field's
//     controls: QoS 0, CodeLineMQTT.md 5 rule 1).
//   - Incoming messages are queued by libmosquitto's callback and handed to
//     handlers by pump(), outside the callback. A handler that publishes
//     (an adapter answering a command) is then written at once, in program
//     order, rather than whenever libmosquitto next writes from a callback.
//   - A broken connection (not one disconnect() asked for) is reported once
//     through the link observer; there is no automatic reconnect.
#pragma once

#include <mosquitto.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "codeline/mqtt/IMqttClient.h"

namespace codeline::mqtt {

class MosquittoClient final : public IMqttClient {
public:
  MosquittoClient(std::string host, int port) : host_(std::move(host)), port_(port) {
    static const int init = mosquitto_lib_init();
    (void)init;
  }
  ~MosquittoClient() override {
    disconnect();
    if (mosq_) mosquitto_destroy(mosq_);
  }
  MosquittoClient(const MosquittoClient&) = delete;
  MosquittoClient& operator=(const MosquittoClient&) = delete;

  // Blocks until the broker accepts the connection, or kConnectTimeoutMs.
  void connect(std::string_view client_id, const MqttWill* will) override {
    if (mosq_) {
      disconnect();
      mosquitto_destroy(mosq_);
    }
    handlers_.clear();
    inbox_.clear();
    connected_ = lost_ = false;
    connack_ = -1;
    id_ = std::string(client_id);
    mosq_ = mosquitto_new(id_.c_str(), true, this);
    if (!mosq_) return;
    mosquitto_int_option(mosq_, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);
    mosquitto_connect_callback_set(mosq_, &MosquittoClient::onConnect);
    mosquitto_disconnect_callback_set(mosq_, &MosquittoClient::onDisconnect);
    mosquitto_message_callback_set(mosq_, &MosquittoClient::onMessageCb);
    if (will) {
      const std::string t(will->topic), p(will->payload);
      mosquitto_will_set_v5(mosq_, t.c_str(), static_cast<int>(p.size()), p.data(), will->retained ? 1 : 0,
                            will->retained, nullptr);
    }
    if (mosquitto_connect(mosq_, host_.c_str(), port_, kKeepaliveS) != MOSQ_ERR_SUCCESS) {
      std::fprintf(stderr, "mqtt %s: cannot connect to %s:%d\n", id_.c_str(), host_.c_str(), port_);
      return;
    }
    const auto start = std::chrono::steady_clock::now();
    while (connack_ < 0 && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(kConnectTimeoutMs)) {
      if (mosquitto_loop(mosq_, 20, 1) != MOSQ_ERR_SUCCESS) break;
    }
    connected_ = connack_ == 0;
    if (!connected_) std::fprintf(stderr, "mqtt %s: broker refused or did not answer\n", id_.c_str());
  }

  // Clean: DISCONNECT is sent, so the broker does not publish the will.
  void disconnect() override {
    if (!mosq_ || !connected_) return;
    connected_ = false;
    mosquitto_disconnect(mosq_);
    mosquitto_loop(mosq_, 0, 1);
    handlers_.clear();
  }

  bool connected() const override { return connected_; }
  bool lost() const { return lost_; }
  const std::string& id() const { return id_; }

  void publish(std::string_view topic, std::string_view payload, bool retained) override {
    if (!connected_) return;
    const std::string t(topic);
    mosquitto_publish_v5(mosq_, nullptr, t.c_str(), static_cast<int>(payload.size()), payload.data(), retained ? 1 : 0,
                         retained, nullptr);
  }

  void subscribe(std::string_view topic, IMqttHandler* handler) override { subscribeAtQos(topic, handler, 1); }

  void subscribeAtQos(std::string_view topic, IMqttHandler* handler, uint8_t qos) override {
    if (!connected_) return;
    handlers_.emplace_back(std::string(topic), handler);
    mosquitto_subscribe_v5(mosq_, nullptr, handlers_.back().first.c_str(), qos > 1 ? 1 : qos,
                           MQTT_SUB_OPT_RETAIN_AS_PUBLISHED, nullptr);
  }

  void setLinkObserver(IMqttLinkObserver* observer) override { observer_ = observer; }

  // Reads and writes the socket without waiting, then hands queued messages
  // to their handlers.
  void pump() {
    if (!mosq_ || !connected_) return;
    mosquitto_loop(mosq_, 0, 1);
    std::vector<Incoming> batch;
    batch.swap(inbox_);
    for (const Incoming& m : batch) {
      for (size_t i = 0; i < handlers_.size(); ++i)  // a handler may subscribe: index, not iterator
        if (handlers_[i].first == m.topic) handlers_[i].second->onMessage(m.topic, m.payload, m.retained);
    }
    if (lost_ && connected_) {
      connected_ = false;
      handlers_.clear();
      std::fprintf(stderr, "mqtt %s: connection lost\n", id_.c_str());
      if (observer_) observer_->onConnectionLost();
    }
  }

private:
  static constexpr int kKeepaliveS = 10;
  static constexpr int kConnectTimeoutMs = 5000;

  struct Incoming {
    std::string topic, payload;
    bool retained;
  };

  std::string host_;
  int port_;
  std::string id_;
  struct mosquitto* mosq_ = nullptr;
  bool connected_ = false;
  bool lost_ = false;
  int connack_ = -1;
  std::vector<std::pair<std::string, IMqttHandler*>> handlers_;
  std::vector<Incoming> inbox_;
  IMqttLinkObserver* observer_ = nullptr;

  static void onConnect(struct mosquitto*, void* self, int rc) { static_cast<MosquittoClient*>(self)->connack_ = rc; }
  static void onDisconnect(struct mosquitto*, void* self, int rc) {
    if (rc != 0) static_cast<MosquittoClient*>(self)->lost_ = true;
  }
  static void onMessageCb(struct mosquitto*, void* self, const struct mosquitto_message* m) {
    auto* c = static_cast<MosquittoClient*>(self);
    std::string payload;
    if (m->payload != nullptr && m->payloadlen > 0)
      payload.assign(static_cast<const char*>(m->payload), static_cast<size_t>(m->payloadlen));
    c->inbox_.push_back(Incoming{m->topic, std::move(payload), m->retain});
  }
};

}  // namespace codeline::mqtt
