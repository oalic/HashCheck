/**
 * HashCheck Shell Extension
 * Original work copyright (C) Kai Liu.  All rights reserved.
 * Modified work copyright (C) 2014, 2016 Christopher Gurnee.  All rights reserved.
 * Modified work copyright (C) 2016 Tim Schlueter.  All rights reserved.
 * Modified work copyright (C) 2021-2026 Mounir IDRASSI.  All rights reserved.
 *
 * Please refer to readme.txt for information about this source code.
 * Please refer to license.txt for details about distribution and modification.
 **/

#include "globals.h"
#include "HashCheckCommon.h"
#include "HashCalc.h"
#include "libs/WinHash.h"
#include <commctrl.h>
#include <Strsafe.h>
#include <assert.h>

// Control structures, from HashCalc.h
#define  HASHPROPSCRATCH  HASHCALCSCRATCH
#define PHASHPROPSCRATCH PHASHCALCSCRATCH
#define  HASHPROPCONTEXT  HASHCALCCONTEXT
#define PHASHPROPCONTEXT PHASHCALCCONTEXT
#define  HASHPROPITEM     HASHCALCITEM
#define PHASHPROPITEM    PHASHCALCITEM

#ifdef UNICODE
#define StrCmpLogical StrCmpLogicalW
#else
#define StrCmpLogical StrCmpIA
#endif

typedef struct {
	PHASHPROPITEM pItem;
	UINT iOriginal;
} HASHPROPSORTITEM, *PHASHPROPSORTITEM;


/*============================================================================*\
	Function declarations
\*============================================================================*/

// Worker thread
VOID __fastcall HashPropWorkerMain( PHASHPROPCONTEXT phpctx );
VOID WINAPI HashPropRestart( PHASHPROPCONTEXT phpctx );

// Dialog general
VOID WINAPI HashPropDlgInit( PHASHPROPCONTEXT phpctx );
VOID WINAPI HashPropFitDialog( HWND hWnd );
VOID WINAPI HashPropForceLTR( HWND hWndEdit );

// Dialog status
LRESULT CALLBACK HashPropEditProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
LRESULT CALLBACK HashPropResultsProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
VOID WINAPI HashPropUpdateResults( PHASHPROPCONTEXT phpctx, PHASHPROPITEM pItem );
VOID WINAPI HashPropFinalStatus( PHASHPROPCONTEXT phpctx );

// Results list view
VOID WINAPI HashPropSizeListColumn( HWND hWndResults );
VOID WINAPI HashPropListClear( HWND hWndResults );
VOID WINAPI HashPropListAddFile( PHASHPROPCONTEXT phpctx, PHASHPROPITEM pItem );
VOID WINAPI HashPropCopySelection( HWND hWnd );

// Dialog commands
VOID WINAPI HashPropFindText( PHASHPROPCONTEXT phpctx, BOOL bIncremental );
VOID WINAPI HashPropSaveResults( PHASHPROPCONTEXT phpctx );
VOID WINAPI HashPropDoSaveResults( PHASHPROPCONTEXT phpctx );
INT __cdecl HashPropSortCompare( void *pvContext, const void *pvItemA, const void *pvItemB );
VOID WINAPI HashPropSaveResultsCleanup( PHASHPROPCONTEXT phpctx );
VOID WINAPI HashPropOptions( PHASHPROPCONTEXT phpctx );



/*============================================================================*\
	Entry points / main functions
\*============================================================================*/

UINT CALLBACK HashPropCallback( HWND hWnd, UINT uMsg, LPPROPSHEETPAGE ppsp )
{
	HSIMPLELIST hList = (HSIMPLELIST)(ppsp->lParam);

	switch (uMsg)
	{
		case PSPCB_ADDREF:
			SLAddRef(hList);
			break;

		case PSPCB_RELEASE:
			SLRelease(hList);
			break;

		case PSPCB_CREATE:
		{
			PHASHPROPCONTEXT phpctx = SLSetContextSize(hList, sizeof(HASHPROPCONTEXT));

			if (phpctx)
			{
				ZeroMemory(phpctx, sizeof(HASHPROPCONTEXT));
				phpctx->status = INACTIVE;
				phpctx->hListRaw = hList;
				return(1);
			}
		}
	}

	return(0);
}



/*============================================================================*\
	Worker thread
\*============================================================================*/

VOID __fastcall HashPropWorkerMain( PHASHPROPCONTEXT phpctx )
{
	// Note that ALL message communication to and from the main window MUST
	// be asynchronous, or else there may be a deadlock.

	PHASHPROPITEM pItem;
    WHCTXEX whctx;
    PBYTE pbBuffer = NULL;
	HANDLE hJobSlot = WorkerThreadAcquireJobSlot((PCOMMONCONTEXT)phpctx);
	if (!hJobSlot)
		return;

	// Prep: if not already done, expand directories, establish prefix, etc.
    if (! (phpctx->dwFlags & HPF_HLIST_PREPPED))
    {
        PostMessage(phpctx->hWnd, HM_WORKERTHREAD_TOGGLEPREP, (WPARAM)phpctx, TRUE);
        if (! HashCalcPrepare(phpctx))
            goto cleanup;
        phpctx->dwFlags |= HPF_HLIST_PREPPED;
    }
	PostMessage(phpctx->hWnd, HM_WORKERTHREAD_TOGGLEPREP, (WPARAM)phpctx, FALSE);

	// Which checksum types we want to calculate
    // (this is loaded earlier in HashPropDlgInit())
	DWORD checksumFlags = phpctx->opt.dwChecksums;

	phpctx->dwReadBufferSize = 0;
	phpctx->bOuterMultithreaded = FALSE;

#ifdef _TIMED
    DWORD dwStarted;
    dwStarted = GetTickCount();
#endif

	while (pItem = SLGetDataAndStep(phpctx->hList))
	{
        // Some results might already be present if the user changes which checksum types
        // to calculate and we're going through the list a second+ time for all/some items;
        // only calculate the checksums we don't already have (usually all those requested)
        whctx.dwFlags = checksumFlags & ~pItem->results.dwFlags;

		if (phpctx->dwReadBufferSize == 0)
			phpctx->dwReadBufferSize = GetReadBufferSizeForPath(pItem->szPath);

		if (pbBuffer == NULL)
		{
			pbBuffer = (PBYTE)malloc(phpctx->dwReadBufferSize);
			if (pbBuffer == NULL)
				goto cleanup;
		}
		// Get the hash
		WorkerThreadHashFile(
			(PCOMMONCONTEXT)phpctx,
			pItem->szPath,
			&whctx,
			&pItem->results,
			pbBuffer,
			NULL, 0, NULL, NULL
#ifdef _TIMED
          , &pItem->dwElapsed
#endif
        );

        if (phpctx->status == PAUSED)
            WaitForSingleObject(phpctx->hUnpauseEvent, INFINITE);
		if (phpctx->status == CANCEL_REQUESTED)
			break;

		// Update the UI
		++phpctx->cSentMsgs;
		PostMessage(phpctx->hWnd, HM_WORKERTHREAD_UPDATE, (WPARAM)phpctx, (LPARAM)pItem);
	}
#ifdef _TIMED
    phpctx->dwElapsed = GetTickCount() - dwStarted;
#endif

cleanup:
    free(pbBuffer);
	WorkerThreadReleaseJobSlot(hJobSlot);
}



