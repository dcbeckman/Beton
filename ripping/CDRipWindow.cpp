/**
 * @file CDRipWindow.cpp
 * @brief The "Rip CD" window: format, folder and naming, then progress.
 */

#include "CDRipWindow.h"

#include "CDRipper.h"
#include "CddaTracks.h"
#include "Messages.h"
#include "RipNamePattern.h"

#include <Alert.h>
#include <Button.h>
#include <Catalog.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TextControl.h>

#include <algorithm>
#include <string.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "CDRipWindow"

namespace {

enum {
  kMsgFormatChanged = 'rfmt',
  kMsgFolderEdited = 'rfed',
  kMsgChooseFolder = 'rfch',
  kMsgFolderChosen = 'rfcd',
  kMsgPatternEdited = 'rped',
  kMsgPreset = 'rprs',
  kMsgRip = 'rrip',
  kMsgClose = 'rcls',
  kMsgShowFolder = 'rshw'
};

const float kProgressMax = 1000.0f;

struct Preset {
  const char *label;
  const char *pattern;
};

const Preset kPresets[] = {
    {B_TRANSLATE_MARK("Album artist/Album/01 - Title"),
     "{albumartist}/{album}/{track} - {title}"},
    {B_TRANSLATE_MARK("Album artist/Year - Album/01 - Title"),
     "{albumartist}/{year} - {album}/{track} - {title}"},
    {B_TRANSLATE_MARK("Album artist - Album/01 - Title"),
     "{albumartist} - {album}/{track} - {title}"},
    {B_TRANSLATE_MARK("Album/01 - Artist - Title"),
     "{album}/{track} - {artist} - {title}"},
    {B_TRANSLATE_MARK("01 - Artist - Title (no folders)"),
     "{track} - {artist} - {title}"},
};

BString FormatDuration(int32 seconds) {
  BString s;
  if (seconds >= 3600)
    s.SetToFormat("%ld:%02ld:%02ld", (long)(seconds / 3600),
                  (long)(seconds / 60 % 60), (long)(seconds % 60));
  else
    s.SetToFormat("%ld:%02ld", (long)(seconds / 60), (long)(seconds % 60));
  return s;
}

} // namespace

CDRipWindow::CDRipWindow(const BString &discPath, const BString &discLabel,
                         BMessenger libraryTarget)
    : BWindow(BRect(0, 0, 560, 300), B_TRANSLATE("Rip audio CD"),
              B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS |
                  B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE),
      fDiscPath(discPath), fDiscLabel(discLabel), fLibraryTarget(libraryTarget) {
  fSettings.Load();
  _LoadTracks();
  _BuildUI();
  _ApplySettings();
  _SetRipping(false);
  _UpdatePreview();

  ResizeToPreferred();
  if (Bounds().Width() < 560)
    ResizeTo(560, Bounds().Height());
  CenterOnScreen();
}

CDRipWindow::~CDRipWindow() {
  delete fRipper;
  delete fFolderPanel;
}

void CDRipWindow::_LoadTracks() {
  fTracks = CddaTracks::Read(fDiscPath);
  for (MediaItem &t : fTracks) {
    t.trackTotal = (int32)fTracks.size();
    // Untitled discs fall back to the file name ("01 Audio Track.wav");
    // drop the extension so it doesn't end up inside the new file name.
    BPath p(t.path.String());
    if (p.InitCheck() == B_OK && t.title == p.Leaf()) {
      int32 dot = t.title.FindLast('.');
      if (dot > 0)
        t.title.Truncate(dot);
    }
  }
}

