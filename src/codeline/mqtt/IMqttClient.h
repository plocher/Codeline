// The few MQTT client operations the bindings need. A real client library
// would sit behind this; scenarios use InMemoryMockBroker::Client.
#pragma once

#include <cstdint>
#include <string_view>

namespace codeline::mqtt {

struct IMqttHandler {
  virtual ~IMqttHandler() = default;
  // Views are valid only during the call. `retained` is true for a stored
  // message delivered on subscribe, and for a live message its publisher
  // marked retained (MQTT 5 retain-as-published). Bindings must not act on a
  // retained message arriving on a topic that is never meant to be retained.
  virtual void onMessage(std::string_view topic, std::string_view payload, bool retained) = 0;
};

struct IMqttLinkObserver {
  virtual ~IMqttLinkObserver() = default;
  // The client's own connection to the broker broke (not a disconnect() it asked for).
  virtual void onConnectionLost() = 0;
};

struct MqttWill {
  std::string_view topic;
  std::string_view payload;
  bool retained;
};

struct IMqttClient {
  virtual ~IMqttClient() = default;
  // A clean session: subscriptions do not survive a disconnect.
  virtual void connect(std::string_view client_id, const MqttWill* will) = 0;
  virtual void disconnect() = 0;  // clean: the will is not published
  virtual bool connected() const = 0;
  virtual void publish(std::string_view topic, std::string_view payload, bool retained) = 0;
  virtual void subscribe(std::string_view topic, IMqttHandler* handler) = 0;
  // A subscription at a QoS a binding states (CodeLineMQTT.md 5 rule 1:
  // controls are subscribed at QoS 0). A client that carries no QoS
  // (InMemoryMockBroker) subscribes as subscribe() does.
  virtual void subscribeAtQos(std::string_view topic, IMqttHandler* handler, uint8_t qos) {
    (void)qos;
    subscribe(topic, handler);
  }
  virtual void setLinkObserver(IMqttLinkObserver* observer) = 0;
};

}  // namespace codeline::mqtt
