#include <windows.h>
#include <winscard.h>
#include <shellapi.h>
#include <commctrl.h>
#include <process.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <regex>
#include "resource.h"

// Configuration settings
struct Config {
    std::vector<std::string> insertAPDUs;
    std::wstring insertCommand;
    std::wstring removeCommand;
};

// Global variables
HWND g_hwnd = NULL;
NOTIFYICONDATA g_nid = {};
HANDLE g_cardMonitorThread = NULL;
bool g_running = true;
Config g_config;
SCARDCONTEXT g_hContext = 0; // Global SCARDCONTEXT

// ID values for tray icon menu
#define IDM_EXIT 1001
#define IDM_FIRST_READER 2000

// Window messages
#define WM_TRAYICON (WM_USER + 1)
#define WM_CARD_INSERTED (WM_USER + 2)
#define WM_CARD_REMOVED (WM_USER + 3)
#define WM_READER_CHANGE (WM_USER + 4)

// Reader state structure
struct ReaderInfo {
    std::wstring name;
    bool hasCard;
};

std::vector<ReaderInfo> g_readers;

// Forward declarations
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
unsigned __stdcall CardMonitorThreadProc(void* pArg);
void LoadConfiguration();
void ExecuteCommand(const std::wstring& command);
void UpdateTrayMenu();
void RefreshReaderList();

// Function to convert hex string to byte array
std::vector<BYTE> HexStringToBytes(const std::string& hex) {
    std::vector<BYTE> bytes;
    
    for (size_t i = 0; i < hex.length(); i += 2) {
        if (i + 1 < hex.length()) {
            std::string byteString = hex.substr(i, 2);
            BYTE byte = (BYTE)strtol(byteString.c_str(), NULL, 16);
            bytes.push_back(byte);
        }
    }
    
    return bytes;
}

// Function to convert byte array to hex string
std::string BytesToHexString(const BYTE* bytes, DWORD length) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    
    for (DWORD i = 0; i < length; i++) {
        ss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    
    return ss.str();
}

// Send APDUs to card and return responses
std::vector<std::string> SendAPDUs(int readerIndex) {
    std::vector<std::string> responses;
    
    // Check if reader index is valid
    if (readerIndex < 0 || readerIndex >= g_readers.size()) {
        return responses;
    }
    
    // If no APDUs to send, return empty responses
    if (g_config.insertAPDUs.empty()) {
        return responses;
    }
    
    // Get reader name as ANSI string for SCardConnect
    std::string readerNameA;
    std::wstring readerNameW = g_readers[readerIndex].name;
    int len = WideCharToMultiByte(CP_ACP, 0, readerNameW.c_str(), -1, NULL, 0, NULL, NULL);
    readerNameA.resize(len);
    WideCharToMultiByte(CP_ACP, 0, readerNameW.c_str(), -1, &readerNameA[0], len, NULL, NULL);
    
    // Connect to card
    SCARDHANDLE hCard;
    DWORD dwActiveProtocol;
    LONG status = SCardConnectA(g_hContext, readerNameA.c_str(), SCARD_SHARE_EXCLUSIVE,
                          SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
                          &hCard, &dwActiveProtocol);
    
    if (status != SCARD_S_SUCCESS) {
        return responses;
    }
    
    // Determine protocol string based on active protocol
    LPCSCARD_IO_REQUEST pioSendPci;
    switch (dwActiveProtocol) {
        case SCARD_PROTOCOL_T0:
            pioSendPci = SCARD_PCI_T0;
            break;
        case SCARD_PROTOCOL_T1:
            pioSendPci = SCARD_PCI_T1;
            break;
        default:
            pioSendPci = SCARD_PCI_RAW;
            break;
    }
    
    // Send each APDU and collect responses
    for (const auto& apduHex : g_config.insertAPDUs) {
        std::vector<BYTE> apduBytes = HexStringToBytes(apduHex);
        
        if (apduBytes.empty()) {
            responses.push_back("");
            continue;
        }
        
        BYTE recvBuffer[256];
        DWORD recvLength = sizeof(recvBuffer);
        
        status = SCardTransmit(hCard, 
                               pioSendPci,
                               apduBytes.data(), 
                               (DWORD)apduBytes.size(), 
                               NULL, 
                               recvBuffer, 
                               &recvLength);
        
        if (status == SCARD_S_SUCCESS && recvLength > 0) {
            responses.push_back(BytesToHexString(recvBuffer, recvLength));
        } else {
            responses.push_back("");
        }
    }
    
    // Disconnect from card
    SCardDisconnect(hCard, SCARD_LEAVE_CARD);
    
    return responses;
}

