#include "filebrowser_provider.h"
#include "json.h"
#include "log.h"

#include <thread>
#include <chrono>
#include <ctime>
#include <charconv>

// ── Init / Shutdown / IsAuthenticated ────────────────────────────────────

bool FilebrowserQuantumProvider::Init(const std::string& configPath) {
    if (m_initialized) return true;
    m_configPath = configPath;

    m_tokenStore = CreateTokenStore();
    m_transport = CreateHttpTransport("[FBQuantum]");

    if (!m_transport->Init()) {
        LOG("[FBQuantum] Transport init failed");
        return false;
    }

    if (!LoadConfig()) {
        LOG("[FBQuantum] No config at %s", configPath.c_str());
    }

    m_initialized = true;
    LOG("[FBQuantumProvider] Initialized (config: %s)", configPath.c_str());
    return true;
}

void FilebrowserQuantumProvider::Shutdown() {
    m_initialized = false;
    LOG("[FBQuantumProvider] Shutdown");
}

bool FilebrowserQuantumProvider::IsAuthenticated() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_initialized && !m_serverUrl.empty() && !m_apiToken.empty();
}

// ── Config loading ───────────────────────────────────────────────────────

bool FilebrowserQuantumProvider::LoadConfig() {
    if (!m_tokenStore) return false;
    auto content = m_tokenStore->Read(m_configPath);
    if (content.empty()) return false;

    auto j = Json::Parse(content);
    m_serverUrl = j["server_url"].str();
    m_apiToken = j["api_token"].str();
    m_rootPath = j["root_path"].str();

    // Strip trailing slash from root path
    while (!m_rootPath.empty() && m_rootPath.back() == '/')
        m_rootPath.pop_back();

    if (m_serverUrl.empty() || m_apiToken.empty()) {
        LOG("[FBQuantum] LoadConfig: missing server_url or api_token");
        return false;
    }

    LOG("[FBQuantum] LoadConfig: server=%s root=%s", m_serverUrl.c_str(),
        m_rootPath.empty() ? "(none)" : m_rootPath.c_str());
    return true;
}

// ── Path helpers ─────────────────────────────────────────────────────────

std::string FilebrowserQuantumProvider::RootPrefix() const {
    if (m_rootPath.empty()) return "/CloudRedirect";
    return m_rootPath + "/CloudRedirect";
}

std::string FilebrowserQuantumProvider::RemotePath(uint32_t accountId, uint32_t appId,
                                                    const std::string& filename) const {
    return RootPrefix() + "/" + std::to_string(accountId) + "/"
         + std::to_string(appId) + "/" + filename;
}

std::string FilebrowserQuantumProvider::RemoteDir(uint32_t accountId, uint32_t appId) const {
    return RootPrefix() + "/" + std::to_string(accountId) + "/"
         + std::to_string(appId);
}

// ── URL builders ─────────────────────────────────────────────────────────

std::string FilebrowserQuantumProvider::ApiUrl(const std::string& endpoint,
                                                const std::string& path) const {
    std::string url = m_serverUrl;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += endpoint;
    if (!path.empty()) {
        url += "?path=";
        url += UrlEncode(path);
    }
    return url;
}

std::string FilebrowserQuantumProvider::RawUrl(const std::string& path) const {
    return ApiUrl("/api/raw", path);
}

// ── Auth ─────────────────────────────────────────────────────────────────

std::vector<std::string> FilebrowserQuantumProvider::BuildAuthHeaders() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    std::vector<std::string> hdrs;
    hdrs.push_back("Authorization: Bearer " + m_apiToken);
    return hdrs;
}

// ── Rate limiting ────────────────────────────────────────────────────────

void FilebrowserQuantumProvider::ThrottleApiCall() {
    using namespace std::chrono;
    uint64_t desired, last;
    do {
        last = m_lastApiCallTick.load(std::memory_order_acquire);
        uint64_t now = (uint64_t)duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
        desired = (last != 0 && now < last + 150) ? last + 150 : now;
    } while (!m_lastApiCallTick.compare_exchange_weak(last, desired,
                std::memory_order_acq_rel, std::memory_order_acquire));
    uint64_t now = (uint64_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
    if (now < desired)
        std::this_thread::sleep_for(milliseconds(desired - now));
}

// ── HTTP helpers with retry ──────────────────────────────────────────────

auto FilebrowserQuantumProvider::ApiGet(const std::string& path) -> HttpUtil::HttpResp {
    std::string url = path; // path is already a full URL
    HttpResp lastResp;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
        ThrottleApiCall();
        auto headers = BuildAuthHeaders();
        lastResp = m_transport->RequestUrl("GET", url, "", headers);
        // Only retry on 429/503 rate limits
        if (lastResp.status != 429 && lastResp.status != 503)
            return lastResp;
        LOG("[FBQuantum] GET rate limited (attempt %d, HTTP %d), retrying",
            attempt + 1, lastResp.status);
    }
    return lastResp;
}

