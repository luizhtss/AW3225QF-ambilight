// Alienware Monitor Ambilight - Direct HID Version
// Captures screen colors via DXGI and maps them to monitor LEDs.
//
// LED mask mapping (confirmed via hid_debug):
//   mask 0x01 = Alien head logo
//   mask 0x02 = "32" inch number
//   mask 0x03 = both front LEDs
//   mask 0xFF = all LEDs
//
// AWCC/AlienFXSubAgent must be stopped for direct HID access.

#include "DXGIManager.hpp"

#include <windows.h>
extern "C" {
#include <initguid.h>
#include <hidclass.h>
#include <hidsdi.h>
}
#include <SetupAPI.h>

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cmath>
#include <csignal>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ---- Configuration ----
static int TARGET_FPS       = 30;
static int SAMPLE_STEP      = 8;
static float SMOOTHING      = 0.3f;
static int MIN_BRIGHTNESS   = 5;
static int MONITOR_INDEX    = 0;

// LED masks (bitmask, not index)
static unsigned char MASK_ALIEN = 0x01;  // Alien head logo
static unsigned char MASK_32    = 0x02;  // "32" inch number
static unsigned char MASK_ALL   = 0xFF;  // All LEDs
// ---- End Configuration ----

static std::atomic<bool> g_running(true);
static bool g_awcc_was_killed = false;

void signalHandler(int) {
    g_running = false;
}

// ---- HID Device Handle ----

static HANDLE g_hidDevice = INVALID_HANDLE_VALUE;
static int g_reportLen = 0;

HANDLE openAlienwareHID() {
    HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_HID, NULL, NULL,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA did = { sizeof(SP_DEVICE_INTERFACE_DATA) };
    int idx = 0;
    HANDLE found = INVALID_HANDLE_VALUE;

    while (SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_HID, idx++, &did)) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetail(hDevInfo, &did, NULL, 0, &reqSize, NULL);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA*)new unsigned char[reqSize];
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (SetupDiGetDeviceInterfaceDetail(hDevInfo, &did, detail, reqSize, NULL, NULL)) {
            HANDLE h = CreateFile(detail->DevicePath,
                                  GENERIC_WRITE | GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, 0, NULL);

            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attr = { sizeof(HIDD_ATTRIBUTES) };
                if (HidD_GetAttributes(h, &attr)) {
                    if (attr.VendorID == 0x187c) {
                        PHIDP_PREPARSED_DATA prep;
                        HIDP_CAPS caps;
                        if (HidD_GetPreparsedData(h, &prep)) {
                            HidP_GetCaps(prep, &caps);
                            HidD_FreePreparsedData(prep);

                            if (caps.OutputReportByteLength == 65) {
                                g_reportLen = 65;
                                found = h;
                                h = INVALID_HANDLE_VALUE;

                                wchar_t wbuf[256];
                                std::string prod;
                                if (HidD_GetProductString(h == INVALID_HANDLE_VALUE ? found : h, wbuf, 255))
                                    for (int i = 0; i < (int)wcslen(wbuf); i++) prod += (char)wbuf[i];

                                std::cout << "Found monitor HID: VID=0x" << std::hex << attr.VendorID
                                     << " PID=0x" << attr.ProductID << std::dec
                                     << " (" << prod << ")\n";

                                delete[] (unsigned char*)detail;
                                SetupDiDestroyDeviceInfoList(hDevInfo);
                                return found;
                            }
                        }
                    }
                }
                if (h != INVALID_HANDLE_VALUE)
                    CloseHandle(h);
            }
        }
        delete[] (unsigned char*)detail;
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return INVALID_HANDLE_VALUE;
}

// ---- V6 Protocol Commands ----

bool hidSend(const unsigned char* buf, int len) {
    DWORD written = 0;
    return WriteFile(g_hidDevice, buf, len, &written, NULL) != 0;
}

void v6Reset() {
    unsigned char buf[65];
    memset(buf, 0xFF, 65);
    buf[0] = 0x00;
    buf[1] = 0x95;
    buf[2] = 0x00;
    buf[3] = 0x00;
    buf[4] = 0x00;
    hidSend(buf, 65);
}

