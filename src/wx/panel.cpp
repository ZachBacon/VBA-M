#include "wxvbam.h"
#include "drawing.h"
#include "../gba/RTC.h"
#include "../gba/agbprint.h"
#include "../gb/gbPrinter.h"
#include "../common/Patch.h"
#include <wx/dcbuffer.h>
#include "../sdl/text.h"
#include "../filters/filters.hpp"

int emulating;

IMPLEMENT_DYNAMIC_CLASS(GameArea, wxPanel)

GameArea::GameArea()
    : wxPanel(), panel(NULL), emusys(NULL),
      was_paused(false),
      rewind_time(0),do_rewind(false),rewind_mem(0),
      loaded(IMAGE_UNKNOWN), basic_width(GBWidth), basic_height(GBHeight),
      fullscreen(false), paused(false),
      pointer_blanked(false), mouse_active_time(0)
{
    SetSizer(new wxBoxSizer(wxVERTICAL));
    // all renderers prefer 32-bit
    // well, "simple" prefers 24-bit, but that's not available for filters
    systemColorDepth = 32;
    hq2x_init(32);
    Init_2xSaI(32);
}

void GameArea::LoadGame(const wxString &name)
{
    // fex just crashes if file does not exist and it's compressed,
    // so check first
    wxFileName fnfn(name);
    bool badfile = !fnfn.IsFileReadable();
    // if path was relative, look for it before giving up
    if(badfile && !fnfn.IsAbsolute()) {
	wxString rp = fnfn.GetPath();
	// can't really decide which dir to use, so try GBA first, then GB
	if(!gopts.gba_rom_dir.empty()) {
	    fnfn.SetPath(gopts.gba_rom_dir + wxT('/') + rp);
	    badfile = !fnfn.IsFileReadable();
	}
	if(badfile && !gopts.gb_rom_dir.empty()) {
	    fnfn.SetPath(gopts.gb_rom_dir + wxT('/') + rp);
	    badfile = !fnfn.IsFileReadable();
	}
    }
    // auto-conversion of wxCharBuffer to const char * seems broken
    // so save underlying wxCharBuffer (or create one of none is used)
    wxCharBuffer fnb(fnfn.GetFullPath().mb_fn_str());
    const char *fn = fnb.data();
    IMAGE_TYPE t = badfile ? IMAGE_UNKNOWN : utilFindType(fn);
    if(t == IMAGE_UNKNOWN) {
	wxString s;
	s.Printf(_("%s is not a valid ROM file"), name.c_str());
	wxMessageDialog dlg(GetParent(), s, _("Problem loading file"), wxOK|wxICON_ERROR);
	dlg.ShowModal();
	return;
    }
    {
	wxConfig *cfg = wxGetApp().cfg;
	if(!gopts.recent_freeze) {
	    gopts.recent->AddFileToHistory(name);
	    wxGetApp().frame->SetRecentAccels();
	    cfg->SetPath(wxT("/Recent"));
	    gopts.recent->Save(*cfg);
	    cfg->SetPath(wxT("/"));
	    cfg->Flush();
	}
    }
    UnloadGame();

    // strip extension from actual game file name
    // FIXME: save actual file name of archive so patches & cheats can
    // be loaded from archive
    // FIXME: if archive name does not match game name, prepend archive
    // name to uniquify game name
    loaded_game = fnfn;
    loaded_game.ClearExt();
    loaded_game.MakeAbsolute();

    // load patch, if enabled
    // note that it is difficult to load from archive due to
    // ../common/Patch.cpp depending on opening the file itself and doing
    // lots of seeking & such.  The only way would be to copy the patch
    // out to a temporary file and load it (and can't just use
    // AssignTempFileName because it needs correct extension)
    // too much trouble for now, though
    bool loadpatch = gopts.apply_patches;
    wxFileName pfn = loaded_game;
    if(loadpatch) {
	// SetExt may strip something off by accident, so append to text instead
	// pfn.SetExt(wxT(".ips")
	pfn.SetFullName(pfn.GetFullName() + wxT(".ips"));
	if(!pfn.IsFileReadable()) {
	    pfn.SetExt(wxT(".ups"));
	    if(!pfn.IsFileReadable()) {
		pfn.SetExt(wxT(".ppf"));
		loadpatch = pfn.IsFileReadable();
	    }
	}
    }

    if(t == IMAGE_GB) {
	if(!gbLoadRom(fn)) {
	    wxString s;
	    s.Printf(_("Unable to load Game Boy ROM %s"), name.c_str());
	    wxMessageDialog dlg(GetParent(), s, _("Problem loading file"), wxOK|wxICON_ERROR);
	    dlg.ShowModal();
	    return;
	}
	rom_size = gbRomSize;
	if(loadpatch) {
	    unsigned int size = rom_size;
	    // auto-conversion of wxCharBuffer to const char * seems broken
	    // so save underlying wxCharBuffer (or create one of none is used)
	    wxCharBuffer pfnb(pfn.GetFullPath().mb_fn_str());
	    applyPatch(pfnb.data(), &gbRom, &size);
	    if(size != rom_size)
		gbUpdateSizes();
	    rom_size = size;
	}
		   
	// start sound; this must happen before CPU stuff
	gb_effects_config.echo = (float)gopts.gb_echo / 100.0;
	gb_effects_config.stereo = (float)gopts.gb_stereo / 100.0;
	gbSoundSetDeclicking(gopts.gb_declick);
	soundInit();
	soundSetThrottle(gopts.throttle);
	soundSetEnable(gopts.sound_en);
	gbSoundSetSampleRate(!gopts.sound_qual ? 48000 :
			     44100 / (1 << (gopts.sound_qual - 1)));
	soundSetVolume((float)gopts.sound_vol / 100.0);

	gbGetHardwareType();
	bool use_bios = false;
	// auto-conversion of wxCharBuffer to const char * seems broken
	// so save underlying wxCharBuffer (or create one of none is used)
	const char *fn = NULL;
	wxCharBuffer fnb;
	if(gbCgbMode) {
	    use_bios = gopts.gbc_use_bios;
	    fnb = gopts.gbc_bios.mb_fn_str();
	} else {
	    use_bios = gopts.gb_use_bios;
	    fnb = gopts.gb_bios.mb_fn_str();
	}
	fn = fnb.data();
	gbCPUInit(fn, use_bios);
	if(use_bios && !useBios) {
	    wxLogError(_("Could not load BIOS %s"), (gbCgbMode ? gopts.gbc_bios : gopts.gb_bios).c_str());
	    // could clear use flag & file name now, but better to force
	    // user to do it
	}
	gbReset();
	if(gbBorderOn) {
	    basic_width = gbBorderLineSkip = SGBWidth;
	    basic_height = SGBHeight;
	    gbBorderColumnSkip = (SGBWidth - GBWidth) / 2;
	    gbBorderRowSkip = (SGBHeight - GBHeight) / 2;
	} else {
	    basic_width = gbBorderLineSkip = GBWidth;
	    basic_height = GBHeight;
	    gbBorderColumnSkip = gbBorderRowSkip = 0;
	}
	emusys = &GBSystem;
    } else /* if(t == IMAGE_GBA) */ {
	if(!(rom_size = CPULoadRom(fn))) {
	    wxString s;
	    s.Printf(_("Unable to load Game Boy Advance ROM %s"), name.c_str());
	    wxMessageDialog dlg(GetParent(), s, _("Problem loading file"), wxOK|wxICON_ERROR);
	    dlg.ShowModal();
	    return;
	}
	if(loadpatch) {
	    // don't use real rom size or it might try to resize rom[]
	    // instead, use known size of rom[]
	    unsigned int size = 0x2000000;
	    // auto-conversion of wxCharBuffer to const char * seems broken
	    // so save underlying wxCharBuffer (or create one of none is used)
	    wxCharBuffer pfnb(pfn.GetFullPath().mb_fn_str());
	    applyPatch(pfnb.data(), &rom, &size);
	    // that means we no longer really know rom_size either <sigh>
	}

#if 0 // disabled in win32 version for undocumented "problems"
	// FIXME: store original value
	if(gopts.skip_intro)
	    *((u32 *)rom) = 0xea00002e;
#endif
	wxFileConfig *cfg = wxGetApp().overrides;
	wxString id = wxString((const char *)&rom[0xac], wxConvLibc, 4);

	if(cfg->HasGroup(id)) {
	    cfg->SetPath(id);

	    rtcEnable(cfg->Read(wxT("rtcEnabled"), gopts.rtc));
	    int fsz = cfg->Read(wxT("flashSize"), (long)0);
	    if(fsz != 0x10000 && fsz != 0x20000)
		fsz = 0x10000 << gopts.flash_size;
	    flashSetSize(fsz);
	    cpuSaveType = cfg->Read(wxT("saveType"), gopts.save_type);
	    if(cpuSaveType < 0 || cpuSaveType > 5)
		cpuSaveType = gopts.save_type;
	    mirroringEnable = cfg->Read(wxT("mirroringEnabled"), (long)0);

	    cfg->SetPath(wxT("/"));
	} else {
	    rtcEnable(gopts.rtc);
	    flashSetSize(0x10000 << gopts.flash_size);
	    cpuSaveType = gopts.save_type;
	    // mirroring short ROMs is such an uncommon thing that any
	    // carts needing it should be added to vba-over.ini.
	    // on the other hand, I would see nothing wrong with enabling
	    // by default on carts that are small enough (i.e., always
	    // set this to true and ignore vba-over.ini).  It's just a one-time
	    // init.
	    mirroringEnable = false;
	}
	doMirroring(mirroringEnable);

	// start sound; this must happen before CPU stuff
	soundInit();
	soundSetThrottle(gopts.throttle);
	soundSetEnable(gopts.sound_en);
	soundSetSampleRate(!gopts.sound_qual ? 48000 :
			   44100 / (1 << (gopts.sound_qual - 1)));
	soundSetVolume((float)gopts.sound_vol / 100.0);

	CPUInit(gopts.gba_bios.mb_fn_str(), gopts.gba_use_bios);
	if(gopts.gba_use_bios && !useBios) {
	    wxLogError(_("Could not load BIOS %s"), gopts.gba_bios.c_str());
	    // could clear use flag & file name now, but better to force
	    // user to do it
	}
	CPUReset();
	basic_width = GBAWidth;
	basic_height = GBAHeight;
	emusys = &GBASystem;
    }

    loaded = t;

    SetFrameTitle();
    SetFocus();
    AdjustSize(true);

    emulating = true;
    was_paused = true;
    MainFrame *mf = wxGetApp().frame;
    mf->SetJoystick();
    mf->cmd_enable &= ~(CMDEN_GB|CMDEN_GBA);
    mf->cmd_enable |= ONLOAD_CMDEN;
    mf->cmd_enable |= loaded == IMAGE_GB ? CMDEN_GB : (CMDEN_GBA | CMDEN_NGDB_GBA);
    mf->enable_menus();

    // probably only need to do this for GB carts
    if(gopts.gbprint)
	gbSerialFunction = gbPrinterSend;
    else
	gbSerialFunction = NULL;

    // probably only need to do this for GBA carts
    agbPrintEnable(gopts.agbprint);

    // set frame skip based on ROM type
    systemFrameSkip = loaded == IMAGE_GB ? gopts.gb_frameskip : gopts.gba_frameskip;
    if(systemFrameSkip < 0)
	systemFrameSkip = 0;

    // load battery and/or saved state
    recompute_dirs();
    mf->update_state_ts(true);

    bool did_autoload = gopts.autoload_state ? LoadState() : false;
    if(!did_autoload || skipSaveGameBattery) {
	wxString bname = loaded_game.GetFullName();
#ifndef NO_LINK
	// MakeInstanceFilename doesn't do wxString, so just add slave ID here
	if(vbaid) {
	    bname.append(wxT('-'));
	    bname.append(wxChar(wxT('1') + vbaid));
	}
#endif
	bname.append(wxT(".sav"));
	wxFileName bat(batdir, bname);
	fnb = bat.GetFullPath().mb_fn_str();
	if(emusys->emuReadBattery(fnb.data())) {
	    wxString msg;
	    msg.Printf(_("Loaded battery %s"), bat.GetFullPath().c_str());
	    systemScreenMessage(msg);
	}
	// forget old save writes
	systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;
    }
    // do an immediate rewind save
    // even if loaded from state file: not smart enough yet to just
    // do a reset or load from state file when # rewinds == 0
    do_rewind = gopts.rewind_interval > 0;

    // FIXME: backup battery file (useful if game name conflict)

    cheats_dirty = (did_autoload && !skipSaveGameCheats) ||
	(loaded == IMAGE_GB ? gbCheatNumber > 0 : cheatsNumber > 0);
    if(gopts.autoload_cheats && (!did_autoload || skipSaveGameCheats)) {
	wxFileName cfn = loaded_game;
	// SetExt may strip something off by accident, so append to text instead
	cfn.SetFullName(cfn.GetFullName() + wxT(".clt"));
	if(cfn.IsFileReadable()) {
	    bool cld;
	    if(loaded == IMAGE_GB)
		cld = gbCheatsLoadCheatList(cfn.GetFullPath().mb_fn_str());
	    else
		cld = cheatsLoadCheatList(cfn.GetFullPath().mb_fn_str());
	    if(cld) {
		systemScreenMessage(_("Loaded cheats"));
		cheats_dirty = false;
	    }
	}
    }
}

