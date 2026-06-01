#include "ClaudeContextClient.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

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
}  // namespace

ClaudeContextClient::Error ClaudeContextClient::postFile(const char* path) {
  lastHttpCode = 0;

  ClaudeContextStore& store = CLAUDE_CONTEXT_STORE;
  if (!store.isConfigured()) {
    return NOT_CONFIGURED;
  }

  const std::string url = store.getNormalisedUrl();
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
      const int w = esp_http_client_write(client, reinterpret_cast<const char*>(buf.get()) + written, readLen - written);
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
