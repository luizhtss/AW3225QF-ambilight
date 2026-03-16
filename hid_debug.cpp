// AlienFX HID Debug Tool v2
// Non-blocking reads, tests multiple command formats.

#include <windows.h>
extern "C" {
#include <initguid.h>
#include <hidclass.h>
#include <hidsdi.h>
}
#include <SetupAPI.h>
#include <iostream>
#include <iomanip>
#include <cstring>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

using namespace std;

void hexDump(const char* label, const unsigned char* buf, int len) {
    cout << label << " (" << len << " bytes):\n  ";
    for (int i = 0; i < len; i++) {
        cout << hex << setw(2) << setfill('0') << (int)buf[i] << " ";
        if ((i + 1) % 16 == 0 && i + 1 < len) cout << "\n  ";
    }
    cout << dec << "\n";
}

bool trySend(HANDLE h, unsigned char* buf, int len, const char* label) {
    DWORD written = 0;
    hexDump(label, buf, len);

    BOOL ok = WriteFile(h, buf, len, &written, NULL);
    cout << "  WriteFile: " << (ok ? "OK" : "FAIL")
         << " written=" << written << " err=" << GetLastError() << "\n";
    return ok != 0;
}

int main() {
    cout << "=== AlienFX HID Debug Tool v2 ===\n\n";

    HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_HID, NULL, NULL,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        cerr << "SetupDiGetClassDevs failed\n";
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA did = { sizeof(SP_DEVICE_INTERFACE_DATA) };
    int devIdx = 0;
    HANDLE alienDevice = INVALID_HANDLE_VALUE;
    int alienLength = 0;

    cout << "--- Scanning HID devices ---\n\n";

    while (SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_HID, devIdx++, &did)) {
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
                    if (attr.VendorID == 0x187c || attr.VendorID == 0x0424) {
                        PHIDP_PREPARSED_DATA prep;
                        HIDP_CAPS caps;
                        if (HidD_GetPreparsedData(h, &prep)) {
                            HidP_GetCaps(prep, &caps);
                            HidD_FreePreparsedData(prep);

                            wchar_t wbuf[256];
                            string mfg, prod;
                            if (HidD_GetManufacturerString(h, wbuf, 255))
                                for (int i = 0; i < (int)wcslen(wbuf); i++) mfg += (char)wbuf[i];
                            if (HidD_GetProductString(h, wbuf, 255))
                                for (int i = 0; i < (int)wcslen(wbuf); i++) prod += (char)wbuf[i];

                            cout << "FOUND: " << mfg << " " << prod << "\n"
                                 << "  VID=0x" << hex << attr.VendorID
                                 << " PID=0x" << attr.ProductID << dec
                                 << " OutLen=" << caps.OutputReportByteLength
                                 << " InLen=" << caps.InputReportByteLength
                                 << " FeatLen=" << caps.FeatureReportByteLength
                                 << " Usage=0x" << hex << caps.Usage
                                 << " Page=0x" << caps.UsagePage << dec << "\n\n";

                            if (caps.OutputReportByteLength > 0 && alienDevice == INVALID_HANDLE_VALUE) {
                                alienDevice = h;
                                alienLength = caps.OutputReportByteLength;
                                h = INVALID_HANDLE_VALUE;
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

    if (alienDevice == INVALID_HANDLE_VALUE) {
        cerr << "No Alienware HID device found! Kill AWCC first.\n";
        return 1;
    }

    // Short timeouts so reads don't block
    COMMTIMEOUTS timeouts = {200, 0, 200, 0, 200};
    SetCommTimeouts(alienDevice, &timeouts);

    int len = alienLength;
    unsigned char buf[256];

    // ==== TEST 1: V6 Reset ====
    cout << "=== TEST 1: V6 Reset ===\n";
    memset(buf, 0xff, len);
    buf[0] = 0; buf[1] = 0x95; buf[2] = 0; buf[3] = 0; buf[4] = 0;
    trySend(alienDevice, buf, len, "V6 Reset");
    Sleep(300);

    // ==== TEST 2: V6 Color mask=1 RED ====
    cout << "\n=== TEST 2: V6 Color (mask=0x01, RED) ===\n";
    {
        unsigned char cmd[] = { 0x51, 0x87, 0xd0, 0x04, 0x01, 255, 0, 0, 0x64, 0 };
        cmd[9] = (unsigned char)(255 ^ 0 ^ 0 ^ 1 ^ 8);
        memset(buf, 0xff, len);
        buf[0] = 0; buf[1] = 0x92; buf[2] = 0x37;
        buf[3] = sizeof(cmd); buf[4] = 0;
        memcpy(buf + 5, cmd, sizeof(cmd));
        trySend(alienDevice, buf, len, "V6 Color RED mask=1");
    }
    cout << "  >> Wait 3s - check if any LED turned RED\n";
    Sleep(3000);

    // ==== TEST 3: V6 Color mask=2 GREEN ====
    cout << "\n=== TEST 3: V6 Color (mask=0x02, GREEN) ===\n";
    {
        unsigned char cmd[] = { 0x51, 0x87, 0xd0, 0x04, 0x02, 0, 255, 0, 0x64, 0 };
        cmd[9] = (unsigned char)(0 ^ 255 ^ 0 ^ 2 ^ 8);
        memset(buf, 0xff, len);
        buf[0] = 0; buf[1] = 0x92; buf[2] = 0x37;
        buf[3] = sizeof(cmd); buf[4] = 0;
        memcpy(buf + 5, cmd, sizeof(cmd));
        trySend(alienDevice, buf, len, "V6 Color GREEN mask=2");
    }
    cout << "  >> Wait 3s - check if any LED turned GREEN\n";
    Sleep(3000);

    // ==== TEST 4: V6 Color mask=4 BLUE ====
    cout << "\n=== TEST 4: V6 Color (mask=0x04, BLUE) ===\n";
    {
        unsigned char cmd[] = { 0x51, 0x87, 0xd0, 0x04, 0x04, 0, 0, 255, 0x64, 0 };
        cmd[9] = (unsigned char)(0 ^ 0 ^ 255 ^ 4 ^ 8);
        memset(buf, 0xff, len);
        buf[0] = 0; buf[1] = 0x92; buf[2] = 0x37;
        buf[3] = sizeof(cmd); buf[4] = 0;
        memcpy(buf + 5, cmd, sizeof(cmd));
        trySend(alienDevice, buf, len, "V6 Color BLUE mask=4");
    }
    cout << "  >> Wait 3s - check if any LED turned BLUE\n";
    Sleep(3000);

    // ==== TEST 5: Try mask=0xFF (all LEDs) WHITE ====
    cout << "\n=== TEST 5: V6 Color (mask=0xFF, WHITE) ===\n";
    {
        unsigned char cmd[] = { 0x51, 0x87, 0xd0, 0x04, 0xFF, 255, 255, 255, 0x64, 0 };
        cmd[9] = (unsigned char)(255 ^ 255 ^ 255 ^ 0xFF ^ 8);
        memset(buf, 0xff, len);
        buf[0] = 0; buf[1] = 0x92; buf[2] = 0x37;
        buf[3] = sizeof(cmd); buf[4] = 0;
        memcpy(buf + 5, cmd, sizeof(cmd));
        trySend(alienDevice, buf, len, "V6 Color WHITE mask=FF");
    }
    cout << "  >> Wait 3s - check if any LED turned WHITE\n";
    Sleep(3000);

    // ==== Turn off ====
    cout << "\n=== Resetting LEDs ===\n";
    memset(buf, 0xff, len);
    buf[0] = 0; buf[1] = 0x95; buf[2] = 0; buf[3] = 0; buf[4] = 0;
    trySend(alienDevice, buf, len, "V6 Reset (cleanup)");
    Sleep(300);
    {
        unsigned char cmd[] = { 0x51, 0x87, 0xd0, 0x04, 0xFF, 0, 0, 0, 0x64, 0 };
        cmd[9] = (unsigned char)(0 ^ 0 ^ 0 ^ 0xFF ^ 8);
        memset(buf, 0xff, len);
        buf[0] = 0; buf[1] = 0x92; buf[2] = 0x37;
        buf[3] = sizeof(cmd); buf[4] = 0;
        memcpy(buf + 5, cmd, sizeof(cmd));
        trySend(alienDevice, buf, len, "V6 All LEDs OFF");
    }

    cout << "\n=== All tests complete ===\n";

    CloseHandle(alienDevice);
    return 0;
}
