#include "metrics_collector.hpp"
#include "../utils/logger.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>

namespace dedup {

static size_t write_cb(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string MetricsCollector::prometheus_query(const std::string& query) {
#ifdef DEDUP_DRY_RUN
    LOG_DBG("[metrics] DRY RUN query: %s", query.c_str());
    return R"({"data":{"result":[{"value":[0,"0"]}]}})";
#endif

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string url = prometheus_.url + "/api/v1/query?query=" + query;
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERR("[metrics] Prometheus query failed: %s", curl_easy_strerror(res));
        return "";
    }
    return response;
}

int64_t MetricsCollector::get_longhorn_actual_size(const std::string& volume_name) {
    std::string query = "longhorn_volume_actual_size_bytes{volume=\"" + volume_name + "\"}";
    std::string resp = prometheus_query(query);
    if (resp.empty()) return -1;

    try {
        auto j = nlohmann::json::parse(resp);
        auto& result = j["data"]["result"];
        if (result.empty()) return -1;
        std::string val = result[0]["value"][1].get<std::string>();
        return std::strtoll(val.c_str(), nullptr, 10);
    } catch (const std::exception& e) {
        LOG_ERR("[metrics] JSON parse error: %s", e.what());
        return -1;
    }
}

std::string MetricsCollector::get_volume_for_pvc(const std::string& pvc_name, const std::string& ns) {
    // Query Prometheus for kube_persistentvolumeclaim_info
    std::string query = "kube_persistentvolumeclaim_info{persistentvolumeclaim=\""
                        + pvc_name + "\",namespace=\"" + ns + "\"}";
    std::string resp = prometheus_query(query);
    if (resp.empty()) return "";

    try {
        auto j = nlohmann::json::parse(resp);
        auto& result = j["data"]["result"];
        if (result.empty()) return "";
        return result[0]["metric"]["volumename"].get<std::string>();
    } catch (...) {
        return "";
    }
}

bool MetricsCollector::push_metric(const std::string& metric_name, double value,
                                     const std::string& db_system, const std::string& dup_grade,
                                     const std::string& stage) {
    if (grafana_.url.empty()) {
        LOG_DBG("[metrics] No Grafana endpoint configured, skipping push");
        return false;
    }

#ifdef DEDUP_DRY_RUN
    LOG_INF("[metrics] DRY RUN push: %s{db=%s,grade=%s,stage=%s} = %.3f",
        metric_name.c_str(), db_system.c_str(), dup_grade.c_str(), stage.c_str(), value);
    return true;
#endif

    // Push to Prometheus Pushgateway (Grafana reads from Prometheus)
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = grafana_.url + "/metrics/job/dedup-test"
                      "/db_system/" + db_system
                      + "/dup_grade/" + dup_grade
                      + "/stage/" + stage;

    std::ostringstream body;
    body << metric_name << " " << value << "\n";
    std::string body_str = body.str();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return res == CURLE_OK;
}

} // namespace dedup