auto FilebrowserQuantumProvider::ApiPost(const std::string& path,
                                         const std::string& body,
                                         const std::string& contentType) -> HttpUtil::HttpResp {
    std::string url = path;
    HttpResp lastResp;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
        ThrottleApiCall();
        auto headers = BuildAuthHeaders();
        if (!contentType.empty())
            headers.push_back("Content-Type: " + contentType);
        lastResp = m_transport->RequestUrl("POST", url, body, headers);
        if (lastResp.status != 429 && lastResp.status != 503)
            return lastResp;
        LOG("[FBQuantum] POST rate limited (attempt %d, HTTP %d), retrying",
            attempt + 1, lastResp.status);
    }
    return lastResp;
}

auto FilebrowserQuantumProvider::ApiDelete(const std::string& path) -> HttpUtil::HttpResp {
    std::string url = path;
    HttpResp lastResp;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
        ThrottleApiCall();
        auto headers = BuildAuthHeaders();
        lastResp = m_transport->RequestUrl("DELETE", url, "", headers);
        if (lastResp.status != 429 && lastResp.status != 503)
            return lastResp;
        LOG("[FBQuantum] DELETE rate limited (attempt %d, HTTP %d), retrying",
            attempt + 1, lastResp.status);
    }
    return lastResp;
}

// ── Directory creation ───────────────────────────────────────────────────

bool FilebrowserQuantumProvider::EnsureDirExists(const std::string& dirPath) {
    // Check if directory exists
    std::string checkUrl = ApiUrl("/api/resources", dirPath);
    auto r = ApiGet(checkUrl);
    if (r.status == 200) return true;
    if (r.status != 404) {
        LOG("[FBQuantum] EnsureDirExists: unexpected HTTP %d for %s", r.status, dirPath.c_str());
        return false;
    }

    // Directory doesn't exist — try creating it
    LOG("[FBQuantum] EnsureDirExists: creating %s", dirPath.c_str());
    std::string createUrl = ApiUrl("/api/resources", dirPath);
    auto cr = ApiPost(createUrl, "{}", "application/json");
    if (cr.status == 200 || cr.status == 201 || cr.status == 409) return true;

    LOG("[FBQuantum] EnsureDirExists: create failed HTTP %d for %s", cr.status, dirPath.c_str());
    return false;
}

bool FilebrowserQuantumProvider::EnsureParentsExist(const std::string& remotePath) {
    std::string path = RootPrefix();
    if (!EnsureDirExists(path)) return false;

    std::string rel = remotePath.substr(RootPrefix().size());
    // rel starts with "/{accountId}/{appId}/{filename}"
    // Create directories for all segments except the last (the filename).
    size_t start = 1;
    while (start < rel.size()) {
        size_t slash = rel.find('/', start);
        if (slash == std::string::npos) break;
        std::string segName = rel.substr(start, slash - start);
        path = path + "/" + segName;
        if (!EnsureDirExists(path)) return false;
        start = slash + 1;
    }
    return true;
}

// ── Upload ───────────────────────────────────────────────────────────────

bool FilebrowserQuantumProvider::Upload(const std::string& path,
                                         const uint8_t* data, size_t len) {
    uint32_t accountId, appId;
    std::string filename;
    if (!CloudProviderBase::ParsePath(path, accountId, appId, filename) || filename.empty()) {
        LOG("[FBQuantumProvider] Upload: bad path '%s'", path.c_str());
        return false;
    }

    std::string remotePath = RemotePath(accountId, appId, filename);
    std::string parentDir = RemoteDir(accountId, appId);

    // Ensure parent directory exists
    if (!EnsureParentsExist(remotePath)) {
        LOG("[FBQuantumProvider] Upload: failed to ensure parents for %s", remotePath.c_str());
        return false;
    }

    std::string uploadUrl = ApiUrl("/api/resources", remotePath);
    auto r = ApiPost(uploadUrl,
                     std::string(reinterpret_cast<const char*>(data), len),
                     "application/octet-stream");

    if (r.status == 200 || r.status == 201) {
        LOG("[FBQuantumProvider] Uploaded %s (%zu bytes)", path.c_str(), len);
        return true;
    }

    LOG("[FBQuantumProvider] Upload FAILED %s: HTTP %d", path.c_str(), r.status);
    return false;
}