void CDRipWindow::_BuildUI() {
  int32 seconds = 0;
  for (const MediaItem &t : fTracks)
    seconds += t.duration;
  BString info(fDiscLabel);
  BString detail;
  detail.SetToFormat(B_TRANSLATE("%ld tracks, %s"), (long)fTracks.size(),
                     FormatDuration(seconds).String());
  info << "  (" << detail << ")";

  fDiscInfo = new BStringView("disc", info.String());
  fDiscInfo->SetFont(be_bold_font);
  fDiscInfo->SetTruncation(B_TRUNCATE_END);
  fDiscInfo->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));
  fDiscInfo->SetExplicitPreferredSize(BSize(400, B_SIZE_UNSET));

  fFormatMenu = new BPopUpMenu("format");
  const struct {
    RipFormat format;
    const char *label;
  } formats[] = {
      {RipFormat::MP3_320, B_TRANSLATE("MP3 (320 kbps)")},
      {RipFormat::FLAC, B_TRANSLATE("FLAC (lossless)")},
      {RipFormat::WAV, B_TRANSLATE("WAV (uncompressed)")},
  };
  for (const auto &f : formats) {
    BMessage *m = new BMessage(kMsgFormatChanged);
    m->AddInt32("format", (int32)f.format);
    fFormatMenu->AddItem(new BMenuItem(f.label, m));
  }
  fFormatField = new BMenuField("format", B_TRANSLATE("Format:"), fFormatMenu);

  fFolderControl = new BTextControl("folder", B_TRANSLATE("Save to:"), "",
                                    new BMessage(kMsgFolderEdited));
  fFolderControl->SetModificationMessage(new BMessage(kMsgFolderEdited));
  fChooseButton = new BButton("choose", B_TRANSLATE("Choose" B_UTF8_ELLIPSIS),
                              new BMessage(kMsgChooseFolder));

  fPatternControl = new BTextControl("pattern", B_TRANSLATE("File names:"), "",
                                     new BMessage(kMsgPatternEdited));
  fPatternControl->SetModificationMessage(new BMessage(kMsgPatternEdited));

  BPopUpMenu *presets = new BPopUpMenu(B_TRANSLATE("Presets"), false, false);
  for (const Preset &p : kPresets) {
    BMessage *m = new BMessage(kMsgPreset);
    m->AddString("pattern", p.pattern);
    presets->AddItem(new BMenuItem(B_TRANSLATE(p.label), m));
  }
  fPresetField = new BMenuField("presets", nullptr, presets);

  BString fields;
  for (int32 i = 0; i < RipNamePattern::kFieldCount; i++) {
    if (i > 0)
      fields << "  ";
    fields << RipNamePattern::kFields[i];
  }
  BStringView *help =
      new BStringView("help", B_TRANSLATE("Use / to make subfolders. Fields:"));
  BStringView *fieldList = new BStringView("fields", fields.String());

  fPreview = new BStringView("preview", "");
  fPreview->SetTruncation(B_TRUNCATE_MIDDLE);
  fPreview->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));
  fPreview->SetExplicitPreferredSize(BSize(400, B_SIZE_UNSET));
  fPreview->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

  fProgress = new BStatusBar("progress");
  fProgress->SetMaxValue(kProgressMax);

  fShowButton = new BButton("show", B_TRANSLATE("Show folder"),
                            new BMessage(kMsgShowFolder));
  fCloseButton =
      new BButton("close", B_TRANSLATE("Close"), new BMessage(kMsgClose));
  fRipButton = new BButton("rip", B_TRANSLATE("Rip"), new BMessage(kMsgRip));

  // clang-format off
  BLayoutBuilder::Group<>(this, B_VERTICAL)
    .SetInsets(B_USE_WINDOW_INSETS)
    .Add(fDiscInfo)
    .AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
      .Add(fFormatField->CreateLabelLayoutItem(), 0, 0)
      .Add(fFormatField->CreateMenuBarLayoutItem(), 1, 0)
      .AddTextControl(fFolderControl, 0, 1)
      .Add(fChooseButton, 2, 1)
      .AddTextControl(fPatternControl, 0, 2)
      .Add(fPresetField, 2, 2)
      .Add(help, 1, 3, 2)
      .Add(fieldList, 1, 4, 2)
      .Add(fPreview, 1, 5, 2)
    .End()
    .Add(fProgress)
    .AddGroup(B_HORIZONTAL)
      .Add(fShowButton)
      .AddGlue()
      .Add(fCloseButton)
      .Add(fRipButton)
    .End();
  // clang-format on

  fRipButton->MakeDefault(true);
}

