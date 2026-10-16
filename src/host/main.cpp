#include "openxr_manager.h"
#include "ipc_bridge.h"
#include <iostream>
#include <windows.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

static RiddickVR::OpenXRManager* g_pManager = nullptr;

static float ReadConfigFloat(const char* path, const char* key, float fallback) {
    char value[64] = {};
    if (GetPrivateProfileStringA("VR", key, "", value, sizeof(value), path) == 0 || value[0] == '\0')
        return fallback;
    char* end = nullptr;
    float parsed = std::strtof(value, &end);
    return (end == value || !std::isfinite(parsed)) ? fallback : parsed;
}

static std::string FindConfigPath(const std::string& requested) {
    if (!requested.empty())
        return GetFileAttributesA(requested.c_str()) != INVALID_FILE_ATTRIBUTES ? requested : std::string{};

    char path[MAX_PATH] = {};
    if (GetFileAttributesA("riddick_vr.ini") != INVALID_FILE_ATTRIBUTES)
        return "riddick_vr.ini";

    if (GetModuleFileNameA(nullptr, path, MAX_PATH) > 0) {
        char* slash = std::strrchr(path, '\\');
        if (slash) {
            slash[1] = '\0';
            strcat_s(path, MAX_PATH, "riddick_vr.ini");
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return path;
        }
    }
    return {};
}

