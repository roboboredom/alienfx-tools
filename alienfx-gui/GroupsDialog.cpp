#include "alienfx-gui.h"

extern void RemoveMapping(groupset* lightsets);

int	gLid = -1, gItem = -1;

void UpdateLightListG(HWND light_list, AlienFX_SDK::group* grp) {
	size_t lights = conf->afx_dev.GetMappings()->size();
	ListBox_ResetContent(light_list);
	for (auto i = 0; i < lights; i++) {
		AlienFX_SDK::mapping* lgh = conf->afx_dev.GetMappings()->at(i);
		if (!grp ||	find_if(grp->lights.begin(), grp->lights.end(), [lgh](pair<DWORD,DWORD> t) {
				return t.first == lgh->devid &&
					t.second == lgh->lightid;
				}) == grp->lights.end()) {
			ListBox_SetItemData(light_list, ListBox_AddString(light_list, lgh->name.c_str()), i);
		}
	}
}

void UpdateGroupLights(HWND light_list, int gID, int sel) {
	ListBox_ResetContent(light_list);
	AlienFX_SDK::group* grp = conf->afx_dev.GetGroupById(gID);
	for (int i = 0; grp && i < grp->lights.size(); i++) {
		AlienFX_SDK::mapping* lgh = conf->afx_dev.GetMappingById(grp->lights[i].first, (WORD)grp->lights[i].second);
		ListBox_SetItemData(light_list, ListBox_AddString(light_list,lgh->name.c_str()), i);
	}
	ListBox_SetCurSel(light_list, sel);
}