// Process command with response placeholders
std::wstring ProcessCommand(const std::wstring& command, const std::vector<std::string>& responses) {
    std::wstring result = command;
    
    // Look for patterns like {1}, {2}, etc. and replace with corresponding response
    std::wregex pattern(L"\\{([0-9]+)\\}");
    std::wstring processed;
    std::wstring::const_iterator start = result.begin();
    std::wstring::const_iterator end = result.end();
    std::wsmatch matches;
    
    // Search and replace all occurrences
    while (std::regex_search(start, end, matches, pattern)) {
        // Append everything up to the match
        processed.append(start, matches[0].first);
        
        // Get index from {n} pattern
        int index = std::stoi(matches[1].str());
        
        // Check if index is valid (1-based index in the pattern, but 0-based in the array)
        if (index > 0 && index <= responses.size()) {
            // Convert ANSI response to wide string
            std::string response = responses[index-1];
            int len = MultiByteToWideChar(CP_ACP, 0, response.c_str(), -1, NULL, 0);
            std::wstring wideResponse(len, 0);
            MultiByteToWideChar(CP_ACP, 0, response.c_str(), -1, &wideResponse[0], len);
            wideResponse.resize(len-1);  // Remove null terminator
            
            processed += wideResponse;
        } else {
            // Keep original placeholder if index is invalid
            processed += matches[0].str();
        }
        
        // Move to the end of the current match
        start = matches[0].second;
    }
    
    // Append any remaining part of the string
    processed.append(start, end);
    return processed;
}

// Entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Initialize COM for shell API
    CoInitialize(NULL);
    
    // Establish global context
    LONG status = SCardEstablishContext(SCARD_SCOPE_USER, NULL, NULL, &g_hContext);
    if (status != SCARD_S_SUCCESS) {
        MessageBox(NULL, L"Failed to establish smart card context!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        return 1;
    }
    
    // Register window class
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"CardActionWindowClass";
    
    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, L"Window Registration Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }
    
    // Create hidden window
    g_hwnd = CreateWindowEx(
        0,
        L"CardActionWindowClass",
        L"Card Action Monitor",
        0,
        CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_hwnd) {
        MessageBox(NULL, L"Window Creation Failed!", L"Error", MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }
    
    // Initialize notification icon
    g_nid.cbSize = sizeof(NOTIFYICONDATA);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CARDICON));
    wcscpy_s(g_nid.szTip, L"Card Action Monitor");
    
    Shell_NotifyIcon(NIM_ADD, &g_nid);
    
    // Load configuration
    LoadConfiguration();
    
    // Start card monitor thread
    g_cardMonitorThread = (HANDLE)_beginthreadex(NULL, 0, CardMonitorThreadProc, NULL, 0, NULL);
    
    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Cleanup
    g_running = false;
    if (g_cardMonitorThread) {
        WaitForSingleObject(g_cardMonitorThread, INFINITE);
        CloseHandle(g_cardMonitorThread);
    }
    
    // Release global context
    SCardReleaseContext(g_hContext);
    
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
    CoUninitialize();
    
    return (int)msg.wParam;
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                
                HMENU hMenu = CreatePopupMenu();
                
                // Add reader items
                int menuIndex = 0;
                for (const auto& reader : g_readers) {
                    std::wstring menuText = reader.name;
                    if (reader.hasCard) {
                        menuText += L" [Card present]";
                    }
                    InsertMenu(hMenu, menuIndex++, MF_BYPOSITION | MF_STRING | MF_DISABLED, 
                               IDM_FIRST_READER + menuIndex, menuText.c_str());
                }
                
                // Add separator and exit option
                if (!g_readers.empty()) {
                    InsertMenu(hMenu, menuIndex++, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
                }
                InsertMenu(hMenu, menuIndex, MF_BYPOSITION | MF_STRING, IDM_EXIT, L"Exit");
                
                // Display menu
                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, 
                              pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            break;
            
        case WM_COMMAND:
            if (LOWORD(wParam) == IDM_EXIT) {
                DestroyWindow(hwnd);
            }
            break;
            
        case WM_CARD_INSERTED:
            {
                int readerIndex = (int)wParam;
                if (readerIndex >= 0 && readerIndex < g_readers.size()) {
                    g_readers[readerIndex].hasCard = true;
                    
                    // Send APDUs and process command with responses
                    std::vector<std::string> responses = SendAPDUs(readerIndex);
                    std::wstring command = ProcessCommand(g_config.insertCommand, responses);
                    
                    // Execute the processed command
                    ExecuteCommand(command);
                    UpdateTrayMenu();
                }
            }
            break;
            
        case WM_CARD_REMOVED:
            {
                int readerIndex = (int)wParam;
                // Execute command regardless of reader list size
                ExecuteCommand(g_config.removeCommand);
                
                // Update reader state if the reader still exists
                if (readerIndex >= 0 && readerIndex < g_readers.size()) {
                    g_readers[readerIndex].hasCard = false;
                }
                UpdateTrayMenu();
            }
            break;
            
        case WM_READER_CHANGE:
            RefreshReaderList();
            UpdateTrayMenu();
            break;
            
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
            
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    
    return 0;
}