/*============================================================================*\
	Dialog general
\*============================================================================*/

INT_PTR CALLBACK HashPropDlgProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	PHASHPROPCONTEXT phpctx;

	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			phpctx = (PHASHPROPCONTEXT)SLGetContextData((HSIMPLELIST)(((LPPROPSHEETPAGE)lParam)->lParam));

			// Associate the window with the context and vice-versa
			phpctx->hWnd = hWnd;
			SetWindowLongPtr(hWnd, DWLP_USER, (LONG_PTR)phpctx);

			HashPropDlgInit(phpctx);

			phpctx->pfnWorkerMain = HashPropWorkerMain;
			phpctx->hThread = CreateThreadCRT(NULL, phpctx);

			if (!phpctx->hThread)
				WorkerThreadCleanup((PCOMMONCONTEXT)phpctx);

			return(TRUE);
		}

		case WM_SHOWWINDOW:
		{
			phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);

			if (wParam && !(phpctx->dwFlags & HPF_HAS_RESIZED))
			{
				// We wait for WM_SHOWWINDOW because the reported size may not
				// be accurate during WM_INITDIALOG
				HashPropFitDialog(hWnd);
				phpctx->dwFlags |= HPF_HAS_RESIZED;
			}

			HashPropForceLTR(GetDlgItem(hWnd, IDC_RESULTS));

			break;
		}

		case WM_SIZE:
		{
			phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			if (phpctx)
				HashPropSizeListColumn(GetDlgItem(hWnd, IDC_RESULTS));

			break;
		}

		case WM_ENDSESSION:
        {
            if (wParam == FALSE)  // if TRUE, fall through to WM_DESTROY
                break;
        }
		case WM_DESTROY:
		{
			phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);

			// Undo the search box subclassing
			SetWindowLongPtr(
				GetDlgItem(hWnd, IDC_SEARCHBOX),
				GWLP_WNDPROC,
				(LONG_PTR)phpctx->wpSearchBox
			);

            // Undo the results box subclassing
            SetWindowLongPtr(
                GetDlgItem(hWnd, IDC_RESULTS),
                GWLP_WNDPROC,
                (LONG_PTR)phpctx->wpResultsBox
            );

			// Kill the worker thread; HCF_EXIT_PENDING indicates
            // we don't need to do any further GUI updates
			phpctx->dwFlags |= HCF_EXIT_PENDING;
			WorkerThreadStop((PCOMMONCONTEXT)phpctx);
			WorkerThreadCleanup((PCOMMONCONTEXT)phpctx);

			// Cleanup
            HashPropSaveResultsCleanup(phpctx);
			if (phpctx->hFont) DeleteObject(phpctx->hFont);
			if (phpctx->hList) SLRelease(phpctx->hList);

			break;
		}

		case WM_COMMAND:
		{
			phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);

			switch (LOWORD(wParam))
			{
				case IDC_SEARCHBOX:
				{
					if (HIWORD(wParam) == EN_CHANGE)
					{
						HashPropFindText(phpctx, TRUE);
						return(TRUE);
					}

					break;
				}

				case IDC_FIND_NEXT:
				{
					HashPropFindText(phpctx, FALSE);
					return(TRUE);
				}

				case IDC_PAUSE:
				{
					if (WorkerThreadIsRunNowAvailable((PCOMMONCONTEXT)phpctx))
						WorkerThreadRequestRunNow((PCOMMONCONTEXT)phpctx);
					else
						WorkerThreadTogglePause((PCOMMONCONTEXT)phpctx);
					return(TRUE);
				}

				case IDC_STOP:
				{
                    phpctx->dwFlags |= HPF_INTERRUPTED;
					WorkerThreadStop((PCOMMONCONTEXT)phpctx);
                    HashPropSaveResultsCleanup(phpctx);
					return(TRUE);
				}

				case IDC_SAVE:
				{
					HashPropSaveResults(phpctx);
					return(TRUE);
				}

				case IDC_OPTIONS:
				{
					HashPropOptions(phpctx);
					return(TRUE);
				}
			}

			break;
		}

		case WM_TIMER:
		{
			// Vista: Workaround to fix their buggy progress bar
			KillTimer(hWnd, TIMER_ID_PAUSE);
			phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(hWnd, DWLP_USER);
			if (phpctx->status == PAUSED || phpctx->status == QUEUED)
				SetProgressBarPause((PCOMMONCONTEXT)phpctx, PBST_PAUSED);
			return(TRUE);
		}

		case HM_WORKERTHREAD_DONE:
		{
			phpctx = (PHASHPROPCONTEXT)wParam;
			WorkerThreadCleanup((PCOMMONCONTEXT)phpctx);
            if (phpctx->hFileOut != INVALID_HANDLE_VALUE)
                HashPropDoSaveResults(phpctx);
			HashPropFinalStatus(phpctx);
			return(TRUE);
		}

		case HM_WORKERTHREAD_UPDATE:
		{
			phpctx = (PHASHPROPCONTEXT)wParam;
			++phpctx->cHandledMsgs;
            // If we're restarting the worker thread, no need to update the GUI
            if (phpctx->dwFlags & HCF_RESTARTING)
            {
                // Only restart the worker thread once we're caught up on handled messages
                if (phpctx->cHandledMsgs >= phpctx->cSentMsgs)
                    HashPropRestart(phpctx);
            }
            else
                HashPropUpdateResults(phpctx, (PHASHPROPITEM)lParam);
			return(TRUE);
		}

		case HM_WORKERTHREAD_TOGGLEPREP:
		{
			HashCalcTogglePrep((PHASHPROPCONTEXT)wParam, (BOOL)lParam);
			return(TRUE);
		}

		case HM_WORKERTHREAD_QUEUESTATE:
		{
			phpctx = (PHASHPROPCONTEXT)wParam;
			WorkerThreadSetRunNowAvailable((PCOMMONCONTEXT)phpctx, FALSE);
			if ((BOOL)lParam)
			{
				WorkerThreadSetRunNowAvailable((PCOMMONCONTEXT)phpctx, TRUE);
				SetControlText(hWnd, IDC_PAUSE, IDS_HP_RUN_NOW);
				EnableWindow(GetDlgItem(hWnd, IDC_PAUSE), TRUE);
				SetProgressBarPause((PCOMMONCONTEXT)phpctx, PBST_PAUSED);
			}
			else if (!(phpctx->dwFlags & HCF_EXIT_PENDING) && phpctx->status != CANCEL_REQUESTED)
			{
				EnableWindow(GetDlgItem(hWnd, IDC_PAUSE), TRUE);
				SetControlText(hWnd, IDC_PAUSE, IDS_HP_PAUSE);
				SetProgressBarPause((PCOMMONCONTEXT)phpctx, PBST_NORMAL);
			}
			return(TRUE);
		}
	}

	return(FALSE);
}