void GameArea::SetFrameTitle()
{
    wxString tit;
    if(loaded != IMAGE_UNKNOWN) {
	tit = wxT("VBA-M ");
	tit.append(loaded_game.GetFullName());
    } else
	tit = wxT("VisualBoyAdvance-M " VERSION);
#ifndef NO_LINK
    if(vbaid > 0 || linkid > 0) {
	tit.append(_(" player "));
	tit.append(wxChar(wxT('1') + (linkid > 0 ? linkid : vbaid)));
    }
#endif
    wxGetApp().frame->SetTitle(tit);
}

void GameArea::recompute_dirs()
{
    batdir = gopts.battery_dir;
    if(!batdir.size())
	batdir = wxStandardPaths::Get().GetUserLocalDataDir();
    else {
	wxFileName bp(batdir, wxEmptyString);
	if(!bp.IsAbsolute())
	    batdir = loaded_game.GetPathWithSep() + batdir;
    }
    statedir = gopts.state_dir;
    if(!statedir.size())
	statedir = batdir;
    else {
	wxFileName sp(statedir, wxEmptyString);
	if(!sp.IsAbsolute())
	    statedir = batdir + wxT('/') + statedir;
    }
}

void GameArea::UnloadGame(bool destruct)
{
    if(!emulating)
	return;

    // last opportunity to autosave cheats
    if(gopts.autoload_cheats && cheats_dirty) {
	wxFileName cfn = loaded_game;
	// SetExt may strip something off by accident, so append to text instead
	cfn.SetFullName(cfn.GetFullName() + wxT(".clt"));
	if(loaded == IMAGE_GB) {
	    if(!gbCheatNumber)
		wxRemoveFile(cfn.GetFullPath());
	    else
		gbCheatsSaveCheatList(cfn.GetFullPath().mb_fn_str());
	} else {
	    if(!cheatsNumber)
		wxRemoveFile(cfn.GetFullPath());
	    else
		cheatsSaveCheatList(cfn.GetFullPath().mb_fn_str());
	}
    }

    // if timer was counting down for save, go ahead and save
    // this might not be safe, though..
    if(systemSaveUpdateCounter > SYSTEM_SAVE_NOT_UPDATED) {
	SaveBattery(destruct);
    }

    MainFrame *mf = wxGetApp().frame;

#ifndef NO_FFMPEG
    snd_rec.Stop();
    vid_rec.Stop();
#endif
    systemStopGameRecording();
    systemStopGamePlayback();

    debugger = false;
    remoteCleanUp();
    mf->cmd_enable |= CMDEN_NGDB_ANY;

    if(loaded == IMAGE_GB) {
	gbCleanUp();
	gbCheatRemoveAll();
    } else if(loaded == IMAGE_GBA) {
	CPUCleanUp();
	cheatsDeleteAll(false);
    }

    emulating = false;
    loaded = IMAGE_UNKNOWN;
    emusys = NULL;
    soundShutdown();

    if(destruct)
	return;

    // in destructor, panel should be auto-deleted by wx since all panels
    // are derived from a window attached as child to GameArea
    if(panel)
	panel->Delete();
    panel = NULL;

    // close any game-related viewer windows
    // in destructor, viewer windows are in process of being deleted anyway
    while(!mf->popups.empty())
	mf->popups.front()->Close(true);

    // remaining items are GUI updates that should not be needed in destructor
    SetFrameTitle();
    mf->cmd_enable &= UNLOAD_CMDEN_KEEP;
    mf->update_state_ts(true);
    mf->enable_menus();
    mf->SetJoystick();
    mf->ResetCheatSearch();

    if(rewind_mem)
	num_rewind_states = 0;
}

