// Alienware AW3225QF LED Demo
// Interactive tool to test LED colors with clear feedback.
//
// Runs a pre-defined sequence showing which LED changes and to what color,
// then offers an interactive mode.

#include <windows.h>
extern "C" {
#include <initguid.h>
#include <hidclass.h>
#include <hidsdi.h>
}
#include <SetupAPI.h>
#include <iostream>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static HANDLE g_hid = INVALID_HANDLE_VALUE;

// ---- HID helpers ----

HANDLE openDevice() {
    HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_HID, NULL, NULL,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA did = { sizeof(SP_DEVICE_INTERFACE_DATA) };
    int idx = 0;

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
                if (HidD_GetAttributes(h, &attr) && attr.VendorID == 0x187c) {
                    PHIDP_PREPARSED_DATA prep;
                    HIDP_CAPS caps;
                    if (HidD_GetPreparsedData(h, &prep)) {
                        HidP_GetCaps(prep, &caps);
                        HidD_FreePreparsedData(prep);
                        if (caps.OutputReportByteLength == 65) {
                            delete[] (unsigned char*)detail;
                            SetupDiDestroyDeviceInfoList(hDevInfo);
                            return h;
                        }
                    }
                }
                CloseHandle(h);
            }
        }
        delete[] (unsigned char*)detail;
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return INVALID_HANDLE_VALUE;
}

void hidSend(const unsigned char* buf) {
    DWORD written = 0;
    WriteFile(g_hid, buf, 65, &written, NULL);
}

void v6Reset() {
    unsigned char buf[65];
    memset(buf, 0xFF, 65);
    buf[0] = 0x00; buf[1] = 0x95; buf[2] = 0x00; buf[3] = 0x00; buf[4] = 0x00;
    hidSend(buf);
}

void v6SetColor(unsigned char mask, unsigned char r, unsigned char g, unsigned char b) {
    unsigned char buf[65];
    memset(buf, 0xFF, 65);
    buf[0]  = 0x00; buf[1]  = 0x92; buf[2]  = 0x37;
    buf[3]  = 0x0A; buf[4]  = 0x00;
    buf[5]  = 0x51; buf[6]  = 0x87; buf[7]  = 0xD0; buf[8]  = 0x04;
    buf[9]  = mask;
    buf[10] = r; buf[11] = g; buf[12] = b;
    buf[13] = 0x64;
    buf[14] = (unsigned char)(r ^ g ^ b ^ mask ^ 8);
    hidSend(buf);
}

// ---- Color names ----

struct NamedColor {
    const char* name;
    unsigned char r, g, b;
};

static NamedColor COLORS[] = {
    {"VERMELHO",  255,   0,   0},
    {"VERDE",       0, 255,   0},
    {"AZUL",        0,   0, 255},
    {"AMARELO",   255, 255,   0},
    {"CIANO",       0, 255, 255},
    {"MAGENTA",   255,   0, 255},
    {"BRANCO",    255, 255, 255},
    {"LARANJA",   255, 128,   0},
    {"ROXO",      128,   0, 255},
    {"ROSA",      255,  50, 150},
    {"OFF",         0,   0,   0},
};

struct LedTarget {
    const char* name;
    unsigned char mask;
};

static LedTarget LEDS[] = {
    {"Alien Head",  0x01},
    {"32 (numero)", 0x02},
    {"Ambos",       0x03},
    {"Todos",       0xFF},
};

void printStep(const char* led, unsigned char mask, const char* color, unsigned char r, unsigned char g, unsigned char b) {
    printf("  >>> %-15s (mask=0x%02X)  ->  %-10s (R=%3d G=%3d B=%3d)\n",
           led, mask, color, r, g, b);
}

// ---- Demo Sequence ----