LRESULT CALLBACK HashPropEditProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	PHASHPROPCONTEXT phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(
		(HWND)GetWindowLongPtr(hWnd, GWLP_HWNDPARENT),
		DWLP_USER
	);

	if (wParam == VK_RETURN)
	{
		if (uMsg == WM_GETDLGCODE)
		{
			return(DLGC_WANTALLKEYS);
		}
		else if (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST)
		{
			if (uMsg == WM_KEYDOWN)
				HashPropFindText(phpctx, FALSE);

			return(0);
		}
	}

	return(CallWindowProc(phpctx->wpSearchBox, hWnd, uMsg, wParam, lParam));
}

LRESULT CALLBACK HashPropResultsProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PHASHPROPCONTEXT phpctx = (PHASHPROPCONTEXT)GetWindowLongPtr(
        (HWND)GetWindowLongPtr(hWnd, GWLP_HWNDPARENT),
        DWLP_USER
    );

    if (wParam == VK_ESCAPE)
    {
        if (uMsg == WM_GETDLGCODE)
        {
            return(DLGC_WANTALLKEYS);
        }
        else if (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST)
        {
            PostMessage(phpctx->hWnd, uMsg, wParam, lParam);

            return(0);
        }
    }
    else if (uMsg == WM_CHAR && wParam == 1)  // CTRL-a: select all items
    {
        LVITEM lvi;

        ZeroMemory(&lvi, sizeof(lvi));
        lvi.stateMask = LVIS_SELECTED;
        lvi.state = LVIS_SELECTED;
        SendMessage(hWnd, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&lvi);

        return(1);
    }
    else if (uMsg == WM_COPY || (uMsg == WM_CHAR && wParam == 3))  // CTRL-c: copy
    {
        HashPropCopySelection(hWnd);

        return(1);
    }

    return(CallWindowProc(phpctx->wpResultsBox, hWnd, uMsg, wParam, lParam));
}