bool GameArea::LoadState()
{
    int slot = wxGetApp().frame->newest_state_slot();
    if(slot < 1)
	return false;
    return LoadState(slot);
}

bool GameArea::LoadState(int slot)
{
    wxString fname;
    fname.Printf(SAVESLOT_FMT, game_name().c_str(), slot);
    return LoadState(wxFileName(statedir, fname));
}

bool GameArea::LoadState(const wxFileName &fname)
{
    // FIXME: first save to backup state if not backup state
    bool ret = emusys->emuReadState(fname.GetFullPath().mb_fn_str());
    if(ret && num_rewind_states) {
	MainFrame *mf = wxGetApp().frame;
	mf->cmd_enable &= ~CMDEN_REWIND;
	mf->enable_menus();
	num_rewind_states = 0;
	// do an immediate rewind save
	// even if loaded from state file: not smart enough yet to just
	// do a reset or load from state file when # rewinds == 0
	do_rewind = true;
	rewind_time = gopts.rewind_interval * 6;
    }
    if(ret) {
	// forget old save writes
	systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;
	// frame rate calc should probably reset as well
	was_paused = true;
	// save state had a screen frame, so draw it
	systemDrawScreen();
    }
    wxString msg;
    msg.Printf(ret ? _("Loaded state %s") : _("Error loading state %s"),
	       fname.GetFullPath().c_str());
    systemScreenMessage(msg);
    return ret;
}

bool GameArea::SaveState()
{
    return SaveState(wxGetApp().frame->oldest_state_slot());
}

bool GameArea::SaveState(int slot)
{
    wxString fname;
    fname.Printf(SAVESLOT_FMT, game_name().c_str(), slot);
    return SaveState(wxFileName(statedir, fname));
}

bool GameArea::SaveState(const wxFileName &fname)
{
    // FIXME: first copy to backup state if not backup state
    bool ret = emusys->emuWriteState(fname.GetFullPath().mb_fn_str());
    wxGetApp().frame->update_state_ts(true);
    wxString msg;
    msg.Printf(ret ? _("Saved state %s") : _("Error saving state %s"),
	       fname.GetFullPath().c_str());
    systemScreenMessage(msg);
    return ret;
}

void GameArea::SaveBattery(bool quiet)
{
    // MakeInstanceFilename doesn't do wxString, so just add slave ID here
    wxString bname = game_name();
#ifndef NO_LINK
    if(vbaid) {
	bname.append(wxT('-'));
	bname.append(wxChar(wxT('1') + vbaid));
    }
#endif
    bname.append(wxT(".sav"));
    wxFileName bat(batdir, bname);
    bat.Mkdir(0777, wxPATH_MKDIR_FULL);
    wxString fn = bat.GetFullPath();
    // auto-conversion of wxCharBuffer to const char * seems broken
    // so save underlying wxCharBuffer (or create one of none is used)
    wxCharBuffer fnb = fn.mb_fn_str();
    wxString msg;
    // FIXME: add option to support ring of backups
    // of course some games just write battery way too often for such
    // a thing to be useful
    if(emusys->emuWriteBattery(fnb.data()))
	msg.Printf(_("Wrote battery %s"), fn.c_str());
    else
	msg.Printf(_("Error writing battery %s"), fn.c_str());
    systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;
    if(!quiet)
	systemScreenMessage(msg);
}

void GameArea::AddBorder()
{
    if(basic_width != GBWidth)
	return;
    basic_width = SGBWidth;
    basic_height = SGBHeight;
    gbBorderLineSkip = SGBWidth;
    gbBorderColumnSkip = (SGBWidth - GBWidth) / 2;
    gbBorderRowSkip = (SGBHeight - GBHeight) / 2;
    AdjustSize(false);
    wxGetApp().frame->Fit();
    GetSizer()->Detach(panel->GetWindow());
    if(panel)
	panel->Delete();
    panel = NULL;
}

void GameArea::DelBorder()
{
    if(basic_width != SGBWidth)
	return;
    basic_width = GBWidth;
    basic_height = GBHeight;
    gbBorderLineSkip = GBWidth;
    gbBorderColumnSkip = gbBorderRowSkip = 0;
    AdjustSize(false);
    wxGetApp().frame->Fit();
    GetSizer()->Detach(panel->GetWindow());
    if(panel)
	panel->Delete();
    panel = NULL;
}

void GameArea::AdjustMinSize()
{
    wxWindow *frame = wxGetApp().frame;
    // note: could safely set min size to 1x or less regardless of video_scale
    // but setting it to scaled size makes resizing to default easier
    wxSize sz(basic_width * gopts.video_scale, basic_height * gopts.video_scale);
    SetMinSize(sz);
#if wxCHECK_VERSION(2,8,8)
    sz = frame->ClientToWindowSize(sz);
#else
    sz += frame->GetSize() - frame->GetClientSize();
#endif
    frame->SetMinSize(sz);
}

void GameArea::LowerMinSize()
{
    wxWindow *frame = wxGetApp().frame;
    wxSize sz(basic_width , basic_height);
    SetMinSize(sz);
    // do not take decorations into account
    frame->SetMinSize(sz);
}

void GameArea::AdjustSize(bool force)
{
    AdjustMinSize();
    if(fullscreen)
	return;
    const wxSize newsz(basic_width * gopts.video_scale, basic_height * gopts.video_scale);
    if(!force) {
	wxSize sz = GetSize();
	if(sz.GetWidth() >= newsz.GetWidth() && sz.GetHeight() >= newsz.GetHeight())
	    return;
    }
    SetSize(newsz);
    GetParent()->SetClientSize(newsz);
    wxGetApp().frame->Fit();
}

