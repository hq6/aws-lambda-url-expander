#include <aws/lambda-runtime/runtime.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <curl/curl.h>

#include <cstdlib>
#include <string>
#include <vector>
#include <iostream>

#include <chrono>
typedef std::chrono::high_resolution_clock Clock;

using namespace aws::lambda_runtime;

/**
 * A function that does nothing and consumes all the data that curl produces.
 * Used to prevent curl from printing output.
 */
size_t do_nothing(void *buffer, size_t size, size_t nmemb, void *userp)
{
  return size * nmemb;
}

/**
 * Single global curl handle scoped to this translation unit. Lambda is
 * single-threaded so this can be shared across invocations to share the kept
 * alive connections and protect against the need to re-establish SSL.
 */
static CURL* curl;

/**
 * The maximum number of connections curl should cache. Overridable via
 * MAX_CONNECTIONS env variable. Note that this is directly correlated with
 * memory usage.
 */
static int max_connections = 500;

/**
 * The maximum redirects curl should follow when the request does not override
 * this value. Overridable via DEFAULT_MAX_REDIRECTS env variable.
 */
static long default_max_redirects = 5L;

/**
 * The default max timeout on total time curl will spend issuing requests to
 * follow redirects. Overridable via DEFAULT_MAX_TIME_MS env variable.
 */
static long default_max_time_ms = 500L;

/**
 * Expand the given URL. Returns true if the request completed without error.
 *
 * Output parameters
 *     output_url: The expanded URL, after following  redirects up to the
 *                 max_redirects. Value is not valid if the request times out
 *                 before it reaches max_redirects.
 *     reached_redirect_limit: True means that we do not know whether output_url has
 *                 further redirects.
 * Input parameters
 *     url: The URL to expand.
 *     max_time_ms: The total amount of time we are wlling to spend on the URL expansion.
 *     max_redirects: The maximum number of redirects we are willing to follow.
 * Returns the return value of curl_easy_perform. Will never return CURLE_TOO_MANY_REDIRECTS.
 */
CURLcode expand_url(std::string& output_url, bool& reached_redirect_limit, const char* url, long max_time_ms, long max_redirects) {
  CURLcode res;
  std::string output;

  // Set request-specific options
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, max_time_ms);
  if (max_redirects > 0) {
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, max_redirects);
  }

  res = curl_easy_perform(curl);

  // Restore request-specific options to defaults
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, max_time_ms);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, max_time_ms);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, -1);

  if(res != CURLE_OK) {
    fprintf(stderr, "curl_easy_perform() failed: %d %s\n",
        res,
        curl_easy_strerror(res));
    // We extract information when we reach the redirect limit but not the
    // timeout limit, because we only know the original URL in that case.
    if (res != CURLE_TOO_MANY_REDIRECTS) {
      return res;
    }
  }

  // Extract URL.
  // 1. We first check whether there is an additional redirect step because
  //    we hit our limit and return that if there is one. In this scenario,
  //    there could be additional hops but we do not know.
  // 2. If there is no additional redirect, then we can be certain this is a
  //    final URL.
  char* extracted_url = NULL;
  curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &extracted_url);
  if (extracted_url != NULL) {
    output_url = extracted_url;
    reached_redirect_limit = true;
    return CURLE_OK;
  }

  curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &extracted_url);
  if (extracted_url != NULL) {
    output_url = extracted_url;
    reached_redirect_limit = false;
    return CURLE_OK;
  }
  // Arbitrary choice of error code here, but it's accurate enough to describe the problem.
  return CURLE_FAILED_INIT;
}

/**
 * Lambda handler wrapper over expand_url function that unpacks the request and
 * packs the response.
 *
 * Input keys:
 *     url: The initial url we want to expand / unshorten.
 *     max_time_ms: The maximum amount of time we want curl to spend on making
 *                  requests to expand the URL. This is best-effort, so callers
 *                  should set it but still timeout their lambda invocations
 *                  themselves. It is best-effort because even curl with
 *                  libc-ares sometimes fails to respect the timeout for DNS
 *                  queries.
 *     max_redirects: The maximum number of redirects curl should follow. This
 *                    should be set low enough to complete under the
 *                    max_time_ms for most urls because curl can still retrieve
 *                    the last url it followed when this is hit, while it
 *                    cannot do so on a timeout.
 * Output keys:
 *     error_code: Always present. This is set to 0 when the request finishes
 *                 successfully. Hitting a redirect limit is considered
 *                 success. In the case of failure, this is set to an integer
 *                 that corresponds to a CURLcode.
 *     duration_ms: The amount of time the execution spent executing curl_easy_perform.
 *     expanded_url: Present iff error_code == 0. This is either the final URL
 *                   or the last URL we found before hitting the redirect limit.
 *     reached_redirect_limit: Present iff error_code == 0. True means that
 *                             curl reached the redirect limit, so it is
 *                             unknown whether expanded_url is the final URL in
 *                             the redirect chain.
 *     error_message: Present iff error_code != 0. This is the string
 *                    description of the returned CURL error code.
 */
