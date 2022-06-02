#include <pdh.h>
#include <pdhmsg.h>
#include <Psapi.h>
#include <shlwapi.h>
#pragma comment(lib, "pdh.lib")

#include "alienfx-gui.h"
#include "EventHandler.h"
#include "AlienFX_SDK.h"

// debug print
#ifdef _DEBUG
#define DebugPrint(_x_) OutputDebugString(_x_);
#else
#define DebugPrint(_x_)
#endif

extern AlienFan_SDK::Control* acpi;
extern EventHandler* eve;

DWORD WINAPI CEventProc(LPVOID);
VOID CALLBACK CForegroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

EventHandler::EventHandler()
{
	StartProfiles();
	StartEffects();
	ChangePowerState();
	StartFanMon();
}

EventHandler::~EventHandler()
{
	StopProfiles();
	StopEffects();
	StopFanMon();
}

void EventHandler::ChangePowerState()
{
	SYSTEM_POWER_STATUS state;
	GetSystemPowerStatus(&state);
	bool sameState = true;
	if (conf->statePower = state.ACLineStatus) {
		// AC line
		switch (state.BatteryFlag) {
		case 8: // charging
			sameState = fxhl->SetMode(MODE_CHARGE);
			break;
		default:
			sameState = fxhl->SetMode(MODE_AC);
			break;
		}
	}
	else {
		// Battery - check BatteryFlag for details
		switch (state.BatteryFlag) {
		case 1: // ok
			sameState = fxhl->SetMode(MODE_BAT);
			break;
		case 2: case 4: // low/critical
			sameState = fxhl->SetMode(MODE_LOW);
			break;
		}
	}
	if (!sameState) {
		DebugPrint(("Power state changed to " + to_string(conf->statePower) + "\n").c_str());
		fxhl->ChangeState();
		fxhl->Refresh();
	}
}

void EventHandler::ChangeScreenState(DWORD state)
{
	if (conf->lightsOn && (conf->offWithScreen || capt)) {
		if (state == 2) {
			// Dim display
			conf->dimmedScreen = true;
			conf->stateScreen = true;
		}
		else {
			conf->stateScreen = state;
			conf->dimmedScreen = false;
		}
		DebugPrint("Display state changed\n");
	} else {
		conf->dimmedScreen = false;
		conf->stateScreen = true;
	}
	fxhl->ChangeState();
}

void EventHandler::SwitchActiveProfile(profile* newID)
{
	if (!newID) newID = conf->FindDefaultProfile();
	if (conf->foregroundProfile && newID->id != conf->foregroundProfile->id) conf->foregroundProfile = NULL;
	if (newID->id != conf->activeProfile->id) {
			modifyProfile.lock();
			conf->activeProfile = newID;
			conf->active_set = &newID->lightsets;
			conf->fan_conf->lastProf = newID->flags & PROF_FANS ? &newID->fansets : &conf->fan_conf->prof;
			if (mon) {
				acpi->SetPower(conf->fan_conf->lastProf->powerStage);
				acpi->SetGPU(conf->fan_conf->lastProf->GPUPower);
			}
			modifyProfile.unlock();

			// change global effect
			if (conf->haveV5) {
				fxhl->UnblockUpdates(false);
				fxhl->UpdateGlobalEffect();
				fxhl->UnblockUpdates(true);
			}

			fxhl->ChangeState();
			ChangeEffectMode();

			DebugPrint((string("Profile switched to ") + to_string(newID->id) + " (" + newID->name + ")\n").c_str());
	} else {
		DebugPrint((string("Same profile \"") + newID->name + "\", skipping switch.\n").c_str());
	}
	return;
}

void EventHandler::StartEvents()
{
	if (!dwHandle) {
		fxhl->RefreshMon();
		// start thread...

		DebugPrint("Event thread start.\n");

		stopEvents = CreateEvent(NULL, true, false, NULL);
		dwHandle = CreateThread(NULL, 0, CEventProc, this, 0, NULL);
	}
}

