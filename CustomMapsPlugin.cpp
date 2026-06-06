#include "pch.h"
#include "CustomMapsPlugin.h"
#include <fstream>

BAKKESMOD_PLUGIN(CustomMapsPlugin, "Custom Maps Plugin", plugin_version, PLUGINTYPE_FREEPLAY)

void CustomMapsPlugin::onLoad() {
    EnsureFolderExists();
    LoadFavorites();
    LoadMapIndex();
    installedMaps = GetInstalledMaps();

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
        winsockInitialized = true;
    }

    cvarManager->registerNotifier("custommaps_open", [this](std::vector<std::string> args) {
        cvarManager->executeCommand("togglemenu custommaps");
        }, "Open Custom Maps Plugin", PERMISSION_ALL);
}

void CustomMapsPlugin::onUnload() {
    SaveFavorites();
    previewImage = nullptr;
    if (winsockInitialized) {
        WSACleanup();
        winsockInitialized = false;
    }
}

std::filesystem::path CustomMapsPlugin::GetModsFolder() {
    // Primary: Documents\My Games\Rocket League (always writable, no admin needed)
    char docPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, docPath))) {
        auto modsPath = std::filesystem::path(docPath) /
            "My Games" / "Rocket League" / "TAGame" / "CookedPCConsole" / "mods";
        if (!std::filesystem::exists(modsPath)) {
            std::filesystem::create_directories(modsPath);
        }
        return modsPath;
    }

    // Fallback: Epic Games registry
    auto tryRegistry = [](HKEY root, const std::string& subKey,
        const std::string& value) -> std::string {
            HKEY key;
            if (RegOpenKeyExA(root, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
                return "";
            char buf[512];
            DWORD size = sizeof(buf);
            DWORD type = REG_SZ;
            if (RegQueryValueExA(key, value.c_str(), NULL, &type,
                (LPBYTE)buf, &size) != ERROR_SUCCESS) {
                RegCloseKey(key);
                return "";
            }
            RegCloseKey(key);
            return std::string(buf);
        };

    std::string epicPath = tryRegistry(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\EpicGames\\Unreal Engine\\RocketLeague",
        "InstalledDirectory");
    if (!epicPath.empty()) {
        return std::filesystem::path(epicPath) /
            "TAGame" / "CookedPCConsole" / "mods";
    }

    std::string steamPath = tryRegistry(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 252950",
        "InstallLocation");
    if (!steamPath.empty()) {
        return std::filesystem::path(steamPath) /
            "TAGame" / "CookedPCConsole" / "mods";
    }

    std::vector<std::string> commonPaths = {
        "C:\\Program Files\\Epic Games\\rocketleague",
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\rocketleague",
        "D:\\Program Files\\Epic Games\\rocketleague",
        "D:\\Steam\\steamapps\\common\\rocketleague",
        "C:\\Steam\\steamapps\\common\\rocketleague",
        "E:\\Program Files\\Epic Games\\rocketleague",
        "F:\\Program Files\\Epic Games\\rocketleague",
    };
    for (auto& p : commonPaths) {
        auto rlPath = std::filesystem::path(p) / "TAGame" / "CookedPCConsole";
        if (std::filesystem::exists(rlPath)) {
            return rlPath / "mods";
        }
    }

    return "C:\\Program Files\\Epic Games\\rocketleague\\TAGame\\CookedPCConsole\\mods";
}

std::filesystem::path CustomMapsPlugin::GetImageCacheFolder() {
    auto folder = gameWrapper->GetDataFolder() / "plugins" / "CustomMaps" / "cache";
    if (!std::filesystem::exists(folder)) {
        std::filesystem::create_directories(folder);
    }
    return folder;
}

std::filesystem::path CustomMapsPlugin::GetFavoritesPath() {
    return gameWrapper->GetDataFolder() / "plugins" / "CustomMaps" / "favorites.txt";
}

void CustomMapsPlugin::LoadFavorites() {
    favorites.clear();
    auto path = GetFavoritesPath();
    if (!std::filesystem::exists(path)) return;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) favorites.insert(line);
    }
}

void CustomMapsPlugin::SaveFavorites() {
    auto path = GetFavoritesPath();
    std::ofstream file(path);
    for (auto& fav : favorites) {
        file << fav << "\n";
    }
}

bool CustomMapsPlugin::IsFavorite(const std::string& mapName) {
    return favorites.count(mapName) > 0;
}

void CustomMapsPlugin::ToggleFavorite(const std::string& mapName) {
    if (IsFavorite(mapName)) {
        favorites.erase(mapName);
    }
    else {
        favorites.insert(mapName);
    }
    SaveFavorites();
}

void CustomMapsPlugin::EnsureFolderExists() {
    auto folder = GetModsFolder();
    if (!std::filesystem::exists(folder)) {
        std::filesystem::create_directories(folder);
    }
    auto cacheFolder = gameWrapper->GetDataFolder() / "plugins" / "CustomMaps";
    if (!std::filesystem::exists(cacheFolder)) {
        std::filesystem::create_directories(cacheFolder);
    }
}