BOOL TabGroupsDialog(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
	HWND light_list = GetDlgItem(hDlg, IDC_LIGHTS),
		groups_list = GetDlgItem(hDlg, IDC_GROUPS),
		glights_list = GetDlgItem(hDlg, IDC_LIST_INGROUP);

	switch (message) {
	case WM_INITDIALOG:
	{
		int pos = -1;
		AlienFX_SDK::group* grp = NULL;
		size_t numgroups = conf->afx_dev.GetGroups()->size();
		if (numgroups > 0) {
			if (gLid < 0)
				gLid = conf->afx_dev.GetGroups()->at(0).gid;
			for (UINT i = 0; i < numgroups; i++) {
				pos = (int) ComboBox_AddString(groups_list, conf->afx_dev.GetGroups()->at(i).name.c_str());
				ComboBox_SetItemData(groups_list, pos, conf->afx_dev.GetGroups()->at(i).gid);
				if (conf->afx_dev.GetGroups()->at(i).gid == gLid) {
					ComboBox_SetCurSel(groups_list, pos);
					gItem = pos;
					grp = &conf->afx_dev.GetGroups()->at(i);
				}
			}
			UpdateGroupLights(glights_list, gLid, 0);
		} else {
			EnableWindow(groups_list, false);
			EnableWindow(glights_list, false);
		}
		UpdateLightListG(light_list, grp);
	} break;
	case WM_COMMAND:
	{
		int glItem = (int)ListBox_GetCurSel(glights_list);
		AlienFX_SDK::group* grp = conf->afx_dev.GetGroupById(gLid);
		switch (LOWORD(wParam)) {
		case IDC_GROUPS: {
			switch (HIWORD(wParam))
			{
			case CBN_SELCHANGE: {
				gItem = ComboBox_GetCurSel(groups_list);
				gLid = (int)ComboBox_GetItemData(groups_list, gItem);
				grp = conf->afx_dev.GetGroupById(gLid);
				UpdateGroupLights(glights_list, gLid, 0);
				UpdateLightListG(light_list, grp);
			} break;
			case CBN_EDITCHANGE:
				char buffer[MAX_PATH];
				GetWindowTextA(groups_list, buffer, MAX_PATH);
				if (grp) {
					grp->name = buffer;
					conf->afx_dev.SaveMappings();
					ComboBox_DeleteString(groups_list, gItem);
					ComboBox_InsertString(groups_list, gItem, buffer);
					ComboBox_SetItemData(groups_list, gItem, gLid);
				}
				break;
			}
		} break;
		case IDC_BUT_GRP_UP:
			if (grp && glItem > 0) {
				auto gIter = grp->lights.begin();
				iter_swap(gIter + glItem, gIter + glItem -1);
				UpdateGroupLights(glights_list,gLid, glItem-1);
			}
			break;
		case IDC_BUT_GRP_DOWN:
			if (grp && glItem < grp->lights.size() - 1 ) {
				auto gIter = grp->lights.begin();
				iter_swap(gIter + glItem, gIter + glItem +1);
				UpdateGroupLights(glights_list,gLid, glItem+1);
			}
			break;
		case IDC_BUTTON_ADDG: {
			AlienFX_SDK::group* grp = conf->CreateGroup("Group");
			conf->afx_dev.GetGroups()->push_back(*grp);
			conf->afx_dev.SaveMappings();
			gLid = grp->gid;
			gItem = (int) ComboBox_AddString(groups_list, grp->name.c_str());
			ComboBox_SetItemData(groups_list, gItem, gLid);
			ComboBox_SetCurSel(groups_list, gItem);
			EnableWindow(groups_list, true);
			EnableWindow(glights_list, true);
			UpdateGroupLights(glights_list,gLid,0);
			UpdateLightListG(light_list, grp);
			delete grp;
		} break;
		case IDC_BUTTON_REMG: {
			if (gLid > 0) {
				// delete from all profiles...
				for (auto Iter = conf->profiles.begin(); Iter != conf->profiles.end(); Iter++) {
					// erase group from list
					for (auto it = (*Iter)->lightsets.colors.begin(); it < (*Iter)->lightsets.colors.end(); it++) {
						for (auto lList = it->groups.begin(); lList < it->groups.end(); lList++)
							if ((*lList)->gid == gLid) {
								it->groups.erase(lList);
								break;
							}
						if (it->groups.empty()) {
							(*Iter)->lightsets.colors.erase(it);
							it--;
						}
					}
				}
				for (auto Iter = conf->afx_dev.GetGroups()->begin(); Iter != conf->afx_dev.GetGroups()->end(); Iter++)
					if (Iter->gid == gLid) {
						conf->afx_dev.GetGroups()->erase(Iter);
						break;
					}
				conf->afx_dev.SaveMappings();
				conf->Save();
				ComboBox_DeleteString(groups_list, gItem);
				if (conf->afx_dev.GetGroups()->size() > 0) {
					if (gItem >= conf->afx_dev.GetGroups()->size())
						gItem--;
					gLid = conf->afx_dev.GetGroups()->at(gItem).gid;
					grp = &conf->afx_dev.GetGroups()->at(gItem);
					UpdateLightListG(light_list, grp);
				} else {
					gLid = -1;
					gItem = -1;
					EnableWindow(groups_list, false);
					EnableWindow(glights_list, false);
				}
				ComboBox_SetCurSel(groups_list, gItem);
				UpdateGroupLights(glights_list, gLid,0);
			}
		} break;
		case IDC_BUT_ADDTOG:
		{
			int numSelLights = ListBox_GetSelCount(light_list);
			if (grp && numSelLights > 0) {
				int* selLights = new int[numSelLights];
				ListBox_GetSelItems(light_list, numSelLights, selLights);
				for (int i = 0; i < numSelLights; i++) {
					AlienFX_SDK::mapping* clight = conf->afx_dev.GetMappings()->at(ListBox_GetItemData(light_list, selLights[i]));
					if (clight) {
						grp->lights.push_back({ clight->devid,clight->lightid });
					}
				}
				UpdateGroupLights(glights_list, gLid, (int) grp->lights.size() - 1);
				UpdateLightListG(light_list, grp);
			}
		} break;
		case IDC_BUT_DELFROMG:
			if (grp && glItem >= 0) {
				if (grp && glItem < grp->lights.size()) {
					auto Iter = grp->lights.begin() + glItem;
					grp->lights.erase(Iter);
				}
				UpdateGroupLights(glights_list, gLid, --glItem);
				UpdateLightListG(light_list, grp);
			}
			break;
		}
	} break;
	default: return false;
	}
	return true;
}