void EventHandler::StopEvents()
{
	if (dwHandle) {
		DebugPrint("Event thread stop.\n");

		SetEvent(stopEvents);
		WaitForSingleObject(dwHandle, conf->monDelay << 1);
		CloseHandle(dwHandle);
		CloseHandle(stopEvents);
		dwHandle = 0;
	}
}

void EventHandler::ChangeEffectMode() {
	if (conf->enableMon && conf->stateOn) {
		if (conf->GetEffect() != effMode)
			StopEffects();
		else
			fxhl->Refresh();
		StartEffects();
	}
	else
		StopEffects();
	conf->SetToolTip();
}

void EventHandler::StopEffects() {
	switch (effMode) {
	case 1:	StopEvents(); break;
	case 2: if (capt) {
		delete capt; capt = NULL;
	} break;
	case 3: if (audio) {
		delete audio; audio = NULL;
	} break;
	}
	effMode = 0;
	fxhl->Refresh(true);
}

void EventHandler::StartEffects() {
	if (conf->enableMon) {
		// start new mode...
		switch (effMode = conf->GetEffect()) {
		case 1:
			StartEvents();
			break;
		case 2:
			if (!capt) capt = new CaptureHelper();
			break;
		case 3:
			if (!audio) audio = new WSAudioIn();
			break;
		}
	}
}

void EventHandler::StartFanMon() {
	if (conf->fanControl && acpi && !mon)
		mon = new MonHelper(conf->fan_conf);
}

void EventHandler::StopFanMon() {
	if (mon) {
		delete mon;
		mon = NULL;
	}
}

profile* EventHandler::ScanTaskList() {
	DWORD maxProcess=256, maxFileName=MAX_PATH, cbNeeded, cProcesses, cFileName = maxFileName;
	DWORD* aProcesses = new DWORD[maxProcess];
	TCHAR *szProcessName = new TCHAR[maxFileName]{0};

	profile* newp = NULL, *finalP = NULL;

	if (EnumProcesses(aProcesses, maxProcess * sizeof(DWORD), &cbNeeded))
	{
		while ((cProcesses = cbNeeded/sizeof(DWORD))==maxProcess) {
			maxProcess=maxProcess<<1;
			delete[] aProcesses;
			aProcesses=new DWORD[maxProcess];
			EnumProcesses(aProcesses, maxProcess * sizeof(DWORD), &cbNeeded);
		}

		for (UINT i = 0; i < cProcesses; i++)
		{
			if (aProcesses[i])
			{
				HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
					PROCESS_VM_READ,
					FALSE, aProcesses[i]);
				if (hProcess)
				{
					cFileName = GetProcessImageFileName(hProcess, szProcessName, maxFileName); //GetModuleFileNameEx(hProcess, NULL /*hMod*/, szProcessName, maxFileName);
					while (maxFileName==cFileName) {
						maxFileName=maxFileName<<1;
						delete[] szProcessName;
						szProcessName=new TCHAR[maxFileName];
						cFileName = GetProcessImageFileName(hProcess, szProcessName, maxFileName);// GetModuleFileNameEx(hProcess, NULL /*hMod*/, szProcessName, maxFileName);
					}
					PathStripPath(szProcessName);
					// is it related to profile?
					if (newp = conf->FindProfileByApp(string(szProcessName)))
						if (!finalP || !(finalP->flags & PROF_PRIORITY))
							finalP = newp;
					CloseHandle(hProcess);
				}
			}
		}
	}
	delete[] szProcessName;
	delete[] aProcesses;
	return finalP;
}