void CustomMapsPlugin::LoadMapIndex() {
    maps.clear();
    filteredMaps.clear();
    statusMessage = "Loading map list...";

    std::thread([this]() {
        HINTERNET hInternet = InternetOpenA("CustomMapsPlugin",
            INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (!hInternet) { statusMessage = "Network error."; return; }

        HINTERNET hUrl = InternetOpenUrlA(hInternet,
            "https://raw.githubusercontent.com/Capstoner1/CustomMapsPlugin/main/CustomMapsPlugin.json",
            NULL, 0, INTERNET_FLAG_RELOAD, 0);
        if (!hUrl) {
            InternetCloseHandle(hInternet);
            statusMessage = "Failed to reach map list.";
            return;
        }

        std::string data;
        char buf[4096];
        DWORD bytesRead = 0;
        while (InternetReadFile(hUrl, buf, sizeof(buf), &bytesRead)
            && bytesRead > 0) {
            data.append(buf, bytesRead);
        }
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);

        std::vector<MapEntry> loaded;
        size_t pos = 0;
        while ((pos = data.find("\"name\"", pos)) != std::string::npos) {
            MapEntry m;
            auto extract = [&](const std::string& key) -> std::string {
                size_t k = data.find("\"" + key + "\"", pos);
                if (k == std::string::npos) return "";
                k = data.find(":", k) + 1;
                k = data.find("\"", k) + 1;
                size_t end = data.find("\"", k);
                return data.substr(k, end - k);
                };
            m.name = extract("name");
            m.author = extract("author");
            m.description = extract("description");
            m.category = extract("category");
            m.downloadUrl = extract("downloadUrl");
            m.previewUrl = extract("previewUrl");
            m.fileSize = extract("fileSize");
            if (!m.name.empty()) loaded.push_back(m);
            pos++;
        }

        maps = loaded;
        filteredMaps = maps;
        statusMessage = "Loaded " + std::to_string(maps.size()) + " maps.";
        }).detach();
}

void CustomMapsPlugin::LoadPreviewImage(const std::string& url) {
    if (url.empty() || url == currentPreviewUrl || loadingPreview) return;
    currentPreviewUrl = url;
    loadingPreview = true;
    previewImage = nullptr;

    std::thread([this, url]() {
        std::string filename = url.substr(url.find_last_of('/') + 1);
        auto cachePath = GetImageCacheFolder() / filename;

        if (!std::filesystem::exists(cachePath)) {
            HINTERNET hInternet = InternetOpenA("CustomMapsPlugin",
                INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
            if (!hInternet) { loadingPreview = false; return; }

            HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(),
                NULL, 0, INTERNET_FLAG_RELOAD, 0);
            if (!hUrl) {
                InternetCloseHandle(hInternet);
                loadingPreview = false;
                return;
            }

            std::ofstream outFile(cachePath, std::ios::binary);
            char buf[8192];
            DWORD bytesRead = 0;
            while (InternetReadFile(hUrl, buf, sizeof(buf), &bytesRead)
                && bytesRead > 0) {
                outFile.write(buf, bytesRead);
            }
            outFile.close();
            InternetCloseHandle(hUrl);
            InternetCloseHandle(hInternet);
        }

        previewImage = std::make_shared<ImageWrapper>(cachePath, false, true);
        loadingPreview = false;
        }).detach();
}