void CDRipWindow::_ApplySettings() {
  BMenuItem *item = fFormatMenu->ItemAt((int32)fSettings.format);
  if (item)
    item->SetMarked(true);
  fFolderControl->SetText(fSettings.directory.String());
  fPatternControl->SetText(fSettings.pattern.String());
}

RipFormat CDRipWindow::_SelectedFormat() const {
  int32 index = fFormatMenu->IndexOf(fFormatMenu->FindMarked());
  if (index < (int32)RipFormat::MP3_320 || index > (int32)RipFormat::WAV)
    return RipFormat::FLAC;
  return (RipFormat)index;
}

void CDRipWindow::_ReadControls() {
  fSettings.format = _SelectedFormat();
  fSettings.directory = fFolderControl->Text();
  fSettings.directory.Trim();
  while (fSettings.directory.Length() > 1 && fSettings.directory.EndsWith("/"))
    fSettings.directory.Truncate(fSettings.directory.Length() - 1);
  fSettings.pattern = fPatternControl->Text();
  fSettings.pattern.Trim();
}

void CDRipWindow::_UpdatePreview() {
  _ReadControls();

  BString text;
  bool ready = false;
  if (fTracks.empty()) {
    text = B_TRANSLATE("No audio tracks were found on this disc.");
  } else if (!fSettings.directory.StartsWith("/")) {
    text = B_TRANSLATE("Choose the folder to save the tracks in.");
  } else if (fSettings.pattern.IsEmpty()) {
    text = B_TRANSLATE("Enter a pattern for the file names.");
  } else {
    BString unknown;
    RipNamePattern::Expand(fSettings.pattern, fTracks.front(), &unknown);
    CDRipJob job;
    job.format = fSettings.format;
    job.directory = fSettings.directory;
    job.pattern = fSettings.pattern;
    BString example = CDRipper::DestinationFor(job, fTracks.front());
    if (!unknown.IsEmpty()) {
      text.SetToFormat(B_TRANSLATE("%s is not a field and is kept as text: %s"),
                       unknown.String(), example.String());
    } else {
      text.SetToFormat(B_TRANSLATE("Example: %s"), example.String());
    }
    ready = true;
  }

  fPreview->SetText(text.String());
  if (fRipper == nullptr)
    fRipButton->SetEnabled(ready);
}

void CDRipWindow::_ChooseFolder() {
  if (fFolderPanel == nullptr) {
    BMessenger self(this);
    fFolderPanel = new BFilePanel(B_OPEN_PANEL, &self, nullptr, B_DIRECTORY_NODE,
                                  false, new BMessage(kMsgFolderChosen), nullptr,
                                  true, true);
    fFolderPanel->Window()->SetTitle(B_TRANSLATE("Beton: Choose rip folder"));
    fFolderPanel->SetButtonLabel(B_DEFAULT_BUTTON, B_TRANSLATE("Select"));
  }

  // Open where the current folder is, or its closest existing parent.
  BPath start(fFolderControl->Text());
  while (start.InitCheck() == B_OK && !BEntry(start.Path()).Exists()) {
    if (start.GetParent(&start) != B_OK)
      break;
  }
  if (start.InitCheck() == B_OK) {
    entry_ref ref;
    if (get_ref_for_path(start.Path(), &ref) == B_OK)
      fFolderPanel->SetPanelDirectory(&ref);
  }
  fFolderPanel->Show();
}

void CDRipWindow::_SetRipping(bool ripping) {
  fFormatField->SetEnabled(!ripping);
  fFolderControl->SetEnabled(!ripping);
  fChooseButton->SetEnabled(!ripping);
  fPatternControl->SetEnabled(!ripping);
  fPresetField->SetEnabled(!ripping);
  fShowButton->SetEnabled(!fOutputFolder.IsEmpty());
  fRipButton->SetLabel(ripping ? B_TRANSLATE("Stop") : B_TRANSLATE("Rip"));
  fRipButton->SetEnabled(true);
}

