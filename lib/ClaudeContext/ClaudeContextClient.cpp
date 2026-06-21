#include "ClaudeContextClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <cctype>
#include <string>

#include "ClaudeContextStore.h"

int ClaudeContextClient::lastHttpCode = 0;

namespace {
// Small TLS buffers to fit in the ESP32-C3's limited heap (see KOReaderSyncClient for the
// same reasoning — the default 16 KB buffers OOM during the handshake).
constexpr int HTTP_BUF_SIZE = 2048;

// The TLS handshake makes many small allocations that aggregate to ~48 KB; guard against
// starting it without enough total free heap. Only relevant for https relays.
constexpr uint32_t MIN_HEAP_FOR_TLS = 55000;

// Upload one SD block at a time; the body never has to be resident in RAM.
constexpr size_t UPLOAD_CHUNK = 1024;

// Matches the relay's body-size cap (a full novel's plain text is ~1.2 MB).
constexpr size_t MAX_BODY_BYTES = 5000000;

// Percent-encode a value for an application/x-www-form-urlencoded body (RFC 3986 unreserved
// set passes through). Used for the non-secret device_label in the pairing request.
std::string urlEncode(const std::string& s) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += HEX_DIGITS[c >> 4];
      out += HEX_DIGITS[c & 0x0F];
    }
  }
  return out;
}
}  // namespace