BOOL WINAPI ConsoleHandlerRoutine(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_CLOSE_EVENT) {
        std::cout << "\n[Host] Termination signal received. Cleaning up..." << std::endl;
        if (g_pManager) {
            g_pManager->RequestExit();
        }
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[]) {
    bool validateOnly = false;
    bool verbose = false;
    bool autoExitWithGame = false;
    bool mockMode = false;
    bool scenarioImmediate = false;
    std::string dumpDir = "";
    std::string configPath = "";
    std::string scenarioPath = "";
    std::string scenarioName = "";
    std::string scenarioDir = "";
    uint32_t maxFrames = 0;
    uint32_t dumpInterval = 10;
    bool dumpGameplayOnly = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--validate-only") {
            validateOnly = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--auto-exit-with-game") {
            autoExitWithGame = true;
        } else if (arg == "--mock") {
            mockMode = true;
        } else if (arg == "--dump-dir" && i + 1 < argc) {
            dumpDir = argv[++i];
        } else if (arg == "--max-frames" && i + 1 < argc) {
            maxFrames = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--dump-interval" && i + 1 < argc) {
            dumpInterval = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--dump-gameplay-only") {
            dumpGameplayOnly = true;
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--scenario-file" && i + 1 < argc) {
            scenarioPath = argv[++i];
        } else if (arg == "--scenario" && i + 1 < argc) {
            scenarioName = argv[++i];
        } else if (arg == "--scenario-immediate") {
            scenarioImmediate = true;
        } else if (arg == "--scenario-dir" && i + 1 < argc) {
            scenarioDir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: riddickvr-host [OPTIONS]\n"
                      << "Options:\n"
                      << "  --validate-only         Verify OpenXR runtime and exit without running VR loop\n"
                      << "  --auto-exit-with-game   Exit automatically when game process disconnects\n"
                      << "  --mock                  Run in headless simulated VR mock mode (no headset needed)\n"
                      << "  --dump-dir <PATH>       Directory to save test frame screenshots (BMP format)\n"
                      << "  --dump-interval <N>     Save screenshot every N frames (default: 10)\n"
                      << "  --dump-gameplay-only    Only dump frames the hook reports as 3D gameplay (#69)\n"
                      << "  --max-frames <N>        Exit automatically after N rendered frames\n"
                      << "  --config <PATH>         Read VR settings from this riddick_vr.ini\n"
                      << "  --scenario-file <PATH>  Play a JSON mock-controller scenario\n"
                      << "  --scenario <NAME>       Play tools/scenarios/<NAME>.json trajectory in mock mode\n"
                      << "  --scenario-dir <PATH>   Scenario directory override\n"
                      << "  --scenario-immediate    Start the scenario timeline at frame 0 (default: wait for gameplay)\n"
                      << "  --verbose               Enable verbose debug logging\n"
                      << "  --help, -h              Show this help message\n";
            return 0;
        }
    }

    std::cout << "========================================\n"
              << "   Riddick VR - x64 OpenXR Companion    \n"
              << "========================================\n";

    if (validateOnly) {
        std::cout << "[Host] Running in validation-only mode (--validate-only)...\n";
    }

    SetConsoleCtrlHandler(ConsoleHandlerRoutine, TRUE);

    // Initialize IPC Bridge Host
    RiddickVR::IpcBridgeHost ipcHost;
    if (!ipcHost.Initialize()) {
        std::cerr << "[Host] ERROR: Failed to create IPC shared memory mapping!\n";
        return 1;
    }
    std::cout << "[Host] Shared memory IPC bridge initialized successfully.\n";

    // Initialize OpenXR Manager
    RiddickVR::OpenXRManager xrManager;
    g_pManager = &xrManager;

    const std::string resolvedConfig = FindConfigPath(configPath);
    if (!resolvedConfig.empty()) {
        constexpr float defaultX = 0.75f, defaultY = 0.75f, defaultW = 0.23f, defaultH = 0.23f;
        xrManager.SetAmmoQuadCrop(
            ReadConfigFloat(resolvedConfig.c_str(), "AmmoQuadCropX", defaultX),
            ReadConfigFloat(resolvedConfig.c_str(), "AmmoQuadCropY", defaultY),
            ReadConfigFloat(resolvedConfig.c_str(), "AmmoQuadCropW", defaultW),
            ReadConfigFloat(resolvedConfig.c_str(), "AmmoQuadCropH", defaultH));
        std::cout << "[Host] Ammo quad crop config: " << resolvedConfig << std::endl;
    } else if (!configPath.empty()) {
        std::cerr << "[Host] WARNING: config file not found: " << configPath << " (using defaults)\n";
    }

    if (mockMode) {
        xrManager.SetMockMode(true, dumpDir, maxFrames, dumpInterval);
        xrManager.SetDumpGameplayOnly(dumpGameplayOnly);
    }
    xrManager.SetScenarioStartOnGameplay(!scenarioImmediate);
    if (!scenarioPath.empty()) {
        xrManager.SetScenarioFile(scenarioPath);
    }

    // GH #48: resolve scenario file (name -> <scenario-dir>/<name>.json).
    if (!scenarioName.empty()) {
        std::string dir = scenarioDir;
        if (dir.empty()) {
            char mod[MAX_PATH] = {};
            if (GetModuleFileNameA(nullptr, mod, MAX_PATH) > 0) {
                std::string m(mod);
                size_t p = m.find_last_of("\\/");
                dir = (p == std::string::npos) ? "." : m.substr(0, p);
                dir += "\\..\\..\\tools\\scenarios";
            } else {
                dir = "tools/scenarios";
            }
        }
        xrManager.SetScenarioFile(dir + "\\" + scenarioName + ".json");
    }

    if (validateOnly) {
        std::string report;
        bool valid = xrManager.Validate(report);
        std::cout << report << std::endl;
        xrManager.Shutdown();
        ipcHost.Shutdown();
        return valid ? 0 : 1;
    }

    if (!xrManager.Initialize()) {
        std::cerr << "[Host] ERROR: Failed to initialize OpenXR Manager!\n";
        ipcHost.Shutdown();
        return 1;
    }

    // Run VR loop
    xrManager.RunLoop(ipcHost, autoExitWithGame);

    // Cleanup
    xrManager.Shutdown();
    ipcHost.Shutdown();
    std::cout << "[Host] Exited cleanly.\n";
    return 0;
}