invocation_response expand_url_handler(invocation_request const& request)
{
  using namespace Aws::Utils::Json;
  // Validate request
  JsonValue json(request.payload);
  if (!json.WasParseSuccessful()) {
    return invocation_response::failure("Failed to parse input JSON", "InvalidJSON");
  }
  auto v = json.View();
  if (!v.ValueExists("url")) {
    return invocation_response::failure("Missing URL argument", "InvalidJSON");
  }

  // Extract arguments
  auto url = v.GetString("url");
  long max_time_ms = default_max_time_ms;
  int max_redirects = default_max_redirects;
  if (v.ValueExists("max_time_ms")) {
    max_time_ms = v.GetInt64("max_time_ms");
  }
  if (v.ValueExists("max_redirects")) {
    max_redirects = v.GetInt64("max_redirects");
  }

  // Output arguments
  std::string expanded_url;
  bool reached_redirect_limit;
  auto before = Clock::now();
  CURLcode res = expand_url(expanded_url, reached_redirect_limit, url.c_str(), max_time_ms, max_redirects);
  auto after = Clock::now();
  auto duration = after - before;

  // Construct response
  JsonValue response;
  response.WithInt64("duration_ms",
      std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
  if (res == CURLE_OK) {
    response.WithInt64("error_code", 0);
    response.WithString("expanded_url", expanded_url);
    response.WithBool("reached_redirect_limit", reached_redirect_limit);
  } else {
    response.WithInt64("error_code", res);
    response.WithString("error_message", curl_easy_strerror(res));
  }

  return invocation_response::success(response.View().WriteCompact(), "application/json");
}

/**
 * String split function that destroys its input. Only used for local testing.
 */
static std::vector<std::string> split(std::string s, std::string delimiter = " ") {
  std::vector<std::string> res;
  size_t pos = 0;
  std::string token;
  while ((pos = s.find(delimiter)) != std::string::npos) {
    token = s.substr(0, pos);
    if (token.length() > 0) {
      res.push_back(token);
    }
    s.erase(0, pos + delimiter.length());
  }
  if (s.length() > 0) {
    res.push_back(s);
  }
  return res;
}

/**
 * Entry point.
 *
 * When running in AWS Lambda, process Lambda requests minimally containing the
 * "url" key. Other keys are documented in expand_url_handler.
 *
 * Otherwise, read URLs to unshorten from stdin.  When reading from standard
 * input, each lines should be of the following form.
 *    <url> [max_time_ms] [max_redirects]
 */
int main()
{
  // Allow override of global configurations based on env variables.
  const char* env_MAX_CONNECTIONS = std::getenv("MAX_CONNECTIONS");
  const char* env_DEFAULT_MAX_REDIRECTS = std::getenv("DEFAULT_MAX_REDIRECTS");
  const char* env_DEFAULT_MAX_TIME_MS = std::getenv("DDEFAULT_MAX_TIME_MS");
  if (env_MAX_CONNECTIONS) {
    max_connections = std::atoll(env_MAX_CONNECTIONS);
  }
  if (env_DEFAULT_MAX_TIME_MS) {
    default_max_time_ms = std::atoll(env_DEFAULT_MAX_TIME_MS);
  }
  if (env_DEFAULT_MAX_REDIRECTS) {
    default_max_redirects = std::atoll(env_DEFAULT_MAX_REDIRECTS);
  }

  // Initialize curl
  CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
  if (res != CURLE_OK) {
    fprintf(stderr, "Failed global curl init with error code %d: %s\n", res, curl_easy_strerror(res));
    exit(1);
  }
  curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "Failed to create curl handle");
    exit(1);
  }

  // Ignore SSL errors. Equivalent to --insecure.
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

  // Use HEAD request
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

  // Suppress normal output, since we are only interested in the URL
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, do_nothing);

  // Increase connection cache
  curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, max_connections);

  // Check if we are running in Lambda
  bool is_lambda = std::getenv("AWS_LAMBDA_FUNCTION_NAME") != NULL;
  if (is_lambda) {
    run_handler(expand_url_handler);
  } else {
    // Read commands from stdin when running locally, and output times
    for (std::string line; std::getline(std::cin, line);) {
      std::vector<std::string> parts = split(line);
      if (parts.size() == 0) {
        continue;
      }
      const char* url = parts[0].c_str();
      long max_time_ms = default_max_time_ms;
      int max_redirects = default_max_redirects;
      if (parts.size() > 1) {
        max_time_ms = std::stoll(parts[1]);
      }
      if (parts.size() > 2) {
        max_redirects = std::stoi(parts[2]);
      }
      std::string expanded_url;
      bool reached_redirect_limit;
      auto before = Clock::now();
      CURLcode res = expand_url(expanded_url, reached_redirect_limit, url, max_time_ms, max_redirects);
      auto after = Clock::now();
      if (res == CURLE_OK) {
        printf("URL '%s': %s completed in %ld ms\n", url, expanded_url.c_str(),
            std::chrono::duration_cast<std::chrono::milliseconds>(after - before).count());
      } else {
        fprintf(stderr, "URL '%s': An error occurred while calling curl: %d %s. Error detected in %ld ms\n",
            url, res, curl_easy_strerror(res),
            std::chrono::duration_cast<std::chrono::milliseconds>(after - before).count());
      }
    }
  }
  // Cleanup curl
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  return 0;
}