void runDemo() {
    std::cout << "\n========================================\n"
              << "  DEMO: Sequencia de cores nos LEDs\n"
              << "========================================\n\n"
              << "  Cada passo mostra qual LED muda e qual cor.\n"
              << "  Espere 2 segundos entre cada passo.\n\n";

    struct Step {
        const char* led;
        unsigned char mask;
        const char* color;
        unsigned char r, g, b;
    };

    Step steps[] = {
        // Alien head tests
        {"Alien Head",  0x01, "VERMELHO",  255,   0,   0},
        {"Alien Head",  0x01, "VERDE",       0, 255,   0},
        {"Alien Head",  0x01, "AZUL",        0,   0, 255},

        // 32 number tests
        {"32 (numero)", 0x02, "VERMELHO",  255,   0,   0},
        {"32 (numero)", 0x02, "VERDE",       0, 255,   0},
        {"32 (numero)", 0x02, "AZUL",        0,   0, 255},

        // Both front LEDs
        {"Ambos",       0x03, "AMARELO",   255, 255,   0},
        {"Ambos",       0x03, "CIANO",       0, 255, 255},
        {"Ambos",       0x03, "MAGENTA",   255,   0, 255},
        {"Ambos",       0x03, "BRANCO",    255, 255, 255},

        // Individual colors on different LEDs simultaneously
        {"Alien Head",  0x01, "VERMELHO",  255,   0,   0},
        {"32 (numero)", 0x02, "AZUL",        0,   0, 255},

        // Rainbow on both
        {"Ambos",       0x03, "LARANJA",   255, 128,   0},
        {"Ambos",       0x03, "ROXO",      128,   0, 255},
        {"Ambos",       0x03, "ROSA",      255,  50, 150},

        // Turn off
        {"Todos",       0xFF, "OFF",         0,   0,   0},
    };

    int numSteps = sizeof(steps) / sizeof(steps[0]);

    for (int i = 0; i < numSteps; i++) {
        auto& s = steps[i];

        printf("[%2d/%2d] ", i + 1, numSteps);
        printStep(s.led, s.mask, s.color, s.r, s.g, s.b);

        v6Reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        v6SetColor(s.mask, s.r, s.g, s.b);

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    std::cout << "\n  Demo completa!\n\n";
}

// ---- Interactive Mode ----

void runInteractive() {
    std::cout << "========================================\n"
              << "  MODO INTERATIVO\n"
              << "========================================\n\n"
              << "  LEDs disponiveis:\n";
    for (int i = 0; i < 4; i++)
        printf("    %d = %s (mask=0x%02X)\n", i, LEDS[i].name, LEDS[i].mask);

    std::cout << "\n  Cores disponiveis:\n";
    for (int i = 0; i < 11; i++)
        printf("    %2d = %-10s (R=%3d G=%3d B=%3d)\n", i, COLORS[i].name, COLORS[i].r, COLORS[i].g, COLORS[i].b);

    std::cout << "\n  Digite: LED COR  (ex: '0 0' = Alien Head Vermelho)\n"
              << "  Ou 'r g b' apos o LED para cor personalizada (ex: '0 128 0 255')\n"
              << "  'q' para sair, 'd' para rodar demo novamente\n\n";

    while (true) {
        std::cout << "> ";
        std::string line;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line[0] == 'q' || line[0] == 'Q') break;
        if (line[0] == 'd' || line[0] == 'D') { runDemo(); continue; }

        int ledIdx = -1, colorIdx = -1;
        int cr = -1, cg = -1, cb = -1;

        int parsed = sscanf(line.c_str(), "%d %d %d %d", &ledIdx, &cr, &cg, &cb);

        if (parsed < 2 || ledIdx < 0 || ledIdx > 3) {
            std::cout << "  Formato: LED COR  ou  LED R G B\n";
            continue;
        }

        unsigned char r, g, b;
        const char* colorName;

        if (parsed == 2) {
            // LED + color index
            colorIdx = cr;
            if (colorIdx < 0 || colorIdx > 10) {
                std::cout << "  Cor invalida (0-10)\n";
                continue;
            }
            r = COLORS[colorIdx].r;
            g = COLORS[colorIdx].g;
            b = COLORS[colorIdx].b;
            colorName = COLORS[colorIdx].name;
        } else if (parsed == 4) {
            // LED + R G B
            r = (unsigned char)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
            g = (unsigned char)(cg < 0 ? 0 : cg > 255 ? 255 : cg);
            b = (unsigned char)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
            colorName = "CUSTOM";
        } else {
            std::cout << "  Formato: LED COR  ou  LED R G B\n";
            continue;
        }

        printStep(LEDS[ledIdx].name, LEDS[ledIdx].mask, colorName, r, g, b);

        v6Reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        v6SetColor(LEDS[ledIdx].mask, r, g, b);
    }

    // Turn off on exit
    v6Reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    v6SetColor(0xFF, 0, 0, 0);
    std::cout << "LEDs desligados.\n";
}

int main(int argc, char* argv[]) {
    std::cout << "=== Alienware AW3225QF LED Demo ===\n\n";

    // Kill AWCC if --kill-awcc passed
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--kill-awcc") {
            std::cout << "Parando AWCC...\n";
            system("taskkill /F /IM AlienFXSubAgent.exe >nul 2>&1");
            system("taskkill /F /IM alienfx-gui.exe >nul 2>&1");
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    g_hid = openDevice();
    if (g_hid == INVALID_HANDLE_VALUE) {
        std::cerr << "ERRO: Nao foi possivel abrir o dispositivo HID!\n"
                  << "Use --kill-awcc para parar o AWCC primeiro.\n";
        return 1;
    }
    std::cout << "Dispositivo HID aberto com sucesso!\n\n";

    // Send initial reset
    v6Reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Run demo sequence first
    runDemo();

    // Then interactive mode
    runInteractive();

    CloseHandle(g_hid);
    return 0;
}
