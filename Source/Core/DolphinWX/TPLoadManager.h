// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

//Dragonbane

#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/gdicmn.h>
#include <wx/string.h>
#include <wx/translation.h>
#include <wx/windowid.h>
#include <wx/artprov.h>
#include <wx/stattext.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/sizer.h>

#include "Common/CommonTypes.h"
#include <string>



class wxWindow;
class wxButton;

class TPLoadManager : public wxDialog
{
private:

	DECLARE_EVENT_TABLE();

protected:

	wxStaticText* m_staticText1;
	wxChoice* m_choice_type;
	wxStaticText* m_staticText11;
	wxChoice* m_choice_stage;
	wxStaticText* m_staticText2;
	wxChoice* m_choice_room;
	wxStaticText* m_staticText21;
	wxChoice* m_choice_spawn;
	wxStaticText* m_staticText22;
	wxChoice* m_choice_state;
	wxButton* m_button_load;


	wxArrayString OverworldResultList;
	wxArrayString InteriorResultList;
	wxArrayString DungeonResultList;
	wxArrayString CaveResultList;

public:

	TPLoadManager(wxWindow* parent, wxWindowID id = wxID_ANY, const wxString& title = wxT("TP Area Load by DB"), const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style  = wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP);
	void OnSelectionChanged(wxCommandEvent& event);
	void OnButtonPressed(wxCommandEvent& event);
	void OnCloseWindow(wxCloseEvent& event);
	void Shown();

	void FillStages(int selectedID);
	void FillRooms(std::string selectedStage);
	std::string GetDisplayedStageName(std::string stageName);

	//Stuff
	u8 Read8(u8* Data, int Offset);
	u16 Read16(u8* Data, int Offset);
	u32 Read32(u8* Data, int Offset);
	std::string ReadString(u8* Data, int Offset, int Length);
	std::string ReadStringFull(u8* Data, int Offset, u32 dataSize);
	void GetFileNode(u8* Data, u32 Offset, u32 Size);
	void AddSpawnPoint(u8* Data, u32 Offset);
	void AddStates(u8* Data, u32 Offset);
	void DecompressYaz0(u8* Input, u32 Offset, u8* Output);
};
