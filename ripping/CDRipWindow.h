#ifndef BETON_CD_RIP_WINDOW_H
#define BETON_CD_RIP_WINDOW_H

#include "CDRipSettings.h"
#include "MediaItem.h"

#include <Messenger.h>
#include <String.h>
#include <Window.h>

#include <vector>

class BButton;
class BFilePanel;
class BMenuField;
class BPopUpMenu;
class BStatusBar;
class BStringView;
class BTextControl;
class CDRipper;

/**
 * @class CDRipWindow
 * @brief Lets the user pick a format, folder and naming pattern, then rips
 *        the whole disc with CDRipper.
 *
 * Opened from the sidebar's CD row. When a rip completes, the new files are
 * sent to @p libraryTarget as MSG_CD_RIP_IMPORT so the ones inside a library
 * folder are added to the library.
 */
class CDRipWindow : public BWindow {
public:
  CDRipWindow(const BString &discPath, const BString &discLabel,
              BMessenger libraryTarget);
  ~CDRipWindow() override;

  void MessageReceived(BMessage *msg) override;
  bool QuitRequested() override;

private:
  void _BuildUI();
  void _LoadTracks();
  void _ApplySettings();
  void _ReadControls();
  RipFormat _SelectedFormat() const;

  void _UpdatePreview();
  void _ChooseFolder();
  void _StartRip();
  void _SetRipping(bool ripping);
  void _HandleProgress(BMessage *msg);
  void _HandleTrackDone(BMessage *msg);
  void _HandleFinished(BMessage *msg);
  void _ShowOutputFolder();

  BString fDiscPath;
  BString fDiscLabel;
  BMessenger fLibraryTarget;
  std::vector<MediaItem> fTracks;
  CDRipSettings fSettings;

  CDRipper *fRipper = nullptr;
  BFilePanel *fFolderPanel = nullptr;
  std::vector<BString> fFailures;
  BString fOutputFolder; ///< Folder of the first file written by the last rip.

  BStringView *fDiscInfo = nullptr;
  BPopUpMenu *fFormatMenu = nullptr;
  BMenuField *fFormatField = nullptr;
  BTextControl *fFolderControl = nullptr;
  BButton *fChooseButton = nullptr;
  BTextControl *fPatternControl = nullptr;
  BMenuField *fPresetField = nullptr;
  BStringView *fPreview = nullptr;
  BStatusBar *fProgress = nullptr;
  BButton *fShowButton = nullptr;
  BButton *fCloseButton = nullptr;
  BButton *fRipButton = nullptr;
};

#endif // BETON_CD_RIP_WINDOW_H