// Card monitor thread
unsigned __stdcall CardMonitorThreadProc(void* pArg) {
    // No need to establish context here anymore
    
    // Get initial list of readers
    RefreshReaderList();
    
    // Main monitoring loop
    while (g_running) {
        // Get current list of readers
        LPSTR mszReaders = NULL;
        DWORD cchReaders = SCARD_AUTOALLOCATE;
        
        LONG status = SCardListReadersA(g_hContext, NULL, (LPSTR)&mszReaders, &cchReaders);
        if (status != SCARD_S_SUCCESS) {
            if (status == SCARD_E_NO_READERS_AVAILABLE) {
                // Wait for a reader to be connected
                SCARD_READERSTATE rgReaderStates[1];
                // Convert ANSI string to wide string for SCARD_READERSTATE
                const char* pnpNotification = "\\\\?PnP?\\Notification";
                int len = MultiByteToWideChar(CP_ACP, 0, pnpNotification, -1, NULL, 0);
                wchar_t* widePnpNotification = new wchar_t[len];
                MultiByteToWideChar(CP_ACP, 0, pnpNotification, -1, widePnpNotification, len);
                
                rgReaderStates[0].szReader = (LPCWSTR)widePnpNotification;
                rgReaderStates[0].dwCurrentState = SCARD_STATE_UNAWARE;
                
                status = SCardGetStatusChange(g_hContext, INFINITE, rgReaderStates, 1);
                delete[] widePnpNotification;
                
                if (status == SCARD_S_SUCCESS) {
                    PostMessage(g_hwnd, WM_READER_CHANGE, 0, 0);
                }
                continue;
            }
            Sleep(1000);  // Wait and retry
            continue;
        }
        
        // Count readers
        size_t readerCount = 0;
        LPSTR pReader = mszReaders;
        while (*pReader != '\0') {
            readerCount++;
            pReader += strlen(pReader) + 1;
        }
        
        // Prepare state array
        SCARD_READERSTATE* rgReaderStates = new SCARD_READERSTATE[readerCount + 1];
        ZeroMemory(rgReaderStates, sizeof(SCARD_READERSTATE) * (readerCount + 1));
        
        // Set up reader states
        // Convert ANSI string to wide string for SCARD_READERSTATE
        const char* pnpNotification = "\\\\?PnP?\\Notification";
        int len = MultiByteToWideChar(CP_ACP, 0, pnpNotification, -1, NULL, 0);
        wchar_t* widePnpNotification = new wchar_t[len];
        MultiByteToWideChar(CP_ACP, 0, pnpNotification, -1, widePnpNotification, len);
        
        rgReaderStates[0].szReader = (LPCWSTR)widePnpNotification;
        rgReaderStates[0].dwCurrentState = SCARD_STATE_UNAWARE;
        
        // Allocate array to store wide reader names
        wchar_t** wideReaderNames = new wchar_t*[readerCount];
        
        pReader = mszReaders;
        for (size_t i = 0; i < readerCount; i++) {
            // Convert reader name to wide string
            len = MultiByteToWideChar(CP_ACP, 0, pReader, -1, NULL, 0);
            wideReaderNames[i] = new wchar_t[len];
            MultiByteToWideChar(CP_ACP, 0, pReader, -1, wideReaderNames[i], len);
            
            rgReaderStates[i + 1].szReader = (LPCWSTR)wideReaderNames[i];
            rgReaderStates[i + 1].dwCurrentState = SCARD_STATE_UNAWARE;
            pReader += strlen(pReader) + 1;
        }
        
        // Get initial state
        status = SCardGetStatusChange(g_hContext, 0, rgReaderStates, (DWORD)readerCount + 1);
        if (status != SCARD_S_SUCCESS) {
            // Cleanup allocated memory
            delete[] widePnpNotification;
            for (size_t i = 0; i < readerCount; i++) {
                delete[] wideReaderNames[i];
            }
            delete[] wideReaderNames;
            delete[] rgReaderStates;
            SCardFreeMemory(g_hContext, mszReaders);
            Sleep(1000);
            continue;
        }
        
        // Update current state
        for (size_t i = 0; i < readerCount + 1; i++) {
            rgReaderStates[i].dwCurrentState = rgReaderStates[i].dwEventState;
        }
        
        // Wait for status change
        status = SCardGetStatusChange(g_hContext, 1000, rgReaderStates, (DWORD)readerCount + 1);
        
        // Check for events
        if (status == SCARD_S_SUCCESS) {
            // Check for reader change
            if (rgReaderStates[0].dwEventState & SCARD_STATE_CHANGED) {
                PostMessage(g_hwnd, WM_READER_CHANGE, 0, 0);
            }
            
            // Check for card events
            for (size_t i = 1; i <= readerCount; i++) {
                if (rgReaderStates[i].dwEventState & SCARD_STATE_CHANGED) {
                    
                    if (rgReaderStates[i].dwCurrentState - rgReaderStates[i].dwEventState == SCARD_STATE_INUSE) {
                        // Card inserted
                        PostMessage(g_hwnd, WM_CARD_INSERTED, (WPARAM)(i - 1), 0);
                    }
                    else if ((rgReaderStates[i].dwEventState & SCARD_STATE_EMPTY) || (rgReaderStates[i].dwEventState & SCARD_STATE_UNAVAILABLE)) {
                        // Card removed
                        PostMessage(g_hwnd, WM_CARD_REMOVED, (WPARAM)(i - 1), 0);
                    }
                }
            }
        }
        
        // Cleanup allocated memory
        delete[] widePnpNotification;
        for (size_t i = 0; i < readerCount; i++) {
            delete[] wideReaderNames[i];
        }
        delete[] wideReaderNames;
        delete[] rgReaderStates;
        SCardFreeMemory(g_hContext, mszReaders);
    }
    
    // No need to release context here anymore
    return 0;
}