void v6SetColor(unsigned char mask, unsigned char r, unsigned char g, unsigned char b) {
    unsigned char buf[65];
    memset(buf, 0xFF, 65);

    // V6 color command envelope
    buf[0]  = 0x00;  // Report ID
    buf[1]  = 0x92;  // COMMV6_colorSet[0]
    buf[2]  = 0x37;  // COMMV6_colorSet[1]
    buf[3]  = 0x0A;  // Payload length = 10
    buf[4]  = 0x00;

    // Color command payload
    buf[5]  = 0x51;  // Sub-command
    buf[6]  = 0x87;  // Opcode: color set
    buf[7]  = 0xD0;  // ?
    buf[8]  = 0x04;  // Sub-length
    buf[9]  = mask;  // Light bitmask
    buf[10] = r;
    buf[11] = g;
    buf[12] = b;
    buf[13] = 0x64;  // Max brightness for V6 protocol (0..100)
    buf[14] = (unsigned char)(r ^ g ^ b ^ mask ^ 8);  // Checksum

    hidSend(buf, 65);
}

// ---- AWCC Process Management ----

static const char* AWCC_PROCESSES[] = {
    "AlienFXSubAgent.exe",
    "alienfx-gui.exe",
    NULL
};

bool killAwccProcesses() {
    bool killed = false;
    for (int i = 0; AWCC_PROCESSES[i]; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "taskkill /F /IM %s >nul 2>&1", AWCC_PROCESSES[i]);
        if (system(cmd) == 0) {
            std::cout << "  Stopped: " << AWCC_PROCESSES[i] << "\n";
            killed = true;
        }
    }
    if (killed) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return killed;
}

void restoreAwccProcesses() {
    std::cout << "Restarting AWCC sub-agents...\n";
    system("start \"\" \"C:\\Program Files\\Alienware\\Alienware Command Center\\AWCC\\AWCC.SCSubAgent.exe\" >nul 2>&1");
    system("start \"\" \"C:\\Program Files\\Alienware\\Alienware Command Center\\AWCC\\AWCC.UCSubAgent.exe\" >nul 2>&1");
}

// ---- Color Helpers ----

struct ColorRGB {
    float r, g, b;
    ColorRGB() : r(0), g(0), b(0) {}
    ColorRGB(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}

    ColorRGB lerp(const ColorRGB& target, float t) const {
        return ColorRGB(
            r + (target.r - r) * t,
            g + (target.g - g) * t,
            b + (target.b - b) * t
        );
    }

    ColorRGB saturate(float factor) const {
        float gray = 0.299f * r + 0.587f * g + 0.114f * b;
        return ColorRGB(
            (std::min)(255.0f, (std::max)(0.0f, gray + (r - gray) * factor)),
            (std::min)(255.0f, (std::max)(0.0f, gray + (g - gray) * factor)),
            (std::min)(255.0f, (std::max)(0.0f, gray + (b - gray) * factor))
        );
    }
};

ColorRGB averageColor(const BYTE* buf, int imgWidth, int imgHeight,
                      int x0, int y0, int x1, int y1, int step) {
    double rSum = 0, gSum = 0, bSum = 0;
    int count = 0;

    x0 = (std::max)(0, x0);
    y0 = (std::max)(0, y0);
    x1 = (std::min)(imgWidth, x1);
    y1 = (std::min)(imgHeight, y1);

    for (int y = y0; y < y1; y += step) {
        const BYTE* row = buf + (size_t)y * imgWidth * 4;
        for (int x = x0; x < x1; x += step) {
            int idx = x * 4;
            bSum += row[idx + 0];
            gSum += row[idx + 1];
            rSum += row[idx + 2];
            count++;
        }
    }

    if (count == 0) return ColorRGB();
    return ColorRGB((float)(rSum / count), (float)(gSum / count), (float)(bSum / count));
}

// ---- Probe Mode ----

