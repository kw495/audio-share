// ServerDialog.cpp : implementation file
//

#include "pch.h"
#include "CServerTabPanel.h"
#include "resource.h"
#include "audio_manager.hpp"
#include "network_manager.hpp"
#include "AudioShareServer.h"
#include "AppMsg.h"

// CServerTabPanel dialog

IMPLEMENT_DYNAMIC(CServerTabPanel, CTabPanel)

CServerTabPanel::CServerTabPanel(CWnd* pParent)
    : CTabPanel(IDD_SERVER, IDS_SERVER, pParent)
    , m_nDeviceCount(0)
    , m_bStarted(false)
{
}

CServerTabPanel::~CServerTabPanel()
{
}

void CServerTabPanel::SwitchServer()
{
    m_buttonServer.PostMessageW(BM_CLICK);
}

bool CServerTabPanel::IsRunning()
{
    return m_bStarted;
}

void CServerTabPanel::DoDataExchange(CDataExchange* pDX)
{
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_COMBO_HOST, m_comboBoxHost);
    DDX_Control(pDX, IDC_EDIT_PORT, m_editPort);
    DDX_Control(pDX, IDC_COMBO_AUDIO_ENDPOINT, m_comboBoxAudioEndpoint);
    DDX_Control(pDX, IDC_BUTTON_SERVER, m_buttonServer);
    DDX_Control(pDX, IDC_BUTTON_RESET, m_buttonReset);
    DDX_Control(pDX, IDC_COMBO_ENCODING, m_comboEncoding);
    DDX_Control(pDX, IDC_BUTTON_SOUND_PANEL, m_buttonSoundPanel);
    DDX_Control(pDX, IDC_EDIT_TEST_TONE_TEXT, m_editTestToneText);
    DDX_Control(pDX, IDC_STATIC_TEST_TONE_TEXT, m_staticTestToneText);
}


BEGIN_MESSAGE_MAP(CServerTabPanel, CTabPanel)
    ON_BN_CLICKED(IDC_BUTTON_SERVER, &CServerTabPanel::OnBnClickedStartServer)
    ON_BN_CLICKED(IDC_BUTTON_RESET, &CServerTabPanel::OnBnClickedButtonReset)
    ON_BN_CLICKED(IDC_BUTTON_SOUND_PANEL, &CServerTabPanel::OnBnClickedButtonSoundPanel)
    ON_CBN_SELCHANGE(IDC_COMBO_AUDIO_ENDPOINT, &CServerTabPanel::OnCbnSelchangeComboAudioEndpoint)
    ON_MESSAGE(WM_APP_CAPTURE_ERROR, &CServerTabPanel::OnCaptureError)
END_MESSAGE_MAP()


// CServerTabPanel message handlers

BOOL CServerTabPanel::OnInitDialog()
{
    CDialogEx::OnInitDialog();

    // TODO:  Add extra initialization here
    m_editPort.SetLimitText(5);
    m_editTestToneText.SetLimitText(64);

    // create network_manager (the endpoint list below needs audio_manager)
    m_audio_manager = std::make_shared<audio_manager>();
    m_network_manager = std::make_shared<network_manager>(m_audio_manager);

    // A capture failure (e.g. no sound device) must not end the app: report it
    // on the UI thread and stop the server there.
    HWND hWnd = this->GetSafeHwnd();
    m_audio_manager->on_capture_error = [hWnd](const std::string& what) {
        auto pMessage = new CString(mbs_to_wchars(what).c_str());
        if (!::PostMessageW(hWnd, WM_APP_CAPTURE_ERROR, 0, (LPARAM)pMessage)) {
            delete pMessage;
        }
    };

    // init controls data
    this->OnBnClickedButtonReset();

    auto nWhenAppStart = theApp.GetProfileIntW(L"App Settings", L"WhenAppStart", 0);
    if (nWhenAppStart == 1 || nWhenAppStart == 2 && theApp.GetProfileIntW(L"App", L"Running", false)) {
        m_buttonServer.PostMessageW(BM_CLICK);
    }

    return TRUE;  // return TRUE unless you set the focus to a control
    // EXCEPTION: OCX Property Pages should return FALSE
}