void GameArea::ShowFullScreen(bool full)
{
    if(full == fullscreen) {
	// in case the tlw somehow lost its mind, force it to proper mode
	if(wxGetApp().frame->IsFullScreen() != fullscreen)
	    wxGetApp().frame->ShowFullScreen(full);
	return;
    }
    fullscreen = full;

    // just in case screen mode is going to change, go ahead and preemptively
    // delete panel to be recreated immediately after resize
    if(panel) {
	panel->Delete();
	panel = NULL;
    }

    // Windows does not restore old window size/pos
    // at least under Wine
    // so store them before entering fullscreen
    static bool cursz_valid = false;
    static wxSize cursz;
    static wxPoint curpos;
    MainFrame *tlw = wxGetApp().frame;
    int dno = wxDisplay::GetFromWindow(tlw);
    if(!full) {
	if(gopts.fs_mode.w && gopts.fs_mode.h) {
	    wxDisplay d(dno);
	    d.ChangeMode(wxDefaultVideoMode);
	}
	tlw->ShowFullScreen(false);
	if(!cursz_valid) {
	    curpos = wxDefaultPosition;
	    cursz = tlw->GetMinSize();
	}
	tlw->SetSize(cursz);
	tlw->SetPosition(curpos);
	AdjustMinSize();
    } else {
	// close all non-modal dialogs
	while(!tlw->popups.empty())
	    tlw->popups.front()->Close();
	// mouse stays blank whenever full-screen
	HidePointer();
	cursz_valid = true;
	cursz = tlw->GetSize();
	curpos = tlw->GetPosition();
	LowerMinSize();
	if(gopts.fs_mode.w && gopts.fs_mode.h) {
	    // grglflargm
	    // stupid wx does not do fullscreen properly when video mode is
	    // changed under X11.   Since it does not use xrandr to switch, it
	    // just changes the video mode without resizing the desktop.  It
	    // still uses the original desktop size for fullscreen, though.
	    // It also seems to stick the top-left corner wherever it pleases,
	    // so I can't just resize the panel and expect it to show up.
	    // ^(*&^^&!!!!  maybe just disable this code altogether on UNIX
	    // most 3d cards are fine with full screen size, anyway.
	    wxDisplay d(dno);

	    if(!d.ChangeMode(gopts.fs_mode)) {
		wxLogInfo(_("Fullscreen mode %dx%d-%d@%d not supported; looking for another"),
			  gopts.fs_mode.w, gopts.fs_mode.h, gopts.fs_mode.bpp, gopts.fs_mode.refresh);
		// specifying a mode may not work with bpp/rate of 0
		// in particular, unix does Matches() in wrong direction
		wxArrayVideoModes vm = d.GetModes();
		int best_mode = -1;
		unsigned int i = 0;
		for(; i < vm.size(); i++) {
		    if(vm[i].w != gopts.fs_mode.w || vm[i].h != gopts.fs_mode.h)
			continue;
		    int bpp = vm[i].bpp;
		    if(gopts.fs_mode.bpp && bpp == gopts.fs_mode.bpp)
			break;
		    if(!gopts.fs_mode.bpp && bpp == 32) {
			gopts.fs_mode.bpp = 32;
			break;
		    }
		    int bm_bpp = best_mode < 0 ? 0 : vm[i].bpp;
		    if(bpp == 32 && bm_bpp != 32)
			best_mode = i;
		    else if(bpp == 24 && bm_bpp < 24)
			best_mode = i;
		    else if(bpp == 16 && bm_bpp < 24 && bm_bpp != 16)
			best_mode = i;
		    else if(!best_mode)
			best_mode = i;
		}
		if(i == vm.size() && best_mode >= 0)
		    i = best_mode;
		if(i == vm.size()) {
		    wxLogWarning(_("Fullscreen mode %dx%d-%d@%d not supported"),
				 gopts.fs_mode.w, gopts.fs_mode.h, gopts.fs_mode.bpp, gopts.fs_mode.refresh);
		    gopts.fs_mode = wxVideoMode();
		    for(i = 0; i < vm.size(); i++)
			wxLogInfo(_("Valid mode: %dx%d-%d@%d"), vm[i].w,
				  vm[i].h, vm[i].bpp,
				  vm[i].refresh);
		} else {
		    gopts.fs_mode.bpp = vm[i].bpp;
		    gopts.fs_mode.refresh = vm[i].refresh;
		}
		wxLogInfo(_("Chose mode %dx%d-%d@%d"),
			  gopts.fs_mode.w, gopts.fs_mode.h, gopts.fs_mode.bpp, gopts.fs_mode.refresh);
		if(!d.ChangeMode(gopts.fs_mode)) {
		   wxLogWarning(_("Failed to change mode to %dx%d-%d@%d"),
				gopts.fs_mode.w, gopts.fs_mode.h, gopts.fs_mode.bpp, gopts.fs_mode.refresh);
		    gopts.fs_mode.w = 0;
		}
	    }
	}
	tlw->ShowFullScreen(true);
    }
}

GameArea::~GameArea()
{
    UnloadGame(true);
    if(rewind_mem)
	free(rewind_mem);
    if(gopts.fs_mode.w && gopts.fs_mode.h && fullscreen) {
	MainFrame *tlw = wxGetApp().frame;
	int dno = wxDisplay::GetFromWindow(tlw);
	wxDisplay d(dno);
	d.ChangeMode(wxDefaultVideoMode);
    }
}

void GameArea::Pause()
{
    if(paused)
	return;
    paused = was_paused = true;
    if(loaded != IMAGE_UNKNOWN)
	soundPause();
}

void GameArea::Resume()
{
    if(!paused)
	return;
    paused = false;
    SetExtraStyle(GetExtraStyle() | wxWS_EX_PROCESS_IDLE);
    if(loaded != IMAGE_UNKNOWN)
	soundResume();
    SetFocus();
}

