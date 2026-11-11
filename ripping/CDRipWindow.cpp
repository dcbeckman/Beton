/**
 * @file CDRipWindow.cpp
 * @brief The "Rip CD" window: format, folder and naming, then progress.
 */

#include "CDRipWindow.h"

#include "AccurateRip.h"
#include "CDRipper.h"
#include "CddaTracks.h"
#include "Messages.h"
#include "RipNamePattern.h"

#include <Alert.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <ControlLook.h>
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
#include <stdlib.h>
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
  kMsgShowFolder = 'rshw',
  kMsgOffsetEdited = 'roed',
  kMsgVerifyChanged = 'rver',
  kMsgToggleAll = 'rtal',
  kMsgTrackToggled = 'rttg',
  kMsgPlayTrack = 'rply'
};

/** How a finished track came out; picks its text color in the track list. */
enum TrackResult : int32 {
  kResultNone = 0, ///< Not ripped yet, skipped, stopped, or nothing checked.
  kResultClean,    ///< No read errors left, but no AccurateRip match.
  kResultVerified, ///< No read errors left and an AccurateRip match.
  kResultBad       ///< Read errors left after re-reading, or the rip failed.
};

/** Whether a track is to be ripped. */
class CheckField : public BField {
public:
  bool checked = true;
};

/**
 * A checkbox per track. Toggles on click (unless locked during a rip) and
 * tells the window, which keeps the "All tracks" box in step.
 *
 * Haiku's OutlineView looks the clicked field up by the column's display
 * position, not its field number, so this column has to be field 0 and the
 * list must not let columns be moved.
 */
class CheckColumn : public BColumn {
public:
  CheckColumn(float width, BMessenger target)
      : BColumn(width, width, width, B_ALIGN_CENTER), fTarget(target) {
    SetWantsEvents(true);
  }

  void SetLocked(bool locked) { fLocked = locked; }

  void GetColumnName(BString *into) const override {
    *into = B_TRANSLATE("Rip");
  }
  void DrawTitle(BRect, BView *) override {}

  void DrawField(BField *field, BRect rect, BView *parent) override {
    CheckField *f = dynamic_cast<CheckField *>(field);
    if (f == nullptr)
      return;
    const float size = std::min(14.0f, rect.Height() - 4);
    BRect box(0, 0, size, size);
    box.OffsetTo(rect.left + floorf((rect.Width() - size) / 2),
                 rect.top + floorf((rect.Height() - size) / 2));
    uint32 flags = f->checked ? BControlLook::B_ACTIVATED : 0;
    if (fLocked)
      flags |= BControlLook::B_DISABLED;
    be_control_look->DrawCheckBox(parent, box, box,
                                  ui_color(B_CONTROL_BACKGROUND_COLOR), flags);
  }

  int CompareFields(BField *a, BField *b) override {
    return (int)static_cast<CheckField *>(a)->checked -
           (int)static_cast<CheckField *>(b)->checked;
  }

  bool AcceptsField(const BField *field) const override {
    return dynamic_cast<const CheckField *>(field) != nullptr;
  }

  void MouseDown(BColumnListView *parent, BRow *row, BField *field, BRect,
                 BPoint, uint32) override {
    CheckField *f = dynamic_cast<CheckField *>(field);
    if (fLocked || f == nullptr)
      return;
    f->checked = !f->checked;
    parent->UpdateRow(row);
    fTarget.SendMessage(kMsgTrackToggled);
  }

private:
  BMessenger fTarget;
  bool fLocked = false;
};

/** A track-list cell that remembers its row's result. */
class TrackField : public BStringField {
public:
  explicit TrackField(const char *text) : BStringField(text) {}
  TrackResult result = kResultNone;
};

/**
 * Draws a cell in its row's result color. Shades depend on the list's
 * background so they stay readable in light and dark themes.
 */
class TrackColumn : public BStringColumn {
public:
  using BStringColumn::BStringColumn;