void CServerTabPanel::OnBnClickedButtonReset()
{
    // host list
    {
        m_comboBoxHost.ResetContent();
        auto address_list = network_manager::get_address_list();
        for (auto address : address_list) {
            auto nIndex = m_comboBoxHost.AddString(mbs_to_wchars(address).c_str());
        }
        auto default_address = network_manager::get_default_address();
        auto configHost = theApp.GetProfileStringW(L"Network", L"host", default_address.empty() ? nullptr : mbs_to_wchars(default_address).c_str());
        m_comboBoxHost.SetWindowTextW(configHost);
    }

    // port
    {
        m_editPort.SetWindowTextW(theApp.GetProfileStringW(L"Network", L"port", L"65530"));
    }

    CString defaultString;
    (void)defaultString.LoadStringW(IDS_DEFAULT);

    // audio endpoint list
    {
        for (int nIndex = m_comboBoxAudioEndpoint.GetCount() - 1; nIndex >= 0; --nIndex) {
            free(m_comboBoxAudioEndpoint.GetItemDataPtr(nIndex));
        }
        m_comboBoxAudioEndpoint.ResetContent();

        auto nDefaultIndex = m_comboBoxAudioEndpoint.AddString(defaultString);
        m_comboBoxAudioEndpoint.SetItemDataPtr(nDefaultIndex, _wcsdup(L"default"));
        
        audio_manager::endpoint_list_t endpoint_list = m_audio_manager->get_endpoint_list();
        for (auto&& [id, name] : endpoint_list) {
            int nIndex = m_comboBoxAudioEndpoint.AddString(mbs_to_wchars(name).c_str());
            m_comboBoxAudioEndpoint.SetItemDataPtr(nIndex, _wcsdup(mbs_to_wchars(id).c_str()));
        }
        m_nDeviceCount = (int)endpoint_list.size();

        // always available, even without any sound device
        CString testToneString;
        (void)testToneString.LoadStringW(IDS_TEST_TONE);
        int nTestToneIndex = m_comboBoxAudioEndpoint.AddString(testToneString);
        m_comboBoxAudioEndpoint.SetItemDataPtr(nTestToneIndex, _wcsdup(mbs_to_wchars(audio_manager::test_tone_endpoint).c_str()));

        auto configEndpoint = theApp.GetProfileStringW(L"Capture", L"endpoint", L"default");
        for (int nIndex = 0; nIndex < m_comboBoxAudioEndpoint.GetCount(); ++nIndex) {
            if (configEndpoint == (LPCWSTR)m_comboBoxAudioEndpoint.GetItemDataPtr(nIndex)) {
                m_comboBoxAudioEndpoint.SetCurSel(nIndex);
                break;
            }
        }
        if (m_comboBoxAudioEndpoint.GetCurSel() == CB_ERR) {
            // selected endpoint is not in list, no selected
            theApp.WriteProfileStringW(L"Capture", L"endpoint", L"default");
            m_comboBoxAudioEndpoint.SetCurSel(nDefaultIndex);
        }

        m_editTestToneText.SetWindowTextW(theApp.GetProfileStringW(L"Capture", L"testToneText", L"TEST"));
        OnCbnSelchangeComboAudioEndpoint();
    }

    // encoding list
    {
        m_comboEncoding.ResetContent();
        using encoding_t = audio_manager::encoding_t;
        std::vector<std::pair<encoding_t, UINT>> array = {
            { encoding_t::encoding_default, IDS_DEFAULT },
            { encoding_t::encoding_f32, IDS_ENCODING_F32 },
            { encoding_t::encoding_s8, IDS_ENCODING_S8 },
            { encoding_t::encoding_s16, IDS_ENCODING_S16 },
            { encoding_t::encoding_s24, IDS_ENCODING_S24 },
            { encoding_t::encoding_s32, IDS_ENCODING_S32 },
            { encoding_t::encoding_opus, IDS_ENCODING_OPUS },
        };
        for (auto&& [encoding, nID] : array) {
            CString name;
            (void)name.LoadStringW(nID);
            auto nIndex = m_comboEncoding.AddString(name);
            m_comboEncoding.SetItemData(nIndex, (int)encoding);
        }
        
        // select
        auto configEncoding = (encoding_t)theApp.GetProfileIntW(L"Capture", L"encoding", (int)encoding_t::encoding_default);
        for (int nIndex = 0; nIndex < m_comboEncoding.GetCount(); ++nIndex) {
            if (configEncoding == (encoding_t)m_comboEncoding.GetItemData(nIndex)) {
                m_comboEncoding.SetCurSel(nIndex);
                break;
            }
        }
        if (m_comboEncoding.GetCurSel() == CB_ERR) {
            theApp.WriteProfileInt(L"Capture", L"encoding", (int)encoding_t::encoding_default);
            m_comboEncoding.SetCurSel(0);
        }
    }
}