void GameArea::OnIdle(wxIdleEvent &event)
{
    wxString pl = wxGetApp().pending_load;
    if(pl.size()) {
	// sometimes this gets into a loop if LoadGame() called before
	// clearing pending_load.  weird.
	wxGetApp().pending_load = wxEmptyString;
	LoadGame(pl);
	if(debugger && loaded != IMAGE_GBA) {
	    wxLogError(_("Not a valid GBA cartridge"));
	    UnloadGame();
	}
    }
    // stupid wx doesn't resize to screen size
    // forcing it this way just puts it in an infinite loop, though
    // with wx trying to resize/reposition to what it thinks is full screen
    // every time it detects a manual resize like this
    if(gopts.fs_mode.w && gopts.fs_mode.h && fullscreen) {
	wxSize sz = GetSize();
	if(sz.GetWidth() != gopts.fs_mode.w || sz.GetHeight() != gopts.fs_mode.h) {
	    wxGetApp().frame->Move(0,84);
	    SetSize(gopts.fs_mode.w, gopts.fs_mode.h);
	}
    }
    if(!emusys)
	return;
    if(!panel) {
	switch(gopts.render_method) {
	case RND_SIMPLE:
	    panel = new BasicDrawingPanel(this, basic_width, basic_height);
	    break;
#ifndef NO_OGL
	case RND_OPENGL:
	    panel = new GLDrawingPanel(this, basic_width, basic_height);
	    break;
#endif
#ifndef NO_CAIRO
	case RND_CAIRO:
	    panel = new CairoDrawingPanel(this, basic_width, basic_height);
	    break;
#endif
#ifdef __WXMSW__
	case RND_DIRECT3D:
	    panel = new DXDrawingPanel(this, basic_width, basic_height);
	    break;
#endif
	}
	wxWindow *w = panel->GetWindow();
	w->SetBackgroundStyle(wxBG_STYLE_CUSTOM);
	w->Enable(false); // never give it the keyboard focus
	w->SetSize(wxSize(basic_width, basic_height));
	if(gopts.max_scale)
	    w->SetMaxSize(wxSize(basic_width * gopts.max_scale,
				 basic_height * gopts.max_scale));
	GetSizer()->Add(w, 1, gopts.retain_aspect ?
			(wxSHAPED|wxALIGN_CENTER) : wxEXPAND);
	Layout();
	if(pointer_blanked)
	    w->SetCursor(wxCursor(wxCURSOR_BLANK));
    }
    if(!paused && (!gopts.defocus_pause || wxGetApp().frame->HasFocus())) {
	HidePointer();
	event.RequestMore();
	if(debugger) {
	    was_paused = true;
	    dbgMain();
	    if(!emulating) {
		emulating = true;
		UnloadGame();
	    }
	    return;
	}
	emusys->emuMain(emusys->emuCount);
#ifndef NO_LINK
	if(loaded == IMAGE_GBA && lanlink.connected && linkid && lc.numtransfers == 0)
	    lc.CheckConn();
#endif
    } else {
	was_paused = true;
	if(paused)
	    SetExtraStyle(GetExtraStyle() & ~wxWS_EX_PROCESS_IDLE);
    }
    if(do_rewind && emusys->emuWriteMemState) {
	if(!rewind_mem) {
	    rewind_mem = (char *)malloc(NUM_REWINDS * REWIND_SIZE);
	    num_rewind_states = next_rewind_state = 0;
	}
	if(!rewind_mem) {
	    wxLogError(_("No memory for rewinding"));
	    wxGetApp().frame->Close(true);
	    return;
	}
	if(!emusys->emuWriteMemState(&rewind_mem[REWIND_SIZE * next_rewind_state],
				     REWIND_SIZE))
	    // if you see a lot of these, maybe increase REWIND_SIZE
	    wxLogInfo(_("Error writing rewind state"));
	else {
	    if(!num_rewind_states) {
		MainFrame *mf = wxGetApp().frame;
		mf->cmd_enable |= CMDEN_REWIND;
		mf->enable_menus();
	    }
	    if(num_rewind_states < NUM_REWINDS)
		++num_rewind_states;
	    next_rewind_state = (next_rewind_state + 1) % NUM_REWINDS;
	}
	do_rewind = false;
    }
}

// Note: keys will get stuck if they are released while window has no focus
// can't really do anything about it, except scan for pressed keys on
// activate events.  Maybe later.

static uint32_t bmask[NUM_KEYS] = {
    KEYM_UP, KEYM_DOWN, KEYM_LEFT, KEYM_RIGHT, KEYM_A, KEYM_B, KEYM_L, KEYM_R,
    KEYM_SELECT, KEYM_START, KEYM_MOTION_UP, KEYM_MOTION_DOWN, KEYM_MOTION_LEFT,
    KEYM_MOTION_RIGHT, KEYM_AUTO_A, KEYM_AUTO_B, KEYM_SPEED, KEYM_CAPTURE,
    KEYM_GS
};

static wxJoyKeyBinding_v keys_pressed;

static void process_key_press(bool down, int key, int mod, int joy = 0)
{
    // check if key is already pressed
    unsigned int kpno;
    for(kpno = 0; kpno < keys_pressed.size(); kpno++)
	if(keys_pressed[kpno].key == key && keys_pressed[kpno].mod == mod &&
	   keys_pressed[kpno].joy == joy)
	    break;
    if(kpno < keys_pressed.size()) {
	// double press is noop
	if(down)
	    return;
	// otherwise forget it
	keys_pressed.erase(keys_pressed.begin() + kpno);
    } else {
	// double release is noop
	if(!down)
	    return;
	// otherwise remember it
	// c++0x
	// keys_pressed.push_back({ key, mod, joy });
	wxJoyKeyBinding jb = { key, mod, joy };
	keys_pressed.push_back(jb);
    }
    // find all game keys this is bound to
    for(int i = 0; i < 4; i++)
	for(int j = 0; j < NUM_KEYS; j++) {
	    wxJoyKeyBinding_v &b = gopts.joykey_bindings[i][j];
	    for(unsigned int k = 0; k < b.size(); k++)
		if(b[k].key == key && b[k].mod == mod && b[k].joy == joy) {
		    if(down)
			joypress[i] |= bmask[j];
		    else {
			// only release if no others pressed
			unsigned int k2;
			for(k2 = 0; k2 < b.size(); k2++) {
			    if(k == k2 || (b[k2].key == key && b[k2].mod == mod &&
					   b[k2].joy == joy))
				continue;
			    for(kpno = 0; kpno < keys_pressed.size(); kpno++)
				if(keys_pressed[kpno].key == b[k2].key &&
				   keys_pressed[kpno].mod == b[k2].mod &&
				   keys_pressed[kpno].joy == b[k2].joy)
				    break;
			    if(kpno < keys_pressed.size())
				break;
			}
			if(k2 == b.size())
			    joypress[i] &= ~bmask[j];
		    }
		    break;
		}
	}
}

void GameArea::OnKeyDown(wxKeyEvent &ev)
{
    process_key_press(true, ev.GetKeyCode(), 0 /* ev.GetModifiers() */);
    ev.Skip();  // process accelerators
}

void GameArea::OnKeyUp(wxKeyEvent &ev)
{
    process_key_press(false, ev.GetKeyCode(), 0 /* ev.GetModifiers() */);
    ev.Skip();  // process accelerators
}

void GameArea::OnSDLJoy(wxSDLJoyEvent &ev)
{
    int key = ev.GetControlIndex();
    int mod = wxJoyKeyTextCtrl::DigitalButton(ev);
    int joy = ev.GetJoy() + 1;
    // mutually exclusive key types unpress their opposite
    if(mod == WXJB_AXIS_PLUS)
	process_key_press(false, key, WXJB_AXIS_MINUS, joy);
    else if(mod == WXJB_AXIS_MINUS)
	process_key_press(false, key, WXJB_AXIS_PLUS, joy);
    else if(mod >= WXJB_HAT_FIRST && mod <= WXJB_HAT_LAST) {
	for(int i = WXJB_HAT_FIRST; i < WXJB_HAT_LAST; i++)
	    if(i != mod)
		process_key_press(false, key, i, joy);
    }

    process_key_press(ev.GetControlValue() != 0, key, mod, joy);
}

BEGIN_EVENT_TABLE(GameArea, wxPanel)
    EVT_IDLE(GameArea::OnIdle)
    EVT_SDLJOY(GameArea::OnSDLJoy)
    EVT_KEY_DOWN(GameArea::OnKeyDown)
    EVT_KEY_UP(GameArea::OnKeyUp)
    // FIXME: wxGTK does not generate motion events in MainFrame (not sure
    // what to do about it)
    EVT_MOUSE_EVENTS(GameArea::MouseEvent)
END_EVENT_TABLE()

IMPLEMENT_ABSTRACT_CLASS(DrawingPanel, wxEvtHandler)

void DrawingPanel::PaintEv(wxPaintEvent &ev)
{
    wxPaintDC dc(GetWindow());
    DrawArea(dc);
}

// In order to run filters in parallel, they have to run from the method of
// a wxThread-derived class
//   threads are only created once, if possible, to avoid thread creation
//   overhead every frame; mutex+cond used to signal start and sem for end