  void DrawField(BField *field, BRect rect, BView *parent) override {
    TrackField *f = dynamic_cast<TrackField *>(field);
    if (f == nullptr || f->result == kResultNone) {
      BStringColumn::DrawField(field, rect, parent);
      return;
    }
    const rgb_color bg = ui_color(B_LIST_BACKGROUND_COLOR);
    const bool dark = bg.red * 0.299f + bg.green * 0.587f + bg.blue * 0.114f < 128;
    rgb_color color;
    switch (f->result) {
    case kResultVerified:
      color = dark ? make_color(80, 235, 80) : make_color(0, 140, 0);
      break;
    case kResultClean:
      color = dark ? make_color(165, 205, 165) : make_color(90, 140, 90);
      break;
    default:
      color = dark ? make_color(255, 95, 95) : make_color(200, 0, 0);
      break;
    }
    const rgb_color old = parent->HighColor();
    parent->SetHighColor(color);
    BStringColumn::DrawField(field, rect, parent);
    parent->SetHighColor(old);
  }
};


enum {
  kColCheck, // Must stay field 0: see CheckColumn.
  kColNumber,
  kColTitle,
  kColArtist,
  kColLength,
  kColGain,
  kColErrors,
  kColStatus
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
    : BWindow(BRect(0, 0, 800, 300), B_TRANSLATE("Rip audio CD"),
              B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS |
                  B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE),
      fDiscPath(discPath), fDiscLabel(discLabel), fLibraryTarget(libraryTarget) {
  fSettings.Load();
  fDriveName = AccurateRip::DriveName(fDiscPath);
  _LoadTracks();
  _BuildUI();
  _ApplySettings();
  _SetRipping(false);
  _UpdatePreview();

  ResizeToPreferred();
  if (Bounds().Width() < 800)
    ResizeTo(800, Bounds().Height());
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
  fDiscInfo->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

  fTrackList = new BColumnListView("tracks", B_NAVIGABLE, B_FANCY_BORDER, false);
  fTrackList->SetSelectionMode(B_SINGLE_SELECTION_LIST);
  fTrackList->SetColor(B_COLOR_BACKGROUND, ui_color(B_LIST_BACKGROUND_COLOR));
  fTrackList->SetColor(B_COLOR_TEXT, ui_color(B_LIST_ITEM_TEXT_COLOR));
  fTrackList->SetColor(B_COLOR_SELECTION,
                       ui_color(B_LIST_SELECTED_BACKGROUND_COLOR));
  fTrackList->SetColor(B_COLOR_SELECTION_TEXT,
                       ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR));
  fTrackList->SetColor(B_COLOR_ROW_DIVIDER, B_TRANSPARENT_COLOR);
  fTrackList->SetColor(B_COLOR_HEADER_BACKGROUND,
                       ui_color(B_PANEL_BACKGROUND_COLOR));
  fTrackList->SetColor(B_COLOR_HEADER_TEXT, ui_color(B_PANEL_TEXT_COLOR));
  // Resizing only: moving columns would break the checkbox column's clicks.
  fTrackList->SetColumnFlags(B_ALLOW_COLUMN_RESIZE);
  fCheckColumn = new CheckColumn(24, BMessenger(this));
  fTrackList->AddColumn(fCheckColumn, kColCheck);
  fTrackList->AddColumn(new TrackColumn("#", 30, 26, 50, B_TRUNCATE_END,
                                          B_ALIGN_RIGHT), kColNumber);
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Title"), 170, 60, 500,
                                          B_TRUNCATE_END), kColTitle);
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Artist"), 125, 50, 400,
                                          B_TRUNCATE_END), kColArtist);
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Length"), 52, 40, 80,
                                          B_TRUNCATE_END, B_ALIGN_RIGHT),
                        kColLength);
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Gain"), 62, 40, 90,
                                          B_TRUNCATE_END, B_ALIGN_RIGHT),
                        kColGain);
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Read errors"), 110, 50,
                                          300, B_TRUNCATE_END), kColErrors);
  fTrackList->AddColumn(new TrackColumn(B_TRANSLATE("Status"), 195, 60, 500,
                                          B_TRUNCATE_END), kColStatus);
  for (const MediaItem &t : fTracks) {
    BRow *row = new BRow();
    BString number;
    number << t.track;
    row->SetField(new CheckField(), kColCheck);
    row->SetField(new TrackField(number.String()), kColNumber);
    row->SetField(new TrackField(t.title.String()), kColTitle);
    row->SetField(new TrackField(t.artist.String()), kColArtist);
    row->SetField(new TrackField(FormatDuration(t.duration).String()),
                  kColLength);
    row->SetField(new TrackField(""), kColGain);
    row->SetField(new TrackField(""), kColErrors);
    row->SetField(new TrackField(""), kColStatus);
    fTrackList->AddRow(row);
  }
  fTrackList->SetExplicitMinSize(BSize(B_SIZE_UNSET, 180));
  fTrackList->SetInvocationMessage(new BMessage(kMsgPlayTrack));
  fRippedPaths.assign(fTracks.size(), BString());

  fAllCheck = new BCheckBox("all", B_TRANSLATE("All tracks"),
                            new BMessage(kMsgToggleAll));
  fAllCheck->SetValue(B_CONTROL_ON);
  // A checkbox's maximum width is its text width, which would cap the
  // whole window's width (see the other views above).
  fAllCheck->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

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
  // A string view's maximum width defaults to its text width, which would
  // cap how wide the whole grid -- and the window -- can get.
  help->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  fieldList->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

  fPreview = new BStringView("preview", "");
  fPreview->SetTruncation(B_TRUNCATE_MIDDLE);
  fPreview->SetExplicitMinSize(BSize(200, B_SIZE_UNSET));
  fPreview->SetExplicitPreferredSize(BSize(400, B_SIZE_UNSET));
  fPreview->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

  fOffsetControl = new BTextControl("offset", B_TRANSLATE("Read offset:"), "",
                                    new BMessage(kMsgOffsetEdited));
  fOffsetControl->SetModificationMessage(new BMessage(kMsgOffsetEdited));
  fDriveInfo = new BStringView("drive", "");
  fDriveInfo->SetTruncation(B_TRUNCATE_END);
  fDriveInfo->SetExplicitMinSize(BSize(100, B_SIZE_UNSET));
  fDriveInfo->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  fVerifyCheck = new BCheckBox(
      "verify", B_TRANSLATE("Check rips with AccurateRip (needs internet)"),
      new BMessage(kMsgVerifyChanged));
  fVerifyCheck->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  fLibraryCheck = new BCheckBox("library", B_TRANSLATE("Add tracks to library"),
                                nullptr);
  fLibraryCheck->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
  fLibraryCheck->SetToolTip(B_TRANSLATE(
      "If the save folder isn't one of the library's folders yet, it is "
      "added to them."));

  fProgress = new BStatusBar("progress");
  fProgress->SetMaxValue(kProgressMax);

  fShowButton = new BButton("show", B_TRANSLATE("Show folder"),
                            new BMessage(kMsgShowFolder));
  fCloseButton =
      new BButton("close", B_TRANSLATE("Close"), new BMessage(kMsgClose));
  fRipButton = new BButton("rip", B_TRANSLATE("Rip"), new BMessage(kMsgRip));

  BLayoutItem *offsetText = fOffsetControl->CreateTextViewLayoutItem();
  offsetText->SetExplicitMinSize(BSize(60, B_SIZE_UNSET));
  offsetText->SetExplicitMaxSize(BSize(60, B_SIZE_UNSET));

  // clang-format off
  BLayoutBuilder::Group<>(this, B_VERTICAL)
    .SetInsets(B_USE_WINDOW_INSETS)
    .Add(fDiscInfo)
    .Add(fAllCheck)
    .Add(fTrackList, 10)
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
      .Add(fOffsetControl->CreateLabelLayoutItem(), 0, 6)
      .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING, 1, 6, 2)
        .Add(offsetText)
        .Add(fDriveInfo)
      .End()
      .Add(fVerifyCheck, 1, 7, 2)
      .Add(fLibraryCheck, 1, 8, 2)
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
  fVerifyCheck->SetValue(fSettings.verify ? B_CONTROL_ON : B_CONTROL_OFF);
  fLibraryCheck->SetValue(fSettings.addToLibrary ? B_CONTROL_ON
                                                 : B_CONTROL_OFF);

  int32 offset = 0;
  BString text;
  if (fSettings.DriveOffset(fDriveName, offset))
    text.SetToFormat("%+ld", (long)offset);
  fOffsetControl->SetText(text.String());
  _UpdateDriveInfo();
}