// ── Download ─────────────────────────────────────────────────────────────

bool FilebrowserQuantumProvider::Download(const std::string& path,
                                           std::vector<uint8_t>& outData) {
    uint32_t accountId, appId;
    std::string filename;
    if (!CloudProviderBase::ParsePath(path, accountId, appId, filename) || filename.empty()) {
        LOG("[FBQuantumProvider] Download: bad path '%s'", path.c_str());
        return false;
    }

    if (!IsAuthenticated()) return false;

    // Build the raw download URL
    std::string remotePath = RemotePath(accountId, appId, filename);
    std::string url = RawUrl(remotePath);

    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
        ThrottleApiCall();
        auto headers = BuildAuthHeaders();
        auto r = m_transport->RequestUrl("GET", url, "", headers);

        if (r.status == 429 || r.status == 503) {
            LOG("[FBQuantum] Download rate limited (attempt %d), retrying", attempt + 1);
            continue;
        }

        if (r.status == 200) {
            outData.assign(r.body.begin(), r.body.end());
            LOG("[FBQuantumProvider] Downloaded %s (%zu bytes)", path.c_str(), outData.size());
            return true;
        }

        if (r.status == 404) {
            LOG("[FBQuantumProvider] Download: %s not found", path.c_str());
            return false;
        }

        LOG("[FBQuantumProvider] Download: HTTP %d for %s", r.status, path.c_str());
        return false;
    }

    return false;
}

// ── Remove ───────────────────────────────────────────────────────────────

bool FilebrowserQuantumProvider::Remove(const std::string& path) {
    uint32_t accountId, appId;
    std::string filename;
    if (!CloudProviderBase::ParsePath(path, accountId, appId, filename) || filename.empty()) {
        LOG("[FBQuantumProvider] Remove: bad path '%s'", path.c_str());
        return false;
    }

    if (!IsAuthenticated()) return false;

    std::string remotePath = RemotePath(accountId, appId, filename);
    std::string url = ApiUrl("/api/resources", remotePath);
    auto r = ApiDelete(url);

    if (r.status == 200 || r.status == 204 || r.status == 404) {
        // 404 means already gone — that's fine
        LOG("[FBQuantumProvider] Removed %s", path.c_str());
        return true;
    }

    LOG("[FBQuantumProvider] Remove FAILED %s: HTTP %d", path.c_str(), r.status);
    return false;
}

// ── CheckExists ──────────────────────────────────────────────────────────

ICloudProvider::ExistsStatus FilebrowserQuantumProvider::CheckExists(const std::string& path) {
    uint32_t accountId, appId;
    std::string filename;
    if (!CloudProviderBase::ParsePath(path, accountId, appId, filename) || filename.empty()) {
        LOG("[FBQuantumProvider] CheckExists: bad path '%s'", path.c_str());
        return ExistsStatus::Error;
    }

    if (!IsAuthenticated()) return ExistsStatus::Error;

    std::string remotePath = RemotePath(accountId, appId, filename);
    std::string url = ApiUrl("/api/resources", remotePath);
    auto r = ApiGet(url);

    if (r.status == 200) return ExistsStatus::Exists;
    if (r.status == 404) return ExistsStatus::Missing;
    return ExistsStatus::Error;
}

// ── List / ListChecked ───────────────────────────────────────────────────