//   when threading, there _will_ be bands for any nontrivial filter
//   usually the top and bottom line(s) of each region will look a little
//   different.  To ease this a tiny bit, 2 extra lines are generated at
//   the top of each region, so hopefully only the bottom of the previous
//   region will look screwed up.  The only correct way to handle this
//   is to draw an increasingly larger band around the seam until the
//   seam cover matches a line in both top & bottom regions, and then apply
//   cover to seam.   Way too much trouble for this, though.

//   another issue to consider is whether or not these filters are thread-
//   safe to begin with.  The built-in ones are verifyable (I didn't verify
//   them, though).  The plugins cannot be verified.  However, unlike the MFC
//   interface, I will allow them to be threaded at user's discretion.
class FilterThread : public wxThread
{
private:
    // largest buffer required is 32-bit * (max width + 1) * (max height + 2)
    // No scaling factor is needed, since it is taking output from the ifb filter
    u32 buffer[257 * 226 ];
public:
    FilterThread() : wxThread(wxTHREAD_JOINABLE), lock(), sig(lock) {
    }

    //Cleanup on exit
    ~FilterThread()
    {
        if(iFilter)
        {
            delete iFilter;
            iFilter = NULL;
        }
        if(mainFilter)
        {
            delete mainFilter;
            mainFilter = NULL;
        }
    }

    wxMutex lock;
    wxCondition sig;
    wxSemaphore *done;

    // Set these params before running
    unsigned int nthreads, threadno;
    unsigned int width, height, scale;
    u32 *dst;
    filter_base * mainFilter;
    filter_base * iFilter;

    // set this param every round
    // if NULL, end thread
    u32 *src;

    ExitCode Entry()
    {
    scale = mainFilter->getScale();

	// This is the lower height value of the band this thread will process
	int band_lower = height * threadno;

    //Set the starting location for the destination buffer
    dst += width * scale * band_lower * scale;

	while(sig.Wait() == wxCOND_NO_ERROR) {
        //If no source, do thread cleanup before exiting
	    if(!src ) {
            lock.Unlock();
            return (wxThread::ExitCode) 0;
	    }

	    if(!mainFilter || !iFilter)
        {
            std::runtime_error("ERROR:  Filter not initialized!");
            return (wxThread::ExitCode) -1;
        }

        //Set the start of the source pointer to the first pixel of the appropriate height
        src += width * band_lower;

        //Run the interframe blending filter
        iFilter->run(src,buffer);

	    // naturally, any of these with accumulation buffers like those of
	    // the IFB filters will screw up royally as well
        mainFilter->run(buffer, dst);

        done->Post();
	}
	return (wxThread::ExitCode) 0;
    }
};

DrawingPanel::DrawingPanel(int _width, int _height) :
    wxObject(), width(_width+(systemColorDepth != 24)), height(_height), scale(1),
    isFiltered(false),nthreads(gopts.max_threads)
{

    // Create and start up new threads
    if(nthreads) {
        //The filter is run with each thread handling bands of data
        //This is how tall each of those bands are
        unsigned int band_height = height/nthreads;
        //Create and initialize the threads
        threads = new FilterThread[nthreads];
        for(int i = 0; i < nthreads; i++) {
            threads[i].threadno = i;
            threads[i].nthreads = nthreads;
            threads[i].width = width;
            threads[i].height = band_height;
            threads[i].dst = reinterpret_cast<u32 *>(&todraw);
            threads[i].mainFilter=filter_factory::createFilter(ToString(gopts.filter),width,band_height);
            threads[i].iFilter=interframe_factory::createIFB((ifbfunc)gopts.ifb,width,band_height);
            threads[i].done = &filt_done;
            threads[i].lock.Lock();
            threads[i].Create();
            threads[i].Run();
        }
        //Set some important variables
        scale=threads[0].scale;
        isFiltered = threads[0].mainFilter->exists() || threads[0].iFilter->exists();
    }

    std::cerr << "width: " << width << " Height:  " << height << std::endl;

    systemColorDepth = 32;

    // FIXME: should be "true" for GBA carts if lcd mode selected
    // which means this needs to be re-run at pref change time
    utilUpdateSystemColorMaps(false);
}

void DrawingPanel::DrawArea(u8 **data)
{
    // double-buffer buffer:
    //   if filtering, this is filter output, retained for redraws
    //   if not filtering, we still retain current image for redraws

    //The number of bytes per pixel, as determined by the systemColorDepth
    int bytes_per_pixel = systemColorDepth/8;
    //Used for determining size of the buffer to allocate
    int horiz_bytes_out = width * bytes_per_pixel * scale;

    // First, apply filters, if applicable, in parallel, if enabled
    if(nthreads)
    {
        for(int i = 0; i < nthreads; i++) {
            threads[i].lock.Lock();
            threads[i].src = reinterpret_cast<u32 *>(*data);
            threads[i].sig.Signal();
            threads[i].lock.Unlock();
        }
        for(int i = 0; i < nthreads; i++)
            filt_done.Wait();
    }
    else
    {
        //If no filter to copy data to output buffer, do it ourselves
        memcpy(todraw,*data, horiz_bytes_out*height*scale);
    }

    // draw OSD text old-style (directly into output buffer)
	GameArea *panel = wxGetApp().frame->GetPanel();
	if(panel->osdstat.size())
	    drawText(todraw,width*scale,bytes_per_pixel,
		     0, 2, panel->osdstat.utf8_str(), gopts.osd_transparent);
	if(!gopts.no_osd_status && !panel->osdtext.empty()) {
	    if(systemGetClock() - panel->osdtime < OSD_TIME) {
            std::string message = ToString(panel->osdtext);
            drawText(todraw,width*scale,bytes_per_pixel,
            0,height/2,
            message.c_str(),gopts.osd_transparent);
	    } else
		panel->osdtext.clear();
	}

    // next, draw the game
    wxClientDC dc(GetWindow());
    DrawArea(dc);

}

DrawingPanel::~DrawingPanel()
{
    //Kill and delete all threads
    if(nthreads) {
        for(int i = 0; i < nthreads; i++) {
            threads[i].lock.Lock();
            threads[i].src = NULL;
            threads[i].sig.Signal();
            threads[i].lock.Unlock();
            threads[i].Wait();
        }
        delete[] threads;
    }
}

IMPLEMENT_CLASS2(BasicDrawingPanel, DrawingPanel, wxPanel)

BEGIN_EVENT_TABLE(BasicDrawingPanel, wxPanel)
    EVT_PAINT(BasicDrawingPanel::PaintEv2)
END_EVENT_TABLE()

BasicDrawingPanel::BasicDrawingPanel(wxWindow *parent, int _width, int _height)
     : DrawingPanel(_width, _height),
        wxPanel(parent, wxID_ANY, wxPoint(0, 0), parent->GetSize(), wxFULL_REPAINT_ON_RESIZE)
{
    // wxImage is 24-bit RGB, so 24-bit is preferred.  Filters require 32, though
    if(!isFiltered)
    {
        // changing from 32 to 24 does not require regenerating color tables
        systemColorDepth = 24;
    }
}

void BasicDrawingPanel::DrawArea(wxWindowDC &dc)
{
    wxBitmap *bm;
    if(systemColorDepth == 24) {
	// never scaled, no borders, no transformations needed
	wxImage im(width, height, todraw, true);
	bm = new wxBitmap(im);
    }
    else
    { // 32-bit
	// scaled by filters, top/right borders, transform to 24-bit
	wxImage im(width * scale, height * scale, false);
    convert32To24((u32 *)todraw,(u8 *)im.GetData(),width * scale,height * scale);
    bm = new wxBitmap(im);
    }
    double sx, sy;
    int w, h;
    GetClientSize(&w, &h);
    sx =(double)w / (double)(width * scale);
    sy = (double)h / (double)(height * scale);
    dc.SetUserScale(sx, sy);
    dc.DrawBitmap(*bm, 0, 0);
    delete bm;
}

