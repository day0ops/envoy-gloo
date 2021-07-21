#include "source/common/nats/streaming/pub_request_handler.h"

#include "source/common/common/assert.h"
#include "source/common/nats/streaming/message_utility.h"

namespace Envoy {
namespace Nats {
namespace Streaming {

void PubRequestHandler::onMessage(const absl::optional<std::string> &reply_to,
                                  const std::string &payload,
                                  InboxCallbacks &inbox_callbacks,
                                  PublishCallbacks &publish_callbacks) {
  if (reply_to.has_value()) {
    inbox_callbacks.onFailure("incoming PubAck with non-empty reply subject");
    return;
  }

  if (payload.empty()) {
    inbox_callbacks.onFailure("incoming PubAck without payload");
    return;
  }

  auto &&maybe_pub_ack = MessageUtility::parsePubAckMessage(payload);
  if (maybe_pub_ack.has_value() && maybe_pub_ack.value().error().empty()) {
    publish_callbacks.onResponse();
  } else {
    publish_callbacks.onFailure();
  }
}

void PubRequestHandler::onMessage(
    const std::string &inbox, const absl::optional<std::string> &reply_to,
    const std::string &payload, InboxCallbacks &inbox_callbacks,
    std::map<std::string, PubRequest> &request_per_inbox) {
  // Find the inbox in the map.
  auto it = request_per_inbox.find(inbox);

  // Gracefully ignore a missing inbox.
  if (it == request_per_inbox.end()) {
    // TODO(talnordan): consider logging the message and/or updating stats.
    return;
  }

  // Handle the message using the publish callbacks associated with the inbox.
  PubRequest &request = it->second;
  PublishCallbacks &publish_callbacks = request.callbacks();
  onMessage(reply_to, payload, inbox_callbacks, publish_callbacks);

  // Remove the inbox from the map.
  eraseRequest(request_per_inbox, it);
}

void PubRequestHandler::onTimeout(
    const std::string &inbox,
    std::map<std::string, PubRequest> &request_per_inbox) {
  // Find the inbox in the map.
  auto it = request_per_inbox.find(inbox);

  // Gracefully ignore a missing inbox.
  if (it == request_per_inbox.end()) {
    return;
  }

  // Notify of a timeout using the publish callbacks associated with the inbox.
  PubRequest &request = it->second;
  request.callbacks().onTimeout();

  // Remove the inbox from the map.
  eraseRequest(request_per_inbox, it);
}

void PubRequestHandler::onCancel(
    const std::string &inbox,
    std::map<std::string, PubRequest> &request_per_inbox) {
  // Find the inbox in the map.
  auto it = request_per_inbox.find(inbox);

  // Gracefully ignore a missing inbox.
  if (it == request_per_inbox.end()) {
    return;
  }

  // Remove the inbox from the map.
  eraseRequest(request_per_inbox, it);
}

void PubRequestHandler::eraseRequest(
    std::map<std::string, PubRequest> &request_per_inbox,
    std::map<std::string, PubRequest>::iterator position) {
  PubRequest &request = position->second;
  request.onDestroy();
  request_per_inbox.erase(position);
}

} // namespace Streaming
} // namespace Nats
} // namespace Envoy