void EventHandler::CheckProfileWindow(HWND hwnd) {

	if (hwnd) {
		DWORD prcId = 0;
		GetWindowThreadProcessId(hwnd, &prcId);
		HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
			PROCESS_VM_READ,
			FALSE, prcId);

		DWORD nameSize = MAX_PATH, cFileName = nameSize;
		TCHAR* szProcessName = new TCHAR[nameSize]{ 0 };

		cFileName = GetProcessImageFileName(hProcess, szProcessName, nameSize); //GetModuleFileNameEx(hProcess, NULL /*hMod*/, szProcessName, nameSize);
		while (nameSize == cFileName) {
			nameSize = nameSize << 1;
			delete[] szProcessName;
			szProcessName = new TCHAR[nameSize];
			cFileName = GetProcessImageFileName(hProcess, szProcessName, nameSize); //GetModuleFileNameEx(hProcess, NULL/*hMod*/, szProcessName, nameSize);
		}
		CloseHandle(hProcess);
		PathStripPath(szProcessName);

		DebugPrint((string("Foreground switched to ") + szProcessName + "\n").c_str());

		string pName = szProcessName;

		profile* newp = conf->FindProfileByApp(pName, true);
		conf->foregroundProfile = newp ? newp : NULL;

		if (newp || !conf->noDesktop || (pName != "ShellExperienceHost.exe"
			&& pName != "alienfx-gui.exe"
			&& pName != "explorer.exe"
			&& pName != "SearchApp.exe"
#ifdef _DEBUG
			&& pName != "devenv.exe"
#endif
			)) {

			if (!newp) {
				SwitchActiveProfile(ScanTaskList());
			}
			else {
				if (conf->IsPriorityProfile(newp) || !conf->IsPriorityProfile(conf->activeProfile))
					SwitchActiveProfile(newp);
			}
		}
		else {
			DebugPrint("Forbidden app, switch blocked!\n");
		}
		delete[] szProcessName;
	}
	else {
		SwitchActiveProfile(ScanTaskList());
	}
}

// Create - Check process ID, switch if found and no foreground active.
// Foreground - Check process ID, switch if found, clear foreground if not.
// Close - Check process list, switch if found and no foreground active.

VOID CALLBACK CForegroundProc(HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
	eve->CheckProfileWindow(hwnd);
}

VOID CALLBACK CCreateProc(HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {

	HANDLE hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION,
		FALSE, dwEventThread);
	if (hThread) {
		DWORD prcId = GetProcessIdOfThread(hThread);
		if (prcId &&
			idChild == CHILDID_SELF && conf->foregroundProfile != conf->activeProfile) {
			HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
				PROCESS_VM_READ,
				FALSE, prcId);
			DWORD nameSize = MAX_PATH, cFileName = nameSize;
			TCHAR* szProcessName=new TCHAR[nameSize];
			szProcessName[0]=0;
			cFileName = GetProcessImageFileName(hProcess, szProcessName, nameSize);// GetModuleFileNameEx(hProcess, NULL, szProcessName, nameSize);
			while (nameSize==cFileName) {
				nameSize=nameSize<<1;
				delete[] szProcessName;
				szProcessName=new TCHAR[nameSize];
				cFileName = GetProcessImageFileName(hProcess, szProcessName, nameSize); //GetModuleFileNameEx(hProcess, NULL, szProcessName, nameSize);
			}

			PathStripPath(szProcessName);

			switch (dwEvent) {
			case EVENT_OBJECT_CREATE: case EVENT_OBJECT_DESTROY:

				if (conf->FindProfileByApp(string(szProcessName))) {
					eve->SwitchActiveProfile(eve->ScanTaskList());
				}
				break;

			//case EVENT_OBJECT_DESTROY:

			//	if (conf->FindProfileByApp(string(szProcessName))) {
			//		//DebugPrint((string("Process (") + szProcessName + ") status " + to_string(idChild) + "\n").c_str());
			//		eve->SwitchActiveProfile(eve->ScanTaskList());
			//	}

			//	break;
			}

			CloseHandle(hProcess);
			delete[] szProcessName;
		}
		CloseHandle(hThread);
	}
}