void CustomMapsPlugin::DownloadAndInstallMap(const MapEntry& map) {
    if (isDownloading) return;
    isDownloading = true;
    downloadProgress = 0.0f;
    downloadingMapName = map.name;
    statusMessage = "Downloading: " + map.name + "...";

    std::string url = map.downloadUrl;
    std::string mapName = map.name;
    std::filesystem::path modsFolder = GetModsFolder();

    std::thread([this, url, mapName, modsFolder]() {
        HINTERNET hInternet = InternetOpenA("CustomMapsPlugin",
            INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (!hInternet) {
            statusMessage = "Network error.";
            isDownloading = false;
            return;
        }

        HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(),
            NULL, 0, INTERNET_FLAG_RELOAD, 0);
        if (!hUrl) {
            InternetCloseHandle(hInternet);
            statusMessage = "Failed to download map.";
            isDownloading = false;
            return;
        }

        DWORD totalSize = 0;
        DWORD bufSize = sizeof(DWORD);
        HttpQueryInfo(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
            &totalSize, &bufSize, NULL);

        // Download to temp folder — always writable
        std::filesystem::path tempZip = std::filesystem::temp_directory_path() / (mapName + ".zip");
        std::ofstream outFile(tempZip, std::ios::binary);
        if (!outFile) {
            InternetCloseHandle(hUrl);
            InternetCloseHandle(hInternet);
            statusMessage = "Failed to create temp file.";
            isDownloading = false;
            return;
        }

        char buf[8192];
        DWORD bytesRead = 0;
        DWORD totalDownloaded = 0;
        while (InternetReadFile(hUrl, buf, sizeof(buf), &bytesRead)
            && bytesRead > 0) {
            outFile.write(buf, bytesRead);
            totalDownloaded += bytesRead;
            if (totalSize > 0) {
                downloadProgress = (float)totalDownloaded / (float)totalSize;
            }
        }
        outFile.close();
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);

        downloadProgress = 1.0f;
        statusMessage = "Extracting: " + mapName + "...";

        // Extract to temp folder too
        std::filesystem::path tempExtractFolder =
            std::filesystem::temp_directory_path() / "CustomMapsPlugin_Extract";
        std::filesystem::create_directories(tempExtractFolder);

        std::string zipPath = tempZip.string();
        std::string outPath = tempExtractFolder.string();

        bool extractSuccess = false;
        {
            std::wstring wZipPath(zipPath.begin(), zipPath.end());
            std::wstring wOutPath(outPath.begin(), outPath.end());

            IShellDispatch* pShell = nullptr;
            CoInitialize(NULL);
            HRESULT hr = CoCreateInstance(CLSID_Shell, NULL, CLSCTX_INPROC_SERVER,
                IID_IShellDispatch, (void**)&pShell);

            if (SUCCEEDED(hr) && pShell) {
                VARIANT vZip, vOut, vOpts;
                VariantInit(&vZip);
                VariantInit(&vOut);
                VariantInit(&vOpts);

                vZip.vt = VT_BSTR;
                vZip.bstrVal = SysAllocString(wZipPath.c_str());
                vOut.vt = VT_BSTR;
                vOut.bstrVal = SysAllocString(wOutPath.c_str());
                vOpts.vt = VT_I4;
                vOpts.lVal = 4 | 16 | 512 | 1024;

                Folder* pZipFolder = nullptr;
                Folder* pOutFolder = nullptr;

                hr = pShell->NameSpace(vZip, &pZipFolder);
                if (SUCCEEDED(hr) && pZipFolder) {
                    hr = pShell->NameSpace(vOut, &pOutFolder);
                    if (SUCCEEDED(hr) && pOutFolder) {
                        FolderItems* pItems = nullptr;
                        pZipFolder->Items(&pItems);
                        if (pItems) {
                            VARIANT vItems;
                            VariantInit(&vItems);
                            vItems.vt = VT_DISPATCH;
                            vItems.pdispVal = pItems;
                            hr = pOutFolder->CopyHere(vItems, vOpts);
                            Sleep(3000);
                            extractSuccess = SUCCEEDED(hr);
                            pItems->Release();
                        }
                        pOutFolder->Release();
                    }
                    pZipFolder->Release();
                }

                SysFreeString(vZip.bstrVal);
                SysFreeString(vOut.bstrVal);
                pShell->Release();
            }
            CoUninitialize();
        }

        std::filesystem::remove(tempZip);

        if (extractSuccess) {
            // Copy map files from temp to mods folder (Documents - always writable)
            bool moveSuccess = false;
            for (auto& entry : std::filesystem::directory_iterator(tempExtractFolder)) {
                auto ext = entry.path().extension().string();
                if (ext == ".upk" || ext == ".udk" || ext == ".umap") {
                    try {
                        auto dest = modsFolder / entry.path().filename();
                        std::filesystem::copy_file(entry.path(), dest,
                            std::filesystem::copy_options::overwrite_existing);
                        moveSuccess = true;
                    }
                    catch (...) {}
                }
            }
            std::filesystem::remove_all(tempExtractFolder);
            isDownloading = false;
            downloadProgress = 0.0f;

            if (moveSuccess) {
                installedMaps = GetInstalledMaps();
                statusMessage = "Installed: " + mapName + " — Ready in freeplay!";
            }
            else {
                statusMessage = "Could not save map file. Please try again.";
            }
        }
        else {
            std::filesystem::remove_all(tempExtractFolder);
            isDownloading = false;
            downloadProgress = 0.0f;
            statusMessage = "Extraction failed. Please try again.";
        }
        }).detach();
}

void CustomMapsPlugin::LaunchMap(const std::filesystem::path& mapPath) {
    pendingMapPath = mapPath.string();
    pendingLaunch = true;
    statusMessage = "Launching: " + mapPath.filename().string();
}

std::vector<MapEntry> CustomMapsPlugin::GetInstalledMaps() {
    std::vector<MapEntry> installed;
    auto folder = GetModsFolder();
    if (!std::filesystem::exists(folder)) return installed;
    for (auto& entry : std::filesystem::directory_iterator(folder)) {
        auto ext = entry.path().extension().string();
        if (ext == ".upk" || ext == ".udk") {
            MapEntry m;
            m.name = entry.path().stem().string();
            m.filePath = entry.path().string();
            installed.push_back(m);
        }
    }
    return installed;
}

static std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    return value;
}

