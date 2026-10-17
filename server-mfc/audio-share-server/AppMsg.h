#pragma once

#define WM_APP_NOTIFYICON (WM_APP + 1)
// posted by the capture thread when capturing fails; lParam is a heap-allocated CString*
#define WM_APP_CAPTURE_ERROR (WM_APP + 2)

#define TIMER_ID_CHECK_UPDATE 1