void EventHandler::StartProfiles()
{
	if (conf->enableProf && !cEvent) {

		DebugPrint("Profile hooks starting.\n");

		// Need to switch if already running....
		CheckProfileWindow(GetForegroundWindow());

		hEvent = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
								 EVENT_SYSTEM_FOREGROUND, NULL,
								 CForegroundProc, 0, 0,
								 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

		cEvent = SetWinEventHook(EVENT_OBJECT_CREATE,
			EVENT_OBJECT_DESTROY, NULL,
			CCreateProc, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
	}
}

void EventHandler::StopProfiles()
{
	if (cEvent) {
		DebugPrint("Profile hooks stop.\n");
		UnhookWinEvent(hEvent);
		UnhookWinEvent(cEvent);
		cEvent = hEvent = 0;
	}
}

PDH_FMT_COUNTERVALUE_ITEM *counterValues = new PDH_FMT_COUNTERVALUE_ITEM[1];
DWORD counterSize = sizeof(PDH_FMT_COUNTERVALUE_ITEM);

int GetValuesArray(HCOUNTER counter) {
	PDH_STATUS pdhStatus;
	DWORD count;
	while ((pdhStatus = PdhGetFormattedCounterArray( counter, PDH_FMT_LONG, &counterSize, &count, counterValues )) == PDH_MORE_DATA) {
		delete[] counterValues;
		counterValues = new PDH_FMT_COUNTERVALUE_ITEM[counterSize / sizeof(PDH_FMT_COUNTERVALUE_ITEM) + 1];
	}

	if (pdhStatus != ERROR_SUCCESS) {
		return -1;
	}
	return count - 1;
}

DWORD WINAPI CEventProc(LPVOID param)
{
	EventHandler* src = (EventHandler*)param;

	// locales block
	HKL* locIDs = new HKL[10];

	LPCTSTR COUNTER_PATH_CPU = "\\Processor Information(_Total)\\% Processor Time",
		COUNTER_PATH_NET = "\\Network Interface(*)\\Bytes Total/sec",
		COUNTER_PATH_GPU = "\\GPU Engine(*)\\Utilization Percentage",
		COUNTER_PATH_HOT = "\\Thermal Zone Information(*)\\Temperature",
		COUNTER_PATH_HOT2 = "\\EsifDeviceInformation(*)\\Temperature",
		COUNTER_PATH_PWR = "\\EsifDeviceInformation(*)\\RAPL Power",
		COUNTER_PATH_HDD = "\\PhysicalDisk(_Total)\\% Idle Time";

	HQUERY hQuery = NULL;
	HCOUNTER hCPUCounter, hHDDCounter, hNETCounter, hGPUCounter, hTempCounter, hTempCounter2, hPwrCounter;

	MEMORYSTATUSEX memStat{ sizeof(MEMORYSTATUSEX) };

	SYSTEM_POWER_STATUS state;

	EventData cData;

	// Set data source...

	if (PdhOpenQuery(NULL, 0, &hQuery) != ERROR_SUCCESS)
	{
		goto cleanup;
	}

	PdhAddCounter(hQuery, COUNTER_PATH_CPU, 0, &hCPUCounter);
	PdhAddCounter(hQuery, COUNTER_PATH_HDD, 0, &hHDDCounter);
	PdhAddCounter(hQuery, COUNTER_PATH_NET, 0, &hNETCounter);
	PdhAddCounter(hQuery, COUNTER_PATH_GPU, 0, &hGPUCounter);
	PdhAddCounter(hQuery, COUNTER_PATH_HOT, 0, &hTempCounter);
	PdhAddCounter(hQuery, COUNTER_PATH_HOT2, 0, &hTempCounter2);
	PdhAddCounter(hQuery, COUNTER_PATH_PWR, 0, &hPwrCounter);

	PDH_FMT_COUNTERVALUE cCPUVal, cHDDVal;
	DWORD cType = 0;

	while (WaitForSingleObject(src->stopEvents, conf->monDelay) == WAIT_TIMEOUT) {
		// get indicators...

		if (!fxhl->unblockUpdates)
			continue;

		PdhCollectQueryData(hQuery);

		cData = { 0 };

		PdhGetFormattedCounterValue( hCPUCounter, PDH_FMT_LONG, &cType, &cCPUVal );
		PdhGetFormattedCounterValue( hHDDCounter, PDH_FMT_LONG, &cType, &cHDDVal );

		// Network load
		long totalNet = 0;
		for (int i = GetValuesArray(hNETCounter); i >= 0; i--) {
			totalNet += counterValues[i].FmtValue.longValue;
		}

		fxhl->maxData.NET = max(fxhl->maxData.NET, totalNet);

		// GPU load
		for (int i = GetValuesArray(hGPUCounter); i >= 0 && counterValues[i].szName != NULL; i--) {
			cData.GPU = (byte)max(cData.GPU, counterValues[i].FmtValue.longValue);
		}

		// Temperatures
		for (int i = GetValuesArray(hTempCounter); i >= 0 ; i--) {
			if (((int)cData.Temp) + 273 < counterValues[i].FmtValue.longValue)
				cData.Temp = (byte) (counterValues[i].FmtValue.longValue - 273);
		}

		if (src->mon) {
			// Check fan RPMs
			for (unsigned i = 0; i < src->mon->fanRpm.size(); i++) {
				cData.Fan = max(cData.Fan, acpi->GetFanPercent(i));
			}
		}

		// Now other temp sensor block and power block...
		short totalPwr = 0;
		if (conf->esif_temp) {
			if (src->mon) {
				// Let's get temperatures from fan sensors
				for (unsigned i = 0; i < src->mon->senValues.size(); i++)
					cData.Temp = max(cData.Temp, src->mon->senValues[i]);
			}

			// Added other set maximum temp...
			for (int i = GetValuesArray(hTempCounter2); i >= 0; i--) {
				cData.Temp = (byte)max(cData.Temp, counterValues[i].FmtValue.longValue);
			}

			// Powers
			for (int i = GetValuesArray(hPwrCounter); i >= 0; i--) {
				if (counterValues[i].FmtValue.longValue) {
					totalPwr = (short)counterValues[i].FmtValue.longValue;
					//break;
				}
			}
			//src->fxhl->maxData.PWR = max(src->fxhl->maxData.PWR,(totalPwr & 0xff) << 1 + 1);
			while (totalPwr >= fxhl->maxData.PWR)
				fxhl->maxData.PWR <<= 1;
		}

		GlobalMemoryStatusEx(&memStat);
		GetSystemPowerStatus(&state);

		HKL curLocale = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), NULL));

		if (curLocale) {
			for (int i = GetKeyboardLayoutList(10, locIDs); i >= 0; i--) {
				if (curLocale == locIDs[i]) {
					cData.KBD = i > 0 ? 100 : 0;
					break;
				}
			}
		}

		// Leveling...
		cData.Temp = min(100, max(0, cData.Temp));
		cData.Batt = state.BatteryLifePercent > 100 ? 0 : state.BatteryLifePercent;
		cData.HDD = (byte) max(0, 99 - cHDDVal.longValue);
		cData.Fan = min(100, cData.Fan);
		cData.CPU = (byte) cCPUVal.longValue;
		cData.RAM = (byte) memStat.dwMemoryLoad;
		cData.NET = (byte) totalNet * 100 / fxhl->maxData.NET;
		cData.PWR = (byte) totalPwr * 100 / fxhl->maxData.PWR;
		fxhl->maxData.GPU = max(fxhl->maxData.GPU, cData.GPU);
		fxhl->maxData.Temp = max(fxhl->maxData.Temp, cData.Temp);
		fxhl->maxData.RAM = max(fxhl->maxData.RAM, cData.RAM);
		fxhl->maxData.CPU = max(fxhl->maxData.CPU, cData.CPU);

		src->modifyProfile.lock();
		fxhl->SetCounterColor(&cData);
		src->modifyProfile.unlock();

		/*DebugPrint((string("Counters: Temp=") + to_string(cData.Temp) +
					", Power=" + to_string(cData.PWR) +
					", Max. power=" + to_string(maxPower) + "\n").c_str());*/
	}

cleanup:

	if (hQuery)
		PdhCloseQuery(hQuery);

	delete[] locIDs;

	return 0;
}