bool CustomMapsPlugin::IsPrivateIPv4(const std::string& ip) {
    in_addr addr{};
    if (InetPtonA(AF_INET, ip.c_str(), &addr) != 1) return false;
    unsigned long host = ntohl(addr.s_addr);
    return (host >= 0x0A000000 && host <= 0x0AFFFFFF)       // 10.0.0.0/8
        || (host >= 0xAC100000 && host <= 0xAC1FFFFF)       // 172.16.0.0/12
        || (host >= 0xC0A80000 && host <= 0xC0A8FFFF);     // 192.168.0.0/16
}

bool CustomMapsPlugin::IsVpnAdapter(const std::string& adapterName) {
    std::string name = ToLowerCopy(adapterName);
    return name.find("hamachi") != std::string::npos
        || name.find("zerotier") != std::string::npos
        || name.find("radmin") != std::string::npos
        || name.find("tap") != std::string::npos
        || name.find("tun") != std::string::npos
        || name.find("wireguard") != std::string::npos
        || name.find("nordlynx") != std::string::npos;
}

bool CustomMapsPlugin::IsPortOpen(const std::string& ip, int port, int timeoutMs) {
    if (!winsockInitialized) return false;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;

    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    if (InetPtonA(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        closesocket(sock);
        return false;
    }

    connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);

    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    bool open = select(0, nullptr, &writeSet, nullptr, &timeout) > 0;
    closesocket(sock);
    return open;
}

void CustomMapsPlugin::RefreshNetworkDetection() {
    if (isDetectingNetwork) return;
    isDetectingNetwork = true;
    networkAddresses.clear();

    std::thread([this]() {
        ULONG bufferSize = 15000;
        std::vector<BYTE> buffer(bufferSize);
        IP_ADAPTER_ADDRESSES* adapters =
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

        ULONG result = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            adapters,
            &bufferSize);

        if (result == ERROR_BUFFER_OVERFLOW) {
            buffer.resize(bufferSize);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            result = GetAdaptersAddresses(
                AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr,
                adapters,
                &bufferSize);
        }

        std::vector<NetworkAddress> found;
        if (result == NO_ERROR) {
            for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp) continue;

                std::string adapterName;
                if (adapter->FriendlyName) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                        nullptr, 0, nullptr, nullptr);
                    if (len > 1) {
                        adapterName.resize(len - 1);
                        WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                            adapterName.data(), len, nullptr, nullptr);
                    }
                }
                bool isVpn = IsVpnAdapter(adapterName);

                for (auto* address = adapter->FirstUnicastAddress;
                    address;
                    address = address->Next) {
                    if (address->Address.lpSockaddr->sa_family != AF_INET) continue;

                    char ipStr[INET_ADDRSTRLEN]{};
                    auto* sa = reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr);
                    InetNtopA(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));

                    std::string ip(ipStr);
                    if (ip == "127.0.0.1" || ip == "0.0.0.0") continue;
                    if (!IsPrivateIPv4(ip) && !isVpn) continue;

                    NetworkAddress entry;
                    entry.ip = ip;
                    entry.isVpn = isVpn;
                    entry.label = isVpn
                        ? "VPN (" + adapterName + ")"
                        : "LAN (" + adapterName + ")";
                    found.push_back(entry);
                }
            }
        }

        networkAddresses = found;
        isDetectingNetwork = false;
        }).detach();
}

void CustomMapsPlugin::ScanNetworkForHosts() {
    if (isScanningNetwork || networkAddresses.empty()) return;
    isScanningNetwork = true;
    discoveredHosts.clear();
    scanProgress = 0;

    std::set<std::string> scanIpSet;
    for (auto& addr : networkAddresses) {
        size_t lastDot = addr.ip.rfind('.');
        if (lastDot == std::string::npos) continue;

        std::string prefix = addr.ip.substr(0, lastDot + 1);
        int ownOctet = std::stoi(addr.ip.substr(lastDot + 1));
        for (int i = 1; i <= 254; i++) {
            if (i == ownOctet) continue;
            scanIpSet.insert(prefix + std::to_string(i));
        }
    }

    std::vector<std::string> scanIps(scanIpSet.begin(), scanIpSet.end());

    scanTotal = static_cast<int>(scanIps.size());
    statusMessage = "Scanning network for hosts...";

    std::thread([this, scanIps]() {
        std::vector<DiscoveredHost> found;
        const int batchSize = 24;

        for (size_t offset = 0; offset < scanIps.size(); offset += batchSize) {
            std::vector<std::thread> workers;
            std::mutex foundMutex;

            size_t end = (offset + batchSize < scanIps.size()) ? offset + batchSize : scanIps.size();
            for (size_t i = offset; i < end; i++) {
                workers.emplace_back([&, i]() {
                    if (IsPortOpen(scanIps[i], 7777, 120)) {
                        DiscoveredHost host;
                        host.hostIp = scanIps[i];
                        host.port = 7777;
                        host.gameName = "Rocket League Host";
                        host.players = "?";
                        std::lock_guard<std::mutex> lock(foundMutex);
                        found.push_back(host);
                    }
                    scanProgress = static_cast<int>(i + 1);
                    });
            }

            for (auto& worker : workers) {
                if (worker.joinable()) worker.join();
            }
        }

        discoveredHosts = found;
        isScanningNetwork = false;
        statusMessage = found.empty()
            ? "Scan complete — no hosts found on port 7777."
            : "Scan complete — found " + std::to_string(found.size()) + " host(s).";
        }).detach();
}