void CServerTabPanel::OnBnClickedStartServer()
{
    if (!m_network_manager) {
        AfxMessageBox(L"network_manager is nullptr", MB_OK | MB_ICONSTOP);
        EndDialog(-1);
        return;
    }

    if (!IsRunning()) {
        if (m_comboBoxAudioEndpoint.GetCurSel() == CB_ERR) {
            AfxMessageBox(IDS_NO_SELECTED_ENDPOINT, MB_OK | MB_ICONSTOP);
            return;
        }

        // the test tone needs no sound device; everything else does
        if (!IsTestToneSelected() && m_nDeviceCount == 0) {
            AfxMessageBox(IDS_NO_ENDPOINT, MB_OK | MB_ICONSTOP);
            return;
        }

        // start
        EnableInputControls(false);
        m_buttonServer.EnableWindow(false);

        CString host_str, port_str;
        m_comboBoxHost.GetWindowTextW(host_str);
        m_editPort.GetWindowTextW(port_str);
        std::string host = wchars_to_mbs(host_str.GetString());
        std::uint16_t port = std::stoi(wchars_to_mbs(port_str.GetString()));
        audio_manager::capture_config config;
        config.endpoint_id = wchars_to_mbs((LPCWSTR)m_comboBoxAudioEndpoint.GetItemDataPtr(m_comboBoxAudioEndpoint.GetCurSel()));
        config.encoding = (audio_manager::encoding_t)m_comboEncoding.GetItemData(m_comboEncoding.GetCurSel());
        CString testToneText;
        m_editTestToneText.GetWindowTextW(testToneText);
        config.test_tone.text = wchars_to_utf8(testToneText.GetString());
        try {
            m_network_manager->start_server(host, port, config);
        }
        catch (std::exception& e) {
            AfxMessageBox(CString(e.what()), MB_OK | MB_ICONSTOP);
            EnableInputControls(true);
            m_buttonServer.EnableWindow(true);
            CString s;
            (void)s.LoadStringW(IDS_START_SERVER);
            m_buttonServer.SetWindowTextW(s);
            m_buttonServer.SetFocus();
            return;
        }

        m_buttonServer.EnableWindow(true);
        CString s;
        (void)s.LoadStringW(IDS_STOP_SERVER);
        m_buttonServer.SetWindowTextW(s);
        m_buttonServer.SetFocus();
        theApp.WriteProfileStringW(L"Network", L"host", host_str);
        theApp.WriteProfileStringW(L"Network", L"port", port_str);
        theApp.WriteProfileStringW(L"Capture", L"endpoint", mbs_to_wchars(config.endpoint_id).c_str());
        theApp.WriteProfileInt(L"Capture", L"encoding", (int)config.encoding);
        theApp.WriteProfileStringW(L"Capture", L"testToneText", testToneText);
        theApp.WriteProfileInt(L"App", L"Running", true);
        m_bStarted = true;
    }
    else {
        StopServer();
    }
}

void CServerTabPanel::StopServer()
{
    m_buttonServer.EnableWindow(false);
    m_network_manager->stop_server();

    EnableInputControls(true);
    m_buttonServer.EnableWindow(true);
    CString s;
    (void)s.LoadStringW(IDS_START_SERVER);
    m_buttonServer.SetWindowTextW(s);
    m_buttonServer.SetFocus();
    theApp.WriteProfileInt(L"App", L"Running", false);
    m_bStarted = false;
}

LRESULT CServerTabPanel::OnCaptureError(WPARAM wParam, LPARAM lParam)
{
    std::unique_ptr<CString> pDetail((CString*)lParam);
    if (IsRunning()) {
        StopServer();
    }
    CString message;
    (void)message.LoadStringW(IDS_CAPTURE_FAILED);
    message += L"\n\n";
    message += *pDetail;
    AfxMessageBox(message, MB_OK | MB_ICONSTOP);
    return 0;
}

bool CServerTabPanel::IsTestToneSelected()
{
    int nIndex = m_comboBoxAudioEndpoint.GetCurSel();
    if (nIndex == CB_ERR) {
        return false;
    }
    return CString((LPCWSTR)m_comboBoxAudioEndpoint.GetItemDataPtr(nIndex)) == mbs_to_wchars(audio_manager::test_tone_endpoint).c_str();
}

void CServerTabPanel::OnCbnSelchangeComboAudioEndpoint()
{
    const bool bTestTone = IsTestToneSelected();
    m_editTestToneText.EnableWindow(bTestTone && !IsRunning());
    m_staticTestToneText.EnableWindow(bTestTone);
}

void CServerTabPanel::EnableInputControls(bool bEnable)
{
    m_comboBoxHost.EnableWindow(bEnable);
    m_editPort.EnableWindow(bEnable);
    m_comboBoxAudioEndpoint.EnableWindow(bEnable);
    m_buttonReset.EnableWindow(bEnable);
    m_comboEncoding.EnableWindow(bEnable);
    m_buttonSoundPanel.EnableWindow(bEnable);
    m_editTestToneText.EnableWindow(bEnable && IsTestToneSelected());
}


void CServerTabPanel::OnBnClickedButtonSoundPanel()
{
    STARTUPINFO si{};
    PROCESS_INFORMATION pi{};

    LPWSTR lpCommandLine = _wcsdup(L"control.exe /name Microsoft.Sound");
    auto hr = CreateProcessW(nullptr, lpCommandLine, nullptr, nullptr, false, 0, nullptr, nullptr, &si, &pi);
    
    WaitForSingleObject(pi.hProcess, INFINITE);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    free(lpCommandLine);
}