VOID WINAPI HashPropDlgInit( PHASHPROPCONTEXT phpctx )
{
	HWND hWnd = phpctx->hWnd;
	UINT i;

	// Load strings
	{
		static const UINT16 arStrMap[][2] =
		{
			{ IDC_STATUSBOX, IDS_HP_STATUSBOX },
			{ IDC_FIND_NEXT, IDS_HP_FIND      },
			{ IDC_PAUSE,     IDS_HP_PAUSE     },
			{ IDC_STOP,      IDS_HP_STOP      },
			{ IDC_SAVE,      IDS_HP_SAVE      },
			{ IDC_OPTIONS,   IDS_HP_OPTIONS   }
		};

		for (i = 0; i < countof(arStrMap); ++i)
			SetControlText(hWnd, arStrMap[i][0], arStrMap[i][1]);
	}

    // Load the two configuration items we need
    phpctx->opt.dwFlags = HCOF_FONT | HCOF_CHECKSUMS;
    OptionsLoad(&phpctx->opt);

	// Initialize the results list view
	{
		// Set the font
		if (phpctx->hFont = CreateFontIndirect(&phpctx->opt.lfFont))
			SendDlgItemMessage(hWnd, IDC_RESULTS, WM_SETFONT, (WPARAM)phpctx->hFont, FALSE);
        HWND hWndResults = GetDlgItem(hWnd, IDC_RESULTS);

		// Set the extended view styles and enable grouping
		SendMessage(hWndResults, LVM_SETEXTENDEDLISTVIEWSTYLE,
			(WPARAM)LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER,
			(LPARAM)LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
		SendMessage(hWndResults, LVM_ENABLEGROUPVIEW, TRUE, 0);

		// Insert the algorithm and value columns
		{
			LVCOLUMN lvc;
			TCHAR szColAlg[32];
			TCHAR szColVal[32];

			ZeroMemory(&lvc, sizeof(lvc));
			lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
			lvc.iSubItem = 0;
			lvc.cx = 200;
			LoadString(g_hModThisDll, IDS_HP_COL_ALGORITHM, szColAlg, countof(szColAlg));
			lvc.pszText = szColAlg;
			SendMessage(hWndResults, LVM_INSERTCOLUMN, 0, (LPARAM)&lvc);

			ZeroMemory(&lvc, sizeof(lvc));
			lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
			lvc.iSubItem = 1;
			lvc.cx = 300;
			LoadString(g_hModThisDll, IDS_HP_COL_VALUE, szColVal, countof(szColVal));
			lvc.pszText = szColVal;
			SendMessage(hWndResults, LVM_INSERTCOLUMN, 1, (LPARAM)&lvc);
		}

        // Subclass it to handle CTRL-a, copy and escape
        phpctx->wpResultsBox = (WNDPROC)SetWindowLongPtr(
            GetDlgItem(hWnd, IDC_RESULTS),
            GWLP_WNDPROC,
            (LONG_PTR)HashPropResultsProc
        );
	}

	// Initialize the search text box
	{
		// Subclass it to handle pressing the return key
		phpctx->wpSearchBox = (WNDPROC)SetWindowLongPtr(
			GetDlgItem(hWnd, IDC_SEARCHBOX),
			GWLP_WNDPROC,
			(LONG_PTR)HashPropEditProc
		);
	}

	// Initialize miscellaneous stuff
	{
		phpctx->hList = SLCreateEx(TRUE);
		phpctx->dwFlags = 0;
		phpctx->cTotal = 0;
		phpctx->cSuccess = 0;
		phpctx->obScratch = 0;
		phpctx->iGroupId = 0;
        phpctx->hThread = NULL;
        phpctx->hUnpauseEvent = NULL;
        phpctx->hFileOut = INVALID_HANDLE_VALUE;
		ZeroMemory(&phpctx->ofn, sizeof(phpctx->ofn));
	}
}

VOID WINAPI HashPropFitDialog( HWND hWnd )
{
	HWND hWndResults = GetDlgItem(hWnd, IDC_RESULTS);
	RECT rcDlg, rcTop, rcBottom;
	INT dy, dx, i;

	GetClientRect(hWnd, &rcDlg);
	GetWindowRect(hWndResults, &rcTop);
	GetWindowRect(GetDlgItem(hWnd, IDC_OPTIONS), &rcBottom);

	// This is kosher because a RECT is really just two POINTs...
	ScreenToClient(hWnd, (PPOINT)&rcTop.left);
	ScreenToClient(hWnd, (PPOINT)&rcTop.right);
	ScreenToClient(hWnd, (PPOINT)&rcBottom.right);

	// Get the amount that we need to offset by...
	dy = (rcDlg.bottom - rcBottom.bottom) - (rcTop.top - rcDlg.top);
	dx = (rcDlg.right - rcTop.right) - (rcTop.left - rcDlg.left);

	if (dy > 0)
	{
		// Controls to move by dy
		static const UINT16 arCtrlsMY[] =
		{
			IDC_STATUSBOX,
			IDC_PROG_TOTAL,
			IDC_PROG_FILE,
			IDC_SEARCHBOX,
			IDC_FIND_NEXT,
			IDC_PAUSE,
			IDC_STOP,
			IDC_SAVE,
			IDC_OPTIONS
		};

		// Shift controls down by dy
		for (i = 0; i < countof(arCtrlsMY); ++i)
		{
			HWND hWndCtrl = GetDlgItem(hWnd, arCtrlsMY[i]);
			GetWindowRect(hWndCtrl, &rcBottom);
			ScreenToClient(hWnd, (PPOINT)&rcBottom.left);
			SetWindowPos(hWndCtrl, NULL, rcBottom.left, rcBottom.top + dy,
			             0, 0, SWP_NOSIZE | SWP_NOZORDER);
		}

		// Increase the height of the results box by dy
		SetWindowPos(hWndResults, NULL, 0, 0, rcTop.right - rcTop.left,
		             dy + rcTop.bottom - rcTop.top, SWP_NOMOVE | SWP_NOZORDER);
	}

	if (dx > 0)
	{
		// Controls to resize by dx
		static const UINT16 arCtrlsSX[] =
		{
			IDC_RESULTS,
			IDC_STATUSBOX,
			IDC_PROG_TOTAL,
			IDC_PROG_FILE,
			IDC_SEARCHBOX
		};

		// Controls to move by dx
		static const UINT16 arCtrlsMX[] =
		{
			IDC_FIND_NEXT,
			IDC_PAUSE,
			IDC_STOP,
			IDC_SAVE,
			IDC_OPTIONS
		};

		// Expand controls by dx
		for (i = 0; i < countof(arCtrlsSX); ++i)
		{
			HWND hWndCtrl = GetDlgItem(hWnd, arCtrlsSX[i]);
			GetWindowRect(hWndCtrl, &rcBottom);
			ScreenToClient(hWnd, (PPOINT)&rcBottom.left);
			ScreenToClient(hWnd, (PPOINT)&rcBottom.right);
			SetWindowPos(hWndCtrl, NULL, 0, 0, dx + rcBottom.right - rcBottom.left,
			             rcBottom.bottom - rcBottom.top, SWP_NOMOVE | SWP_NOZORDER);
		}

		// Shift controls right by dx
		for (i = 0; i < countof(arCtrlsMX); ++i)
		{
			HWND hWndCtrl = GetDlgItem(hWnd, arCtrlsMX[i]);
			GetWindowRect(hWndCtrl, &rcBottom);
			ScreenToClient(hWnd, (PPOINT)&rcBottom.left);
			SetWindowPos(hWndCtrl, NULL, rcBottom.left + dx, rcBottom.top,
			             0, 0, SWP_NOSIZE | SWP_NOZORDER);
		}
	}

	HashPropSizeListColumn(GetDlgItem(hWnd, IDC_RESULTS));
}

VOID WINAPI HashPropForceLTR( HWND hWndEdit )
{
	DWORD dwExStyle = (DWORD)GetWindowLongPtr(hWndEdit, GWL_EXSTYLE);
	dwExStyle &= ~(WS_EX_RIGHT | WS_EX_RTLREADING | WS_EX_LEFTSCROLLBAR);
	SetWindowLongPtr(hWndEdit, GWL_EXSTYLE, dwExStyle);
}



/*============================================================================*\
	Results list view
\*============================================================================*/

VOID WINAPI HashPropSizeListColumn( HWND hWndResults )
{
	RECT rc;
	INT cx, cxAlg, cxHeader;

	GetClientRect(hWndResults, &rc);

	// Algorithm column: double the wider of the header or the item text, so
	// it never clips regardless of the loaded font
	SendMessage(hWndResults, LVM_SETCOLUMNWIDTH, 0, (LPARAM)LVSCW_AUTOSIZE);
	cxAlg = (INT)SendMessage(hWndResults, LVM_GETCOLUMNWIDTH, 0, 0);
	SendMessage(hWndResults, LVM_SETCOLUMNWIDTH, 0, (LPARAM)LVSCW_AUTOSIZE_USEHEADER);
	cxHeader = (INT)SendMessage(hWndResults, LVM_GETCOLUMNWIDTH, 0, 0);
	if (cxHeader > cxAlg)
		cxAlg = cxHeader;
	cxAlg *= 2;
	if (cxAlg < 16)
		cxAlg = 16;

	// The value column takes the remaining space
	cx = rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL) - cxAlg;
	if (cx < 16)
	{
		cxAlg += cx - 16;
		cx = 16;
	}

	SendMessage(hWndResults, LVM_SETCOLUMNWIDTH, 0, (LPARAM)cxAlg);
	SendMessage(hWndResults, LVM_SETCOLUMNWIDTH, 1, (LPARAM)cx);
}

