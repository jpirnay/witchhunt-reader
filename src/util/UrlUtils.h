#pragma once
#include <string>

namespace UrlUtils {

/**
 * Prepend http:// if no protocol specified (server will redirect to https if needed)
 */
std::string ensureProtocol(const std::string& url);

/**
 * Repair a mistyped http/https scheme, e.g. "https:/example.com" or
 * "https:example.com" -> "https://example.com", and drop a doubled scheme
 * ("http://https://example.com" -> "https://example.com"). A well-formed URL,
 * or one with any other scheme, is returned unchanged. Hand-typed server URLs
 * lose a slash easily, and the result then looks scheme-less to every "://"
 * check, which prepends a second scheme on top.
 */
std::string repairSchemeSeparator(const std::string& url);

/**
 * Extract host with protocol from URL (e.g., "http://example.com" from "http://example.com/path")
 */
std::string extractHost(const std::string& url);

/**
 * Extract hostname only from a URL (e.g., "example.com" from
 * "http://example.com:8080/path"). Returns an empty string if no hostname can
 * be determined.
 */
std::string extractHostname(const std::string& url);

/**
 * Build full URL from server URL and path.
 * If path starts with /, it's an absolute path from the host root.
 * Otherwise, it's relative to the server URL.
 */
std::string buildUrl(const std::string& serverUrl, const std::string& path);

}  // namespace UrlUtils
