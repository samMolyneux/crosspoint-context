#include "ClaudeContextStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

// Initialize the static instance
ClaudeContextStore ClaudeContextStore::instance;

namespace {
constexpr char CLAUDE_CONTEXT_FILE_JSON[] = "/.crosspoint/claude_context.json";
}  // namespace

bool ClaudeContextStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["relayUrl"] = relayUrl;
  doc["writeToken_obf"] = obfuscation::obfuscateToBase64(writeToken);

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(CLAUDE_CONTEXT_FILE_JSON, json);
}

bool ClaudeContextStore::loadFromFile() {
  if (!Storage.exists(CLAUDE_CONTEXT_FILE_JSON)) {
    LOG_DBG("CTX", "No Claude context config found");
    return false;
  }

  const String json = Storage.readFile(CLAUDE_CONTEXT_FILE_JSON);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  const auto error = deserializeJson(doc, json.c_str());
  if (error) {
    LOG_ERR("CTX", "JSON parse error: %s", error.c_str());
    return false;
  }

  relayUrl = doc["relayUrl"] | std::string("");

  bool ok = false;
  writeToken = obfuscation::deobfuscateFromBase64(doc["writeToken_obf"] | "", &ok);
  if (!ok) {
    writeToken.clear();
  }

  LOG_DBG("CTX", "Loaded Claude context config (configured: %d)", isConfigured());
  return true;
}

void ClaudeContextStore::setConfig(const std::string& url, const std::string& token) {
  relayUrl = url;
  writeToken = token;
}

void ClaudeContextStore::setRelayUrl(const std::string& url) { relayUrl = url; }

void ClaudeContextStore::setWriteToken(const std::string& token) { writeToken = token; }

std::string ClaudeContextStore::getNormalisedUrl() const {
  if (relayUrl.empty()) {
    return "";
  }

  std::string url;
  if (relayUrl.find("://") == std::string::npos) {
    // No scheme: default to https (the deployed relay). Local plain-HTTP relays must
    // include the explicit http:// scheme when configured.
    url = "https://" + relayUrl;
  } else {
    url = relayUrl;
  }

  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

bool ClaudeContextStore::isConfigured() const { return !relayUrl.empty() && !writeToken.empty(); }

void ClaudeContextStore::clear() {
  relayUrl.clear();
  writeToken.clear();
  saveToFile();
  LOG_DBG("CTX", "Cleared Claude context config");
}
