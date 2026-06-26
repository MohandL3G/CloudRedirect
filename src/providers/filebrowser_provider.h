#pragma once
#include "cloud_provider.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <limits>

class IHttpTransport;
class ITokenStore;

class FilebrowserQuantumProvider : public ICloudProvider {
public:
    const char* Name() const override { return "FileBrowser Quantum"; }

    bool Init(const std::string& configPath) override;
    void Shutdown() override;
    bool IsAuthenticated() const override;

    bool Upload(const std::string& path, const uint8_t* data, size_t len) override;
    bool Download(const std::string& path, std::vector<uint8_t>& outData) override;
    bool Remove(const std::string& path) override;
    ExistsStatus CheckExists(const std::string& path) override;
    std::vector<FileInfo> List(const std::string& prefix) override;
    std::vector<std::string> ListSubfolders(const std::string& prefix) override;
    bool ListChecked(const std::string& prefix, std::vector<FileInfo>& outFiles,
                     bool* outComplete = nullptr) override;

private:
    static constexpr uint32_t kNoAppId = (std::numeric_limits<uint32_t>::max)();
    static constexpr int MAX_RECURSION_DEPTH = 32;

    bool LoadConfig();
    std::string ApiUrl(const std::string& endpoint, const std::string& path) const;
    std::string RawUrl(const std::string& path) const;
    std::string RemotePath(uint32_t accountId, uint32_t appId,
                           const std::string& filename) const;
    std::string RemoteDir(uint32_t accountId, uint32_t appId) const;
    std::string RootPrefix() const;

    bool EnsureDirExists(const std::string& dirPath);
    bool EnsureParentsExist(const std::string& remotePath);

    struct FileEntry {
        std::string name;
        int64_t size = 0;
        int64_t modified = 0;
        bool isDir = false;
    };
    std::vector<FileEntry> ListDir(const std::string& dirPath, bool* ok = nullptr);

    bool ListRecursive(const std::string& prefix, std::vector<FileInfo>& outFiles,
                       bool* outComplete = nullptr, int depth = 0);

    std::vector<std::string> BuildAuthHeaders() const;
    void ThrottleApiCall();

    using HttpResp = HttpUtil::HttpResp;
    HttpResp ApiGet(const std::string& path);
    HttpResp ApiPost(const std::string& path, const std::string& body,
                     const std::string& contentType);
    HttpResp ApiDelete(const std::string& path);

    static int64_t ParseTime(const std::string& s);
    static std::string UrlEncode(const std::string& s);

    std::string m_serverUrl;
    std::string m_apiToken;
    std::string m_rootPath;
    std::string m_configPath;
    bool m_initialized = false;

    std::unique_ptr<IHttpTransport> m_transport;
    std::unique_ptr<ITokenStore> m_tokenStore;
    mutable std::mutex m_mtx;
    uint64_t m_lastApiCallTick = 0;
};