#ifndef NO_OGL
#include <GL/glext.h> // for 16-bit texture data formats on win32
// following 3 for vsync
#ifdef __WXMAC__
#include <OpenGL/OpenGL.h>
#endif
#ifdef __WXGTK__  // should actually check for X11, but GTK implies X11
#include <GL/glx.h>
#endif
#ifdef __WXMSW__
#include <GL/wglext.h>
#endif

IMPLEMENT_CLASS2(GLDrawingPanel, DrawingPanel, wxGLCanvas)

// this would be easier in 2.9
BEGIN_EVENT_TABLE(GLDrawingPanel, wxGLCanvas)
    EVT_PAINT(GLDrawingPanel::PaintEv2)
    EVT_SIZE(GLDrawingPanel::OnSize)
END_EVENT_TABLE()

// This is supposed to be the default, but DOUBLEBUFFER doesn't seem to be
// turned on by default for wxGTK.
static int glopts[] = {
    WX_GL_RGBA, WX_GL_DOUBLEBUFFER, 0
};

#if wxCHECK_VERSION(2,9,0) || !defined(__WXMAC__)
#define glc wxGLCanvas
#else
// shuffled parms for 2.9 indicates non-auto glcontext
// before 2.9, wxMAC does not have this (but wxGTK & wxMSW do)
#define glc(a,b,c,d,e,f) wxGLCanvas(a, b, d, e, f, wxEmptyString, c)
#endif

GLDrawingPanel::GLDrawingPanel(wxWindow *parent, int _width, int _height) :
    DrawingPanel(_width, _height),
    glc(parent, wxID_ANY, glopts, wxPoint(0, 0), parent->GetSize(), wxFULL_REPAINT_ON_RESIZE),
#if wxCHECK_VERSION(2,9,0) || !defined(__WXMAC__)
	ctx(this),
#endif
    did_init(false)

{
}

GLDrawingPanel::~GLDrawingPanel()
{
#if 0
    // this should be automatically deleted w/ context
    // it's also unsafe if panel no longer displayed
    if(did_init) {
#if wxCHECK_VERSION(2,9,0) || !defined(__WXMAC__)
	SetContext(ctx);
#else
	SetContext();
#endif
	glDeleteLists(vlist, 1);
	glDeleteTextures(1, &texid);
    }
#endif
}

void GLDrawingPanel::Init()
{
    // taken from GTK front end almost verbatim
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, 1.0, 1.0, 0.0, 0.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    vlist = glGenLists(1);
    glNewList(vlist, GL_COMPILE);
    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(0.0, 0.0);
    glVertex3i(0, 0, 0);
    glTexCoord2f(1.0, 0.0);
    glVertex3i(1, 0, 0);
    glTexCoord2f(0.0, 1.0);
    glVertex3i(0, 1, 0);
    glTexCoord2f(1.0, 1.0);
    glVertex3i(1, 1, 0);
    glEnd();
    glEndList();

    glGenTextures(1, &texid);
    glBindTexture(GL_TEXTURE_2D, texid);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
		    gopts.bilinear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		    gopts.bilinear ? GL_LINEAR : GL_NEAREST);
#define int_fmt GL_RGB
#define tex_fmt GL_RGBA, GL_UNSIGNED_BYTE
#if 0
    texsize = width > height ? width : height;
    texsize *= scale;
    // texsize = 1 << ffs(texsize);
    texsize = texsize | (texsize >> 1);
    texsize = texsize | (texsize >> 2);
    texsize = texsize | (texsize >> 4);
    texsize = texsize | (texsize >> 8);
    texsize = (texsize >> 1) + 1;
    glTexImage2D(GL_TEXTURE_2D, 0, int_fmt, texsize, texsize, 0, tex_fmt, NULL);
#else
    // but really, most cards support non-p2 and rect
    // if not, use cairo or wx renderer
    glTexImage2D(GL_TEXTURE_2D, 0, int_fmt, width * scale, height * scale, 0, tex_fmt, NULL);
#endif
    glClearColor(0.0, 0.0, 0.0, 1.0);

    // non-portable vsync code
#if defined(__WXGTK__) && defined(GLX_SGI_swap_control)
    static PFNGLXSWAPINTERVALSGIPROC si = NULL;
    if(!si)
	si = (PFNGLXSWAPINTERVALSGIPROC)glXGetProcAddress((const GLubyte *)"glxSwapIntervalSGI");
    if(si)
	si(gopts.vsync);
#else
#if defined(__WXMSW__) && defined(WGL_EXT_swap_control)
    static PFNWGLSWAPINTERVALEXTPROC si = NULL;
    if(!si)
	si = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if(si)
	si(gopts.vsync);
#else
#ifdef __WXMAC__
    int swap_interval = gopts.vsync ? 1 : 0;
    CGLContextObj cgl_context = CGLGetCurrentContext();
    CGLSetParameter(cgl_context, kCGLCPSwapInterval, &swap_interval);
#else
#warning no vsync support on this platform
#endif
#endif
#endif

    did_init = true;
}

void GLDrawingPanel::DrawArea(wxWindowDC &dc)
{
#if wxCHECK_VERSION(2,9,0) || !defined(__WXMAC__)
    SetCurrent(ctx);
#else
    SetCurrent();
#endif
    if(!did_init)
	Init();

    if(todraw) {
	int rowlen = width * scale;
	glPixelStorei(GL_UNPACK_ROW_LENGTH, rowlen);
	glTexImage2D(GL_TEXTURE_2D, 0, int_fmt, width * scale, height * scale,
		     0, tex_fmt, todraw);
	glCallList(vlist);
    } else
	glClear(GL_COLOR_BUFFER_BIT);
    SwapBuffers();
}

void GLDrawingPanel::OnSize(wxSizeEvent &ev)
{
    if(!did_init)
	return;

    int w, h;
    GetClientSize(&w, &h);

#if wxCHECK_VERSION(2,9,0) || !defined(__WXMAC__)
    SetCurrent(ctx);
#else
    SetCurrent();
#endif
    glViewport(0, 0, w, h);

    ev.Skip(); // propagate to parent
}
#endif

#ifndef NO_CAIRO

IMPLEMENT_CLASS(CairoDrawingPanel, DrawingPanel)

BEGIN_EVENT_TABLE(CairoDrawingPanel, wxPanel)
    EVT_PAINT(CairoDrawingPanel::PaintEv2)
END_EVENT_TABLE()

CairoDrawingPanel::CairoDrawingPanel(wxWindow *parent, int _width, int _height)
     :  DrawingPanel(_width, _height),
        wxPanel(parent, wxID_ANY, wxPoint(0, 0), parent->GetSize(), wxFULL_REPAINT_ON_RESIZE)
{
    conv_surf = NULL;

    // Intialize color tables in reverse order from default
    // probably doesn't help mmx hq3x/hq4x
    if(systemColorDepth == 32) {
#if wxBYTE_ORDER == wxLITTLE_ENDIAN
	systemBlueShift    = 3;
	systemRedShift   = 19;
#else
	systemBlueShift    = 27;
	systemRedShift   = 11;
#endif
    }

    // FIXME: should be "true" for GBA carts if lcd mode selected
    utilUpdateSystemColorMaps(false);
}

CairoDrawingPanel::~CairoDrawingPanel()
{
    if(conv_surf)
	cairo_surface_destroy(conv_surf);
}

#include <wx/graphics.h>
#ifdef __WXMSW__
#include <cairo-win32.h>
#include <gdiplus.h>
#endif
#if defined(__WXMAC__) && wxMAC_USE_CORE_GRAPHICS
#include <cairo-quartz.h>
#endif