// Load configuration from INI file
void LoadConfiguration() {
    wchar_t iniPath[MAX_PATH];
    GetModuleFileName(NULL, iniPath, MAX_PATH);
    
    // Replace .exe with .ini
    wchar_t* dot = wcsrchr(iniPath, L'.');
    if (dot) {
        wcscpy_s(dot, 5, L".ini");
    }
    
    wchar_t buffer[1024];
    
    // Clear existing config
    g_config.insertAPDUs.clear();
    
    // Load insert command
    GetPrivateProfileString(L"OnInsert", L"Command", L"", buffer, sizeof(buffer)/sizeof(wchar_t), iniPath);
    g_config.insertCommand = buffer;
    
    // Load insert APDUs
    GetPrivateProfileString(L"OnInsert", L"APDUs", L"", buffer, sizeof(buffer)/sizeof(wchar_t), iniPath);
    if (buffer[0] != L'\0') {
        std::wstring wApduList = buffer;
        
        // Convert wide string to ANSI for easier manipulation
        int len = WideCharToMultiByte(CP_ACP, 0, wApduList.c_str(), -1, NULL, 0, NULL, NULL);
        std::string apduList(len, 0);
        WideCharToMultiByte(CP_ACP, 0, wApduList.c_str(), -1, &apduList[0], len, NULL, NULL);
        
        // Split by comma
        std::string apdu;
        std::stringstream apduStream(apduList);
        while (std::getline(apduStream, apdu, ',')) {
            // Remove whitespace
            apdu.erase(std::remove_if(apdu.begin(), apdu.end(), ::isspace), apdu.end());
            if (!apdu.empty()) {
                g_config.insertAPDUs.push_back(apdu);
            }
        }
    }
    
    // Load remove command
    GetPrivateProfileString(L"OnRemove", L"Command", L"", buffer, sizeof(buffer)/sizeof(wchar_t), iniPath);
    g_config.removeCommand = buffer;
    
    // Set defaults if not configured
    if (g_config.insertCommand.empty()) {
        g_config.insertCommand = L"cmd.exe /c echo Card inserted > %TEMP%\\card_inserted.txt";
    }
    
    if (g_config.removeCommand.empty()) {
        g_config.removeCommand = L"cmd.exe /c echo Card removed > %TEMP%\\card_removed.txt";
    }
}