void CDRipWindow::_StartRip() {
  _ReadControls();
  if (fTracks.empty() || !fSettings.directory.StartsWith("/") ||
      fSettings.pattern.IsEmpty())
    return;
  fSettings.Save();

  CDRipJob job;
  job.tracks = fTracks;
  job.format = fSettings.format;
  job.directory = fSettings.directory;
  job.pattern = fSettings.pattern;

  fFailures.clear();
  fOutputFolder.Truncate(0);

  fRipper = new CDRipper(job, BMessenger(this));
  status_t status = fRipper->Start();
  if (status != B_OK) {
    delete fRipper;
    fRipper = nullptr;
    BString text;
    text.SetToFormat(B_TRANSLATE("Could not start ripping: %s"), strerror(status));
    (new BAlert(B_TRANSLATE("Rip audio CD"), text.String(), B_TRANSLATE("OK"),
                nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT))
        ->Go(nullptr);
    return;
  }

  fProgress->Reset();
  fProgress->SetMaxValue(kProgressMax);
  fProgress->SetTo(0, B_TRANSLATE("Starting" B_UTF8_ELLIPSIS), "");
  _SetRipping(true);
}

void CDRipWindow::_HandleProgress(BMessage *msg) {
  int32 track = 0, tracks = 0;
  float fraction = 0;
  BString title;
  msg->FindInt32("track", &track);
  msg->FindInt32("tracks", &tracks);
  msg->FindFloat("fraction", &fraction);
  msg->FindString("title", &title);

  BString text;
  text.SetToFormat(B_TRANSLATE("Track %ld of %ld: %s"), (long)track + 1,
                   (long)tracks, title.String());
  BString percent;
  percent.SetToFormat("%d%%", (int)(fraction * 100.0f));
  fProgress->SetTo(fraction * kProgressMax, text.String(), percent.String());
}

void CDRipWindow::_HandleTrackDone(BMessage *msg) {
  int32 track = 0;
  int32 status = B_OK;
  BString path, title, error;
  msg->FindInt32("track", &track);
  msg->FindInt32("status", &status);
  msg->FindString("path", &path);
  msg->FindString("title", &title);
  msg->FindString("error", &error);

  if ((status == B_OK || status == B_FILE_EXISTS) && fOutputFolder.IsEmpty()) {
    BPath parent;
    if (BPath(path.String()).GetParent(&parent) == B_OK)
      fOutputFolder = parent.Path();
  }

  if (status != B_OK && status != B_FILE_EXISTS) {
    BString line;
    line.SetToFormat(B_TRANSLATE("Track %ld (%s): %s"), (long)track + 1,
                     title.String(),
                     error.IsEmpty() ? strerror(status) : error.String());
    fFailures.push_back(line);
  }
}

void CDRipWindow::_HandleFinished(BMessage *msg) {
  if (fRipper) {
    fRipper->Wait();
    delete fRipper;
    fRipper = nullptr;
  }

  int32 ripped = 0, skipped = 0, failed = 0;
  bool cancelled = false;
  msg->FindInt32("ripped", &ripped);
  msg->FindInt32("skipped", &skipped);
  msg->FindInt32("failed", &failed);
  msg->FindBool("cancelled", &cancelled);

  BString summary;
  if (ripped == 1)
    summary = B_TRANSLATE("1 track ripped");
  else
    summary.SetToFormat(B_TRANSLATE("%ld tracks ripped"), (long)ripped);
  if (skipped > 0) {
    BString part;
    part.SetToFormat(B_TRANSLATE("%ld already there"), (long)skipped);
    summary << ", " << part;
  }
  if (failed > 0) {
    BString part;
    part.SetToFormat(B_TRANSLATE("%ld failed"), (long)failed);
    summary << ", " << part;
  }
  if (cancelled)
    summary << " " << B_TRANSLATE("(stopped)");

  fProgress->SetTo(cancelled ? fProgress->CurrentValue() : kProgressMax,
                   summary.String(), "");
  _SetRipping(false);
  _UpdatePreview();

  // Hand the new files over; the window side keeps the ones in the library.
  if (ripped > 0 && fLibraryTarget.IsValid()) {
    BMessage import(MSG_CD_RIP_IMPORT);
    BString path;
    for (int32 i = 0; msg->FindString("path", i, &path) == B_OK; i++)
      import.AddString("path", path);
    fLibraryTarget.SendMessage(&import);
  }

  if (!fFailures.empty()) {
    BString text(B_TRANSLATE("Some tracks could not be ripped:"));
    text << "\n\n";
    const size_t shown = std::min<size_t>(fFailures.size(), 10);
    for (size_t i = 0; i < shown; i++)
      text << fFailures[i] << "\n";
    if (fFailures.size() > shown)
      text << B_UTF8_ELLIPSIS << "\n";
    (new BAlert(B_TRANSLATE("Rip audio CD"), text.String(), B_TRANSLATE("OK"),
                nullptr, nullptr, B_WIDTH_FROM_LABEL, B_WARNING_ALERT))
        ->Go(nullptr);
  }
}