void CairoDrawingPanel::DrawArea(wxWindowDC &dc)
{
    cairo_t *cr;
    wxGraphicsContext *gc = wxGraphicsContext::Create(dc);
#ifdef __WXMSW__
    // not sure why this is so slow
    // doing this only once in constructor and resize handler doesn't seem
    // to help, and may be unsafe
    Gdiplus::Graphics *gr = (Gdiplus::Graphics *)gc->GetNativeContext();
    cairo_surface_t *s = cairo_win32_surface_create(gr->GetHDC());
    cr = cairo_create(s);
    cairo_surface_destroy(s);
#else
#ifdef __WXGTK__
    cr = cairo_reference((cairo_t *)gc->GetNativeContext());
#else
#if defined(__WXMAC__) && wxMAC_USE_CORE_GRAPHICS
    CGContextRef *c = gc->GetNativeContext();
    cairo_surface_t *s = cairo_quartz_surface_create_for_cg_context(c);
    cr = cairo_create(s);
    cairo_surface_destroy(s);
#else
#error Cairo rendering is not supported on this platform
#endif
#endif
#endif
    cairo_surface_t *surf;
	surf = cairo_image_surface_create_for_data(todraw,
						   CAIRO_FORMAT_RGB24,
						   width, height,
						   4 * width);

    cairo_pattern_t *pat = cairo_pattern_create_for_surface(surf);
    // GOOD is "similar to" bilinear, and FAST is "similar to" nearest
    // could also just use BILINEAR and NEAREST directly, I suppose
    cairo_pattern_set_filter(pat, gopts.bilinear ? CAIRO_FILTER_GOOD : CAIRO_FILTER_FAST);
    double sx, sy;
    int w, h;
    GetClientSize(&w, &h);
    sx =(double)width / (double)w;
    sy = (double)height / (double)h;
    cairo_matrix_t mat;
    cairo_matrix_init_scale(&mat, sx, sy);
    cairo_pattern_set_matrix(pat, &mat);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);
    cairo_surface_destroy(surf);
    cairo_destroy(cr);
    delete gc;
}
#endif

#if defined(__WXMSW__) && !defined(NO_D3D)
#define DIRECT3D_VERSION 0x0900
#include <d3d9.h>
//#include <Dxerr.h>

IMPLEMENT_CLASS(DXDrawingPanel, DrawingPanel)

BEGIN_EVENT_TABLE(DXDrawingPanel, wxPanel)
    EVT_PAINT(DXDrawingPanel::PaintEv2)
END_EVENT_TABLE()

DXDrawingPanel::DXDrawingPanel(wxWindow *parent, int _width, int _height)
     : wxPanel(parent, wxID_ANY, wxPoint(0, 0), parent->GetSize(),
	      wxFULL_REPAINT_ON_RESIZE), DrawingPanel(_width, _height)
{
    // FIXME: implement
}

void DXDrawingPanel::DrawArea(wxWindowDC &dc)
{
    // FIXME: implement
}
#endif

#ifndef NO_FFMPEG
static const wxChar *media_err(MediaRet ret)
{
    switch(ret) {
    case MRET_OK:
	return wxT("");
    case MRET_ERR_NOMEM:
	return _("memory allocation error");
    case MRET_ERR_NOCODEC:
	return _("error initializing codec");
    case MRET_ERR_FERR:
	return _("error writing to output file");
    case MRET_ERR_FMTGUESS:
	return _("can't guess output format from file name");
    default:
//    case MRET_ERR_RECORDING:
//    case MRET_ERR_BUFSIZE:
	return _("programming error; aborting!");
    }
}

void GameArea::StartVidRecording(const wxString &fname)
{
    // auto-conversion of wxCharBuffer to const char * seems broken
    // so save underlying wxCharBuffer (or create one of none is used)
    wxCharBuffer fnb(fname.mb_fn_str());
    MediaRet ret;
    if((ret = vid_rec.Record(fnb.data(), basic_width, basic_height,
			     systemColorDepth)) != MRET_OK)
	wxLogError(_("Unable to begin recording to %s (%s)"), fname.c_str(),
		   media_err(ret));
    else {
	MainFrame *mf = wxGetApp().frame;
	mf->cmd_enable &= ~(CMDEN_NVREC|CMDEN_NREC_ANY);
	mf->cmd_enable |= CMDEN_VREC;
	mf->enable_menus();
    }
}

void GameArea::StopVidRecording()
{
    vid_rec.Stop();
    MainFrame *mf = wxGetApp().frame;
    mf->cmd_enable &= ~CMDEN_VREC;
    mf->cmd_enable |= CMDEN_NVREC;
    if(!(mf->cmd_enable & (CMDEN_VREC|CMDEN_SREC)))
	mf->cmd_enable |= CMDEN_NREC_ANY;
    mf->enable_menus();
}

void GameArea::StartSoundRecording(const wxString &fname)
{
    // auto-conversion of wxCharBuffer to const char * seems broken
    // so save underlying wxCharBuffer (or create one of none is used)
    wxCharBuffer fnb(fname.mb_fn_str());
    MediaRet ret;
    if((ret = snd_rec.Record(fnb.data())) != MRET_OK)
	wxLogError(_("Unable to begin recording to %s (%s)"), fname.c_str(),
		   media_err(ret));
    else {
	MainFrame *mf = wxGetApp().frame;
	mf->cmd_enable &= ~(CMDEN_NSREC|CMDEN_NREC_ANY);
	mf->cmd_enable |= CMDEN_SREC;
	mf->enable_menus();
    }
}

void GameArea::StopSoundRecording()
{
    snd_rec.Stop();
    MainFrame *mf = wxGetApp().frame;
    mf->cmd_enable &= ~CMDEN_SREC;
    mf->cmd_enable |= CMDEN_NSREC;
    if(!(mf->cmd_enable & (CMDEN_VREC|CMDEN_SREC)))
	mf->cmd_enable |= CMDEN_NREC_ANY;
    mf->enable_menus();
}

void GameArea::AddFrame(const u16 *data, int length)
{
    MediaRet ret;
    if((ret = vid_rec.AddFrame(data)) != MRET_OK) {
	wxLogError(_("Error in audio/video recording (%s); aborting"),
		   media_err(ret));
	vid_rec.Stop();
    }
    if((ret = snd_rec.AddFrame(data)) != MRET_OK) {
	wxLogError(_("Error in audio recording (%s); aborting"), media_err(ret));
	snd_rec.Stop();
    }
}

void GameArea::AddFrame(const u8 *data)
{
    MediaRet ret;
    if((ret = vid_rec.AddFrame(data)) != MRET_OK) {
	wxLogError(_("Error in video recording (%s); aborting"), media_err(ret));
	vid_rec.Stop();
    }
}
#endif

void GameArea::ShowPointer()
{
    if(fullscreen)
	return;
    mouse_active_time = systemGetClock();
    if(!pointer_blanked)
	return;
    pointer_blanked = false;
    SetCursor(wxNullCursor);
    if(panel)
	panel->GetWindow()->SetCursor(wxNullCursor);
}

void GameArea::HidePointer()
{
    if(pointer_blanked)
	return;
    // FIXME: make time configurable
    if(fullscreen || (systemGetClock() - mouse_active_time) > 3000) {
	pointer_blanked = true;
	SetCursor(wxCursor(wxCURSOR_BLANK));
	// wxGTK requires that subwindows get the cursor as well
	if(panel)
	    panel->GetWindow()->SetCursor(wxCursor(wxCURSOR_BLANK));
    }
}