void CustomMapsPlugin::HostMultiplayerGame() {
    if (selectedHostMap < 0 || selectedHostMap >= (int)installedMaps.size()) {
        statusMessage = "Select an installed map to host.";
        return;
    }

    auto& map = installedMaps[selectedHostMap];
    std::string mapPath = map.filePath;

    // Close our UI so the game can take focus
    cvarManager->executeCommand("togglemenu custommaps", true);

    // Launch the map as a LAN listen server — other players connect via IP:7777
    // Must use ExecuteUnrealCommand for UE console commands like "open"; cvarManager only handles BakkesMod cvars
    LOG("HostMultiplayerGame: map path = {}", mapPath);
    gameWrapper->SetTimeout([this, mapPath, map](GameWrapper* gw) {
        std::string cmd = "open \"" + mapPath + "\"?listen?game=TAGame.GameInfo_Soccar_TA?PRIVATE=1";
        LOG("HostMultiplayerGame: executing UE command: {}", cmd);
        gw->ExecuteUnrealCommand(cmd);
        LOG("HostMultiplayerGame: ExecuteUnrealCommand returned");
        statusMessage = "Hosting \"" + map.name + "\" — share your IP with friends (port 7777).";
        }, 0.1f);
}

void CustomMapsPlugin::JoinMultiplayerGame(const std::string& ip, int port) {
    if (ip.empty()) {
        statusMessage = "Enter a host IP address to join.";
        return;
    }

    strncpy_s(joinIpBuf, ip.c_str(), sizeof(joinIpBuf) - 1);
    joinPort = port;

    cvarManager->executeCommand("plugin load RocketPlugin", true);
    gameWrapper->SetTimeout([this, ip, port](GameWrapper* gw) {
        cvarManager->executeCommand("togglemenu RocketPlugin", true);
        statusMessage = "Join " + ip + ":" + std::to_string(port)
            + " in Rocket Plugin (enter IP on the right, enable custom map, click Join).";
        }, 0.3f);
}