std::vector<FilebrowserQuantumProvider::FileEntry>
FilebrowserQuantumProvider::ListDir(const std::string& dirPath, bool* ok) {
    if (ok) *ok = false;
    std::vector<FileEntry> results;

    if (!IsAuthenticated()) return results;

    std::string url = ApiUrl("/api/resources", dirPath);
    auto r = ApiGet(url);

    if (r.status == 404) {
        // Directory doesn't exist — empty listing, not an error
        if (ok) *ok = true;
        return results;
    }

    if (r.status != 200) {
        LOG("[FBQuantum] ListDir: HTTP %d for %s", r.status, dirPath.c_str());
        return results;
    }

    auto j = Json::Parse(r.body);

    // Try "items" array first (original filebrowser format)
    if (j.has("items")) {
        auto& items = j["items"];
        for (size_t i = 0; i < items.size(); ++i) {
            auto& item = items[i];
            FileEntry fe;
            fe.name = item["name"].str();
            fe.size = item["size"].integer();
            fe.modified = ParseTime(item["modified"].str());
            fe.isDir = item["type"].str() == "directory" || item["isDir"].boolean();
            results.push_back(std::move(fe));
        }
    }
    // Try "folders"/"files" arrays (FileBrowser Quantum format)
    else if (j.has("folders") || j.has("files")) {
        if (j.has("folders")) {
            auto& folders = j["folders"];
            for (size_t i = 0; i < folders.size(); ++i) {
                auto& item = folders[i];
                FileEntry fe;
                fe.name = item["name"].str();
                fe.isDir = true;
                fe.modified = ParseTime(item["modified"].str());
                results.push_back(std::move(fe));
            }
        }
        if (j.has("files")) {
            auto& files = j["files"];
            for (size_t i = 0; i < files.size(); ++i) {
                auto& item = files[i];
                FileEntry fe;
                fe.name = item["name"].str();
                fe.size = item["size"].integer();
                fe.modified = ParseTime(item["modified"].str());
                fe.isDir = false;
                results.push_back(std::move(fe));
            }
        }
    }

    if (ok) *ok = true;
    return results;
}

bool FilebrowserQuantumProvider::ListRecursive(const std::string& prefix,
                                                std::vector<FileInfo>& outFiles,
                                                bool* outComplete, int depth) {
    if (depth >= MAX_RECURSION_DEPTH) {
        LOG("[FBQuantum] ListRecursive: max depth %d reached at %s",
            MAX_RECURSION_DEPTH, prefix.c_str());
        if (outComplete) *outComplete = false;
        return true;
    }

    bool ok = false;
    auto entries = ListDir(prefix, &ok);
    if (!ok) return false;

    for (auto& entry : entries) {
        std::string childPath = prefix.empty()
            ? entry.name
            : prefix + "/" + entry.name;

        if (entry.isDir) {
            if (!ListRecursive(childPath, outFiles, outComplete, depth + 1))
                return false;
        } else {
            FileInfo fi;
            fi.path = childPath;
            fi.size = (uint64_t)entry.size;
            fi.modifiedTime = (uint64_t)entry.modified;
            outFiles.push_back(std::move(fi));
        }
    }

    return true;
}

std::vector<ICloudProvider::FileInfo>
FilebrowserQuantumProvider::List(const std::string& prefix) {
    std::vector<FileInfo> out;
    ListChecked(prefix, out, nullptr);
    return out;
}

std::vector<std::string>
FilebrowserQuantumProvider::ListSubfolders(const std::string& prefix) {
    auto files = List(prefix);
    std::vector<std::string> folders;
    for (const auto& f : files) {
        if (f.path.size() <= prefix.size()) continue;
        std::string rest = f.path.substr(prefix.size());
        size_t slash = rest.find('/');
        if (slash == std::string::npos) continue;
        std::string folder = rest.substr(0, slash);
        bool dup = false;
        for (const auto& existing : folders) {
            if (existing == folder) { dup = true; break; }
        }
        if (!dup) folders.push_back(folder);
    }
    return folders;
}

bool FilebrowserQuantumProvider::ListChecked(const std::string& prefix,
                                              std::vector<FileInfo>& outFiles,
                                              bool* outComplete) {
    if (outComplete) *outComplete = false;

    if (!IsAuthenticated()) return false;

    // Build the remote path from prefix
    // Prefix "12345/67890" → remote path "/CloudRedirect/12345/67890" (or with root_path prefix)
    std::string remotePrefix;
    if (prefix.empty()) {
        remotePrefix = RootPrefix();
    } else {
        remotePrefix = RootPrefix() + "/" + prefix;
    }

    bool ok = ListRecursive(remotePrefix, outFiles, outComplete);
    if (!ok) return false;

    // Strip the root prefix from returned paths
    std::string stripPrefix = RootPrefix() + "/";
    for (auto& fi : outFiles) {
        if (fi.path.compare(0, stripPrefix.size(), stripPrefix) == 0) {
            fi.path = fi.path.substr(stripPrefix.size());
        }
    }

    if (outComplete && *outComplete) {
        // Adjust: if max depth was hit, outComplete is already false
    }

    return true;
}

// ── Utility ──────────────────────────────────────────────────────────────

int64_t FilebrowserQuantumProvider::ParseTime(const std::string& s) {
    if (s.empty()) return 0;
    // Try ISO 8601 format
    return HttpUtil::Iso8601ToUnix(s);
}

std::string FilebrowserQuantumProvider::UrlEncode(const std::string& s) {
    return HttpUtil::UrlEncode(s, true); // preserveSlash=true
}
