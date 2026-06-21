#include "CrossPointContextStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

// Initialize the static instance
CrossPointContextStore CrossPointContextStore::instance;

namespace {
constexpr char CROSSPOINT_CONTEXT_FILE_JSON[] = "/.crosspoint/context.json";
}  // namespace

bool CrossPointContextStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["relayUrl"] = relayUrl;
  doc["writeToken_obf"] = obfuscation::obfuscateToBase64(writeToken);

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(CROSSPOINT_CONTEXT_FILE_JSON, json);
}

bool CrossPointContextStore::loadFromFile() {
  bool loaded = false;

  if (Storage.exists(CROSSPOINT_CONTEXT_FILE_JSON)) {
    const String json = Storage.readFile(CROSSPOINT_CONTEXT_FILE_JSON);
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
    LOG_DBG("CTX", "No CrossPoint Context config found");
  }

  // Fall back to build-time defaults for anything not set on-device (testing convenience).
  applyCompileTimeDefaults();

  LOG_DBG("CTX", "Loaded CrossPoint Context config (configured: %d)", isConfigured());
  return loaded;
}

void CrossPointContextStore::applyCompileTimeDefaults() {
#ifdef CROSSPOINT_DEFAULT_RELAY_URL
  if (relayUrl.empty()) {
    relayUrl = CROSSPOINT_DEFAULT_RELAY_URL;
    LOG_DBG("CTX", "Using compile-time default server origin");
  }
#endif
  // No compile-time default write token: a baked-in credential is a security smell, and the
  // token is now obtained per-device via pairing (or manual entry). See device-pairing-plan.md.
}

void CrossPointContextStore::setConfig(const std::string& url, const std::string& token) {
  relayUrl = url;
  writeToken = token;
}

void CrossPointContextStore::setRelayUrl(const std::string& url) { relayUrl = url; }

void CrossPointContextStore::setWriteToken(const std::string& token) { writeToken = token; }

std::string CrossPointContextStore::getNormalisedUrl() const {
  if (relayUrl.empty()) {
    return "";
  }

  // No scheme: default to https (the deployed server). Local plain-HTTP servers must
  // include the explicit http:// scheme when configured.
  std::string url = relayUrl.find("://") == std::string::npos ? "https://" + relayUrl : relayUrl;

  // Reduce to the origin only (scheme://host[:port]). The store holds an origin; callers
  // append the path themselves (postFile -> /ingest, pairing -> /pair/start). This also
  // strips any trailing slash and any path a prior config or pairing may have left behind.
  const size_t schemeEnd = url.find("://");
  const size_t pathStart = url.find('/', schemeEnd + 3);
  if (pathStart != std::string::npos) {
    url.erase(pathStart);
  }
  return url;
}

bool CrossPointContextStore::isConfigured() const { return !relayUrl.empty() && !writeToken.empty(); }

void CrossPointContextStore::clear() {
  relayUrl.clear();
  writeToken.clear();
  saveToFile();
  LOG_DBG("CTX", "Cleared CrossPoint Context config");
}
