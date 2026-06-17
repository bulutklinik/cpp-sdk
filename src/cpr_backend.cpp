// The cpr-based HTTP backend, isolated so the SDK core can be built and tested
// without cpr/libcurl by injecting a custom HttpBackend.
#include <bulutklinik/bulutklinik.hpp>

#include <algorithm>
#include <cctype>
#include <string>

#include <cpr/cpr.h>

namespace bulutklinik {

HttpResponse CprHttpBackend::send(const HttpRequest& request) {
    cpr::Header headers;
    for (const auto& [key, value] : request.headers) {
        headers[key] = value;
    }

    cpr::Session session;
    session.SetUrl(cpr::Url{request.url});
    session.SetHeader(headers);
    session.SetTimeout(cpr::Timeout{request.timeout_ms});
    if (request.body) {
        session.SetBody(cpr::Body{*request.body});
    }

    cpr::Response r;
    if (request.method == "GET") {
        r = session.Get();
    } else if (request.method == "POST") {
        r = session.Post();
    } else if (request.method == "PUT") {
        r = session.Put();
    } else if (request.method == "DELETE") {
        r = session.Delete();
    } else {
        r = session.Get();
    }

    HttpResponse resp;
    if (r.error.code != cpr::ErrorCode::OK) {
        resp.transport_error = true;
        resp.error_message = r.error.message;
        return resp;
    }
    resp.status = static_cast<int>(r.status_code);
    resp.body = r.text;
    for (const auto& [key, value] : r.header) {
        std::string lower = key;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        resp.headers[lower] = value;
    }
    return resp;
}

}  // namespace bulutklinik
