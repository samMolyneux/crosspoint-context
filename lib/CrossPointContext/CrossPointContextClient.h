#pragma once
#include <cstddef>
#include <string>

/**
 * Uploads the extracted reading context to the configured CrossPoint Context relay.
 *
 * The body (the book-so-far, already truncated at the reading position) is staged to a
 * file on the SD card first, then POSTed with a known Content-Length and streamed off the
 * card in small chunks. Doing it in two phases keeps the EPUB and the network stack from
 * being resident at the same time (the TLS handshake alone needs ~55 KB of heap, more than
 * is free while a book is loaded) and avoids holding the whole body in RAM.
 */
class CrossPointContextClient {
 public:
  enum Error {
    OK,
    NOT_CONFIGURED,
    LOW_MEMORY,
    NETWORK_ERROR,
    UNAUTHORIZED,
    TOO_LARGE,
    SERVER_ERROR,
  };

  // POSTs the file at `path` to the configured relay as the request body. WiFi must already
  // be connected. Streams the file in chunks; only one small buffer is resident.
  static Error postFile(const char* path);

  // Result of a successful device-pairing handshake.
  struct PairResult {
    std::string writeToken;               // T — server-minted bearer token, saved on this device
    std::string nonce;                    // short, human-typeable code shown beside the QR
    std::string verificationUri;          // <origin>/pair — where the user finishes on their phone
    std::string verificationUriComplete;  // <origin>/pair?c=<nonce> — the QR payload
  };

  // POSTs to <origin>/pair/start to begin no-type pairing: the server mints a write token and
  // a short nonce, returned in `out`. WiFi must already be connected. One small HTTPS POST;
  // the token T arrives over TLS in the response body and is never placed in the QR. The
  // caller persists `out.writeToken` (via CrossPointContextStore) and renders the QR + nonce.
  static Error pairStart(const std::string& origin, const std::string& deviceLabel, PairResult& out);

  static const char* errorString(Error error);

  // HTTP status code of the most recent request (0 if the request never completed).
  static int lastHttpCode;
};