void CustomMapsPlugin::Render() {
    if (!windowOpen) return;

    ImGui::SetNextWindowSize(ImVec2(780, 600), ImGuiCond_FirstUseEver);
    ImGui::Begin("Custom Maps", &windowOpen, ImGuiWindowFlags_NoCollapse);

    if (ImGui::BeginTabBar("tabs")) {

        // ---- BROWSE TAB ----
        if (ImGui::BeginTabItem("Browse")) {
            ImGui::Text("Name:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180);
            bool nameChanged = ImGui::InputText("##search", searchBuf, sizeof(searchBuf));
            ImGui::SameLine();
            ImGui::Text("Author:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130);
            bool authorChanged = ImGui::InputText("##authorsearch", authorSearchBuf, sizeof(authorSearchBuf));
            ImGui::SameLine();
            bool favChanged = false;
            if (ImGui::Checkbox("Favorites only", &showFavoritesOnly)) {
                favChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) {
                memset(searchBuf, 0, sizeof(searchBuf));
                memset(authorSearchBuf, 0, sizeof(authorSearchBuf));
                showFavoritesOnly = false;
                LoadMapIndex();
            }

            if (nameChanged || authorChanged || favChanged) {
                std::string nameQuery(searchBuf);
                std::string authorQuery(authorSearchBuf);
                std::transform(nameQuery.begin(), nameQuery.end(), nameQuery.begin(), ::tolower);
                std::transform(authorQuery.begin(), authorQuery.end(), authorQuery.begin(), ::tolower);
                filteredMaps.clear();
                for (auto& m : maps) {
                    std::string name = m.name;
                    std::string author = m.author;
                    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                    std::transform(author.begin(), author.end(), author.begin(), ::tolower);
                    bool nameMatch = nameQuery.empty() || name.find(nameQuery) != std::string::npos;
                    bool authorMatch = authorQuery.empty() || author.find(authorQuery) != std::string::npos;
                    bool favMatch = !showFavoritesOnly || IsFavorite(m.name);
                    if (nameMatch && authorMatch && favMatch) {
                        filteredMaps.push_back(m);
                    }
                }
                selectedMap = -1;
                lastSelectedMap = -2;
                previewImage = nullptr;
                currentPreviewUrl = "";
            }

            ImGui::Separator();
            ImGui::Columns(2, "browselayout");
            ImGui::SetColumnWidth(0, 260);

            ImGui::Text("Maps (%zu)", filteredMaps.size());
            ImGui::BeginChild("maplist", ImVec2(0, 400), true);
            for (int i = 0; i < (int)filteredMaps.size(); i++) {
                std::string label = (IsFavorite(filteredMaps[i].name) ? "* " : "  ")
                    + filteredMaps[i].name;
                if (ImGui::Selectable(label.c_str(), selectedMap == i)) {
                    selectedMap = i;
                }
                ImGui::TextDisabled("    by %s", filteredMaps[i].author.c_str());
            }
            ImGui::EndChild();

            ImGui::NextColumn();

            if (selectedMap != lastSelectedMap) {
                lastSelectedMap = selectedMap;
                previewImage = nullptr;
                currentPreviewUrl = "";
                if (selectedMap >= 0 && selectedMap < (int)filteredMaps.size()) {
                    LoadPreviewImage(filteredMaps[selectedMap].previewUrl);
                }
            }

            ImGui::Text("Details");
            ImGui::BeginChild("mapdetail", ImVec2(0, 400), true);
            if (selectedMap >= 0 && selectedMap < (int)filteredMaps.size()) {
                auto& m = filteredMaps[selectedMap];

                if (previewImage && previewImage->IsLoadedForImGui()) {
                    ImTextureID texID = previewImage->GetImGuiTex();
                    if (texID) {
                        float availWidth = ImGui::GetContentRegionAvail().x;
                        auto size = previewImage->GetSizeF();
                        float aspectRatio = size.Y / size.X;
                        float imgWidth = availWidth;
                        float imgHeight = imgWidth * aspectRatio;
                        if (imgHeight > 140) {
                            imgHeight = 140;
                            imgWidth = imgHeight / aspectRatio;
                        }
                        ImGui::Image(texID, ImVec2(imgWidth, imgHeight));
                        ImGui::Separator();
                    }
                }
                else if (loadingPreview) {
                    ImGui::TextDisabled("Loading preview...");
                    ImGui::Separator();
                }

                ImGui::TextWrapped("Name: %s", m.name.c_str());
                ImGui::TextWrapped("Author: %s", m.author.c_str());
                if (!m.fileSize.empty()) {
                    ImGui::TextWrapped("Size: %s", m.fileSize.c_str());
                }
                ImGui::Separator();
                ImGui::TextWrapped("%s", m.description.c_str());
                ImGui::Separator();

                bool isFav = IsFavorite(m.name);
                if (isFav) {
                    if (ImGui::Button("Remove from Favorites", ImVec2(-1, 0))) {
                        ToggleFavorite(m.name);
                    }
                }
                else {
                    if (ImGui::Button("Add to Favorites", ImVec2(-1, 0))) {
                        ToggleFavorite(m.name);
                    }
                }
                ImGui::Spacing();

                if (isDownloading && downloadingMapName == m.name) {
                    ImGui::Text("Downloading...");
                    ImGui::ProgressBar(downloadProgress, ImVec2(-1, 0));
                }
                else {
                    if (ImGui::Button("Download & Install", ImVec2(-1, 0))) {
                        DownloadAndInstallMap(m);
                    }
                }
            }
            else {
                ImGui::TextDisabled("Select a map on the left.");
            }
            ImGui::EndChild();
            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        // ---- FAVORITES TAB ----
        if (ImGui::BeginTabItem("Favorites")) {
            ImGui::Text("Search:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("##favsearch", favSearchBuf, sizeof(favSearchBuf));

            ImGui::Separator();
            ImGui::Columns(2, "favlayout");
            ImGui::SetColumnWidth(0, 260);

            std::string favQuery(favSearchBuf);
            std::transform(favQuery.begin(), favQuery.end(), favQuery.begin(), ::tolower);

            std::vector<MapEntry> favMaps;
            for (auto& m : maps) {
                if (IsFavorite(m.name)) {
                    if (favQuery.empty()) {
                        favMaps.push_back(m);
                    }
                    else {
                        std::string name = m.name;
                        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                        if (name.find(favQuery) != std::string::npos)
                            favMaps.push_back(m);
                    }
                }
            }

            ImGui::Text("Favorite Maps (%zu)", favMaps.size());
            ImGui::BeginChild("favlist", ImVec2(0, 400), true);
            for (int i = 0; i < (int)favMaps.size(); i++) {
                if (ImGui::Selectable(favMaps[i].name.c_str(), selectedFav == i)) {
                    selectedFav = i;
                }
                ImGui::TextDisabled("  by %s", favMaps[i].author.c_str());
            }
            ImGui::EndChild();

            ImGui::NextColumn();
            ImGui::Text("Details");
            ImGui::BeginChild("favdetail", ImVec2(0, 400), true);
            if (selectedFav >= 0 && selectedFav < (int)favMaps.size()) {
                auto& m = favMaps[selectedFav];
                ImGui::TextWrapped("Name: %s", m.name.c_str());
                ImGui::TextWrapped("Author: %s", m.author.c_str());
                if (!m.fileSize.empty()) {
                    ImGui::TextWrapped("Size: %s", m.fileSize.c_str());
                }
                ImGui::Separator();
                ImGui::TextWrapped("%s", m.description.c_str());
                ImGui::Separator();

                if (ImGui::Button("Remove from Favorites", ImVec2(-1, 0))) {
                    ToggleFavorite(m.name);
                    selectedFav = -1;
                }
                ImGui::Spacing();

                for (auto& inst : installedMaps) {
                    if (inst.name == m.name ||
                        inst.name.find(m.name.substr(0, 8)) != std::string::npos) {
                        if (ImGui::Button("Launch in Freeplay", ImVec2(-1, 0))) {
                            LaunchMap(inst.filePath);
                        }
                        ImGui::Spacing();
                        break;
                    }
                }

                if (isDownloading && downloadingMapName == m.name) {
                    ImGui::Text("Downloading...");
                    ImGui::ProgressBar(downloadProgress, ImVec2(-1, 0));
                }
                else {
                    if (ImGui::Button("Download & Install", ImVec2(-1, 0))) {
                        DownloadAndInstallMap(m);
                    }
                }
            }
            else {
                ImGui::TextDisabled("Select a map on the left.");
            }
            ImGui::EndChild();
            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        // ---- INSTALLED TAB ----
        if (ImGui::BeginTabItem("Installed")) {
            ImGui::Text("Search:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            ImGui::InputText("##instsearch", instSearchBuf, sizeof(instSearchBuf));

            ImGui::Separator();
            ImGui::Columns(2, "installedlayout");
            ImGui::SetColumnWidth(0, 280);

            std::string instQuery(instSearchBuf);
            std::transform(instQuery.begin(), instQuery.end(), instQuery.begin(), ::tolower);

            std::vector<MapEntry> filteredInstalled;
            for (auto& m : installedMaps) {
                if (instQuery.empty()) {
                    filteredInstalled.push_back(m);
                }
                else {
                    std::string name = m.name;
                    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                    if (name.find(instQuery) != std::string::npos)
                        filteredInstalled.push_back(m);
                }
            }

            ImGui::Text("Installed Maps (%zu)", filteredInstalled.size());
            ImGui::BeginChild("installedlist", ImVec2(0, 400), true);
            for (int i = 0; i < (int)filteredInstalled.size(); i++) {
                if (ImGui::Selectable(
                    filteredInstalled[i].name.c_str(),
                    selectedInstalled == i)) {
                    selectedInstalled = i;
                }
            }
            ImGui::EndChild();

            ImGui::NextColumn();
            ImGui::Text("Actions");
            ImGui::BeginChild("installeddetail", ImVec2(0, 400), true);
            if (selectedInstalled >= 0
                && selectedInstalled < (int)filteredInstalled.size()) {
                auto& m = filteredInstalled[selectedInstalled];
                ImGui::TextWrapped("%s", m.name.c_str());
                ImGui::Separator();
                if (ImGui::Button("Launch in Freeplay", ImVec2(-1, 0))) {
                    LaunchMap(m.filePath);
                }
                ImGui::Spacing();
                if (ImGui::Button("Delete Map", ImVec2(-1, 0))) {
                    std::filesystem::remove(m.filePath);
                    selectedInstalled = -1;
                    installedMaps = GetInstalledMaps();
                    statusMessage = "Deleted: " + m.name;
                }
            }
            else {
                ImGui::TextDisabled("Select a map on the left.");
            }
            ImGui::EndChild();
            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        // ---- MULTIPLAYER TAB ----
        if (ImGui::BeginTabItem("Multiplayer")) {
            if (!multiplayerTabOpened) {
                multiplayerTabOpened = true;
                RefreshNetworkDetection();
            }

            ImGui::Columns(2, "multiplayerlayout");
            ImGui::SetColumnWidth(0, 380);

            // --- Host section ---
            ImGui::Text("Host a Game");
            ImGui::Separator();

            ImGui::Text("Lobby Name:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##lobbyname", lobbyNameBuf, sizeof(lobbyNameBuf));

            ImGui::Text("Players:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::InputInt("##hostplayers", &hostPlayerCount, 0, 0);
            if (hostPlayerCount < 2) hostPlayerCount = 2;
            if (hostPlayerCount > 8) hostPlayerCount = 8;

            ImGui::Checkbox("Add Bots", &hostAddBots);
            ImGui::Checkbox("Password protected", &hostPasswordProtected);
            if (hostPasswordProtected) {
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##hostpassword", hostPasswordBuf, sizeof(hostPasswordBuf),
                    ImGuiInputTextFlags_Password);
            }

            ImGui::Spacing();
            ImGui::Text("Select Map to Host:");
            ImGui::BeginChild("hostmaplist", ImVec2(0, 120), true);
            if (installedMaps.empty()) {
                ImGui::TextDisabled("No maps installed. Download maps from Browse tab first!");
            }
            else {
                for (int i = 0; i < (int)installedMaps.size(); i++) {
                    if (ImGui::Selectable(installedMaps[i].name.c_str(), selectedHostMap == i)) {
                        selectedHostMap = i;
                    }
                }
            }
            ImGui::EndChild();

            ImGui::Spacing();
            if (isDetectingNetwork) {
                ImGui::TextDisabled("Detecting network adapters...");
            }
            else if (networkAddresses.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "No network connection detected.");
                if (ImGui::Button("Retry Detection")) {
                    RefreshNetworkDetection();
                }
            }
            else {
                bool hasLan = false;
                bool hasVpn = false;
                for (auto& addr : networkAddresses) {
                    if (addr.isVpn) hasVpn = true;
                    else hasLan = true;
                }

                if (hasLan) {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                        "LAN ready — friends on the same network can join directly.");
                }
                if (hasVpn) {
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                        "VPN detected — remote friends can join via VPN IP.");
                }
                if (!hasLan && hasVpn) {
                    ImGui::TextWrapped(
                        "No physical LAN found. VPN-only mode — install Hamachi, ZeroTier, "
                        "or Radmin VPN for remote friends, or connect to the same Wi-Fi for LAN.");
                }

                ImGui::Text("Share this address with friends:");
                for (auto& addr : networkAddresses) {
                    ImGui::BulletText("%s  %s:7777", addr.label.c_str(), addr.ip.c_str());
                }

                if (ImGui::Button("Retry Detection")) {
                    RefreshNetworkDetection();
                }
            }

            ImGui::Spacing();
            bool canHost = !networkAddresses.empty()
                && selectedHostMap >= 0
                && selectedHostMap < (int)installedMaps.size();
            if (!canHost) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
            if (ImGui::Button("Host Game", ImVec2(-1, 0)) && canHost) {
                HostMultiplayerGame();
            }
            if (!canHost) {
                ImGui::PopStyleVar();
                ImGui::TextDisabled("Select an installed map and connect to a network to host.");
            }

            ImGui::NextColumn();

            // --- Join / browse section ---
            ImGui::Text("Available Games");
            ImGui::SameLine();
            if (isScanningNetwork) {
                ImGui::TextDisabled("Scanning... (%d/%d)", scanProgress, scanTotal);
            }
            else if (ImGui::Button("Scan Network")) {
                if (networkAddresses.empty()) {
                    RefreshNetworkDetection();
                    statusMessage = "Detecting network first — click Scan Network again shortly.";
                }
                else {
                    ScanNetworkForHosts();
                }
            }

            ImGui::Separator();
            ImGui::Text("Join by IP:");
            ImGui::SetNextItemWidth(160);
            ImGui::InputText("##joinip", joinIpBuf, sizeof(joinIpBuf));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            ImGui::InputInt("##joinport", &joinPort, 0, 0);
            if (joinPort < 1) joinPort = 7777;
            ImGui::SameLine();
            if (ImGui::Button("Join")) {
                JoinMultiplayerGame(joinIpBuf, joinPort);
            }

            ImGui::BeginChild("hostlist", ImVec2(0, 320), true);
            if (discoveredHosts.empty() && !isScanningNetwork) {
                ImGui::TextDisabled("No games found. Click 'Scan Network' or ask a friend to host.");
            }
            else {
                ImGui::Columns(4, "hostcols");
                ImGui::SetColumnWidth(0, 140);
                ImGui::SetColumnWidth(1, 140);
                ImGui::SetColumnWidth(2, 60);
                ImGui::Text("Game"); ImGui::NextColumn();
                ImGui::Text("Host IP"); ImGui::NextColumn();
                ImGui::Text("Players"); ImGui::NextColumn();
                ImGui::Text("Actions"); ImGui::NextColumn();
                ImGui::Separator();

                for (int i = 0; i < (int)discoveredHosts.size(); i++) {
                    auto& host = discoveredHosts[i];
                    ImGui::PushID(i);
                    ImGui::Text("%s", host.gameName.c_str()); ImGui::NextColumn();
                    ImGui::Text("%s:%d", host.hostIp.c_str(), host.port); ImGui::NextColumn();
                    ImGui::Text("%s", host.players.c_str()); ImGui::NextColumn();
                    if (ImGui::SmallButton("Join")) {
                        JoinMultiplayerGame(host.hostIp, host.port);
                    }
                    ImGui::NextColumn();
                    ImGui::PopID();
                }
                ImGui::Columns(1);
            }
            ImGui::EndChild();

            ImGui::Columns(1);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    if (isDownloading) {
        ImGui::Separator();
        ImGui::Text("Downloading: %s", downloadingMapName.c_str());
        ImGui::ProgressBar(downloadProgress, ImVec2(-1, 0));
    }
    else if (!statusMessage.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("Status: %s", statusMessage.c_str());
    }

    ImGui::End();

    if (pendingLaunch) {
        pendingLaunch = false;
        std::string mapPath = pendingMapPath;
        gameWrapper->SetTimeout([this, mapPath](GameWrapper* gw) {
            cvarManager->executeCommand("load_workshop \"" + mapPath + "\"", true);
            }, 0.5f);
    }

    if (!windowOpen) {
        windowOpen = true;
        cvarManager->executeCommand("togglemenu custommaps");
    }
}