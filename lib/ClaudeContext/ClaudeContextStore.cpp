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
  bool loaded = false;

  if (Storage.exists(CLAUDE_CONTEXT_FILE_JSON)) {
    const String json = Storage.readFile(CLAUDE_CONTEXT_FILE_JSON);
    if (!json.isEmpty()) {
      JsonDocument doc;
      const auto error = deserializeJson(doc, json.c_str());
      if (error) {
        LOG_ERR("CTX", "JSON parse error: %s", error.c_str());
      } else {
        relayUrl = doc["relayUrl"] | std::string("");
        bool ok = false;
        writeToken = obfuscation::deobfuscateFromBase64(doc["writeToken_obf"] | "", &ok);
        if (!ok) {
          writeToken.clear();
        }
        loaded = true;
      }
    }
  } else {
    LOG_DBG("CTX", "No Claude context config found");
  }

  // Fall back to build-time defaults for anything not set on-device (testing convenience).
  applyCompileTimeDefaults();

  LOG_DBG("CTX", "Loaded Claude context config (configured: %d)", isConfigured());
  return loaded;
}

void ClaudeContextStore::applyCompileTimeDefaults() {
#ifdef CLAUDE_DEFAULT_RELAY_URL
  if (relayUrl.empty()) {
    relayUrl = CLAUDE_DEFAULT_RELAY_URL;
    LOG_DBG("CTX", "Using compile-time default relay URL");
  }
#endif
#ifdef CLAUDE_DEFAULT_WRITE_TOKEN
  if (writeToken.empty()) {
    writeToken = CLAUDE_DEFAULT_WRITE_TOKEN;
    LOG_DBG("CTX", "Using compile-time default write token (temporary/testing)");
  }
#endif
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