bool CDRipWindow::_ParseOffset(int32 &offset, bool &known) const {
  BString text(fOffsetControl->Text());
  text.Trim();
  known = !text.IsEmpty();
  offset = 0;
  if (!known)
    return true;
  char *end = nullptr;
  long value = strtol(text.String(), &end, 10);
  if (end == text.String() || *end != '\0' ||
      labs(value) >= AccurateRip::kSkipFrames)
    return false;
  offset = (int32)value;
  return true;
}

void CDRipWindow::_UpdateDriveInfo() {
  BString drive = fDriveName.IsEmpty() ? BString(B_TRANSLATE("this drive"))
                                       : fDriveName;
  BString text;
  int32 offset;
  bool known;
  BString current(fOffsetControl->Text());
  current.Trim();
  if (!fDetectedOffset.IsEmpty() && current == fDetectedOffset) {
    text.SetToFormat(B_TRANSLATE("samples, detected from AccurateRip for %s"),
                     drive.String());
  } else if (_ParseOffset(offset, known) && !known) {
    text.SetToFormat(B_TRANSLATE("unknown for %s: found from AccurateRip "
                                 "when you rip"), drive.String());
  } else {
    text.SetToFormat(B_TRANSLATE("samples, for %s"), drive.String());
  }
  fDriveInfo->SetText(text.String());
}