ClaudeContextClient::Error ClaudeContextClient::postFile(const char* path) {
  lastHttpCode = 0;

  ClaudeContextStore& store = CLAUDE_CONTEXT_STORE;
  if (!store.isConfigured()) {
    return NOT_CONFIGURED;
  }

  // The store holds the server origin; the device writes to its /ingest endpoint.
  const std::string url = store.getNormalisedUrl() + "/ingest";
  const bool isHttps = url.rfind("https://", 0) == 0;

  HalFile file;
  if (!Storage.openFileForRead("CTX", path, file)) {
    LOG_ERR("CTX", "Cannot open context file for upload");
    return NETWORK_ERROR;
  }
  const size_t contentLength = file.size();
  if (contentLength > MAX_BODY_BYTES) {
    LOG_ERR("CTX", "Context body too large: %u bytes", (unsigned)contentLength);
    return TOO_LARGE;
  }

  if (isHttps) {
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_TLS) {
      LOG_ERR("CTX", "Insufficient heap for TLS handshake: %u bytes free (need %u)", (unsigned)freeHeap,
              (unsigned)MIN_HEAP_FOR_TLS);
      return LOW_MEMORY;
    }
  }

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 30000;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  if (isHttps) {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    return NETWORK_ERROR;
  }

  const std::string authHeader = "Bearer " + store.getWriteToken();
  if (esp_http_client_set_header(client, "Authorization", authHeader.c_str()) != ESP_OK ||
      esp_http_client_set_header(client, "Content-Type", "text/markdown") != ESP_OK) {
    LOG_ERR("CTX", "Failed to set request headers");
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  // Open with a known content length and stream the body in raw writes (no chunked
  // transfer-encoding needed, since the size is known up front).
  esp_err_t err = esp_http_client_open(client, static_cast<int>(contentLength));
  if (err != ESP_OK) {
    LOG_ERR("CTX", "Failed to open connection: %d", err);
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  auto buf = makeUniqueNoThrow<uint8_t[]>(UPLOAD_CHUNK);
  if (!buf) {
    LOG_ERR("CTX", "OOM: %u byte upload buffer", (unsigned)UPLOAD_CHUNK);
    esp_http_client_cleanup(client);
    return LOW_MEMORY;
  }

  size_t remaining = contentLength;
  bool failed = false;
  while (remaining > 0) {
    const size_t toRead = remaining < UPLOAD_CHUNK ? remaining : UPLOAD_CHUNK;
    const int readLen = file.read(buf.get(), toRead);
    if (readLen <= 0) {
      LOG_ERR("CTX", "Short read staging upload (%d)", readLen);
      failed = true;
      break;
    }
    int written = 0;
    while (written < readLen) {
      const int w =
          esp_http_client_write(client, reinterpret_cast<const char*>(buf.get()) + written, readLen - written);
      if (w < 0) {
        LOG_ERR("CTX", "Socket write failed");
        failed = true;
        break;
      }
      written += w;
    }
    if (failed) {
      break;
    }
    remaining -= static_cast<size_t>(readLen);
  }

  if (failed) {
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  esp_http_client_fetch_headers(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;
  esp_http_client_cleanup(client);

  LOG_DBG("CTX", "Upload response: %d (%u bytes sent)", httpCode, (unsigned)contentLength);

  if (httpCode == 200) {
    return OK;
  }
  if (httpCode == 401) {
    return UNAUTHORIZED;
  }
  if (httpCode == 413) {
    return TOO_LARGE;
  }
  if (httpCode == 0) {
    return NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

ClaudeContextClient::Error ClaudeContextClient::pairStart(const std::string& origin, const std::string& deviceLabel,
                                                          PairResult& out) {
  lastHttpCode = 0;
  if (origin.empty()) {
    return NOT_CONFIGURED;
  }

  // Build <origin>/pair/start, tolerating a trailing slash on the configured origin.
  std::string base = origin;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  const std::string url = base + "/pair/start";
  const bool isHttps = url.rfind("https://", 0) == 0;

  if (isHttps) {
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_TLS) {
      LOG_ERR("CTX", "Insufficient heap for TLS handshake: %u bytes free (need %u)", (unsigned)freeHeap,
              (unsigned)MIN_HEAP_FOR_TLS);
      return LOW_MEMORY;
    }
  }

  // x-www-form-urlencoded body: a fixed client_id plus a non-secret device_label that the
  // server shows on the consent screen so the owner confirms it's their device.
  const std::string body = "client_id=crosspoint-reader&device_label=" + urlEncode(deviceLabel);

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 30000;
  config.buffer_size = HTTP_BUF_SIZE;
  config.buffer_size_tx = HTTP_BUF_SIZE;
  if (isHttps) {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    return NETWORK_ERROR;
  }

  if (esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded") != ESP_OK) {
    LOG_ERR("CTX", "Failed to set pairing request header");
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  esp_err_t err = esp_http_client_open(client, static_cast<int>(body.size()));
  if (err != ESP_OK) {
    LOG_ERR("CTX", "Failed to open pairing connection: %d", err);
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  if (esp_http_client_write(client, body.data(), static_cast<int>(body.size())) < 0) {
    LOG_ERR("CTX", "Pairing request write failed");
    esp_http_client_cleanup(client);
    return NETWORK_ERROR;
  }

  esp_http_client_fetch_headers(client);
  const int httpCode = esp_http_client_get_status_code(client);
  lastHttpCode = httpCode;

  // Read the (small) JSON response into a fixed buffer — a handful of short fields.
  constexpr size_t MAX_RESP = 1024;
  char resp[MAX_RESP + 1];
  int total = 0;
  bool readFailed = false;
  while (total < static_cast<int>(MAX_RESP)) {
    const int r = esp_http_client_read(client, resp + total, static_cast<int>(MAX_RESP) - total);
    if (r < 0) {
      readFailed = true;
      break;
    }
    if (r == 0) {
      break;
    }
    total += r;
  }
  resp[total] = '\0';
  esp_http_client_cleanup(client);

  LOG_DBG("CTX", "Pairing response: %d (%d body bytes)", httpCode, total);

  if (httpCode != 200) {
    if (httpCode == 401) {
      return UNAUTHORIZED;
    }
    if (httpCode == 0) {
      return NETWORK_ERROR;
    }
    return SERVER_ERROR;
  }
  if (readFailed) {
    LOG_ERR("CTX", "Pairing response read failed");
    return NETWORK_ERROR;
  }

  // Parse the JSON. T arrives here over TLS — never logged, never placed in the QR.
  JsonDocument doc;
  const auto perr = deserializeJson(doc, resp);
  if (perr) {
    LOG_ERR("CTX", "Pairing response parse error: %s", perr.c_str());
    return SERVER_ERROR;
  }
  out.writeToken = doc["T"] | std::string("");
  out.nonce = doc["nonce"] | std::string("");
  out.verificationUri = doc["verification_uri"] | std::string("");
  out.verificationUriComplete = doc["verification_uri_complete"] | std::string("");

  if (out.writeToken.empty() || out.verificationUriComplete.empty()) {
    LOG_ERR("CTX", "Pairing response missing required fields");
    return SERVER_ERROR;
  }
  return OK;
}

const char* ClaudeContextClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NOT_CONFIGURED:
      return "Relay not configured";
    case LOW_MEMORY:
      return "Not enough memory — please retry";
    case NETWORK_ERROR:
      return "Network error";
    case UNAUTHORIZED:
      return "Bad write token";
    case TOO_LARGE:
      return "Context too large";
    case SERVER_ERROR:
      return "Server error (try again later)";
    default:
      return "Unknown error";
  }
}