void CDRipWindow::_ShowOutputFolder() {
  BString folder = fOutputFolder.IsEmpty() ? fSettings.directory : fOutputFolder;
  entry_ref ref;
  if (get_ref_for_path(folder.String(), &ref) == B_OK)
    be_roster->Launch(&ref);
}

void CDRipWindow::MessageReceived(BMessage *msg) {
  switch (msg->what) {
  case kMsgFormatChanged:
  case kMsgFolderEdited:
  case kMsgPatternEdited:
    _UpdatePreview();
    break;

  case kMsgPreset: {
    const char *pattern = nullptr;
    if (msg->FindString("pattern", &pattern) == B_OK) {
      fPatternControl->SetText(pattern);
      _UpdatePreview();
    }
    break;
  }

  case kMsgChooseFolder:
    _ChooseFolder();
    break;

  case kMsgFolderChosen: {
    entry_ref ref;
    if (msg->FindRef("refs", &ref) == B_OK) {
      BPath path(&ref);
      if (path.InitCheck() == B_OK) {
        fFolderControl->SetText(path.Path());
        _UpdatePreview();
      }
    }
    break;
  }

  case kMsgRip:
    if (fRipper == nullptr) {
      _StartRip();
    } else {
      fRipper->Cancel();
      fRipButton->SetLabel(B_TRANSLATE("Stopping" B_UTF8_ELLIPSIS));
      fRipButton->SetEnabled(false);
    }
    break;

  case kMsgClose:
    PostMessage(B_QUIT_REQUESTED);
    break;

  case kMsgShowFolder:
    _ShowOutputFolder();
    break;

  case MSG_CD_RIP:
    // The sidebar asked for a rip window while this one is open.
    Activate();
    break;

  case MSG_CD_RIP_PROGRESS:
    _HandleProgress(msg);
    break;

  case MSG_CD_RIP_TRACK_DONE:
    _HandleTrackDone(msg);
    break;

  case MSG_CD_RIP_FINISHED:
    _HandleFinished(msg);
    break;

  default:
    BWindow::MessageReceived(msg);
  }
}

bool CDRipWindow::QuitRequested() {
  if (fRipper != nullptr && fRipper->IsRunning()) {
    BAlert *alert = new BAlert(
        B_TRANSLATE("Rip audio CD"),
        B_TRANSLATE("Stop ripping?\n\nTracks that are already finished are "
                    "kept; the track being ripped now is discarded."),
        B_TRANSLATE("Keep ripping"), B_TRANSLATE("Stop and close"), nullptr,
        B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetShortcut(0, B_ESCAPE);
    if (alert->Go() == 0)
      return false;
  }

  if (fRipper != nullptr) {
    fRipper->Cancel();
    fRipper->Wait();
    delete fRipper;
    fRipper = nullptr;
  }

  _ReadControls();
  fSettings.Save();
  return true;
}