bool CDRipWindow::_IsTrackChecked(int32 index) const {
  BRow *row = fTrackList->RowAt(index);
  CheckField *field =
      row ? dynamic_cast<CheckField *>(row->GetField(kColCheck)) : nullptr;
  return field != nullptr && field->checked;
}

int32 CDRipWindow::_CountCheckedTracks() const {
  int32 count = 0;
  for (int32 i = 0; i < fTrackList->CountRows(); i++)
    count += _IsTrackChecked(i) ? 1 : 0;
  return count;
}

void CDRipWindow::_CheckAllTracks(bool checked) {
  for (int32 i = 0; i < fTrackList->CountRows(); i++) {
    BRow *row = fTrackList->RowAt(i);
    if (auto *field = dynamic_cast<CheckField *>(row->GetField(kColCheck)))
      field->checked = checked;
    fTrackList->UpdateRow(row);
  }
  _UpdateAllCheck();
}

void CDRipWindow::_UpdateAllCheck() {
  const int32 checked = _CountCheckedTracks();
  if (checked == 0)
    fAllCheck->SetValue(B_CONTROL_OFF);
  else if (checked == fTrackList->CountRows())
    fAllCheck->SetValue(B_CONTROL_ON);
  else
    fAllCheck->SetValue(B_CONTROL_PARTIALLY_ON);
  _UpdatePreview();
}

void CDRipWindow::_SetTrackResult(int32 index, int32 result) {
  BRow *row = fTrackList->RowAt(index);
  if (row == nullptr)
    return;
  for (int32 c = 0; c < fTrackList->CountColumns(); c++) {
    if (auto *field = dynamic_cast<TrackField *>(row->GetField(c)))
      field->result = (TrackResult)result;
  }
  fTrackList->UpdateRow(row);
}