// Execute a command
void ExecuteCommand(const std::wstring& command) {
    STARTUPINFO si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);
    
    // Create a non-const copy of the command
    wchar_t* cmdLine = _wcsdup(command.c_str());
    
    if (CreateProcess(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    
    free(cmdLine);
}

// Update tray icon
void UpdateTrayMenu() {
    // Update tooltip to show reader count
    std::wstring tooltip = L"Card Action Monitor\n";
    tooltip += std::to_wstring(g_readers.size()) + L" reader(s)";
    
    for (const auto& reader : g_readers) {
        if (reader.hasCard) {
            tooltip += L"\n" + reader.name + L": Card present";
        }
    }
    
    // Update tooltip (truncate if needed)
    wcsncpy_s(g_nid.szTip, tooltip.c_str(), 127);
    g_nid.szTip[127] = L'\0';
    
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

// Refresh the list of readers
void RefreshReaderList() {
    g_readers.clear();
    
    // Get reader list
    LPSTR mszReaders = NULL;
    DWORD cchReaders = SCARD_AUTOALLOCATE;
    
    LONG status = SCardListReadersA(g_hContext, NULL, (LPSTR)&mszReaders, &cchReaders);
    if (status != SCARD_S_SUCCESS) {
        return;
    }
    
    // Process reader list
    LPSTR pReader = mszReaders;
    while (*pReader != '\0') {
        // Convert reader name to wide string
        int len = MultiByteToWideChar(CP_ACP, 0, pReader, -1, NULL, 0);
        std::wstring readerName(len, 0);
        MultiByteToWideChar(CP_ACP, 0, pReader, -1, &readerName[0], len);
        readerName.resize(len - 1);  // Remove null terminator
        
        // Check if card is present
        SCARDHANDLE hCard;
        DWORD dwActiveProtocol;
        bool hasCard = (SCardConnectA(g_hContext, pReader, SCARD_SHARE_SHARED, 
                                     SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
                                     &hCard, &dwActiveProtocol) == SCARD_S_SUCCESS);
        
        if (hasCard) {
            SCardDisconnect(hCard, SCARD_LEAVE_CARD);
        }
        
        // Add to reader list
        ReaderInfo info;
        info.name = readerName;
        info.hasCard = hasCard;
        g_readers.push_back(info);
        
        // Move to next reader
        pReader += strlen(pReader) + 1;
    }
    
    // Cleanup
    SCardFreeMemory(g_hContext, mszReaders);
}
