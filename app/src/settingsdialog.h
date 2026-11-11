// The settings window, with the pages other windows show too (conversion, default script
// properties).
#pragma once

#include "bridge.h"

#include <QDialog>
#include <functional>

class QFormLayout;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent);
    static constexpr int Restored = 2; // exec()'s result after Restore Defaults
    // Kainote's conversion settings on `form`, for this dialog's page and the window shown before a
    // conversion; the function returned stores them
    static std::function<void()> conversionPage(QFormLayout *form, QWidget *owner);
    // those settings for a conversion; converted to ASS, the subtitles take the video's resolution
    // (w, h above 0), else that of new files
    static fsb::ConvertSpec conversionSpec(int w, int h);
    // Kainote's default script properties: the page of this dialog (the function returned stores
    // them) and their applying to `s` before it is saved to `path` (ASS only), asking first when
    // set to; false: the saving was cancelled
    static std::function<void()> scriptDefaultsPage(QFormLayout *form, QWidget *owner);
    static QStringList scriptDefaultKeys();
    static bool applyScriptDefaults(QWidget *parent, fsb::Session &s, const QString &path);

signals:
    void applied(); // OK, Apply or Restore Defaults saved the options

private:
    void buildColorsPage(QFormLayout *form);
};