void CDRipWindow::_SetTrackStatus(int32 index, const char *text) {
  BRow *row = fTrackList->RowAt(index);
  if (row == nullptr)
    return;
  BStringField *field = static_cast<BStringField *>(row->GetField(kColStatus));
  if (field != nullptr)
    field->SetString(text);
  fTrackList->UpdateRow(row);
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
  fSettings.verify = fVerifyCheck->Value() == B_CONTROL_ON;
  fSettings.addToLibrary = fLibraryCheck->Value() == B_CONTROL_ON;
}

void CDRipWindow::_UpdatePreview() {
  _ReadControls();

  BString text;
  bool ready = false;
  int32 offset;
  bool offsetKnown;
  if (fTracks.empty()) {
    text = B_TRANSLATE("No audio tracks were found on this disc.");
  } else if (!fSettings.directory.StartsWith("/")) {
    text = B_TRANSLATE("Choose the folder to save the tracks in.");
  } else if (fSettings.pattern.IsEmpty()) {
    text = B_TRANSLATE("Enter a pattern for the file names.");
  } else if (!_ParseOffset(offset, offsetKnown)) {
    text = B_TRANSLATE("The read offset must be a whole number of samples "
                       "below 2940, such as +6. Leave it empty to detect it.");
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

  if (ready && _CountCheckedTracks() == 0) {
    text = B_TRANSLATE("No tracks are selected.");
    ready = false;
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
  fOffsetControl->SetEnabled(!ripping);
  fVerifyCheck->SetEnabled(!ripping);
  fLibraryCheck->SetEnabled(!ripping);
  fAllCheck->SetEnabled(!ripping);
  static_cast<CheckColumn *>(fCheckColumn)->SetLocked(ripping);
  fTrackList->Invalidate();
  fShowButton->SetEnabled(!fOutputFolder.IsEmpty());
  fRipButton->SetLabel(ripping ? B_TRANSLATE("Stop") : B_TRANSLATE("Rip"));
  fRipButton->SetEnabled(true);
}

void CDRipWindow::_StartRip() {
  _ReadControls();
  int32 offset = 0;
  bool offsetKnown = false;
  if (fTracks.empty() || !fSettings.directory.StartsWith("/") ||
      fSettings.pattern.IsEmpty() || !_ParseOffset(offset, offsetKnown))
    return;

  // An emptied offset field means "detect it again".
  if (offsetKnown)
    fSettings.SetDriveOffset(fDriveName, offset);
  else
    fSettings.ForgetDriveOffset(fDriveName);
  fSettings.Save();

  CDRipJob job;
  job.discPath = fDiscPath;
  job.tracks = fTracks;
  job.format = fSettings.format;
  job.directory = fSettings.directory;
  job.pattern = fSettings.pattern;
  job.verify = fSettings.verify;
  job.offsetKnown = offsetKnown;
  job.offset = offset;
  for (int32 i = 0; i < (int32)fTracks.size(); i++)
    job.selected.push_back(_IsTrackChecked(i));

  for (int32 i = 0; i < (int32)fTracks.size(); i++) {
    if (BRow *row = fTrackList->RowAt(i)) {
      for (int32 column : {kColGain, kColErrors}) {
        if (auto *field = static_cast<BStringField *>(row->GetField(column)))
          field->SetString("");
      }
    }
    _SetTrackStatus(i, _IsTrackChecked(i) ? "" : B_TRANSLATE("Not selected"));
    _SetTrackResult(i, kResultNone);
  }
  fCurrentTrack = -1;
  fCurrentPercent = -1;

  fFailures.clear();
  fOutputFolder.Truncate(0);
  fRippedPaths.assign(fTracks.size(), BString());

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
  float fraction = 0, trackFraction = 0;
  BString title;
  msg->FindInt32("track", &track);
  msg->FindInt32("tracks", &tracks);
  msg->FindFloat("fraction", &fraction);
  msg->FindFloat("track_fraction", &trackFraction);
  msg->FindString("title", &title);

  const int32 trackPercent = (int32)(trackFraction * 100.0f);
  if (track != fCurrentTrack || trackPercent != fCurrentPercent) {
    fCurrentTrack = track;
    fCurrentPercent = trackPercent;
    BString status;
    status.SetToFormat(B_TRANSLATE("Ripping %ld%%"), (long)trackPercent);
    _SetTrackStatus(track, status.String());
    if (BRow *row = fTrackList->RowAt(track))
      fTrackList->ScrollTo(row);
  }

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
  int32 verdict = kRipNotChecked, confidence = -1;
  msg->FindInt32("ar", &verdict);
  msg->FindInt32("ar_confidence", &confidence);

  BString text;
  if (status == B_OK) {
    switch (verdict) {
    case kRipAccurate:
      if (confidence == 1)
        text = B_TRANSLATE("Accurate (1 matching rip)");
      else
        text.SetToFormat(B_TRANSLATE("Accurate (%ld matching rips)"),
                         (long)confidence);
      break;
    case kRipNoMatch:
      text = B_TRANSLATE("Ripped, no AccurateRip match");
      break;
    case kRipNotInDatabase:
      text = B_TRANSLATE("Ripped (not in AccurateRip)");
      break;
    default:
      text = B_TRANSLATE("Ripped");
    }
    float gain;
    if (msg->FindFloat("rg_gain", &gain) == B_OK) {
      BString gainText;
      gainText.SetToFormat("%+.1f dB", gain);
      if (BRow *row = fTrackList->RowAt(track)) {
        if (auto *field = static_cast<BStringField *>(row->GetField(kColGain)))
          field->SetString(gainText.String());
      }
    }
  } else if (status == B_FILE_EXISTS) {
    text = B_TRANSLATE("Already there, not ripped again");
  } else {
    text.SetToFormat(B_TRANSLATE("Failed: %s"),
                     error.IsEmpty() ? strerror(status) : error.String());
  }
  bool checked = false;
  msg->FindBool("errors_checked", &checked);
  int32 damagedSectors = 0, lostSectors = 0;
  msg->FindInt32("err_damaged", &damagedSectors);
  msg->FindInt32("err_unreadable", &lostSectors);

  // Red: errors left, or the rip failed. Green: nothing left to fix, vivid
  // when AccurateRip also confirms it. With neither check available, Beton
  // can't vouch for the track and leaves it uncolored.
  TrackResult result = kResultNone;
  if (status != B_OK && status != B_FILE_EXISTS)
    result = kResultBad;
  else if (status == B_OK && (damagedSectors > 0 || lostSectors > 0))
    result = kResultBad;
  else if (status == B_OK && verdict == kRipAccurate)
    result = kResultVerified;
  else if (status == B_OK && checked)
    result = kResultClean;
  _SetTrackResult(track, result);
  if ((status == B_OK || status == B_FILE_EXISTS) && !path.IsEmpty() &&
      track >= 0 && track < (int32)fRippedPaths.size())
    fRippedPaths[track] = path;
  if (BRow *row = fTrackList->RowAt(track)) {
    if (auto *field = static_cast<BStringField *>(row->GetField(kColErrors))) {
      BString errors;
      if (status == B_OK && checked) {
        int32 fixed = 0, damaged = 0, unreadable = 0;
        msg->FindInt32("err_fixed", &fixed);
        msg->FindInt32("err_damaged", &damaged);
        msg->FindInt32("err_unreadable", &unreadable);
        // Counts are sectors (1/75 s): "fixed" re-read clean, "bad" kept
        // the drive's best guess, "lost" became silence.
        auto add = [&errors](int32 n, const char *format) {
          if (n <= 0)
            return;
          BString part;
          part.SetToFormat(format, (long)n);
          if (!errors.IsEmpty())
            errors << ", ";
          errors << part;
        };
        add(fixed, B_TRANSLATE("%ld fixed"));
        add(damaged, B_TRANSLATE("%ld bad"));
        add(unreadable, B_TRANSLATE("%ld lost"));
        if (errors.IsEmpty())
          errors = B_TRANSLATE("None");
      } else if (status == B_OK) {
        errors = B_TRANSLATE("Not checked");
      }
      field->SetString(errors.String());
    }
  }

  _SetTrackStatus(track, text.String());
  fCurrentTrack = -1;

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

  int32 ripped = 0, skipped = 0, failed = 0, accurate = 0, noMatch = 0;
  bool cancelled = false;
  BString arNote, error, errorsNote;
  int32 tracksWithErrors = 0;
  msg->FindInt32("ripped", &ripped);
  msg->FindInt32("skipped", &skipped);
  msg->FindInt32("failed", &failed);
  msg->FindInt32("accurate", &accurate);
  msg->FindInt32("no_match", &noMatch);
  msg->FindBool("cancelled", &cancelled);
  msg->FindString("ar_note", &arNote);
  msg->FindString("error", &error);
  msg->FindString("errors_note", &errorsNote);
  msg->FindInt32("tracks_with_errors", &tracksWithErrors);

  if (cancelled && fCurrentTrack >= 0)
    _SetTrackStatus(fCurrentTrack, B_TRANSLATE("Stopped"));
  fCurrentTrack = -1;

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
  if (accurate + noMatch > 0) {
    BString part;
    part.SetToFormat(B_TRANSLATE("%ld accurate"), (long)accurate);
    summary << ", " << part;
  }
  if (tracksWithErrors > 0) {
    BString part;
    if (tracksWithErrors == 1)
      part = B_TRANSLATE("1 with read errors");
    else
      part.SetToFormat(B_TRANSLATE("%ld with read errors"), (long)tracksWithErrors);
    summary << ", " << part;
  }
  if (cancelled)
    summary << " " << B_TRANSLATE("(stopped)");
  else if (!arNote.IsEmpty() && ripped > 0)
    summary << " " << B_TRANSLATE("- AccurateRip:") << " " << arNote;
  if (!cancelled && ripped > 0 && !errorsNote.IsEmpty())
    summary << " " << B_TRANSLATE("- Read errors:") << " " << errorsNote;

  fProgress->SetTo(cancelled ? fProgress->CurrentValue() : kProgressMax,
                   summary.String(), "");
  _SetRipping(false);
  _UpdatePreview();

  // Hand the new files over to be added to the library.
  if (ripped > 0 && fSettings.addToLibrary && fLibraryTarget.IsValid()) {
    BMessage import(MSG_CD_RIP_IMPORT);
    import.AddString("folder", fSettings.directory);
    BString path;
    for (int32 i = 0; msg->FindString("path", i, &path) == B_OK; i++)
      import.AddString("path", path);
    fLibraryTarget.SendMessage(&import);
  }

  if (!error.IsEmpty()) {
    BString text;
    text.SetToFormat(B_TRANSLATE("The disc could not be read: %s"),
                     error.String());
    (new BAlert(B_TRANSLATE("Rip audio CD"), text.String(), B_TRANSLATE("OK"),
                nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT))
        ->Go(nullptr);
  } else if (tracksWithErrors > 0 && !cancelled) {
    BString text;
    text.SetToFormat(
        B_TRANSLATE("%ld tracks still had read errors after re-reading. The "
                    "\"Read errors\" column shows how many sectors (1/75 s "
                    "each): \"bad\" sectors hold the drive's best guess, "
                    "\"lost\" ones are silent. Cleaning the disc and ripping "
                    "those tracks again (delete their files first) may help."),
        (long)tracksWithErrors);
    (new BAlert(B_TRANSLATE("Rip audio CD"), text.String(), B_TRANSLATE("OK"),
                nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT))
        ->Go(nullptr);
  } else if (noMatch > 0 && !cancelled) {
    // A partial mismatch points at the tracks; a total one at the disc.
    const char *text =
        accurate > 0
            ? B_TRANSLATE("Some tracks don't match AccurateRip, although the "
                          "rest of the disc does. They probably have read "
                          "errors. Cleaning the disc and ripping those tracks "
                          "again (delete their files first) may help.")
            : B_TRANSLATE("No track matches AccurateRip. Your copy may be a "
                          "pressing nobody has submitted yet, or the drive's "
                          "read offset is wrong. Empty the read offset field "
                          "to detect it again.");
    (new BAlert(B_TRANSLATE("Rip audio CD"), text, B_TRANSLATE("OK"), nullptr,
                nullptr, B_WIDTH_AS_USUAL, B_INFO_ALERT))
        ->Go(nullptr);
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

void CDRipWindow::_HandleOffset(BMessage *msg) {
  int32 offset = 0, confidence = 0;
  if (msg->FindInt32("offset", &offset) != B_OK)
    return;
  msg->FindInt32("confidence", &confidence);

  BString text;
  text.SetToFormat("%+ld", (long)offset);
  fOffsetControl->SetText(text.String());
  fSettings.SetDriveOffset(fDriveName, offset);
  fSettings.Save();

  fDetectedOffset = text;
  _UpdateDriveInfo();
}

void CDRipWindow::_ShowOutputFolder() {
  BString folder = fOutputFolder.IsEmpty() ? fSettings.directory : fOutputFolder;
  entry_ref ref;
  if (get_ref_for_path(folder.String(), &ref) == B_OK)
    be_roster->Launch(&ref);
}

void CDRipWindow::_PlaySelectedTrack() {
  BRow *row = fTrackList->CurrentSelection();
  const int32 clicked = row != nullptr ? fTrackList->IndexOf(row) : -1;
  if (clicked < 0 || clicked >= (int32)fRippedPaths.size() ||
      fRippedPaths[clicked].IsEmpty() || !fLibraryTarget.IsValid())
    return;

  // Queue every ripped track in disc order, so playback carries on to the
  // next one, as it does in the library.
  BMessage play(MSG_CD_RIP_PLAY);
  int32 index = 0;
  for (int32 i = 0; i < (int32)fRippedPaths.size(); i++) {
    if (fRippedPaths[i].IsEmpty())
      continue;
    if (i == clicked)
      play.AddInt32("index", index);
    const MediaItem &t = fTracks[i];
    play.AddString("path", fRippedPaths[i]);
    play.AddString("title", t.title);
    play.AddString("artist", t.artist);
    play.AddString("album", t.album);
    play.AddInt32("track", t.track);
    play.AddInt32("duration", t.duration);
    index++;
  }
  fLibraryTarget.SendMessage(&play);
}

void CDRipWindow::MessageReceived(BMessage *msg) {
  switch (msg->what) {
  case kMsgFormatChanged:
  case kMsgFolderEdited:
  case kMsgPatternEdited:
  case kMsgVerifyChanged:
    _UpdatePreview();
    break;

  case kMsgToggleAll:
    // A click on the mixed state checks everything, like a plain click.
    _CheckAllTracks(fAllCheck->Value() != B_CONTROL_OFF);
    break;

  case kMsgTrackToggled:
    fLastToggle = system_time();
    _UpdateAllCheck();
    break;

  case kMsgPlayTrack:
    // A double click on a checkbox toggles it twice; that is not a request
    // to play.
    if (system_time() - fLastToggle > 1000000)
      _PlaySelectedTrack();
    break;

  case kMsgOffsetEdited:
    _UpdateDriveInfo();
    _UpdatePreview();
    break;

  case MSG_CD_RIP_STATUS: {
    const char *text = nullptr;
    if (msg->FindString("text", &text) == B_OK)
      fProgress->SetTo(fProgress->CurrentValue(), text, "");
    break;
  }

  case MSG_CD_RIP_OFFSET:
    _HandleOffset(msg);
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