void probeDevice() {
    std::cout << "LED Probe Mode - testing masks\n"
         << "Each mask will light RED for 3s.\n\n";

    unsigned char masks[] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
    const char* names[] = { "0x01", "0x02", "0x04", "0x08", "0x10", "0x20", "0x40", "0x80" };

    for (int i = 0; i < 8; i++) {
        std::cout << "  Testing mask " << names[i] << " (RED)... ";
        std::cout.flush();

        v6Reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        v6SetColor(masks[i], 255, 0, 0);

        std::this_thread::sleep_for(std::chrono::seconds(3));

        v6Reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        v6SetColor(masks[i], 0, 0, 0);

        std::cout << "done.\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\nProbe complete!\n"
         << "  mask 0x01 = Alien head logo\n"
         << "  mask 0x02 = 32 inch number\n";
}

// ---- UI ----

void printUsage() {
    std::cout << "Alienware Monitor Ambilight (Direct HID)\n"
         << "==========================================\n"
         << "Maps screen colors to your AW3225QF monitor LEDs.\n\n"
         << "Options:\n"
         << "  --fps N          Update rate (default: 30)\n"
         << "  --step N         Pixel sample step (default: 8)\n"
         << "  --smooth F       Smoothing 0.0-0.9 (default: 0.3)\n"
         << "  --monitor N      Monitor to capture (default: 0)\n"
         << "  --kill-awcc      Stop AWCC for direct HID access\n"
         << "  --restore-awcc   Restart AWCC and exit\n"
         << "  --probe          Test each LED mask\n"
         << "  --help           Show this help\n\n"
         << "Usage:\n"
         << "  ambilight.exe --kill-awcc           (run ambilight)\n"
         << "  ambilight.exe --kill-awcc --probe   (identify LEDs)\n"
         << "  ambilight.exe --restore-awcc        (restart AWCC)\n\n";
}

int main(int argc, char* argv[]) {
    bool doProbe = false;
    bool doKillAwcc = false;
    bool doRestoreAwcc = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { printUsage(); return 0; }
        else if (arg == "--probe") { doProbe = true; }
        else if (arg == "--kill-awcc") { doKillAwcc = true; }
        else if (arg == "--restore-awcc") { doRestoreAwcc = true; }
        else if (arg == "--fps" && i + 1 < argc) TARGET_FPS = atoi(argv[++i]);
        else if (arg == "--step" && i + 1 < argc) SAMPLE_STEP = atoi(argv[++i]);
        else if (arg == "--smooth" && i + 1 < argc) SMOOTHING = (float)atof(argv[++i]);
        else if (arg == "--monitor" && i + 1 < argc) MONITOR_INDEX = atoi(argv[++i]);
    }

    if (doRestoreAwcc) {
        restoreAwccProcesses();
        return 0;
    }

    if (doKillAwcc) {
        std::cout << "Stopping AWCC/AlienFX processes...\n";
        g_awcc_was_killed = killAwccProcesses();
        if (!g_awcc_was_killed) {
            std::cout << "  No AWCC processes found running.\n";
        }
    }

    // Open HID device
    g_hidDevice = openAlienwareHID();
    if (g_hidDevice == INVALID_HANDLE_VALUE) {
        std::cerr << "ERROR: Could not open Alienware HID device!\n";
        if (!doKillAwcc) {
            std::cerr << "AWCC may be blocking access. Try: ambilight.exe --kill-awcc\n";
        }
        return 1;
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    if (doProbe) {
        probeDevice();
        CloseHandle(g_hidDevice);
        if (g_awcc_was_killed) restoreAwccProcesses();
        return 0;
    }

    std::cout << "Alienware Monitor Ambilight (Direct HID)\n"
         << "==========================================\n"
         << "FPS: " << TARGET_FPS
         << "  Sample step: " << SAMPLE_STEP
         << "  Smoothing: " << SMOOTHING << "\n";

    // Init DXGI screen capture
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    DXGIManager dxgi;
    dxgi.set_timeout(100);
    dxgi.set_capture_source(MONITOR_INDEX);

    RECT rect = dxgi.get_output_rect();
    int screenW = rect.right - rect.left;
    int screenH = rect.bottom - rect.top;

    if (screenW <= 0 || screenH <= 0) {
        std::cerr << "ERROR: Could not get screen dimensions.\n";
        CloseHandle(g_hidDevice);
        CoUninitialize();
        return 1;
    }

    std::cout << "Capturing screen: " << screenW << "x" << screenH << "\n";
    std::cout << "LED mapping:\n"
         << "  Left half  -> mask 0x02 (32 number)\n"
         << "  Right half -> mask 0x01 (Alien head)\n";
    std::cout << "Press Ctrl+C to stop.\n\n";

    // Send initial reset
    v6Reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto frameDuration = std::chrono::microseconds(1000000 / TARGET_FPS);

    ColorRGB prevLeft, prevRight;
    bool firstFrame = true;
    int frameCount = 0;

    while (g_running) {
        auto frameStart = std::chrono::steady_clock::now();

        BYTE* frameBuf = nullptr;
        size_t bufSize = 0;

        CaptureResult cr = dxgi.get_output_data(&frameBuf, &bufSize);

        if (cr == CR_OK && frameBuf) {
            int halfW = screenW / 2;

            ColorRGB leftColor = averageColor(frameBuf, screenW, screenH,
                                              0, 0, halfW, screenH, SAMPLE_STEP);
            ColorRGB rightColor = averageColor(frameBuf, screenW, screenH,
                                               halfW, 0, screenW, screenH, SAMPLE_STEP);

            leftColor = leftColor.saturate(1.5f);
            rightColor = rightColor.saturate(1.5f);

            if (!firstFrame) {
                leftColor = prevLeft.lerp(leftColor, 1.0f - SMOOTHING);
                rightColor = prevRight.lerp(rightColor, 1.0f - SMOOTHING);
            }
            firstFrame = false;
            prevLeft = leftColor;
            prevRight = rightColor;

            auto clamp = [](float v) -> unsigned char {
                return (unsigned char)(std::max)(0.0f, (std::min)(255.0f, v));
            };

            unsigned char lr = clamp(leftColor.r), lg = clamp(leftColor.g), lb = clamp(leftColor.b);
            unsigned char rr = clamp(rightColor.r), rg = clamp(rightColor.g), rb = clamp(rightColor.b);

            int brightness0 = (std::max)((int)lr, (std::max)((int)lg, (int)lb));
            int brightness1 = (std::max)((int)rr, (std::max)((int)rg, (int)rb));

            if (brightness0 < MIN_BRIGHTNESS) lr = lg = lb = 0;
            if (brightness1 < MIN_BRIGHTNESS) rr = rg = rb = 0;

            // Send colors directly via HID
            // Left half -> "32" LED (mask 0x02)
            // Right half -> Alien head (mask 0x01)
            v6SetColor(MASK_32, lr, lg, lb);
            v6SetColor(MASK_ALIEN, rr, rg, rb);

            if (++frameCount % TARGET_FPS == 0) {
                printf("\r32[%3d,%3d,%3d] Alien[%3d,%3d,%3d]",
                       lr, lg, lb, rr, rg, rb);
                fflush(stdout);
            }
        } else if (cr == CR_ACCESS_LOST || cr == CR_REFRESH_FAILURE) {
            dxgi.refresh_output();
            rect = dxgi.get_output_rect();
            screenW = rect.right - rect.left;
            screenH = rect.bottom - rect.top;
        }

        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        if (elapsed < frameDuration) {
            std::this_thread::sleep_for(frameDuration - elapsed);
        }
    }

    // Turn off LEDs on exit
    std::cout << "\nShutting down...\n";
    v6Reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    v6SetColor(MASK_ALL, 0, 0, 0);

    CloseHandle(g_hidDevice);
    CoUninitialize();

    if (g_awcc_was_killed) {
        restoreAwccProcesses();
    }

    std::cout << "Done.\n";
    return 0;
}