VOID WINAPI HashPropListClear( HWND hWndResults )
{
	SendMessage(hWndResults, LVM_DELETEALLITEMS, 0, 0);
	SendMessage(hWndResults, LVM_REMOVEALLGROUPS, 0, 0);
}

static PCTSTR HashPropSkipLeadingSpaces( PCTSTR psz )
{
	while (*psz == TEXT(' '))
		++psz;
	return(psz);
}

static BOOL WINAPI HashPropGetItemText( HWND hWndResults, INT iItem, INT iSubItem,
                                        PTSTR pszText, INT cchTextMax )
{
	LVITEM lvi;

	ZeroMemory(&lvi, sizeof(lvi));
	lvi.iSubItem = iSubItem;
	lvi.cchTextMax = cchTextMax;
	lvi.pszText = pszText;

	return((BOOL)SendMessage(hWndResults, LVM_GETITEMTEXT,
	                         (WPARAM)iItem, (LPARAM)&lvi));
}

VOID WINAPI HashPropListAddFile( PHASHPROPCONTEXT phpctx, PHASHPROPITEM pItem )
{
	HWND hWndResults = GetDlgItem(phpctx->hWnd, IDC_RESULTS);
	LVGROUP lvg;
	LVITEM lvi;

	assert(phpctx->opt.dwChecksums != 0);

	// Insert a group header for this file, containing just the file name
	{
		PCTSTR pszPath = pItem->szPath + phpctx->cchPrefix;
		SIZE_T cchPath = pItem->cchPath - phpctx->cchPrefix;
		PCTSTR pszLeaf = pszPath + cchPath;
		SIZE_T cchLeaf;

		while (pszLeaf > pszPath && pszLeaf[-1] != TEXT('\\') && pszLeaf[-1] != TEXT('/'))
			--pszLeaf;

		cchLeaf = cchPath - (SIZE_T)(pszLeaf - pszPath);
		memcpy(phpctx->scratch.szW, pszLeaf, cchLeaf * sizeof(TCHAR));
		phpctx->scratch.szW[cchLeaf] = 0;

		ZeroMemory(&lvg, sizeof(lvg));
		lvg.cbSize = sizeof(lvg);
		lvg.mask = LVGF_GROUPID | LVGF_HEADER;
		lvg.iGroupId = (INT)phpctx->iGroupId;
		lvg.pszHeader = phpctx->scratch.szW;
		lvg.cchHeader = (INT)cchLeaf + 1;
		SendMessage(hWndResults, LVM_INSERTGROUP, (WPARAM)-1, (LPARAM)&lvg);
	}

	// Insert one item per enabled checksum
	ZeroMemory(&lvi, sizeof(lvi));
	lvi.mask = LVIF_TEXT | LVIF_GROUPID;
	lvi.iItem = (INT)SendMessage(hWndResults, LVM_GETITEMCOUNT, 0, 0);
	lvi.iSubItem = 0;
	lvi.iGroupId = (INT)phpctx->iGroupId;

#define HASH_PROP_INSERT_item(alg)                                                  \
    if (phpctx->opt.dwChecksums & WHEX_CHECK##alg)                                  \
    {                                                                               \
        INT iIndex;                                                                 \
        LVITEM lviValue;                                                            \
        lvi.iItem = (INT)SendMessage(hWndResults, LVM_GETITEMCOUNT, 0, 0);          \
        lvi.pszText = (PTSTR)HashPropSkipLeadingSpaces(HASH_RNAME_##alg);           \
        iIndex = (INT)SendMessage(hWndResults, LVM_INSERTITEM,                      \
                                  (WPARAM)-1, (LPARAM)&lvi);                        \
        if (iIndex >= 0)                                                            \
        {                                                                           \
            ZeroMemory(&lviValue, sizeof(lviValue));                                \
            lviValue.mask = LVIF_TEXT;                                              \
            lviValue.iItem = iIndex;                                                \
            lviValue.iSubItem = 1;                                                  \
            lviValue.cchTextMax = -1;                                               \
            lviValue.pszText = pItem->results.szHex##alg;                           \
            SendMessage(hWndResults, LVM_SETITEMTEXT,                               \
                        (WPARAM)iIndex, (LPARAM)&lviValue);                         \
        }                                                                           \
    }
	FOR_EACH_HASH(HASH_PROP_INSERT_item)

	++phpctx->iGroupId;
}

static BOOL WINAPI HashPropCopyAppend( PTSTR *ppszBuffer, SIZE_T *pcbCapacity,
                                       SIZE_T *pcchUsed, PCTSTR pszText, SIZE_T cchText )
{
	SIZE_T cchAdd = cchText + CCH_CRLF;

	if (*pcchUsed + cchAdd + 1 > *pcbCapacity)
	{
		SIZE_T cbNew = (*pcbCapacity) * 2;
		PTSTR pszNew;

		while (*pcchUsed + cchAdd + 1 > cbNew)
			cbNew *= 2;

		pszNew = (PTSTR)realloc(*ppszBuffer, cbNew * sizeof(TCHAR));
		if (!pszNew)
			return(FALSE);

		*ppszBuffer = pszNew;
		*pcbCapacity = cbNew;
	}

	memcpy(*ppszBuffer + *pcchUsed, pszText, cchText * sizeof(TCHAR));
	memcpy(*ppszBuffer + *pcchUsed + cchText, CRLF, CCH_CRLF * sizeof(TCHAR));
	*pcchUsed += cchAdd;
	(*ppszBuffer)[*pcchUsed] = 0;

	return(TRUE);
}

VOID WINAPI HashPropCopySelection( HWND hWnd )
{
	INT iItem, cItems = (INT)SendMessage(hWnd, LVM_GETITEMCOUNT, 0, 0);
	INT iPrevGroup = -1;
	TCHAR szHeader[MAX_PATH_BUFFER + MAX_STRINGRES];
	TCHAR szAlg[MAX_STRINGRES];
	TCHAR szValue[2 * MAX_DIGEST_STRING_LENGTH];
	TCHAR szLine[2 * MAX_DIGEST_STRING_LENGTH + MAX_STRINGRES];
	SIZE_T cbCapacity = 0x2000;
	SIZE_T cchUsed = 0;
	PTSTR pszBuffer = (PTSTR)malloc(cbCapacity * sizeof(TCHAR));

	if (!pszBuffer)
		return;

	pszBuffer[0] = 0;

	for (iItem = 0; iItem < cItems; ++iItem)
	{
		LVITEM lvi;
		LVGROUP lvg;

		if (!(UINT)SendMessage(hWnd, LVM_GETITEMSTATE, (WPARAM)iItem, (LPARAM)LVIS_SELECTED))
			continue;

		// Emit the file header whenever the group changes
		ZeroMemory(&lvi, sizeof(lvi));
		lvi.iItem = iItem;
		lvi.iSubItem = 0;
		lvi.mask = LVIF_GROUPID;
		if (!SendMessage(hWnd, LVM_GETITEM, 0, (LPARAM)&lvi))
			continue;

		if (lvi.iGroupId != iPrevGroup)
		{
			ZeroMemory(&lvg, sizeof(lvg));
			lvg.cbSize = sizeof(lvg);
			lvg.mask = LVGF_GROUPID | LVGF_HEADER;
			lvg.iGroupId = lvi.iGroupId;
			lvg.pszHeader = szHeader;
			lvg.cchHeader = countof(szHeader);

			if (SendMessage(hWnd, LVM_GETGROUPINFO, lvi.iGroupId, (LPARAM)&lvg))
			{
				if (!HashPropCopyAppend(&pszBuffer, &cbCapacity, &cchUsed,
				                       lvg.pszHeader, SSLen(lvg.pszHeader)))
					goto cleanup;
			}

			iPrevGroup = lvi.iGroupId;
		}

		if (HashPropGetItemText(hWnd, iItem, 0, szAlg, (INT)countof(szAlg)) &&
			HashPropGetItemText(hWnd, iItem, 1, szValue, (INT)countof(szValue)))
		{
			StringCchPrintf(szLine, countof(szLine), _T("%s: %s"), szAlg, szValue);

			if (!HashPropCopyAppend(&pszBuffer, &cbCapacity, &cchUsed,
			                       szLine, SSLen(szLine)))
				goto cleanup;
		}
	}

	if (cchUsed && OpenClipboard(hWnd))
	{
		HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, (cchUsed + 1) * sizeof(TCHAR));

		if (hGlobal)
		{
			PTSTR pszClip = (PTSTR)GlobalLock(hGlobal);

			if (pszClip)
			{
				memcpy(pszClip, pszBuffer, (cchUsed + 1) * sizeof(TCHAR));
				GlobalUnlock(hGlobal);
			}

			if (EmptyClipboard())
				SetClipboardData(CF_UNICODETEXT, hGlobal);
		}

		CloseClipboard();
	}

cleanup:
	free(pszBuffer);
}



/*============================================================================*\
	Dialog status
\*============================================================================*/

VOID WINAPI HashPropUpdateResults( PHASHPROPCONTEXT phpctx, PHASHPROPITEM pItem )
{
	HWND hWnd = phpctx->hWnd;
	HWND hWndResults = GetDlgItem(hWnd, IDC_RESULTS);

	/**
	 * It turns out that when hashing large numbers of small files, the
	 * worker thread can far outpace the UI thread, leaving the UI thread
	 * with a large backlog of updates; the solution is to keep track of
	 * the size of this backlog so that steps can be taken to alleviate it.
	 *
	 * 1) cSentMsgs and cHandledMsgs are used to keep track of the backlog;
	 *    cSentMsgs will always be >= to cHandledMsgs (there are no race
	 *    conditions to worry about), and their difference is the number
	 *    of updates pending in the queue AFTER the update currently being
	 *    handled is completed; therefore, zero means no pending backlog.
	 * 2) If there are more than 50 backlogged updates, the worker thread
	 *    will throttle back enough to keep the backlog <= 50.
	 * 3) Unlike the old Edit box, the ListView inserts results immediately
	 *    (they are much cheaper to insert than EM_REPLACESEL replacements)
	 **/

    // Check to see of any desired hashes are not present in the results
    if (phpctx->opt.dwChecksums & ~pItem->results.dwFlags)
        // Replace the invalid hashes with X's
        HashCalcClearInvalid(&pItem->results, TEXT('X'));
    else
		// Otherwise, we can increment the success count
		++phpctx->cSuccess;

	// Insert the group and its items into the results ListView
	HashPropListAddFile(phpctx, pItem);

	// ClearType will sometimes leave artifacts, so redraw if the user will
	// be looking at this text for a while
	if (phpctx->cSentMsgs == phpctx->cHandledMsgs)
		InvalidateRect(hWndResults, NULL, FALSE);

	// Yes, this means that if we defer the text box update, we also end up
	// deferring the progress bar update too, which is what we want; progress
	// bar updates are not deferred for unreadable files, but that is an edge
	// case that we should not dwell too much on.
	SendMessage(phpctx->hWndPBTotal, PBM_SETPOS, phpctx->cHandledMsgs, 0);
}

VOID WINAPI HashPropFinalStatus( PHASHPROPCONTEXT phpctx )
{
	TCHAR szBuffer1[MAX_STRINGRES], szBuffer2[MAX_STRINGMSG], szBuffer3[MAX_STRINGMSG];

	// FormatFractionalResults expects an empty format buffer on the first call
	szBuffer1[0] = 0;

	FormatFractionalResults(szBuffer1, szBuffer2, phpctx->cSuccess, phpctx->cTotal);

	LoadString(
		g_hModThisDll,
		IDS_HP_STATUSTEXT_FMT,
		szBuffer1,
		countof(szBuffer1)
	);

	StringCchPrintf(szBuffer3, countof(szBuffer3), szBuffer1, szBuffer2);

#ifndef _TIMED
	SetDlgItemText(phpctx->hWnd, IDC_STATUSBOX, szBuffer3);
#else
    StringCchPrintf(szBuffer2, countof(szBuffer2), _T("%s in %d ms"), szBuffer3, phpctx->dwElapsed);
    SetDlgItemText(phpctx->hWnd, IDC_STATUSBOX, szBuffer2);
#endif

	// Enable search controls
	EnableControl(phpctx->hWnd, IDC_SEARCHBOX, TRUE);
	EnableControl(phpctx->hWnd, IDC_FIND_NEXT, TRUE);

	// Enable the save button only if there exists completed results to save
	if (!(phpctx->dwFlags & HPF_INTERRUPTED) && phpctx->cSuccess > 0)
		EnableControl(phpctx->hWnd, IDC_SAVE, TRUE);
}



/*============================================================================*\
	Dialog commands
\*============================================================================*/

VOID WINAPI HashPropFindText( PHASHPROPCONTEXT phpctx, BOOL bIncremental )
{
	// Since the SimpleList context for hList is unused (the hListRaw ctx
	// is the HP ctx), we might as well make use of that as an easy fussless
	// way to allocate memory without worrying about freeing it

	HWND hWndResults = GetDlgItem(phpctx->hWnd, IDC_RESULTS);
	HWND hWndSearch = GetDlgItem(phpctx->hWnd, IDC_SEARCHBOX);

	SIZE_T cchNeedle = SendMessage(hWndSearch, WM_GETTEXTLENGTH, 0, 0);
	PTSTR pszNeedle = SLSetContextSize(phpctx->hList, ((UINT)cchNeedle + 1) * sizeof(TCHAR));
	INT iItem, iStart, cItems = (INT)SendMessage(hWndResults, LVM_GETITEMCOUNT, 0, 0);
	INT iFound = -1;
	TCHAR szText[MAX_PATH_BUFFER + MAX_STRINGRES];

	if (pszNeedle && SendMessage(hWndSearch, WM_GETTEXT, cchNeedle + 1, (LPARAM)pszNeedle))
	{
		// Dangling whitespace should not affect search results
		StrTrim(pszNeedle, TEXT(" \t\r\n"));
		cchNeedle = SSLen(pszNeedle);
	}

	if (cchNeedle == 0)
	{
		if (bIncremental)
			return;
	}
	else
	{
		// Continue the search from the current selection
		iStart = (INT)SendMessage(hWndResults, LVM_GETNEXTITEM,
		                         (WPARAM)-1, (LPARAM)(LVNI_FOCUSED | LVNI_SELECTED));

		if (iStart >= 0)
		{
			for (iItem = iStart + 1; iItem < cItems; ++iItem)
			{
				if ((HashPropGetItemText(hWndResults, iItem, 0, szText, (INT)countof(szText)) &&
						StrStrI(szText, pszNeedle)) ||
					(HashPropGetItemText(hWndResults, iItem, 1, szText, (INT)countof(szText)) &&
						StrStrI(szText, pszNeedle)))
				{
					iFound = iItem;
					break;
				}
			}

			if (iFound < 0)
			{
				for (iItem = 0; iItem <= iStart; ++iItem)
				{
					if ((HashPropGetItemText(hWndResults, iItem, 0, szText, (INT)countof(szText)) &&
							StrStrI(szText, pszNeedle)) ||
						(HashPropGetItemText(hWndResults, iItem, 1, szText, (INT)countof(szText)) &&
							StrStrI(szText, pszNeedle)))
					{
						iFound = iItem;
						break;
					}
				}
			}
		}
		else
		{
			for (iItem = 0; iItem < cItems; ++iItem)
			{
				if ((HashPropGetItemText(hWndResults, iItem, 0, szText, (INT)countof(szText)) &&
						StrStrI(szText, pszNeedle)) ||
					(HashPropGetItemText(hWndResults, iItem, 1, szText, (INT)countof(szText)) &&
						StrStrI(szText, pszNeedle)))
				{
					iFound = iItem;
					break;
				}
			}
		}

		if (iFound >= 0)
		{
			LVITEM lvi;

			ZeroMemory(&lvi, sizeof(lvi));
			lvi.stateMask = LVIS_FOCUSED | LVIS_SELECTED;
			lvi.state = LVIS_FOCUSED | LVIS_SELECTED;
			SendMessage(hWndResults, LVM_SETITEMSTATE, (WPARAM)iFound, (LPARAM)&lvi);
			SendMessage(hWndResults, LVM_ENSUREVISIBLE, (WPARAM)iFound, FALSE);
		}
	}

	if (iFound < 0 && cchNeedle != 0)
	{
		TCHAR szBuffer[MAX_STRINGMSG];

		EDITBALLOONTIP ebt;
		ebt.cbStruct = sizeof(ebt);
		ebt.pszTitle = NULL;
		ebt.pszText = szBuffer;
		ebt.ttiIcon = TTI_NONE;

		LoadString(
			g_hModThisDll,
			(cchNeedle) ? IDS_HP_FIND_NOTFOUND : IDS_HP_FIND_NOSTRING,
			szBuffer,
			countof(szBuffer)
		);

		SendMessage(hWndSearch, EM_SHOWBALLOONTIP, 0, (LPARAM)&ebt);
	}
}


VOID WINAPI HashPropSaveResults( PHASHPROPCONTEXT phpctx )
{
    assert(! (phpctx->dwFlags & HPF_INTERRUPTED));
    assert(phpctx->cSuccess > 0);

    // HashCalcInitSave will set the file handle
    HashCalcInitSave(phpctx);

    if (phpctx->hFileOut != INVALID_HANDLE_VALUE)
    {
        // If the last item in the list already has the desired hash computed
        DWORD dwDesiredHash = 1 << (phpctx->ofn.nFilterIndex - 1);
        if (((PHASHPROPITEM)SLGetDataLast(phpctx->hList))->results.dwFlags & dwDesiredHash)
        {
            HashPropDoSaveResults(phpctx);
        }
        else
        {
            // Ensure the desired hash is enabled, and begin generating the new hash(es)
            assert(phpctx->status == CLEANUP_COMPLETED);
            assert(phpctx->cHandledMsgs >= phpctx->cSentMsgs);
            phpctx->opt.dwChecksums |= dwDesiredHash;
            HashPropRestart(phpctx);
            // HashPropDoSaveResults() is called when the worker thread posts a HM_WORKERTHREAD_DONE msg
        }
    }
}

VOID WINAPI HashPropDoSaveResults(PHASHPROPCONTEXT phpctx)
{
    assert(phpctx->hFileOut != INVALID_HANDLE_VALUE);

	if (! (phpctx->dwFlags & HPF_INTERRUPTED))
	{
        HashCalcSetSaveFormat(phpctx);

		PHASHPROPITEM pItem;
		PHASHPROPSORTITEM pSortItems = NULL;
		UINT cItems = 0;

		if (phpctx->cTotal)
			pSortItems = (PHASHPROPSORTITEM)malloc(phpctx->cTotal * sizeof(*pSortItems));

		SLReset(phpctx->hList);
		while (pItem = SLGetDataAndStep(phpctx->hList))
		{
			if (pSortItems)
			{
				pSortItems[cItems].pItem = pItem;
				pSortItems[cItems].iOriginal = cItems;
			}

			++cItems;
		}

		if (pSortItems)
		{
			UINT i;

			qsort_s(pSortItems, cItems, sizeof(*pSortItems), HashPropSortCompare, phpctx);

			for (i = 0; i < cItems; ++i)
				HashCalcWriteResult(phpctx, pSortItems[i].pItem);

			free(pSortItems);
		}
		else
		{
			SLReset(phpctx->hList);

			while (pItem = SLGetDataAndStep(phpctx->hList))
				HashCalcWriteResult(phpctx, pItem);
		}
	}

	CloseHandle(phpctx->hFileOut);
    phpctx->hFileOut = INVALID_HANDLE_VALUE;
}

INT __cdecl HashPropSortCompare( void *pvContext, const void *pvItemA, const void *pvItemB )
{
	PHASHPROPCONTEXT phpctx = (PHASHPROPCONTEXT)pvContext;
	const HASHPROPSORTITEM *pItemA = (const HASHPROPSORTITEM *)pvItemA;
	const HASHPROPSORTITEM *pItemB = (const HASHPROPSORTITEM *)pvItemB;
	INT iCompare = StrCmpLogical(
		pItemA->pItem->szPath + phpctx->cchAdjusted,
		pItemB->pItem->szPath + phpctx->cchAdjusted
	);

	if (iCompare)
		return(iCompare);

	return(pItemA->iOriginal < pItemB->iOriginal ? -1 : (pItemA->iOriginal == pItemB->iOriginal ? 0 : 1));
}

VOID WINAPI HashPropSaveResultsCleanup( PHASHPROPCONTEXT phpctx )
{
    if (phpctx->hFileOut != INVALID_HANDLE_VALUE)
    {
        // Don't keep partially generated checksum files
        BOOL bDeleted = HashCalcDeleteFileByHandle(phpctx->hFileOut);

		CloseHandle(phpctx->hFileOut);

        // Should only happen on Windows XP
        if (!bDeleted)
            DeleteFile(phpctx->ofn.lpstrFile);

        phpctx->hFileOut = INVALID_HANDLE_VALUE;
	}
}


VOID WINAPI HashPropOptions( PHASHPROPCONTEXT phpctx )
{
	OptionsDialog(phpctx->hWnd, &phpctx->opt);

    // Update the results, but only if a file save isn't in progress
    if (phpctx->opt.dwFlags & HCOF_CHECKSUMS && phpctx->hFileOut == INVALID_HANDLE_VALUE)
    {
        phpctx->dwFlags |= HCF_RESTARTING;
        WorkerThreadStop((PCOMMONCONTEXT)phpctx);
        WorkerThreadCleanup((PCOMMONCONTEXT)phpctx);

        if (phpctx->cHandledMsgs >= phpctx->cSentMsgs)
            HashPropRestart(phpctx);
        // Otherwise the call to HashPropRestart() is made following the
        // last pending HM_WORKERTHREAD_UPDATE message in HashPropDlgProc()
    }

	if (phpctx->opt.dwFlags & HCOF_FONT)
	{
		HFONT hFont = CreateFontIndirect(&phpctx->opt.lfFont);

		if (hFont)
		{
			SendDlgItemMessage(phpctx->hWnd, IDC_RESULTS, WM_SETFONT, (WPARAM)hFont, TRUE);
			if (phpctx->hFont) DeleteObject(phpctx->hFont);
			phpctx->hFont = hFont;
		}
	}
}

VOID WINAPI HashPropRestart( PHASHPROPCONTEXT phpctx )
{
    // Reset these flags back to the default
    phpctx->dwFlags &= ~(HCF_RESTARTING | HPF_INTERRUPTED);

    // Just reset the list if it's fully loaded, else reload it from scratch
    if (phpctx->dwFlags & HPF_HLIST_PREPPED)
        SLReset(phpctx->hList);
    else
    {
        SLRelease(phpctx->hList);
        phpctx->hList = SLCreateEx(TRUE);
        phpctx->cTotal = 0;
    }

    // Reset the GUI to its initial state
    EnableControl( phpctx->hWnd, IDC_SAVE,       FALSE);
    EnableControl( phpctx->hWnd, IDC_FIND_NEXT,  FALSE);
    EnableControl( phpctx->hWnd, IDC_SEARCHBOX,  FALSE);
    EnableControl( phpctx->hWnd, IDC_PROG_TOTAL, TRUE);
    EnableControl( phpctx->hWnd, IDC_PROG_FILE,  TRUE);
    EnableControl( phpctx->hWnd, IDC_PAUSE,      TRUE);
    EnableControl( phpctx->hWnd, IDC_STOP,       TRUE);
    HashPropListClear(GetDlgItem(phpctx->hWnd, IDC_RESULTS));
    SetControlText(phpctx->hWnd, IDC_STATUSBOX,  IDS_HP_STATUSBOX);
    SetControlText(phpctx->hWnd, IDC_PAUSE,      IDS_HC_PAUSE);
    SetProgressBarPause((PCOMMONCONTEXT)phpctx,  PBST_NORMAL);
    SendMessage(phpctx->hWndPBFile,  PBM_SETPOS, 0, 0);
    SendMessage(phpctx->hWndPBTotal, PBM_SETPOS, 0, 0);

    phpctx->cSuccess = 0;
    phpctx->obScratch = 0;
    phpctx->iGroupId = 0;

    phpctx->hThread = CreateThreadCRT(NULL, phpctx);

    if (!phpctx->hThread)
        WorkerThreadCleanup((PCOMMONCONTEXT)phpctx);
}
